// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/linux_task.h"

#include <inttypes.h>
#include <lib/fit/defer.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "src/developer/debug/debug_agent/linux_binary_launcher.h"
#include "src/developer/debug/debug_agent/linux_exception_handle.h"
#include "src/developer/debug/debug_agent/linux_process_handle.h"
#include "src/developer/debug/debug_agent/linux_thread_handle.h"
#include "src/developer/debug/debug_agent/linux_utils.h"
#include "src/developer/debug/debug_agent/process_handle_observer.h"
#include "src/developer/debug/shared/message_loop_linux.h"
#include "src/lib/fxl/strings/string_printf.h"

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
  void OnProcessStarting(std::unique_ptr<ProcessHandle> handle,
                         bool inherited_breakpoints) override {
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
    EXPECT_EQ(expected_threads - observer.execs * (std::string(mode) == "worker-exec"),
              observer.thread_exits);
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
      auto written = thread->WriteRegisters(
          {debug::RegisterValue(debug::RegisterID::kX64_r15, uint64_t{12345})});
      ASSERT_TRUE(written.ok()) << written.err().message();
      EXPECT_FALSE(written.value().empty());
      EXPECT_EQ(12345u, thread->GetGeneralRegisters()->GetNativeRegisters().r15);
      EXPECT_TRUE(thread
                      ->WriteRegisters(
                          {debug::RegisterValue(debug::RegisterID::kX64_r15, uint64_t{original})})
                      .ok());
      auto unsupported = thread->WriteRegisters(
          {debug::RegisterValue(debug::RegisterID::kX64_mxcsr, uint64_t{0})});
      ASSERT_TRUE(unsupported.has_error());
      EXPECT_EQ("Linux can't write the mxcsr register.", unsupported.err().message());
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

// A forked child has this variable at the same address as the test.
volatile uint64_t g_watched_variable = 0;

// Returns the debug registers of the thread by ID.
std::map<debug::RegisterID, uint64_t> ReadDebugRegisterValues(const ThreadHandle& thread) {
  std::map<debug::RegisterID, uint64_t> result;
  for (const auto& value : thread.ReadRegisters({debug::RegisterCategory::kDebug}))
    result[value.id] = value.GetValue();
  return result;
}

TEST_F(LinuxTaskTest, DebugRegisters) {
  // The child writes the watched variable after the test detaches and releases it.
  int ready[2], release[2];
  ASSERT_EQ(0, pipe(ready));
  ASSERT_EQ(0, pipe(release));
  pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    close(ready[0]);
    close(release[1]);
    // The test releases the child by closing the pipe.
    char byte = 0;
    if (write(ready[1], &byte, 1) != 1 || read(release[0], &byte, 1) != 0)
      _exit(1);
    g_watched_variable = 1;
    _exit(7);
  }
  ChildGuard cleanup(pid);
  close(ready[1]);
  close(release[0]);
  char byte;
  ASSERT_EQ(1, read(ready[0], &byte, 1));
  close(ready[0]);

  const uint64_t address = reinterpret_cast<uint64_t>(&g_watched_variable);
  // Slot 0 is enabled for writes of 8 bytes.
  constexpr uint64_t kWriteWatch8 = 0x1 | (0b01 << 16) | (0b10 << 18);
  {
    ProcessObserver observer(loop);
    observer.process = std::make_unique<LinuxProcessHandle>(fxl::MakeRefCounted<LinuxTask>(pid));
    ASSERT_TRUE(observer.process->Attach(&observer, {}).ok());
    auto threads = observer.process->GetChildThreads();
    ASSERT_EQ(1u, threads.size());
    ThreadHandle& thread = *threads[0];
    {
      auto token = thread.Suspend();
      ASSERT_TRUE(thread.WaitForSuspension(TickTimePoint::max()));

      std::map<debug::RegisterID, uint64_t> regs = ReadDebugRegisterValues(thread);
      EXPECT_EQ(6u, regs.size());
      EXPECT_EQ(0u, regs[debug::RegisterID::kX64_dr0]);
      EXPECT_EQ(0u, regs[debug::RegisterID::kX64_dr7]);

      auto written =
          thread.WriteRegisters({debug::RegisterValue(debug::RegisterID::kX64_dr0, address),
                                 debug::RegisterValue(debug::RegisterID::kX64_dr7, kWriteWatch8)});
      ASSERT_TRUE(written.ok()) << written.err().message();
      EXPECT_EQ(6u, written.value().size());
      regs = ReadDebugRegisterValues(thread);
      EXPECT_EQ(address, regs[debug::RegisterID::kX64_dr0]);
      EXPECT_EQ(kWriteWatch8, regs[debug::RegisterID::kX64_dr7]);

      // An enabled slot can move to another aligned address.
      written =
          thread.WriteRegisters({debug::RegisterValue(debug::RegisterID::kX64_dr0, address + 8)});
      ASSERT_TRUE(written.ok()) << written.err().message();
      EXPECT_EQ(address + 8, ReadDebugRegisterValues(thread)[debug::RegisterID::kX64_dr0]);
      ASSERT_TRUE(
          thread.WriteRegisters({debug::RegisterValue(debug::RegisterID::kX64_dr0, address)}).ok());

      // Invalid writes fail and don't change the registers.
      auto failed =
          thread.WriteRegisters({debug::RegisterValue(debug::RegisterID::kX64_dr0, address + 1)});
      ASSERT_TRUE(failed.has_error());
      EXPECT_EQ(fxl::StringPrintf("DR0 = 0x%" PRIx64
                                  " isn't aligned to the 8-byte length that DR7 sets for slot 0.",
                                  address + 1),
                failed.err().message());

      constexpr uint64_t kKernelAddress = 0xffff800000000000;
      failed = thread.WriteRegisters(
          {debug::RegisterValue(debug::RegisterID::kX64_dr1, kKernelAddress)});
      ASSERT_TRUE(failed.has_error());
      EXPECT_EQ("DR1 = 0xffff800000000000 isn't a user-space address.", failed.err().message());

      failed =
          thread.WriteRegisters({debug::RegisterValue(debug::RegisterID::kX64_dr2, uint32_t{0})});
      ASSERT_TRUE(failed.has_error());
      EXPECT_EQ("The dr2 register needs an 8-byte value.", failed.err().message());

      regs = ReadDebugRegisterValues(thread);
      EXPECT_EQ(address, regs[debug::RegisterID::kX64_dr0]);
      EXPECT_EQ(0u, regs[debug::RegisterID::kX64_dr1]);
      EXPECT_EQ(kWriteWatch8, regs[debug::RegisterID::kX64_dr7]);
    }

    // The detach restores the debug registers. Otherwise, the write of the child stops it with
    // SIGTRAP and no tracer.
    ASSERT_TRUE(observer.process->Detach().ok());
  }
  close(release[1]);
  int status;
  ASSERT_EQ(pid, waitpid(pid, &status, 0));
  ASSERT_TRUE(WIFEXITED(status)) << "The child stopped with signal " << WTERMSIG(status);
  EXPECT_EQ(7, WEXITSTATUS(status));
}

