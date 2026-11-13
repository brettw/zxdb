// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/shared/message_loop_linux.h"

#include <unistd.h>
#include <sys/wait.h>

#include <gtest/gtest.h>

#include <map>
#include <vector>

namespace debug {

// Tests that we can get SIGCHLD signals.
TEST(MessageLoopLinux, SigChild) {
  MessageLoopLinux loop;
  std::string error_message;
  ASSERT_TRUE(loop.Init(&error_message)) << error_message;

  // Scope everything to before MessageLoop::Cleanup().
  {
    // For and then exit from the child, this will just send the SIGCHLD to the test.
    int child_pid = fork();
    if (child_pid == 0) {
      exit(1);
    }

    std::optional<int> result_status;
    MessageLoop::WatchHandle watch_handle = loop.WatchChildSignals(
        child_pid, [child_pid, &result_status, &loop](pid_t pid, int status) {
          EXPECT_EQ(child_pid, pid);
          result_status = status;
          loop.QuitNow();
        });

    loop.Run();

    // Callback should have run.
    ASSERT_TRUE(result_status);

    // The message should have been that the process exited.
    EXPECT_TRUE(WIFEXITED(*result_status));
  }

  loop.Cleanup();
}

}  // namespace debug

namespace debug {
TEST(MessageLoopLinux, PreservesSignalMask) {
  sigset_t before, after;
  ASSERT_EQ(0, sigprocmask(SIG_SETMASK, nullptr, &before));
  MessageLoopLinux loop;
  ASSERT_EQ(0, sigprocmask(SIG_SETMASK, nullptr, &after));
  EXPECT_EQ(sigismember(&before, SIGCHLD), sigismember(&after, SIGCHLD));
  for (int iteration = 0; iteration < 2; ++iteration) {
    std::string error;
    ASSERT_TRUE(loop.Init(&error));
    loop.Cleanup();
    ASSERT_EQ(0, sigprocmask(SIG_SETMASK, nullptr, &after));
    for (int signal = 1; signal < NSIG; ++signal)
      EXPECT_EQ(sigismember(&before, signal), sigismember(&after, signal));
  }
}

TEST(MessageLoopLinux, CoalescedAndUnownedChildren) {
  MessageLoopLinux loop;
  std::string error;
  ASSERT_TRUE(loop.Init(&error));
  int gate[2];
  ASSERT_EQ(0, pipe(gate));
  pid_t children[3];
  for (auto& child : children) {
    child = fork();
    ASSERT_GE(child, 0);
    if (!child) {
      close(gate[1]);
      char byte;
      read(gate[0], &byte, 1);
      _exit(7);
    }
  }
  close(gate[0]);
  int callbacks = 0;
  {
    MessageLoop::WatchHandle first, second;
    auto on_exit = [&](int status) {
      EXPECT_TRUE(WIFEXITED(status));
      EXPECT_EQ(7, WEXITSTATUS(status));
      if (++callbacks == 2)
        loop.QuitNow();
    };
    first = loop.WatchChildSignals(children[1], [&](pid_t pid, int status) {
      EXPECT_EQ(children[1], pid);
      on_exit(status);
      first.StopWatching();
    });
    second = loop.WatchChildSignals(children[2], [&](pid_t pid, int status) {
      EXPECT_EQ(children[2], pid);
      on_exit(status);
      second.StopWatching();
    });
    // Exhaust registration probes while the children are still blocked.
    loop.PostTask(FROM_HERE, [&] { loop.QuitNow(); });
    loop.Run();
    close(gate[1]);
    // All exits must be waitable before signalfd dispatch; SIGCHLD coalesces.
    for (pid_t child : children) {
      siginfo_t info{};
      ASSERT_EQ(0, waitid(P_PID, child, &info, WEXITED | WNOWAIT));
    }
    loop.PostTimer(FROM_HERE, 1000, [&] { loop.QuitNow(); });
    loop.Run();
    EXPECT_EQ(2, callbacks);
  }
  int status;
  EXPECT_EQ(children[0], waitpid(children[0], &status, WNOHANG));
  for (int i = 1; i < 3; ++i)
    waitpid(children[i], &status, WNOHANG);
  loop.Cleanup();
}

TEST(MessageLoopLinux, ConsumedStatusDoesNotFabricateExit) {
  MessageLoopLinux loop;
  std::string error;
  ASSERT_TRUE(loop.Init(&error));
  pid_t child = fork();
  ASSERT_GE(child, 0);
  if (!child) {
    raise(SIGSTOP);
    _exit(0);
  }
  int status;
  ASSERT_EQ(child, waitpid(child, &status, WUNTRACED));
  int callbacks = 0;
  {
    auto watch = loop.WatchChildSignals(child, [&](pid_t, int) { ++callbacks; });
    loop.PostTask(FROM_HERE, [&] { loop.QuitNow(); });
    loop.Run();
    EXPECT_EQ(0, callbacks);
  }
  kill(child, SIGKILL);
  waitpid(child, &status, 0);
  loop.Cleanup();
}
}  // namespace debug

