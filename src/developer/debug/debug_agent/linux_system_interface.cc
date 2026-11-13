// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/linux_system_interface.h"

#include <unistd.h>
#include <sys/sysinfo.h>

#include "src/developer/debug/debug_agent/linux_binary_launcher.h"
#include "src/developer/debug/debug_agent/linux_job_handle.h"

namespace debug_agent {

LinuxSystemInterface::LinuxSystemInterface(bool inherited_sigpipe_ignored)
    : inherited_sigpipe_ignored_(inherited_sigpipe_ignored), component_manager_(this) {}

uint32_t LinuxSystemInterface::GetNumCpus() const {
  long count = sysconf(_SC_NPROCESSORS_ONLN);
  return count > 0 ? static_cast<uint32_t>(count) : 0;
}

uint64_t LinuxSystemInterface::GetPhysicalMemory() const {
  struct sysinfo info{};
  return sysinfo(&info) == 0 ? static_cast<uint64_t>(info.totalram) * info.mem_unit : 0;
}

std::unique_ptr<JobHandle> LinuxSystemInterface::GetRootJob() const {
  // Linux doesn't have a job hierarchy so the default LinuxJobHandle is always the root.
  return std::make_unique<LinuxJobHandle>();
}

std::unique_ptr<BinaryLauncher> LinuxSystemInterface::GetLauncher() const {
  return std::make_unique<LinuxBinaryLauncher>(inherited_sigpipe_ignored_);
}

}  // namespace debug_agent
