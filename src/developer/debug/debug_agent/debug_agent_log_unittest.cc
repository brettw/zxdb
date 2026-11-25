// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include <gtest/gtest.h>

#include "src/developer/debug/debug_agent/mock_debug_agent_harness.h"
#include "src/developer/debug/shared/logging/logging.h"
#include "src/developer/debug/shared/test_with_loop.h"

namespace debug_agent {

namespace {

class DebugAgentLogTest : public debug::TestWithLoop {};

class NullLogBackend : public debug::LogBackend {
 public:
  void WriteLog(debug::LogSeverity severity, const debug::FileLineFunction& location,
                std::string log) override {}
};

}  // namespace

// The handshake makes an agent the log backend. The agent must unset it when it's destroyed, or
// later logs use freed memory.
TEST_F(DebugAgentLogTest, DestroyedAgentUnsetsItself) {
  {
    MockDebugAgentHarness harness;
    ASSERT_EQ(harness.debug_agent(), debug::LogBackend::Get());
  }
  EXPECT_EQ(nullptr, debug::LogBackend::Get());
}

// A later backend stays set when an earlier agent is destroyed.
TEST_F(DebugAgentLogTest, DestroyedAgentKeepsOtherBackend) {
  NullLogBackend other;
  {
    MockDebugAgentHarness harness;
    debug::LogBackend::Set(&other, false);
  }
  EXPECT_EQ(&other, debug::LogBackend::Get());
  debug::LogBackend::Unset();
}

}  // namespace debug_agent
