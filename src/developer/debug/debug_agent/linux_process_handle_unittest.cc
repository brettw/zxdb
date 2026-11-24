// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/linux_process_handle.h"

#include <lib/fit/defer.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "src/developer/debug/debug_agent/linux_binary_launcher.h"
#include "src/developer/debug/debug_agent/linux_task.h"
#include "src/developer/debug/shared/message_loop_linux.h"

namespace debug_agent {

namespace {

class LinuxProcessHandleTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(loop_.Init(nullptr)); }
  void TearDown() override { loop_.Cleanup(); }

 private:
  debug::MessageLoopLinux loop_;
};

}  // namespace

TEST_F(LinuxProcessHandleTest, GetFilterNames) {
  // The launcher stops the process after exec, so the command line and executable are final.
  LinuxBinaryLauncher launcher;
  ASSERT_TRUE(launcher.Setup({LINUX_LIFECYCLE_INFERIOR, "output"}).ok());
  std::unique_ptr<ProcessHandle> process = launcher.GetProcess();

  std::vector<std::string> names = process->GetFilterNames();

  // The names are the command name, the executable path, and their base names. The launcher
  // passes the path as argv[0].
  const std::string exe = std::filesystem::canonical(LINUX_LIFECYCLE_INFERIOR).string();
  std::set<std::string> expected = {LINUX_LIFECYCLE_INFERIOR, exe, "linux_lifecycle_inferior"};
  EXPECT_EQ(std::set<std::string>(names.begin(), names.end()), expected);

  // Each name occurs once.
  EXPECT_EQ(names.size(), expected.size());
}

TEST_F(LinuxProcessHandleTest, EmptyArgv0) {
  // The child stops after exec with an empty argv[0].
  pid_t pid = fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
    char empty[] = "";
    char mode[] = "output";
    char* argv[] = {empty, mode, nullptr};
    execv(LINUX_LIFECYCLE_INFERIOR, argv);
    _exit(127);
  }
  auto cleanup = fit::defer([pid] {
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
  });
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFSTOPPED(status));

  LinuxProcessHandle process(fxl::MakeRefCounted<LinuxTask>(pid));

  // The name comes from the executable. The second argument isn't a name.
  const std::string exe = std::filesystem::canonical(LINUX_LIFECYCLE_INFERIOR).string();
  EXPECT_EQ(process.GetName(), exe);
  std::vector<std::string> names = process.GetFilterNames();
  EXPECT_EQ(std::set<std::string>(names.begin(), names.end()),
            (std::set<std::string>{exe, "linux_lifecycle_inferior"}));
}

}  // namespace debug_agent
