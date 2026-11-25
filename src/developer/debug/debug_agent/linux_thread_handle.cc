// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/linux_thread_handle.h"

#include <algorithm>
#include <cstring>
#include <map>

#include "src/developer/debug/debug_agent/arch.h"
#include "src/developer/debug/debug_agent/linux_suspend_handle.h"
#include "src/developer/debug/debug_agent/linux_utils.h"
#include "src/developer/debug/shared/logging/logging.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace debug_agent {

namespace {

// Sets the field of a writable general register. Returns false for other registers.
bool SetGeneralRegister(user_regs_struct& native, debug::RegisterID id, uint64_t value) {
  switch (id) {
    case debug::RegisterID::kX64_rax:
      native.rax = value;
      return true;
    case debug::RegisterID::kX64_rbx:
      native.rbx = value;
      return true;
    case debug::RegisterID::kX64_rcx:
      native.rcx = value;
      return true;
    case debug::RegisterID::kX64_rdx:
      native.rdx = value;
      return true;
    case debug::RegisterID::kX64_rsi:
      native.rsi = value;
      return true;
    case debug::RegisterID::kX64_rdi:
      native.rdi = value;
      return true;
    case debug::RegisterID::kX64_rbp:
      native.rbp = value;
      return true;
    case debug::RegisterID::kX64_rsp:
      native.rsp = value;
      return true;
    case debug::RegisterID::kX64_r8:
      native.r8 = value;
      return true;
    case debug::RegisterID::kX64_r9:
      native.r9 = value;
      return true;
    case debug::RegisterID::kX64_r10:
      native.r10 = value;
      return true;
    case debug::RegisterID::kX64_r11:
      native.r11 = value;
      return true;
    case debug::RegisterID::kX64_r12:
      native.r12 = value;
      return true;
    case debug::RegisterID::kX64_r13:
      native.r13 = value;
      return true;
    case debug::RegisterID::kX64_r14:
      native.r14 = value;
      return true;
    case debug::RegisterID::kX64_r15:
      native.r15 = value;
      return true;
    case debug::RegisterID::kX64_rip:
      native.rip = value;
      return true;
    case debug::RegisterID::kX64_rflags:
      native.eflags = value;
      return true;
    case debug::RegisterID::kX64_fsbase:
      native.fs_base = value;
      return true;
    case debug::RegisterID::kX64_gsbase:
      native.gs_base = value;
      return true;
    default:
      return false;
  }
}

// Returns the field for a writable debug register, or null for other registers.
uint64_t* GetDebugRegisterField(zx_thread_state_debug_regs_t& regs, debug::RegisterID id) {
  switch (id) {
    case debug::RegisterID::kX64_dr0:
      return &regs.dr[0];
    case debug::RegisterID::kX64_dr1:
      return &regs.dr[1];
    case debug::RegisterID::kX64_dr2:
      return &regs.dr[2];
    case debug::RegisterID::kX64_dr3:
      return &regs.dr[3];
    case debug::RegisterID::kX64_dr6:
      return &regs.dr6;
    case debug::RegisterID::kX64_dr7:
      return &regs.dr7;
    default:
      return nullptr;
  }
}

}  // namespace

LinuxThreadHandle::LinuxThreadHandle(fxl::RefPtr<LinuxTask> task) : task_(std::move(task)) {}

std::string LinuxThreadHandle::GetName() const { return linux::GetStatus(task_->pid())["Name"]; }

ThreadHandle::State LinuxThreadHandle::GetState() const {
  auto state = linux::ThreadStateFromLinuxState(linux::GetStatus(task_->pid())["State"]);
  if (!state)
    return State(debug_ipc::ThreadRecord::State::kDead);
  return *state;
}

std::unique_ptr<SuspendHandle> LinuxThreadHandle::Suspend() {
  DEBUG_LOG(Thread) << "Suspending thread " << task_->pid();
  return std::make_unique<LinuxSuspendHandle>(task_);
}

bool LinuxThreadHandle::WaitForSuspension(TickTimePoint deadline) const {
  // Linux suspensions are synchronous so we never need to wait for one.
  return task_->is_suspended();
}

debug_ipc::ThreadRecord LinuxThreadHandle::GetThreadRecord(zx_koid_t process_koid) const {
  DEBUG_LOG(Thread) << "LinuxThreadHandle::GetThreadRecord";
  auto map = linux::GetStatus(task_->pid());

  debug_ipc::ThreadRecord record;
  record.id = {.process = process_koid, .thread = static_cast<uint64_t>(task_->pid())};
  record.name = map["Name"];

  if (auto state = linux::ThreadStateFromLinuxState(map["State"])) {
    record.state = state->state;
    record.blocked_reason = state->blocked_reason;
  } else {
    record.state = debug_ipc::ThreadRecord::State::kDead;
  }

  return record;
}

std::optional<GeneralRegisters> LinuxThreadHandle::GetGeneralRegisters() const {
  if (auto result = linux::ReadGeneralRegisters(task_->pid())) {
    DEBUG_LOG(Thread) << "General registers IP = " << std::hex << "0x" << result->rip;
    return GeneralRegisters(*result);
  }
  return std::nullopt;
}

bool LinuxThreadHandle::SetGeneralRegisters(const GeneralRegisters& regs) {
  return linux::WriteGeneralRegisters(task_->pid(), regs.GetNativeRegisters()) == ZX_OK;
}

std::optional<DebugRegisters> LinuxThreadHandle::GetDebugRegisters() const {
  if (std::optional<zx_thread_state_debug_regs_t> regs = task_->ReadDebugRegisters())
    return DebugRegisters(*regs);
  return std::nullopt;
}

