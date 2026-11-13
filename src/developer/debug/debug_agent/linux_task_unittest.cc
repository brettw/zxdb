// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/linux_task.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <chrono>
#include <unistd.h>

#include <thread>

#include "src/developer/debug/debug_agent/linux_binary_launcher.h"
#include "src/developer/debug/debug_agent/linux_exception_handle.h"
#include "src/developer/debug/debug_agent/linux_process_handle.h"
#include "src/developer/debug/debug_agent/linux_thread_handle.h"
#include "src/developer/debug/debug_agent/linux_utils.h"
#include "src/developer/debug/debug_agent/process_handle_observer.h"
#include "src/developer/debug/shared/message_loop_linux.h"

namespace debug_agent {
namespace {

// Own only this test's child. Fatal assertions and timeouts must not leave it alive.
class ChildGuard {
 public:
  explicit ChildGuard(pid_t pid) : pid_(pid) {}
  ~ChildGuard() {
    int status = 0;
    pid_t result = waitpid(pid_, &status, __WALL | WNOHANG);
    if ((result < 0 && errno == ECHILD) ||
        (result == pid_ && (WIFEXITED(status) || WIFSIGNALED(status))))
      return;
    kill(pid_, SIGKILL);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
      result = waitpid(pid_, &status, __WALL | WNOHANG);
      if (result == pid_ && WIFSTOPPED(status))
        ptrace(PTRACE_CONT, pid_, nullptr, nullptr);
      else if (result == pid_ || (result < 0 && errno == ECHILD))
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
  }
 private:
  pid_t pid_;
};

class ProcessObserver : public ProcessHandleObserver {
 public:
  explicit ProcessObserver(debug::MessageLoopLinux& loop) : loop(loop) {}
  debug::Status OnVfork(bool starting) override { return debug::Status(); }
  void OnExec() override { ++execs; }
  void OnProcessTerminated() override {
    ++exits;
    return_code = process->GetReturnCode();
    if (quit_on_exit)
      loop.QuitNow();
  }
  void OnThreadStarting(std::unique_ptr<ExceptionHandle> exception) override {
    ++starts;
    auto thread = exception->GetThreadHandle();
    EXPECT_TRUE(thread->WaitForSuspension(DebuggedDeadline()));
    EXPECT_TRUE(thread->GetGeneralRegisters());
  }
  void OnThreadExiting(std::unique_ptr<ExceptionHandle>) override { ++thread_exits; }
  void OnException(std::unique_ptr<ExceptionHandle> exception) override {
    ++signals;
    auto record = exception->GetRecord();
    EXPECT_NE(0, record.linux_signal);
    if (record.linux_signal == SIGSTOP) {
      loop.PostTimer(FROM_HERE, 20, [this] {
        auto state = linux::GetStatus(process->GetKoid())["State"];
        EXPECT_FALSE(state.empty());
        EXPECT_TRUE(state[0] == 't' || state[0] == 'T');
        kill(process->GetKoid(), SIGCONT);
      });
    }
    EXPECT_TRUE(exception->SetResolution(ExceptionHandle::Resolution::kTryNext).ok());
  }
  void OnProcessStarting(std::unique_ptr<ProcessHandle> handle, bool inherited_breakpoints) override {
    auto child = std::make_unique<ProcessObserver>(loop);
    child->quit_on_exit = false;
    child->process = std::move(handle);
    EXPECT_TRUE(child->process->Attach(child.get(), {}).ok());
    children.push_back(std::move(child));
  }
  static TickTimePoint DebuggedDeadline() { return TickTimePoint::max(); }
  debug::MessageLoopLinux& loop;
  std::unique_ptr<ProcessHandle> process;
  std::vector<std::unique_ptr<ProcessObserver>> children;
  int execs = 0;
  int exits = 0;
  int thread_exits = 0;
  int starts = 0;
  int signals = 0;
  int return_code = -1;
  bool quit_on_exit = true;
};

class LinuxTaskTest : public testing::Test {
 protected:
  void SetUp() override {
    std::string error;
    ASSERT_TRUE(loop.Init(&error)) << error;
  }
  void TearDown() override { loop.Cleanup(); }
  void RunInferior(const char* mode, int expected_threads = 0, int expected_children = 0) {
    LinuxBinaryLauncher launcher;
    ASSERT_TRUE(launcher.Setup({LINUX_LIFECYCLE_INFERIOR, mode}).ok());
    ProcessObserver observer(loop);
    observer.process = launcher.GetProcess();
    ASSERT_TRUE(observer.process->Attach(&observer, {}).ok());
    int pid = observer.process->GetKoid();
    ChildGuard cleanup(pid);
    ASSERT_TRUE(launcher.Start().ok());
    bool timed_out = false;
    loop.PostTimer(FROM_HERE, 3000, [&] {
      timed_out = true;
      observer.process->Kill();
      loop.QuitNow();
    });
    loop.Run();
    EXPECT_FALSE(timed_out);
    EXPECT_EQ(1, observer.exits);
    EXPECT_EQ(7, observer.return_code);
    EXPECT_EQ(expected_threads, observer.starts);
    EXPECT_EQ(expected_threads - observer.execs * (std::string(mode) == "worker-exec"), observer.thread_exits);
    EXPECT_EQ(expected_children, observer.children.size());
    for (const auto& child : observer.children) {
      EXPECT_EQ(1, child->exits);
      EXPECT_EQ(7, child->return_code);
    }
    int status;
    EXPECT_EQ(-1, waitpid(pid, &status, WNOHANG));
    EXPECT_EQ(ECHILD, errno);
  }
  debug::MessageLoopLinux loop;
};

TEST_F(LinuxTaskTest, VforkExec) { RunInferior("vfork", 0, 1); }
TEST_F(LinuxTaskTest, VforkExit) { RunInferior("vfork-exit"); }
TEST_F(LinuxTaskTest, LeaderExec) { RunInferior("exec", 2); }
TEST_F(LinuxTaskTest, WorkerExec) { RunInferior("worker-exec", 3); }

TEST_F(LinuxTaskTest, CloneAndFinalReap) { RunInferior("threads", 2); }
TEST_F(LinuxTaskTest, LeaderExitWaitsForWorker) { RunInferior("leader-exit", 1); }
TEST_F(LinuxTaskTest, CloneProcessUsesItsOwnTgid) { RunInferior("clone-process", 0, 1); }
TEST_F(LinuxTaskTest, ForkCollectsInitialStop) { RunInferior("fork", 0, 1); }
TEST_F(LinuxTaskTest, GroupStopAndContinue) { RunInferior("group-stop"); }
TEST_F(LinuxTaskTest, ForwardSignal) { RunInferior("signal"); }
TEST_F(LinuxTaskTest, ProgramIcebpIsNotDebuggerStep) { RunInferior("trap"); }

TEST_F(LinuxTaskTest, UnattachedSuspensionIsNotKernelStop) {
  auto task = fxl::MakeRefCounted<LinuxTask>(getpid());
  LinuxThreadHandle thread(task);
  auto token = thread.Suspend();
  EXPECT_FALSE(thread.WaitForSuspension(TickTimePoint::max()));
  EXPECT_TRUE(task->last_error().has_error());
}

TEST_F(LinuxTaskTest, AttachExistingWorkersAndRegisters) {
  int ready[2], release[2];
  ASSERT_EQ(0, pipe(ready));
  ASSERT_EQ(0, pipe(release));
  pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    close(ready[0]);
    close(release[1]);
    auto worker = [&] {
      char byte = 0;
      write(ready[1], &byte, 1);
      read(release[0], &byte, 1);
    };
    std::thread first(worker), second(worker);
    first.join();
    second.join();
    _exit(7);
  }
  ChildGuard cleanup(pid);
  close(ready[1]);
  close(release[0]);
  char bytes[2];
  ASSERT_EQ(1, read(ready[0], bytes, 1));
  ASSERT_EQ(1, read(ready[0], bytes, 1));
  close(ready[0]);
  {
    ProcessObserver observer(loop);
    observer.process = std::make_unique<LinuxProcessHandle>(fxl::MakeRefCounted<LinuxTask>(pid));
    ASSERT_TRUE(observer.process->Attach(&observer, {}).ok());
    auto threads = observer.process->GetChildThreads();
    ASSERT_EQ(3u, threads.size());
    for (const auto& thread : threads) {
      EXPECT_EQ(std::to_string(getpid()), linux::GetStatus(thread->GetKoid())["TracerPid"]);
      auto token = thread->Suspend();
      ASSERT_TRUE(thread->WaitForSuspension(TickTimePoint::max()));
      auto regs = thread->GetGeneralRegisters();
      ASSERT_TRUE(regs);
      auto original = regs->GetNativeRegisters().r15;
      auto written = thread->WriteRegisters({debug::RegisterValue(debug::RegisterID::kX64_r15, uint64_t{12345})});
      EXPECT_FALSE(written.empty());
      EXPECT_EQ(12345u, thread->GetGeneralRegisters()->GetNativeRegisters().r15);
      EXPECT_FALSE(thread->WriteRegisters({debug::RegisterValue(debug::RegisterID::kX64_r15, uint64_t{original})}).empty());
      EXPECT_TRUE(thread->WriteRegisters({debug::RegisterValue(debug::RegisterID::kX64_dr0, uint64_t{1})}).empty());
    }
    observer.process->Detach();
    for (const auto& thread : threads)
      EXPECT_EQ("0", linux::GetStatus(thread->GetKoid())["TracerPid"]);
  }
  close(release[1]);
  int status;
  ASSERT_EQ(pid, waitpid(pid, &status, 0));
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(7, WEXITSTATUS(status));
}

TEST_F(LinuxTaskTest, PtraceReadsProtectedMemory) {
  size_t page_size = getpagesize();
  void* mapping = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(MAP_FAILED, mapping);
  memset(mapping, 0x5a, page_size);
  int ready[2];
  ASSERT_EQ(0, pipe(ready));
  pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (!pid) {
    close(ready[0]);
    mprotect(mapping, page_size, PROT_NONE);
    char byte = 0;
    write(ready[1], &byte, 1);
    for (;;) pause();
  }
  ChildGuard cleanup(pid);
  close(ready[1]);
  char ready_byte;
  ASSERT_EQ(1, read(ready[0], &ready_byte, 1));
  close(ready[0]);
  {
    ProcessObserver observer(loop);
    observer.process = std::make_unique<LinuxProcessHandle>(fxl::MakeRefCounted<LinuxTask>(pid));
    ASSERT_TRUE(observer.process->Attach(&observer, {}).ok());
    uint8_t data[13]{};
    size_t actual = 0;
    EXPECT_TRUE(observer.process->ReadMemory(reinterpret_cast<uintptr_t>(mapping) + 3,
                                             data, sizeof(data), &actual).ok());
    EXPECT_EQ(sizeof(data), actual);
    for (auto value : data)
      EXPECT_EQ(0x5a, value);
    observer.process->Kill();
    loop.Run();
    EXPECT_EQ(137, observer.return_code);
  }
  munmap(mapping, page_size);
}

TEST_F(LinuxTaskTest, DetachInvalidatesDeferredStopAndPreservesSignal) {
  int ready[2];
  ASSERT_EQ(0, pipe(ready));
  pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (!pid) {
    close(ready[0]);
    signal(SIGUSR1, [](int) { _exit(7); });
    char byte = 0;
    write(ready[1], &byte, 1);
    for (;;) pause();
  }
  ChildGuard cleanup(pid);
  close(ready[1]);
  char byte;
  ASSERT_EQ(1, read(ready[0], &byte, 1));
  close(ready[0]);
  ProcessObserver observer(loop);
  observer.process = std::make_unique<LinuxProcessHandle>(fxl::MakeRefCounted<LinuxTask>(pid));
  ASSERT_TRUE(observer.process->Attach(&observer, {}).ok());
  ASSERT_EQ(0, kill(pid, SIGUSR1));
  siginfo_t pending{};
  ASSERT_EQ(0, waitid(P_PID, pid, &pending, WSTOPPED | WNOWAIT | __WALL));
  auto threads = observer.process->GetChildThreads();
  auto stop = threads.front()->Suspend();
  ASSERT_TRUE(threads.front()->WaitForSuspension(TickTimePoint::max()));
  ASSERT_TRUE(observer.process->Detach().ok());
  observer.process.reset();
  loop.PostTask(FROM_HERE, [&] { loop.QuitNow(); });
  loop.Run();
  EXPECT_EQ(0, observer.signals);
  int status;
  ASSERT_EQ(pid, waitpid(pid, &status, 0));
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(7, WEXITSTATUS(status));
}

TEST_F(LinuxTaskTest, SharedVmCloneStopsAndDetachesSafely) {
  class UnsupportedObserver : public ProcessObserver {
   public:
    using ProcessObserver::ProcessObserver;
    void OnException(std::unique_ptr<ExceptionHandle> exception) override {
      held = std::move(exception);
      loop.QuitNow();
    }
    std::unique_ptr<ExceptionHandle> held;
  };
  LinuxBinaryLauncher launcher;
  ASSERT_TRUE(launcher.Setup({LINUX_LIFECYCLE_INFERIOR, "clone-vm"}).ok());
  UnsupportedObserver observer(loop);
  observer.process = launcher.GetProcess();
  ASSERT_TRUE(observer.process->Attach(&observer, {}).ok());
  int pid = observer.process->GetKoid();
  ChildGuard cleanup(pid);
  ASSERT_TRUE(launcher.Start().ok());
  loop.PostTimer(FROM_HERE, 3000, [&] { loop.QuitNow(); });
  loop.Run();
  ASSERT_TRUE(observer.held);
  ASSERT_TRUE(observer.process->Detach().ok());
  observer.held.reset();
  int status;
  ASSERT_EQ(pid, waitpid(pid, &status, 0));
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(7, WEXITSTATUS(status));
}

TEST_F(LinuxTaskTest, AbandonedLaunchIsReaped) {
  int pid;
  {
    LinuxBinaryLauncher launcher;
    ASSERT_TRUE(launcher.Setup({LINUX_LIFECYCLE_INFERIOR, "threads"}).ok());
    pid = launcher.GetProcess()->GetKoid();
  }
  int status;
  EXPECT_EQ(-1, waitpid(pid, &status, WNOHANG));
  EXPECT_EQ(ECHILD, errno);
}

}  // namespace
}  // namespace debug_agent