namespace debug {
namespace {
class CountingChildLoop : public MessageLoopLinux {
 public:
  size_t waits = 0;
  std::map<pid_t, int> ready;

 protected:
  pid_t WaitForChildEvent(pid_t pid, int* status) override {
    ++waits;
    auto found = ready.find(pid);
    if (found == ready.end())
      return 0;
    *status = found->second;
    ready.erase(found);
    return pid;
  }
};
}  // namespace

TEST(MessageLoopLinux, RegistrationScansAreLinear) {
  CountingChildLoop loop;
  std::string error;
  ASSERT_TRUE(loop.Init(&error));
  {
    // Removing the watch that first queued the scan must not cancel the batch.
    {
      auto removed = loop.WatchChildSignals(2000, [](pid_t, int) { ADD_FAILURE(); });
    }
    std::vector<MessageLoop::WatchHandle> watches;
    for (int pid = 1; pid <= 1000; ++pid)
      watches.push_back(loop.WatchChildSignals(pid, [](pid_t, int) { ADD_FAILURE(); }));
    loop.RunUntilNoTasks();
    EXPECT_EQ(1000u, loop.waits);

    // A new batch must schedule a scan even after the previous scan completes.
    loop.waits = 0;
    watches.push_back(loop.WatchChildSignals(1001, [](pid_t, int) { ADD_FAILURE(); }));
    loop.RunUntilNoTasks();
    EXPECT_EQ(1001u, loop.waits);
  }
  loop.Cleanup();
}

TEST(MessageLoopLinux, CancelledRegistrationScanDoesNotSurviveCleanup) {
  CountingChildLoop loop;
  std::string error;
  ASSERT_TRUE(loop.Init(&error));
  {
    auto watch = loop.WatchChildSignals(1, [](pid_t, int) { ADD_FAILURE(); });
  }
  // Cleanup cancels the queued scan. Reinitialization must allow a new one.
  loop.Cleanup();
  ASSERT_TRUE(loop.Init(&error));
  {
    auto watch = loop.WatchChildSignals(2, [](pid_t, int) { ADD_FAILURE(); });
    loop.RunUntilNoTasks();
    EXPECT_EQ(1u, loop.waits);
  }
  loop.Cleanup();
}

TEST(MessageLoopLinux, RegistrationDuringCallbackIsCollected) {
  CountingChildLoop loop;
  std::string error;
  ASSERT_TRUE(loop.Init(&error));
  loop.ready = {{1, 7 << 8}, {2, 9 << 8}};
  int callbacks = 0;
  {
    MessageLoop::WatchHandle first, second;
    first = loop.WatchChildSignals(1, [&](pid_t, int status) {
      EXPECT_EQ(7, WEXITSTATUS(status));
      ++callbacks;
      first.StopWatching();
      second = loop.WatchChildSignals(2, [&](pid_t, int child_status) {
        EXPECT_EQ(9, WEXITSTATUS(child_status));
        ++callbacks;
        second.StopWatching();
      });
    });
    loop.RunUntilNoTasks();
    EXPECT_EQ(2, callbacks);
    EXPECT_EQ(2u, loop.waits);
    EXPECT_TRUE(loop.ready.empty());
  }
  loop.Cleanup();
}
}  // namespace debug
