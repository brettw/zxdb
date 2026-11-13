// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/local_agent.h"

#include <fcntl.h>
#include <lib/syslog/cpp/macros.h>

#include <thread>

#include "src/developer/debug/debug_agent/debug_agent.h"
#include "src/developer/debug/debug_agent/linux_system_interface.h"
#include "src/developer/debug/debug_agent/remote_api_adapter.h"
#include "src/developer/debug/shared/buffered_bidi_pipe.h"
#include "src/developer/debug/shared/logging/logging.h"
#include "src/developer/debug/shared/platform_message_loop.h"

namespace zxdb {

int RunLocalAgent(fbl::unique_fd read_pipe, fbl::unique_fd write_pipe,
                  bool inherited_sigpipe_ignored, bool debug_mode) {
  debug::SetLogCategories({debug::LogCategory::kAll});
  debug::SetDebugLogging(debug_mode);

  debug::PlatformMessageLoop message_loop;
  std::string init_error_message;
  if (!message_loop.Init(&init_error_message)) {
    FX_LOGS(ERROR) << init_error_message;
    return 1;
  }

  // The scope ensures the objects are destroyed before calling Cleanup on the MessageLoop.
  {
    // The debug agent is independent of whether it's connected or not.
    // DebugAgent::Disconnect is called by ~SocketConnection is called by ~SocketServer, so the
    // debug agent must be destructed after the SocketServer.
    debug_agent::DebugAgent debug_agent(
        std::make_unique<debug_agent::LinuxSystemInterface>(inherited_sigpipe_ignored));
    auto buffer =
        std::make_unique<debug::BufferedBidiPipe>(std::move(read_pipe), std::move(write_pipe));

    debug_agent.TakeAndConnectRemoteAPIStream(std::move(buffer));
    message_loop.Run();
  }

  message_loop.Cleanup();
  return 0;
}

LocalAgentResult ForkLocalAgent(bool inherited_sigpipe_ignored, bool debug_mode) {
  // Two pipes, one for each direction to communicate with the agent.
  const char kPipeError[] = "Can't create pipe to agent process.\n";
  int client_to_agent[2] = {0, 0};
  if (pipe2(client_to_agent, O_NONBLOCK | O_CLOEXEC) == -1) {
    fprintf(stderr, kPipeError);
    return LocalAgentResult(LocalAgentResult::kFailed, 1);
  }

  fbl::unique_fd client_read(client_to_agent[0]), client_write(client_to_agent[1]);
  int agent_to_client[2] = {0, 0};
  if (pipe2(agent_to_client, O_NONBLOCK | O_CLOEXEC) == -1) {
    fprintf(stderr, kPipeError);
    return LocalAgentResult(LocalAgentResult::kFailed, 1);
  }

  fbl::unique_fd agent_read(agent_to_client[0]), agent_write(agent_to_client[1]);
  pid_t child_pid = fork();
  if (child_pid == -1) {
    fprintf(stderr, "Can't fork agent process\n");
    return LocalAgentResult(LocalAgentResult::kFailed, 1);
  } else if (child_pid == 0) {
    // Run the debug agent.

    // Close the handles the frontend uses.
    client_write.reset();
    agent_read.reset();
    int exit_code =
        RunLocalAgent(std::move(client_read), std::move(agent_write),
                      inherited_sigpipe_ignored, debug_mode);
    return LocalAgentResult(LocalAgentResult::kInForked, exit_code);
  }

  // This is the zxdb frontend process. Close the handles the agent uses.
  client_read.reset();
  agent_write.reset();

  // The other end gets returned as the Bidi pipe.
  auto bidi_pipe = std::make_unique<debug::BufferedBidiPipe>(std::move(agent_read),
                                                             std::move(client_write));

  return LocalAgentResult(child_pid, std::move(bidi_pipe));
}

}  // namespace zxdb