bool LinuxThreadHandle::SetDebugRegisters(const DebugRegisters& regs) {
  debug::Status status = task_->WriteDebugRegisters(regs.GetNativeRegisters());
  if (status.has_error()) {
    DEBUG_LOG(Thread) << "Could not write debug registers: " << status.message();
  }
  return status.ok();
}

void LinuxThreadHandle::SetSingleStep(bool single_step) {
  DEBUG_LOG(Thread) << "LinuxThreadHandle::SetSingleStep " << single_step;
  task_->set_single_step(single_step);
}

std::vector<debug::RegisterValue> LinuxThreadHandle::ReadRegisters(
    const std::vector<debug::RegisterCategory>& cats_to_get) const {
  std::vector<debug::RegisterValue> result;
  if (std::ranges::find(cats_to_get, debug::RegisterCategory::kGeneral) != cats_to_get.end()) {
    if (auto regs = GetGeneralRegisters())
      regs->CopyTo(result);
  }
  if (std::ranges::find(cats_to_get, debug::RegisterCategory::kDebug) != cats_to_get.end()) {
    if (std::optional<zx_thread_state_debug_regs_t> regs = task_->ReadDebugRegisters()) {
      result.emplace_back(debug::RegisterID::kX64_dr0, regs->dr[0]);
      result.emplace_back(debug::RegisterID::kX64_dr1, regs->dr[1]);
      result.emplace_back(debug::RegisterID::kX64_dr2, regs->dr[2]);
      result.emplace_back(debug::RegisterID::kX64_dr3, regs->dr[3]);
      result.emplace_back(debug::RegisterID::kX64_dr6, regs->dr6);
      result.emplace_back(debug::RegisterID::kX64_dr7, regs->dr7);
    }
  }
  return result;
}

debug::Result<std::vector<debug::RegisterValue>, debug::Status> LinuxThreadHandle::WriteRegisters(
    const std::vector<debug::RegisterValue>& regs) {
  // Validate the entire request before changing the kernel.
  std::optional<GeneralRegisters> general;
  std::optional<zx_thread_state_debug_regs_t> debug_regs;
  for (const auto& value : regs) {
    const char* name = debug::RegisterIDToString(value.id);
    if (value.data.size() != sizeof(uint64_t))
      return debug::Status(fxl::StringPrintf("The %s register needs an 8-byte value.", name));

    if (debug::RegisterIDToCategory(value.id) == debug::RegisterCategory::kDebug) {
      if (!debug_regs) {
        debug_regs = task_->ReadDebugRegisters();
        if (!debug_regs) {
          return debug::Status(
              fxl::StringPrintf("Can't read the debug registers of thread %d.", task_->pid()));
        }
      }
      uint64_t* field = GetDebugRegisterField(*debug_regs, value.id);
      if (!field)
        return debug::Status(fxl::StringPrintf("Linux can't write the %s register.", name));
      *field = value.GetValue();
    } else {
      if (!general) {
        general = GetGeneralRegisters();
        if (!general) {
          return debug::Status(
              fxl::StringPrintf("Can't read the general registers of thread %d.", task_->pid()));
        }
      }
      if (!SetGeneralRegister(general->GetNativeRegisters(), value.id, value.GetValue()))
        return debug::Status(fxl::StringPrintf("Linux can't write the %s register.", name));
    }
  }

  std::vector<debug::RegisterCategory> written_categories;
  if (debug_regs) {
    if (debug::Status status = task_->WriteDebugRegisters(*debug_regs); status.has_error())
      return status;
    written_categories.push_back(debug::RegisterCategory::kDebug);
  }
  if (general) {
    if (!SetGeneralRegisters(*general)) {
      return debug::Status(
          fxl::StringPrintf("Can't write the general registers of thread %d.", task_->pid()));
    }
    written_categories.push_back(debug::RegisterCategory::kGeneral);
  }
  return ReadRegisters(written_categories);
}

bool LinuxThreadHandle::InstallHWBreakpoint(uint64_t address) {
  std::optional<DebugRegisters> regs = GetDebugRegisters();
  if (!regs || !regs->SetHWBreakpoint(address))
    return false;
  return SetDebugRegisters(*regs);
}

bool LinuxThreadHandle::UninstallHWBreakpoint(uint64_t address) {
  std::optional<DebugRegisters> regs = GetDebugRegisters();
  if (!regs)
    return IsDead();  // A dead thread has no breakpoints to remove.
  if (!regs->RemoveHWBreakpoint(address))
    return false;
  return SetDebugRegisters(*regs);
}

std::optional<WatchpointInfo> LinuxThreadHandle::InstallWatchpoint(
    debug_ipc::BreakpointType type, const debug::AddressRange& range) {
  if (!debug_ipc::IsWatchpointType(type))
    return std::nullopt;

  std::optional<DebugRegisters> regs = GetDebugRegisters();
  if (!regs)
    return std::nullopt;
  std::optional<WatchpointInfo> result =
      regs->SetWatchpoint(type, range, arch::GetHardwareWatchpointCount());
  if (!result || !SetDebugRegisters(*regs))
    return std::nullopt;
  return result;
}

bool LinuxThreadHandle::UninstallWatchpoint(const debug::AddressRange& range) {
  std::optional<DebugRegisters> regs = GetDebugRegisters();
  if (!regs)
    return IsDead();  // A dead thread has no watchpoints to remove.
  if (!regs->RemoveWatchpoint(range, arch::GetHardwareWatchpointCount()))
    return false;
  return SetDebugRegisters(*regs);
}

bool LinuxThreadHandle::IsDead() const {
  return GetState().state == debug_ipc::ThreadRecord::State::kDead;
}

}  // namespace debug_agent
