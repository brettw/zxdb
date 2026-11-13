// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_LINUX_BINARY_LAUNCHER_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_LINUX_BINARY_LAUNCHER_H_

#include "src/developer/debug/debug_agent/binary_launcher.h"
#include "src/developer/debug/debug_agent/linux_suspend_handle.h"
#include "src/developer/debug/debug_agent/linux_task.h"

namespace debug_agent {

class LinuxBinaryLauncher : public BinaryLauncher {
 public:
  // Preserve ignored SIGPIPE inherited by the frontend, not its own ignored setting.
  explicit LinuxBinaryLauncher(bool inherited_sigpipe_ignored = false)
      : inherited_sigpipe_ignored_(inherited_sigpipe_ignored) {}
  ~LinuxBinaryLauncher() override = default;

  // BinaryLauncher implementation:
  debug::Status Setup(const std::vector<std::string>& argv) override;
  StdioHandles ReleaseStdioHandles() override;
  std::unique_ptr<ProcessHandle> GetProcess() const override;
  debug::Status Start() override;

 private:
  // These are valid after Setup().
  bool inherited_sigpipe_ignored_;
  int pid_ = 0;
  fxl::RefPtr<LinuxTask> task_;

  // Indicates that we're keeping a suspend handle reference for the task.
  bool holding_initial_suspend_ = false;

  StdioHandles stdio_handles_;
};

}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_LINUX_BINARY_LAUNCHER_H_