TEST_F(LinuxTaskTest, FloatingPointRegisters) {
  // The child has three threads that wait for the test.
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

  // 7.5 in the x87 format, in a 16-byte slot.
  const uint8_t kSevenPointFive[16] = {0, 0, 0, 0, 0, 0, 0, 0xf0, 0x01, 0x40};
  {
    ProcessObserver observer(loop);
    observer.process = std::make_unique<LinuxProcessHandle>(fxl::MakeRefCounted<LinuxTask>(pid));
    ASSERT_TRUE(observer.process->Attach(&observer, {}).ok());
    auto threads = observer.process->GetChildThreads();
    ASSERT_EQ(3u, threads.size());
    std::vector<std::unique_ptr<SuspendHandle>> tokens;
    std::vector<user_fpregs_struct> originals;
    for (const auto& thread : threads) {
      tokens.push_back(thread->Suspend());
      ASSERT_TRUE(thread->WaitForSuspension(TickTimePoint::max()));
      std::optional<user_fpregs_struct> original =
          linux::ReadFloatingPointRegisters(thread->GetKoid());
      ASSERT_TRUE(original);
      originals.push_back(*original);
    }

    for (size_t i = 0; i < threads.size(); i++) {
      ThreadHandle& thread = *threads[i];
      const user_fpregs_struct* original = &originals[i];

      // The registers have the widths that Fuchsia reports.
      std::map<debug::RegisterID, size_t> sizes;
      for (const auto& value : thread.ReadRegisters({debug::RegisterCategory::kFloatingPoint}))
        sizes[value.id] = value.data.size();
      EXPECT_EQ(14u, sizes.size());
      EXPECT_EQ(2u, sizes[debug::RegisterID::kX64_fcw]);
      EXPECT_EQ(1u, sizes[debug::RegisterID::kX64_ftw]);
      EXPECT_EQ(8u, sizes[debug::RegisterID::kX64_fip]);
      EXPECT_EQ(16u, sizes[debug::RegisterID::kX64_st7]);

      // Each thread gets different values.
      uint16_t fcw = static_cast<uint16_t>(0x27f + i);
      uint8_t ftw = static_cast<uint8_t>(0x80 >> i);
      std::vector<uint8_t> st_slot(std::begin(kSevenPointFive), std::end(kSevenPointFive));
      st_slot[10] = 0xff;  // Bytes after the first 10 are reserved and aren't written.
      auto written =
          thread.WriteRegisters({debug::RegisterValue(debug::RegisterID::kX64_fcw, fcw),
                                 debug::RegisterValue(debug::RegisterID::kX64_ftw, ftw),
                                 debug::RegisterValue(debug::RegisterID::kX64_st1, st_slot)});
      ASSERT_TRUE(written.ok()) << written.err().message();
      EXPECT_EQ(14u, written.value().size());

      std::optional<user_fpregs_struct> changed =
          linux::ReadFloatingPointRegisters(thread.GetKoid());
      ASSERT_TRUE(changed);
      EXPECT_EQ(fcw, changed->cwd);
      EXPECT_EQ(ftw, changed->ftw & 0xff);
      EXPECT_EQ(original->ftw & 0xff00, changed->ftw & 0xff00);
      EXPECT_EQ(0, memcmp(kSevenPointFive, &changed->st_space[4], 10));
      EXPECT_EQ(0, memcmp(reinterpret_cast<const uint8_t*>(&original->st_space[4]) + 10,
                          reinterpret_cast<const uint8_t*>(&changed->st_space[4]) + 10, 6));

      // Other state doesn't change.
      EXPECT_EQ(original->swd, changed->swd);
      EXPECT_EQ(original->mxcsr, changed->mxcsr);
      EXPECT_EQ(0, memcmp(original->st_space, changed->st_space, 16));
      EXPECT_EQ(0, memcmp(original->xmm_space, changed->xmm_space, sizeof(original->xmm_space)));

      // Invalid writes fail and change nothing.
      auto failed =
          thread.WriteRegisters({debug::RegisterValue(debug::RegisterID::kX64_fcw, uint8_t{0}),
                                 debug::RegisterValue(debug::RegisterID::kX64_fsw, uint16_t{0})});
      ASSERT_TRUE(failed.has_error());
      EXPECT_EQ("The fcw register needs a 2-byte value.", failed.err().message());
      failed = thread.WriteRegisters(
          {debug::RegisterValue(debug::RegisterID::kX64_st0, std::vector<uint8_t>(10))});
      ASSERT_TRUE(failed.has_error());
      EXPECT_EQ("The st0 register needs a 16-byte value.", failed.err().message());
      EXPECT_EQ(fcw, linux::ReadFloatingPointRegisters(thread.GetKoid())->cwd);

      // An invalid debug register in the same request leaves the valid FCW change unwritten.
      constexpr uint64_t kIoAccessDr7 = 0x1 | (0b10 << 16);
      failed = thread.WriteRegisters(
          {debug::RegisterValue(debug::RegisterID::kX64_fcw, uint16_t{0x037f}),
           debug::RegisterValue(debug::RegisterID::kX64_dr7, kIoAccessDr7)});
      ASSERT_TRUE(failed.has_error());
      EXPECT_EQ("DR7 enables slot 0 for I/O access, which isn't supported.",
                failed.err().message());
      EXPECT_EQ(fcw, linux::ReadFloatingPointRegisters(thread.GetKoid())->cwd);
      EXPECT_EQ(0u, ReadDebugRegisterValues(thread)[debug::RegisterID::kX64_dr7]);
    }

    // Each thread kept its own values. Then restore the original state.
    for (size_t i = 0; i < threads.size(); i++) {
      std::optional<user_fpregs_struct> regs =
          linux::ReadFloatingPointRegisters(threads[i]->GetKoid());
      ASSERT_TRUE(regs);
      EXPECT_EQ(0x27f + i, regs->cwd);
      ASSERT_TRUE(linux::WriteFloatingPointRegisters(threads[i]->GetKoid(), originals[i]).ok());
    }
    tokens.clear();
    observer.process->Detach();
  }
  close(release[1]);
  int status;
  ASSERT_EQ(pid, waitpid(pid, &status, 0));
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(7, WEXITSTATUS(status));
}

