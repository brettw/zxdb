// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/linux_exception_handle.h"

#include <string.h>
#include <sys/ptrace.h>
#include <zircon/errors.h>

#include "src/developer/debug/debug_agent/linux_arch.h"
#include "src/developer/debug/debug_agent/linux_task.h"
#include "src/developer/debug/debug_agent/linux_thread_handle.h"
#include "src/developer/debug/debug_agent/linux_utils.h"

namespace debug_agent {

LinuxExceptionHandle::LinuxExceptionHandle(fxl::RefPtr<LinuxTask> task, const siginfo_t& info)
    : task_(std::move(task)),
      type_(arch::DecodeExceptionType(info.si_signo, info.si_code)),
      info_(info) {
  // TRAP_BRKPT can also be a program's ICEBP; only a pending debugger step
  // authorizes interpreting it as step completion.
  if (info.si_signo == SIGTRAP &&
      (info.si_code == TRAP_BRKPT || info.si_code == TRAP_TRACE) && !task_->single_step())
    type_ = debug_ipc::ExceptionType::kGeneral;
  if (info.si_signo == SIGTRAP && info.si_code == TRAP_BRKPT) {
    if (auto regs = linux::ReadGeneralRegisters(task_->pid()); regs && regs->rip) {
      errno = 0;
      long instruction = ptrace(PTRACE_PEEKTEXT, task_->pid(), regs->rip - 1, nullptr);
      if (!errno && (instruction & 0xff) == 0xf1)
        type_ = debug_ipc::ExceptionType::kGeneral;
    }
  }
}

// This contructor creates thread start/exit exceptions. There is no siginfo_t since it doesn't
// correspond to a real Linux exception.
LinuxExceptionHandle::LinuxExceptionHandle(debug_ipc::ExceptionType type,
                                           fxl::RefPtr<LinuxTask> thread)
    : task_(std::move(thread)), type_(type) {
  // This contructor is only used for thread fake-"exception" notifications.
  FX_DCHECK(type == debug_ipc::ExceptionType::kThreadStarting ||
            type == debug_ipc::ExceptionType::kThreadExiting);

  // siginfo_t has a union so 0-initialization requires memset.
  memset(&info_, 0, sizeof(info_));
}

LinuxExceptionHandle::~LinuxExceptionHandle() {
  if (info_.si_signo)
    task_->set_resume_signal(resolution_ == Resolution::kTryNext ? info_.si_signo : 0);
  task_->DecrementSuspendCount();
}

std::unique_ptr<ThreadHandle> LinuxExceptionHandle::GetThreadHandle() const {
  return std::make_unique<LinuxThreadHandle>(task_);
}

std::unique_ptr<ProcessHandle> LinuxExceptionHandle::GetProcessHandle() const {
  // not implemented yet.
  return nullptr;
}

debug_ipc::ExceptionType LinuxExceptionHandle::GetType(const ThreadHandle& thread) const {
  return type_;
}

fit::result<debug::Status, ExceptionHandle::Resolution> LinuxExceptionHandle::GetResolution()
    const {
  return fit::success(resolution_);
}

debug::Status LinuxExceptionHandle::SetResolution(Resolution resolution) {
  resolution_ = resolution;
  return debug::Status();
}

fit::result<debug::Status, debug_ipc::ExceptionStrategy> LinuxExceptionHandle::GetStrategy() const {
  return fit::success(debug_ipc::ExceptionStrategy::kFirstChance);
}

debug::Status LinuxExceptionHandle::SetStrategy(debug_ipc::ExceptionStrategy strategy) {
  if (strategy != debug_ipc::ExceptionStrategy::kSecondChance)
    return debug::Status("Unsupported Linux exception strategy.");
  resolution_ = Resolution::kTryNext;
  return debug::Status();
}

debug_ipc::ExceptionRecord LinuxExceptionHandle::GetRecord() const {
  debug_ipc::ExceptionRecord record;
  record.strategy = debug_ipc::ExceptionStrategy::kFirstChance;
  record.linux_signal = info_.si_signo;
  record.linux_code = info_.si_code;
  if (info_.si_code <= 0)
    record.linux_sender_pid = info_.si_pid;
  else if (info_.si_signo == SIGSEGV || info_.si_signo == SIGBUS ||
           info_.si_signo == SIGILL || info_.si_signo == SIGFPE || info_.si_signo == SIGTRAP)
    record.linux_address = reinterpret_cast<uint64_t>(info_.si_addr);
  return record;
}

uint64_t LinuxExceptionHandle::GetPid() const {
  auto status = linux::GetStatus(task_->pid());
  auto found = status.find("Tgid");
  return found == status.end() ? 0 : strtoull(found->second.c_str(), nullptr, 10);
}

uint64_t LinuxExceptionHandle::GetTid() const { return task_->pid(); }
}  // namespace debug_agent
