// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/linux_task.h"

#include <fcntl.h>
#include <chrono>
#include <thread>
#include <lib/syslog/cpp/macros.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <zircon/errors.h>

#include <linux/unistd.h>

#include "src/developer/debug/debug_agent/linux_exception_handle.h"
#include "src/developer/debug/debug_agent/linux_suspend_handle.h"
#include "src/developer/debug/debug_agent/linux_task_observer.h"
#include "src/developer/debug/debug_agent/linux_utils.h"
#include "src/developer/debug/shared/logging/logging.h"
#include "src/developer/debug/shared/message_loop_linux.h"

namespace debug_agent {

namespace {

const long kPtraceOpts = PTRACE_O_TRACEEXIT | PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC |
                         PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACEVFORKDONE | PTRACE_O_EXITKILL;

// Checks if the given status code from waitpid() is the given ptrace event.
bool IsPtraceEvent(int status, int ptrace_event_type) {
  return status >> 8 == (SIGTRAP | (ptrace_event_type << 8));
}

// Returns true for all messages that can be used with PTRACE_GETEVENTMSG.
bool HasEventMessage(int status) {
  return IsPtraceEvent(status, PTRACE_EVENT_EXEC) || IsPtraceEvent(status, PTRACE_EVENT_CLONE) || IsPtraceEvent(status, PTRACE_EVENT_EXIT) ||
         IsPtraceEvent(status, PTRACE_EVENT_FORK) || IsPtraceEvent(status, PTRACE_EVENT_SECCOMP) ||
         IsPtraceEvent(status, PTRACE_EVENT_VFORK) ||
         IsPtraceEvent(status, PTRACE_EVENT_VFORK_DONE);
}

// Debugging aid for waitpid status values.
std::string WaitpidStatusToString(int status) {
  std::stringstream out;
  out << "status=0x" << std::hex << status << std::dec << ", ";
  if (WIFEXITED(status)) {
    out << "EXITED, exit status=" << WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    out << "SIGNALED";
    if (WCOREDUMP(status))
      out << " CORE DUMP";
    out << ", signal=" << linux::SignalToString(WTERMSIG(status), true);
  } else if (WIFSTOPPED(status)) {
    out << "STOPPED, signal=" << linux::SignalToString(WSTOPSIG(status), true);
  } else if (WIFCONTINUED(status)) {
    out << "CONTINUED";
  } else {
    out << "NOT DECODABLE";
  }

  return out.str();
}

}  // namespace

LinuxTask::LinuxTask(uint64_t pid, bool is_attached)
    : pid_(pid), is_attached_(is_attached), weak_factory_(this) {}

LinuxTask::~LinuxTask() {
  if (is_attached_)
    Detach();
}

debug::Status LinuxTask::Attach() {
  if (is_attached_)
    return debug::Status();

  DEBUG_LOG(Process) << "Attaching to " << pid_;
  if (ptrace(PTRACE_SEIZE, pid_, nullptr, (void*)kPtraceOpts) == -1) {
    DEBUG_LOG(Process) << "PTRACE_SEIZE failed for PID " << pid_ << " errno = " << errno;
    int error = errno;
    // A worker created during attachment may already have been traced by a
    // parent's CLONE option. Adopt it only when this tracer owns it.
    if (error != EPERM || linux::GetStatus(pid_)["TracerPid"] != std::to_string(getpid()))
      return debug::ErrnoStatus(error);
    initial_stop_pending_ = true;
  }

  is_attached_ = true;
  seized_ = true;
  ++generation_;
  return debug::Status();
}

debug::Status LinuxTask::Detach() {
  if (!is_attached_)
    return debug::Status();

  IncrementSuspendCount();
  if (!is_suspended()) {
    DecrementSuspendCount();
    return last_error_;
  }
  if (ptrace(PTRACE_DETACH, pid_, nullptr,
             reinterpret_cast<void*>(static_cast<intptr_t>(resume_signal_))) == -1) {
    last_error_ = debug::ErrnoStatus(errno, "Could not detach task.");
    --suspend_count_;
    return last_error_;
  }
  is_attached_ = false;
  stopped_ = false;
  unsupported_shared_clone_ = false;
  unsupported_abi_ = false;
  ++generation_;
  process_watch_handle_.StopWatching();
  DecrementSuspendCount();
  return debug::Status();
}

void LinuxTask::SetObserver(LinuxTaskObserver* observer) {
  if (observer && !observer_) {
    // Installing observer.
    debug::MessageLoopLinux* loop = debug::MessageLoopLinux::Current();
    process_watch_handle_ =
        loop->WatchChildSignals(pid_, [this](int pid, int status) { OnPidReady(pid, status); });
  } else if (!observer && observer_) {
    // Uninstalling observer.
    process_watch_handle_.StopWatching();
  }
  if (observer_ != observer)
    ++generation_;
  observer_ = observer;
}

void LinuxTask::OnPidReady(int pid, int status) {
  // Notifications can delete the process and its last task reference.
  auto keep_alive = fxl::RefPtr<LinuxTask>(this);
  if (is_attached_ && observer_)
    DispatchEvent(MakeSignalRecord(pid, status));
}

void LinuxTask::DispatchEvent(const SignalRecord& record) {
  if (!is_attached_ || !observer_)
    return;
  if (WIFSTOPPED(record.status))
    stopped_ = true;
  if (IsPtraceEvent(record.status, PTRACE_EVENT_EXEC)) {
    OnExec(record);
  } else if (IsPtraceEvent(record.status, PTRACE_EVENT_CLONE)) {
    OnClone(record);
  } else if (IsPtraceEvent(record.status, PTRACE_EVENT_FORK)) {
    OnFork(record);
  } else if (IsPtraceEvent(record.status, PTRACE_EVENT_VFORK)) {
    OnVFork(record);
  } else if (IsPtraceEvent(record.status, PTRACE_EVENT_EXIT)) {
    OnExit(record);
  } else if (IsPtraceEvent(record.status, PTRACE_EVENT_VFORK_DONE)) {
    Resume();
  } else if (WIFEXITED(record.status) || WIFSIGNALED(record.status)) {
    terminated_ = true;
    exiting_ = true;
    stopped_ = false;
    is_attached_ = false;
    ++generation_;
    process_watch_handle_.StopWatching();
    exit_code_ = WIFEXITED(record.status) ? WEXITSTATUS(record.status)
                                        : 128 + WTERMSIG(record.status);
    ++suspend_count_;
    if (vfork_parent_) {
      --suspend_count_;
      CompleteVfork(false);
      return;
    }
    observer_->OnExited(this, std::make_unique<LinuxExceptionHandle>(
        debug_ipc::ExceptionType::kThreadExiting, fxl::RefPtr<LinuxTask>(this)));
  } else if (WIFSTOPPED(record.status)) {
    OnStopped(record);
  } else if (WIFCONTINUED(record.status)) {
    OnContinued();
  } else {
    // Unexpected wait type.
    DEBUG_LOG(Thread) << "LinuxTask::OnFDReady UNEXPECTED STATUS";
    FX_NOTREACHED();
  }
}

void LinuxTask::OnExec(const SignalRecord& record) {
  unsupported_abi_ = !linux::ReadGeneralRegisters(pid_).has_value();
  if (unsupported_abi_)
    LOGS(Error) << "Exec did not provide native x86-64 registers; detach to release the unsupported image.";
  exiting_ = false;
  single_step_ = false;
  stop_pending_ = false;
  group_stopped_ = false;
  resume_signal_ = 0;
  ++generation_;
  ++suspend_count_;
  LinuxSuspendHandle stop(fxl::RefPtr<LinuxTask>(this), LinuxSuspendHandle::AlreadySuspended());
  if (vfork_parent_) {
    CompleteVfork(true);
    return;
  }
  observer_->OnExec(this, record.event_msg ? static_cast<int>(*record.event_msg) : pid_);
}

void LinuxTask::OnClone(const SignalRecord& record) { OnChild(record); }

void LinuxTask::OnFork(const SignalRecord& record) { OnChild(record); }

void LinuxTask::OnChild(const SignalRecord& record) {
  if (!record.event_msg || *record.event_msg == 0) {
    last_error_ = debug::Status("Could not collect ptrace child identity.");
    LOGS(Error) << last_error_.message();
    return;
  }
  int child_pid = static_cast<int>(*record.event_msg);
  if (observer_->HasTask(child_pid)) {
    // Attach enumeration already collected and registered this kernel child.
    if (suspend_count_ == 0)
      Resume();
    return;
  }
  auto child = fxl::MakeRefCounted<LinuxTask>(child_pid, true);
  child->seized_ = seized_;
  if (auto regs = linux::ReadGeneralRegisters(pid_)) {
    unsigned long flags = 0;
    if (regs->orig_rax == SYS_clone)
      flags = regs->rdi;
#ifdef SYS_clone3
    else if (regs->orig_rax == SYS_clone3) {
      errno = 0;
      auto word = ptrace(PTRACE_PEEKDATA, pid_, regs->rdi, nullptr);
      if (!errno)
        flags = static_cast<unsigned long>(word);
    }
#endif
    child->shared_vm_clone_ = (flags & CLONE_VM) && !(flags & CLONE_THREAD);
  }
  if (IsPtraceEvent(record.status, PTRACE_EVENT_VFORK))
    child->vfork_parent_ = weak_factory_.GetWeakPtr();
  pending_children_[child_pid] = child;
  ++suspend_count_;
  // Waiting by child PID preserves an early initial stop in the kernel. Registration
  // also schedules a scan, so no second SIGCHLD is needed when the child stopped first.
  child->process_watch_handle_ = debug::MessageLoopLinux::Current()->WatchChildSignals(
      child_pid, [weak = weak_factory_.GetWeakPtr(), generation = generation_](int pid, int status) {
        if (weak && weak->generation_ == generation && weak->observer_)
          weak->CollectChild(pid, status);
      });
}

void LinuxTask::CollectChild(int pid, int status) {
  auto keep_alive = fxl::RefPtr<LinuxTask>(this);
  auto found = pending_children_.find(pid);
  if (found == pending_children_.end())
    return;
  auto child = found->second;
  child->process_watch_handle_.StopWatching();
  pending_children_.erase(found);
  if (WIFSTOPPED(status)) {
    child->stopped_ = true;
    ++child->suspend_count_;
    auto exception = std::make_unique<LinuxExceptionHandle>(
        debug_ipc::ExceptionType::kThreadStarting, child);
    auto child_status = linux::GetStatus(pid);
    auto parent_status = linux::GetStatus(pid_);
    if (child->vfork_parent_) {
      observer_->OnVforkStarting(std::move(exception));
      return;  // Hold the parent's event token until the shared image is released.
    }
    if (!child_status["Tgid"].empty() && child_status["Tgid"] == parent_status["Tgid"])
      observer_->OnThreadStarting(std::move(exception));
    else if (child->shared_vm_clone_) {
      unsupported_shared_clone_ = true;
      observer_->OnUnsupportedClone(std::move(exception));
      LOGS(Error) << "A non-thread CLONE_VM process is unsupported. Both tasks remain stopped; "
                    "detach to release them safely.";
      observer_->OnStopSignal(this, std::make_unique<LinuxExceptionHandle>(
          fxl::RefPtr<LinuxTask>(this), siginfo_t{}));
      return;
    } else
      observer_->OnProcessStarting(std::move(exception));
  } else {
    child->is_attached_ = false;
    child->terminated_ = true;
  }
  DecrementSuspendCount();
}

void LinuxTask::OnVFork(const SignalRecord& record) { OnChild(record); }

void LinuxTask::CompleteVfork(bool exec) {
  auto parent = vfork_parent_;
  vfork_parent_.reset();
  if (parent && parent->observer_) {
    parent->observer_->OnVforkFinished(fxl::RefPtr<LinuxTask>(this), exec);
    if (parent)
      parent->DecrementSuspendCount();
  }
}

void LinuxTask::OnExit(const SignalRecord& record) {
  // An exit event is a ptrace stop, not a final wait result. Resume it even when
  // client tokens remain, and retain the watch until the kernel reports death.
  exiting_ = true;
  single_step_ = false;
  resume_signal_ = 0;
  Resume();
}

void LinuxTask::OnSignaled(const SignalRecord& record) {
  DEBUG_LOG(Thread) << "  LinuxTask::OnFDReady SIGNALED "
                    << linux::SignalToString(WTERMSIG(record.status), true);
  observer_->OnTermSignal(pid_, WTERMSIG(record.status));
}

void LinuxTask::OnStopped(const SignalRecord& record) {
  DEBUG_LOG(Thread) << "  LinuxTask::OnFDReady " << pid_ << " STOPPED";
  if ((record.status >> 16) == PTRACE_EVENT_STOP) {
    if (WSTOPSIG(record.status) == SIGTRAP) {
      if (!stop_pending_)
        group_stopped_ = false;  // SIGCONT woke a listening group stop.
      stop_pending_ = false;
      if (suspend_count_ == 0)
        Resume();
    } else {
      group_stopped_ = true;
      if (suspend_count_ == 0)
        Resume();
    }
    return;
  }
  if (!record.siginfo) {
    // GETSIGINFO reports EINVAL for a traditional group stop.
    group_stopped_ = true;
    return;
  }
  group_stopped_ = false;
  if (record.siginfo->si_signo == SIGCHLD || record.siginfo->si_signo == SIGCONT ||
      record.siginfo->si_signo == SIGWINCH) {
    // Routine process and terminal notifications retain their native delivery.
    resume_signal_ = record.siginfo->si_signo;
    if (suspend_count_ == 0)
      Resume();
    return;
  }
  if (vfork_parent_) {
    // The shared image runs without debugger traps until exec or exit.
    resume_signal_ = record.siginfo->si_signo;
    Resume();
    return;
  }

  DEBUG_LOG(Thread) << "Got stop signal " << record.siginfo->si_signo << " code "
                    << record.siginfo->si_code;

  // The thread was stopped and the exception handle is taking ownership of that stop and will
  // resume it in its destructor.
  suspend_count_++;
  DEBUG_LOG(Thread) << "Making exception handle, new suspend count = " << suspend_count_;
  observer_->OnStopSignal(
      this, std::make_unique<LinuxExceptionHandle>(fxl::RefPtr<LinuxTask>(this), *record.siginfo));
}

void LinuxTask::OnContinued() {
  DEBUG_LOG(Thread) << "  LinuxTask::OnFDReady " << pid_ << " CONTINUED";
  observer_->OnContinued(pid_);
}

LinuxTask::SignalRecord LinuxTask::MakeSignalRecord(int pid, int status) {
  SignalRecord rec{.pid = pid_, .status = status};
  if (HasEventMessage(status)) {
    unsigned long msg = 0;
    if (ptrace(PTRACE_GETEVENTMSG, pid, 0, &msg) != -1) {
      rec.event_msg = msg;
    } else {
      DEBUG_LOG(Thread) << "PTRACE_GETEVENTMSG failed with errno=" << errno;
    }
  }

  if (WIFSTOPPED(status)) {
    siginfo_t sig = {};
    if (ptrace(PTRACE_GETSIGINFO, pid, nullptr, &sig) != -1) {
      rec.siginfo = sig;
      if ((status >> 16) == 0 && (sig.si_signo != SIGTRAP || sig.si_code <= 0))
        resume_signal_ = sig.si_signo;
    }
  }

  return rec;
}

void LinuxTask::IncrementSuspendCount() {
  ++suspend_count_;
  if (stopped_ || terminated_)
    return;
  if (!is_attached_) {
    last_error_ = debug::Status("Cannot suspend an unattached task.");
    return;
  }
  if (!stop_pending_ && !initial_stop_pending_) {
    int result = ptrace(PTRACE_INTERRUPT, pid_, nullptr, nullptr);
    if (result == -1) {
      last_error_ = debug::ErrnoStatus(errno, "Could not request task stop.");
      return;
    }
    stop_pending_ = true;
  }

  // Bound synchronous ptrace waits so an uninterruptible task cannot freeze the agent.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  int status = 0;
  for (;;) {
    int result = waitpid(pid_, &status, __WALL | __WNOTHREAD | WNOHANG);
    if (result == pid_)
      break;
    if (result < 0 && errno != EINTR) {
      last_error_ = debug::ErrnoStatus(errno, "Could not collect task stop.");
      return;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      last_error_ = debug::Status("Timed out waiting for task stop.");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  auto record = MakeSignalRecord(pid_, status);
  stopped_ = WIFSTOPPED(status);
  bool our_stop = stopped_ && (initial_stop_pending_ ||
      ((status >> 16) == PTRACE_EVENT_STOP && WSTOPSIG(status) == SIGTRAP));
  initial_stop_pending_ = false;
  if (our_stop) {
    stop_pending_ = false;
    last_error_ = debug::Status();
    return;
  }
  if (!stopped_)
    last_error_ = debug::Status("Task exited while waiting for suspension.");

  // A competing stop must be delivered with the metadata captured at collection.
  // Keep its token until replay, and invalidate replay on detach or observer replacement.
  ++suspend_count_;
  debug::MessageLoop::Current()->PostTask(
      FROM_HERE, [weak = weak_factory_.GetWeakPtr(), generation = generation_, record]() {
        if (!weak)
          return;
        auto keep_alive = fxl::RefPtr<LinuxTask>(weak.get());
        if (weak->generation_ == generation && weak->observer_)
          weak->DispatchEvent(record);
        weak->DecrementSuspendCount();
      });
}

debug::Status LinuxTask::Resume() {
  if (unsupported_abi_) {
    last_error_ = debug::Status("Cannot resume an unsupported inferior ABI; detach first.");
    return last_error_;
  }
  if (unsupported_shared_clone_) {
    last_error_ = debug::Status("Cannot resume an unsupported shared-address-space clone; detach first.");
    LOGS(Error) << last_error_.message();
    return last_error_;
  }
  if (!is_attached_ || !stopped_ || terminated_)
    return debug::Status();
  auto request = group_stopped_ && seized_ ? PTRACE_LISTEN
                                         : (single_step_ ? PTRACE_SINGLESTEP : PTRACE_CONT);
  if (ptrace(request, pid_, nullptr,
             reinterpret_cast<void*>(static_cast<intptr_t>(resume_signal_))) == -1) {
    last_error_ = debug::ErrnoStatus(errno, "Could not resume task.");
    LOGS(Error) << last_error_.message();
    return last_error_;
  }
  stopped_ = false;
  resume_signal_ = 0;
  last_error_ = debug::Status();
  return last_error_;
}

void LinuxTask::DecrementSuspendCount() {
  FX_DCHECK(suspend_count_ > 0);
  --suspend_count_;
  if (suspend_count_ == 0)
    Resume();
}

}  // namespace debug_agent
