#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <atomic>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <thread>

namespace {
volatile sig_atomic_t signals = 0;
void SignalHandler(int) { signals = signals + 1; }
}

extern "C" __attribute__((naked, noinline)) void StepFirst() {
  asm volatile("nop\nnop\nret");
}

unsigned long step_first_address = reinterpret_cast<unsigned long>(&StepFirst);

extern "C" __attribute__((naked, noinline)) void FaultFirst() {
  asm volatile("ud2\nret");
}

int main(int argc, char** argv) {
  if (argc < 2)
    return 2;
  if (!strcmp(argv[1], "shutdown") && argc == 3) {
    std::atomic<bool> done = false;
    auto worker = [&] { while (!done) usleep(1000); };
    std::thread first(worker), second(worker);
    std::ofstream(std::string(argv[2]) + ".ready") << getpid();
    StepFirst();
    StepFirst();  // A forgotten trap would terminate this now-untraced process.
    std::ifstream status("/proc/self/status");
    std::string key;
    int tracer = -1;
    while (status >> key) {
      if (key == "TracerPid:") {
        status >> tracer;
        break;
      }
      std::getline(status, key);
    }
    done = true;
    first.join();
    second.join();
    if (tracer != 0 || *reinterpret_cast<const unsigned char*>(&StepFirst) != 0x90)
      return 6;
    std::ofstream(argv[2]) << "released";
  } else if (!strcmp(argv[1], "fault-step")) {
    struct sigaction action{};
    action.sa_flags = SA_SIGINFO;
    action.sa_sigaction = [](int, siginfo_t*, void* context) {
      static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP] += 2;
      signals = signals + 1;
    };
    sigaction(SIGILL, &action, nullptr);
    std::atomic<bool> done = false;
    std::thread peer([&] { while (!done) usleep(1000); });
    FaultFirst();
    done = true;
    peer.join();
    if (signals != 1)
      return 5;
  } else if (!strcmp(argv[1], "group-stop")) {
    raise(SIGSTOP);
  } else if (!strcmp(argv[1], "stepi")) {
    StepFirst();
  } else if (!strcmp(argv[1], "threads")) {
    std::thread first([] {});
    std::thread second([] {});
    first.join();
    second.join();
  } else if (!strcmp(argv[1], "leader-exit")) {
    std::thread([] { usleep(20000); _exit(7); }).detach();
    pthread_exit(nullptr);
  } else if (!strcmp(argv[1], "clone-process") || !strcmp(argv[1], "clone-vm")) {
    size_t size = 1024 * 1024;
    void* stack = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED)
      return 3;
    int flags = !strcmp(argv[1], "clone-vm") ? CLONE_VM : 0;
    pid_t pid = clone([](void*) { return 7; }, static_cast<char*>(stack) + size, flags, nullptr);
    int status;
    if (pid < 0 || waitpid(pid, &status, __WCLONE) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 7)
      return 3;
    munmap(stack, size);
  } else if (!strcmp(argv[1], "fork")) {
    pid_t pid = fork();
    if (pid == 0)
      _exit(7);
    int status;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 7)
      return 3;
  } else if (!strcmp(argv[1], "vfork") || !strcmp(argv[1], "vfork-exit")) {
    bool exec_child = !strcmp(argv[1], "vfork");
    pid_t pid = vfork();
    if (pid == 0) {
      if (exec_child)
        execl(argv[0], argv[0], "output", nullptr);
      _exit(7);
    }
    int status;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 7)
      return 3;
    StepFirst();
  } else if (!strcmp(argv[1], "signal")) {
    signal(SIGUSR1, SignalHandler);
    raise(SIGUSR1);
    if (signals != 1)
      return 4;
  } else if (!strcmp(argv[1], "trap")) {
    signal(SIGTRAP, SignalHandler);
    asm volatile(".byte 0xf1");
    if (signals != 1)
      return 5;
  } else if (!strcmp(argv[1], "exec-same")) {
    StepFirst();
    execl(argv[0], argv[0], "stepi", nullptr);
    return 6;
  } else if (!strcmp(argv[1], "exec")) {
    execl(argv[0], argv[0], "threads", nullptr);
    return 6;
  } else if (!strcmp(argv[1], "worker-exec")) {
    std::thread([&] { execl(argv[0], argv[0], "threads", nullptr); _exit(6); }).join();
  } else if (!strcmp(argv[1], "output")) {
    write(STDOUT_FILENO, "final stdout sentinel\n", 22);
    write(STDERR_FILENO, "final stderr sentinel\n", 22);
  } else {
    return 2;
  }
  return 7;
}
