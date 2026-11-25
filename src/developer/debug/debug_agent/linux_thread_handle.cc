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

// Each x87 stack register has 10 bytes of data in a 16-byte FXSAVE slot.
constexpr size_t kStackRegisterSlotSize = 16;
constexpr size_t kStackRegisterDataSize = 10;

// Appends the x87 registers with the same widths that Fuchsia reports. The abridged tag word is the
// low byte of ftw, and the high byte is reserved.
void AppendFloatingPointRegisters(const user_fpregs_struct& fp,
                                  std::vector<debug::RegisterValue>& out) {
  out.emplace_back(debug::RegisterID::kX64_fcw, static_cast<uint16_t>(fp.cwd));
  out.emplace_back(debug::RegisterID::kX64_fsw, static_cast<uint16_t>(fp.swd));
  out.emplace_back(debug::RegisterID::kX64_ftw, static_cast<uint8_t>(fp.ftw));
  out.emplace_back(debug::RegisterID::kX64_fop, static_cast<uint16_t>(fp.fop));
  out.emplace_back(debug::RegisterID::kX64_fip, static_cast<uint64_t>(fp.rip));
  out.emplace_back(debug::RegisterID::kX64_fdp, static_cast<uint64_t>(fp.rdp));
  for (int i = 0; i < 8; i++) {
    out.emplace_back(
        static_cast<debug::RegisterID>(static_cast<int>(debug::RegisterID::kX64_st0) + i),
        kStackRegisterSlotSize, &fp.st_space[i * kStackRegisterSlotSize / 4]);
  }
}

// Copies a value of the exact size into a register field.
debug::Status CopyRegisterValue(const debug::RegisterValue& value, void* dest, size_t size) {
  if (value.data.size() != size) {
    return debug::Status(fxl::StringPrintf("The %s register needs a %zu-byte value.",
                                           debug::RegisterIDToString(value.id), size));
  }
  memcpy(dest, value.data.data(), size);
  return debug::Status();
}

// Sets an x87 register. The value must have the width that AppendFloatingPointRegisters() reports.
// The reserved bytes of ftw and of the stack register slots don't change.
debug::Status SetFloatingPointRegister(user_fpregs_struct& fp, const debug::RegisterValue& value) {
  int stack_index = static_cast<int>(value.id) - static_cast<int>(debug::RegisterID::kX64_st0);
  if (stack_index >= 0 && stack_index < 8) {
    uint8_t slot[kStackRegisterSlotSize];
    if (debug::Status status = CopyRegisterValue(value, slot, sizeof(slot)); status.has_error())
      return status;
    memcpy(&fp.st_space[stack_index * kStackRegisterSlotSize / 4], slot, kStackRegisterDataSize);
    return debug::Status();
  }

  switch (value.id) {
    case debug::RegisterID::kX64_fcw:
      return CopyRegisterValue(value, &fp.cwd, sizeof(fp.cwd));
    case debug::RegisterID::kX64_fsw:
      return CopyRegisterValue(value, &fp.swd, sizeof(fp.swd));
    case debug::RegisterID::kX64_ftw: {
      uint8_t tag = 0;
      debug::Status status = CopyRegisterValue(value, &tag, sizeof(tag));
      if (status.ok())
        fp.ftw = static_cast<unsigned short>((fp.ftw & 0xff00) | tag);
      return status;
    }
    case debug::RegisterID::kX64_fop:
      return CopyRegisterValue(value, &fp.fop, sizeof(fp.fop));
    case debug::RegisterID::kX64_fip:
      return CopyRegisterValue(value, &fp.rip, sizeof(fp.rip));
    case debug::RegisterID::kX64_fdp:
      return CopyRegisterValue(value, &fp.rdp, sizeof(fp.rdp));
    default:
      return debug::Status(fxl::StringPrintf("Linux can't write the %s register.",
                                             debug::RegisterIDToString(value.id)));
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
  if (std::ranges::find(cats_to_get, debug::RegisterCategory::kFloatingPoint) !=
      cats_to_get.end()) {
    if (std::optional<user_fpregs_struct> regs = linux::ReadFloatingPointRegisters(task_->pid()))
      AppendFloatingPointRegisters(*regs, result);
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
  std::optional<user_fpregs_struct> floating_point;
  std::optional<zx_thread_state_debug_regs_t> debug_regs;
  for (const auto& value : regs) {
    const char* name = debug::RegisterIDToString(value.id);
    debug::RegisterCategory category = debug::RegisterIDToCategory(value.id);
    if (category == debug::RegisterCategory::kFloatingPoint) {
      if (!floating_point) {
        floating_point = linux::ReadFloatingPointRegisters(task_->pid());
        if (!floating_point) {
          return debug::Status(fxl::StringPrintf(
              "Can't read the floating-point registers of thread %d.", task_->pid()));
        }
      }
      if (debug::Status status = SetFloatingPointRegister(*floating_point, value);
          status.has_error())
        return status;
      continue;
    }

    if (value.data.size() != sizeof(uint64_t))
      return debug::Status(fxl::StringPrintf("The %s register needs an 8-byte value.", name));

    if (category == debug::RegisterCategory::kDebug) {
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

  // The debug registers are written first because only they can fail validation, and their write
  // validates before it changes anything. A failure after the first write restores the register
  // sets that were already written, so a failed request changes nothing.
  std::vector<debug::RegisterCategory> written_categories;
  std::optional<zx_thread_state_debug_regs_t> debug_before;
  std::optional<user_fpregs_struct> floating_point_before;
  auto restore = [&]() {
    if (floating_point_before)
      linux::WriteFloatingPointRegisters(task_->pid(), *floating_point_before);
    if (debug_before)
      task_->WriteDebugRegisters(*debug_before);
  };
  if (debug_regs) {
    debug_before = task_->ReadDebugRegisters();
    if (debug::Status status = task_->WriteDebugRegisters(*debug_regs); status.has_error())
      return status;
    written_categories.push_back(debug::RegisterCategory::kDebug);
  }
  if (floating_point) {
    floating_point_before = linux::ReadFloatingPointRegisters(task_->pid());
    if (debug::Status status = linux::WriteFloatingPointRegisters(task_->pid(), *floating_point);
        status.has_error()) {
      floating_point_before.reset();  // The failed write changed nothing.
      restore();
      return status;
    }
    written_categories.push_back(debug::RegisterCategory::kFloatingPoint);
  }
  if (general) {
    if (!SetGeneralRegisters(*general)) {
      restore();
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
