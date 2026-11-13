// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/linux_thread_handle.h"

#include <map>
#include <algorithm>
#include <cstring>

#include "src/developer/debug/debug_agent/linux_suspend_handle.h"
#include "src/developer/debug/debug_agent/linux_utils.h"
#include "src/developer/debug/shared/logging/logging.h"

namespace debug_agent {

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
  // TODO(brettw) implement this.
  return std::nullopt;
}

bool LinuxThreadHandle::SetDebugRegisters(const DebugRegisters& regs) {
  // TODO(brettw) implement this.
  return false;
}

void LinuxThreadHandle::SetSingleStep(bool single_step) {
  DEBUG_LOG(Thread) << "LinuxThreadHandle::SetSingleStep " << single_step;
  task_->set_single_step(single_step);
}

std::vector<debug::RegisterValue> LinuxThreadHandle::ReadRegisters(
    const std::vector<debug::RegisterCategory>& cats_to_get) const {
  std::vector<debug::RegisterValue> result;
  if (std::find(cats_to_get.begin(), cats_to_get.end(), debug::RegisterCategory::kGeneral) !=
      cats_to_get.end()) {
    if (auto regs = GetGeneralRegisters())
      regs->CopyTo(result);
  }
  return result;
}

std::vector<debug::RegisterValue> LinuxThreadHandle::WriteRegisters(
    const std::vector<debug::RegisterValue>& regs) {
  auto current = GetGeneralRegisters();
  if (!current)
    return {};
  auto& native = current->GetNativeRegisters();
  for (const auto& value : regs) {
    if (value.data.size() != sizeof(uint64_t))
      return {};
    switch (value.id) {
      case debug::RegisterID::kX64_rax: native.rax = value.GetValue(); break;
      case debug::RegisterID::kX64_rbx: native.rbx = value.GetValue(); break;
      case debug::RegisterID::kX64_rcx: native.rcx = value.GetValue(); break;
      case debug::RegisterID::kX64_rdx: native.rdx = value.GetValue(); break;
      case debug::RegisterID::kX64_rsi: native.rsi = value.GetValue(); break;
      case debug::RegisterID::kX64_rdi: native.rdi = value.GetValue(); break;
      case debug::RegisterID::kX64_rbp: native.rbp = value.GetValue(); break;
      case debug::RegisterID::kX64_rsp: native.rsp = value.GetValue(); break;
      case debug::RegisterID::kX64_r8: native.r8 = value.GetValue(); break;
      case debug::RegisterID::kX64_r9: native.r9 = value.GetValue(); break;
      case debug::RegisterID::kX64_r10: native.r10 = value.GetValue(); break;
      case debug::RegisterID::kX64_r11: native.r11 = value.GetValue(); break;
      case debug::RegisterID::kX64_r12: native.r12 = value.GetValue(); break;
      case debug::RegisterID::kX64_r13: native.r13 = value.GetValue(); break;
      case debug::RegisterID::kX64_r14: native.r14 = value.GetValue(); break;
      case debug::RegisterID::kX64_r15: native.r15 = value.GetValue(); break;
      case debug::RegisterID::kX64_rip: native.rip = value.GetValue(); break;
      case debug::RegisterID::kX64_rflags: native.eflags = value.GetValue(); break;
      case debug::RegisterID::kX64_fsbase: native.fs_base = value.GetValue(); break;
      case debug::RegisterID::kX64_gsbase: native.gs_base = value.GetValue(); break;
      default: return {};  // Validate the entire request before changing the kernel.
    }
  }
  if (!SetGeneralRegisters(*current))
    return {};
  return ReadRegisters({debug::RegisterCategory::kGeneral});
}

bool LinuxThreadHandle::InstallHWBreakpoint(uint64_t address) {
  // TODO(brettw) implement this.
  return false;
}

bool LinuxThreadHandle::UninstallHWBreakpoint(uint64_t address) {
  // TODO(brettw) implement this.
  return false;
}

std::optional<WatchpointInfo> LinuxThreadHandle::InstallWatchpoint(
    debug_ipc::BreakpointType type, const debug::AddressRange& range) {
  // TODO(brettw) implement this.
  return std::nullopt;
}

bool LinuxThreadHandle::UninstallWatchpoint(const debug::AddressRange& range) {
  // TODO(brettw) implement this.
  return false;
}

}  // namespace debug_agent
