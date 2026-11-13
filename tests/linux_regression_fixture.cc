#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <filesystem>
#include <string_view>

#include "src/developer/debug/debug_agent/linux_utils.h"

namespace {
int CheckShutdown(const char* mode, const char* debugger, const char* inferior) {
  if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
    return 1;
  char directory[] = "/tmp/zxdb-shutdown-XXXXXX";
  if (!mkdtemp(directory))
    return 1;
  std::string script_path = std::string(directory) + "/commands";
  std::string marker_path = std::string(directory) + "/released";
  std::ofstream script(script_path);
  script << "[zxdb] set console-mode = shell\n[zxdb] status\nConnected to\n"
         << "[zxdb] break StepFirst\nCreated Breakpoint\n[zxdb] run " << inferior
         << " shutdown " << marker_path << "\non bp 1\n";
  if (std::string_view(mode) == "detach")
    script << "[zxdb] detach\ndetached.\n";
  if (std::string_view(mode) == "frontend-crash")
    script << "waiting for external frontend termination\n";
  else
    script << "[zxdb] quit --force\n";
  script.close();
  std::string script_arg = "--script-file=" + script_path;
  std::string symbol_arg = std::string("--symbol-path=") + inferior;
  std::string cache_arg = std::string("--symbol-cache=") + directory;
  pid_t child = fork();
  if (child == 0) {
    setpgid(0, 0);
    execl(debugger, debugger, "--local", "--enable-debug-adapter", "--debug-adapter-port=0",
          script_arg.c_str(), symbol_arg.c_str(), cache_arg.c_str(), nullptr);
    _exit(127);
  }
  if (child < 0)
    return 1;
  setpgid(child, child);
  bool frontend_ok = false;
  bool inferior_ok = false;
  bool failed = false;
  bool crashed = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  for (;;) {
    if (std::string_view(mode) == "frontend-crash" && !crashed) {
      int inferior_pid = 0;
      std::ifstream(marker_path + ".ready") >> inferior_pid;
      if (inferior_pid > 0) {
        auto status = debug_agent::linux::GetStatus(inferior_pid);
        if (!status["State"].empty() && status["State"][0] == 't') {
          kill(child, SIGKILL);
          crashed = true;
        }
      }
    }
    int status = 0;
    pid_t result = waitpid(-1, &status, WNOHANG);
    if (result > 0) {
      if (result == child)
        frontend_ok = crashed ? WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL
                              : WIFEXITED(status) && WEXITSTATUS(status) == 0;
      else if (WIFEXITED(status) && WEXITSTATUS(status) == 7)
        inferior_ok = true;
      else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        failed = true;
    } else if (result < 0 && errno == ECHILD) {
      break;
    } else if (std::chrono::steady_clock::now() >= deadline) {
      kill(-child, SIGKILL);
      while (waitpid(-1, &status, 0) > 0 || errno == EINTR) {}
      failed = true;
      break;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  std::string marker;
  std::ifstream(marker_path) >> marker;
  std::filesystem::remove_all(directory);
  if (!frontend_ok || !inferior_ok || marker != "released" || failed) {
    std::cerr << "Shutdown did not release and reap a trap-free inferior.\n";
    return 1;
  }
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2)
    return 1;
  std::string_view mode(argv[1]);
  if (mode == "--shutdown" && argc == 5)
    return CheckShutdown(argv[2], argv[3], argv[4]);
  if (mode == "--sigpipe" && argc > 3) {
    struct sigaction action{};
    action.sa_handler = std::string_view(argv[2]) == "ignored" ? SIG_IGN : SIG_DFL;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, nullptr) != 0)
      return 1;
    execv(argv[3], argv + 3);
    return 1;
  }
  if (mode == "--auxv-child") {
    auto before = debug_agent::linux::ReadAuxv(getpid());
    if (before.empty() || chmod(argv[0], 0111) != 0)
      return 1;
    int fd = open("/proc/self/exe", O_RDONLY);
    if (fd >= 0) {
      close(fd);
      return 77;  // Privileged users can bypass the permission check.
    }
    if (errno != EACCES)
      return 1;
    return debug_agent::linux::ReadAuxv(getpid()) == before ? 0 : 1;
  }
  if (mode != "--auxv")
    return 1;

  // Change permissions only on a private copy, never on the build artifact.
  char path[] = "/tmp/zxdb-auxv-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0)
    return 1;
  close(fd);
  std::error_code error;
  std::filesystem::copy_file("/proc/self/exe", path,
                            std::filesystem::copy_options::overwrite_existing, error);
  if (error || chmod(path, 0700) != 0) {
    unlink(path);
    return 1;
  }
  pid_t child = fork();
  if (child == 0) {
    execl(path, path, "--auxv-child", nullptr);
    _exit(1);
  }
  int status = 0;
  pid_t waited = -1;
  if (child > 0) {
    do {
      waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
  }
  unlink(path);
  return waited > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