TEST_F(LinuxTaskTest, PtraceReadsProtectedMemory) {
  size_t page_size = getpagesize();
  void* mapping =
      mmap(nullptr, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
    for (;;)
      pause();
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
    EXPECT_TRUE(
        observer.process
            ->ReadMemory(reinterpret_cast<uintptr_t>(mapping) + 3, data, sizeof(data), &actual)
            .ok());
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
    for (;;)
      pause();
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

// The task that a tracer from RunOtherTracer traces.
struct TracedTask {
  pid_t process = 0;
  pid_t tid = 0;
};

// Runs in a forked tracer process. The tracer forks a child that waits, seizes one task of the
// child, and writes the child's PID and the task's TID to the file descriptor. The task is a worker
// thread when trace_worker is set and the main thread otherwise. The tracer exits when a step
// fails, so that the reader gets an end of file and doesn't wait.
[[noreturn]] void RunOtherTracer(int fd, bool trace_worker) {
  int tid_fds[2];
  if (pipe(tid_fds) != 0)
    _exit(1);
  pid_t process = fork();
  if (process == -1)
    _exit(1);
  if (process == 0) {
    // The child reports the task to trace and then waits to be killed.
    auto report_tid = [fd = tid_fds[1]] {
      pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
      if (write(fd, &tid, sizeof(tid)) != sizeof(tid))
        _exit(1);
      while (true)
        pause();
    };
    if (trace_worker) {
      std::thread(report_tid).detach();
    } else {
      report_tid();
    }
    while (true)
      pause();
  }
  close(tid_fds[1]);

  pid_t ids[2] = {process, 0};
  if (read(tid_fds[0], &ids[1], sizeof(ids[1])) != sizeof(ids[1]) ||
      ptrace(PTRACE_SEIZE, ids[1], nullptr, nullptr) != 0 ||
      write(fd, ids, sizeof(ids)) != sizeof(ids)) {
    kill(process, SIGKILL);
    _exit(1);
  }
  while (true)
    pause();
}

// Reads the IDs that RunOtherTracer writes. Returns nullopt when the tracer failed.
std::optional<TracedTask> ReadTracedTask(int fd) {
  pid_t ids[2] = {0, 0};
  if (read(fd, ids, sizeof(ids)) != sizeof(ids) || ids[0] <= 0 || ids[1] <= 0)
    return std::nullopt;
  return TracedTask{.process = ids[0], .tid = ids[1]};
}

// Attaches to a task that another process traces and checks the error message.
void ExpectOtherTracerError(bool trace_worker) {
  int fds[2];
  ASSERT_EQ(0, pipe(fds));
  pid_t tracer = fork();
  ASSERT_NE(-1, tracer);
  if (tracer == 0) {
    close(fds[0]);
    RunOtherTracer(fds[1], trace_worker);
  }
  ChildGuard tracer_guard(tracer);
  close(fds[1]);
  std::optional<TracedTask> traced = ReadTracedTask(fds[0]);
  close(fds[0]);
  ASSERT_TRUE(traced) << "The tracer process couldn't trace its child.";
  auto kill_traced = fit::defer([process = traced->process] { kill(process, SIGKILL); });

  auto task = fxl::MakeRefCounted<LinuxTask>(traced->tid);
  debug::Status status = task->Attach();
  ASSERT_TRUE(status.has_error());

  std::string expected_task =
      trace_worker ? fxl::StringPrintf("thread %d of process %d", traced->tid, traced->process)
                   : fxl::StringPrintf("process %d", traced->process);
  EXPECT_EQ(fxl::StringPrintf("Can't attach to %s: %s. Process %d already traces it.",
                              expected_task.c_str(), strerror(EPERM), tracer),
            status.message());
}

TEST_F(LinuxTaskTest, AttachReportsOtherTracer) { ExpectOtherTracerError(false); }

TEST_F(LinuxTaskTest, AttachReportsOtherTracerOfThread) { ExpectOtherTracerError(true); }

}  // namespace
}  // namespace debug_agent
