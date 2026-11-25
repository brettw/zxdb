// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// These tests check filter requests when the agent doesn't watch a root job. Fuchsia agents always
// watch the root job. Agents on other platforms never do, and the test harness doesn't either.

#include <algorithm>
#include <vector>

#include <gtest/gtest.h>

#include "src/developer/debug/debug_agent/debug_agent.h"
#include "src/developer/debug/debug_agent/mock_debug_agent_harness.h"
#include "src/developer/debug/debug_agent/mock_system_interface.h"
#include "src/developer/debug/ipc/records.h"
#include "src/developer/debug/shared/test_with_loop.h"

namespace debug_agent {

namespace {

class DebugAgentFilterTests : public debug::TestWithLoop {};

debug_ipc::Filter MakeFilter(uint32_t id, debug_ipc::Filter::Type type, std::string pattern) {
  return debug_ipc::Filter{
      .type = type,
      .pattern = std::move(pattern),
      .id = debug_ipc::Filter::Identifier(id, debug_ipc::Filter::Originator::kUnknown)};
}

// Returns the sorted koids of the matches for the filter with the given ID.
std::vector<zx_koid_t> GetMatchedKoids(const debug_ipc::UpdateFilterReply& reply,
                                       const debug_ipc::Filter::Identifier& id) {
  std::vector<zx_koid_t> koids;
  for (const auto& filter_match : reply.matched_processes_for_filter) {
    if (filter_match.id == id) {
      for (const auto& task : filter_match.matches)
        koids.push_back(task.koid);
    }
  }
  std::ranges::sort(koids);
  return koids;
}

}  // namespace

TEST_F(DebugAgentFilterTests, ProcessNameFiltersMatchExistingProcesses) {
  MockDebugAgentHarness harness;

  debug_ipc::UpdateFilterRequest request;
  request.filters.push_back(MakeFilter(1, debug_ipc::Filter::Type::kProcessNameSubstr, "job1"));
  request.filters.push_back(MakeFilter(2, debug_ipc::Filter::Type::kProcessName, "job1-p2"));
  request.filters.push_back(MakeFilter(3, debug_ipc::Filter::Type::kProcessName, "missing"));

  debug_ipc::UpdateFilterReply reply;
  harness.debug_agent()->OnUpdateFilter(request, &reply);

  // The process koids come from MockSystemInterface.
  EXPECT_EQ(GetMatchedKoids(reply, request.filters[0].id),
            (std::vector<zx_koid_t>{9, 11, 14, 19, 21}));
  EXPECT_EQ(GetMatchedKoids(reply, request.filters[1].id), std::vector<zx_koid_t>{11});
  EXPECT_TRUE(GetMatchedKoids(reply, request.filters[2].id).empty());

  // Filters that match nothing are still installed for future processes.
  EXPECT_EQ(harness.debug_agent()->GetIpcFilters().size(), 3u);
}

TEST_F(DebugAgentFilterTests, ExcludedProcessesDontMatch) {
  // The process koids come from MockSystemInterface. Exclude job1-p1 and job11-p1.
  DebugAgent agent(MockSystemInterface::CreateWithData(),
                   DebugAgentOptions{.filter_excluded_koids = {9, 14}});

  debug_ipc::UpdateFilterRequest request;
  request.filters.push_back(MakeFilter(1, debug_ipc::Filter::Type::kProcessNameSubstr, "job1"));
  debug_ipc::UpdateFilterReply reply;
  agent.OnUpdateFilter(request, &reply);

  EXPECT_EQ(GetMatchedKoids(reply, request.filters[0].id), (std::vector<zx_koid_t>{11, 19, 21}));
}

TEST_F(DebugAgentFilterTests, EmptyUpdateRemovesFilters) {
  MockDebugAgentHarness harness;

  debug_ipc::UpdateFilterRequest request;
  request.filters.push_back(MakeFilter(1, debug_ipc::Filter::Type::kProcessNameSubstr, "p1"));
  debug_ipc::UpdateFilterReply reply;
  harness.debug_agent()->OnUpdateFilter(request, &reply);
  ASSERT_EQ(harness.debug_agent()->GetIpcFilters().size(), 1u);

  debug_ipc::UpdateFilterReply empty_reply;
  harness.debug_agent()->OnUpdateFilter(debug_ipc::UpdateFilterRequest{}, &empty_reply);
  EXPECT_TRUE(empty_reply.matched_processes_for_filter.empty());
  EXPECT_TRUE(harness.debug_agent()->GetIpcFilters().empty());
}

TEST_F(DebugAgentFilterTests, RecursiveFilterMatchesExistingProcesses) {
  MockDebugAgentHarness harness;

  debug_ipc::UpdateFilterRequest request;
  auto& filter = request.filters.emplace_back(
      MakeFilter(1, debug_ipc::Filter::Type::kComponentUrl,
                 "fuchsia-pkg://devhost/root_package#meta/root_component.cm"));
  filter.config.recursive = true;

  debug_ipc::UpdateFilterReply reply;
  harness.debug_agent()->OnUpdateFilter(request, &reply);

  // The agent creates a moniker prefix filter. That filter matches the process in the realm of the
  // component.
  const auto& created = harness.stream_backend()->filters();
  ASSERT_EQ(created.size(), 1u);
  EXPECT_EQ(GetMatchedKoids(reply, created[0].filter.id), std::vector<zx_koid_t>{33});
}

TEST_F(DebugAgentFilterTests, AttachToJob) {
  MockDebugAgentHarness harness;

  constexpr zx_koid_t kJobKoid = 8;
  debug_ipc::AttachRequest request;
  request.koid = kJobKoid;
  request.config.target = debug_ipc::TaskType::kJob;
  request.config.priority = debug_ipc::AttachConfig::Priority::kWeak;

  debug_ipc::AttachReply reply;
  harness.debug_agent()->OnAttach(request, &reply);

  EXPECT_TRUE(reply.status.ok()) << reply.status.message();
  EXPECT_TRUE(harness.debug_agent()->GetDebuggedJob(kJobKoid));
}

}  // namespace debug_agent
