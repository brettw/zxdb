// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/shared/message_loop_linux.h"

#include <lib/syslog/cpp/macros.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/wait.h>

#include "src/lib/files/eintr_wrapper.h"

namespace debug {

struct MessageLoopLinux::SignalWatchInfo {
  pid_t pid = -1;
  SignalWatcher watcher;
};

MessageLoopLinux::MessageLoopLinux() = default;

MessageLoopLinux::~MessageLoopLinux() = default;

MessageLoopLinux* MessageLoopLinux::Current() {
  return reinterpret_cast<MessageLoopLinux*>(MessageLoop::Current());
}

bool MessageLoopLinux::Init(std::string* error_message) {
  if (!MessageLoopPoll::Init(error_message))
    return false;

  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  if (sigprocmask(SIG_BLOCK, &mask, &original_mask_) == -1) {
    if (error_message)
      *error_message = "Could not block SIGCHLD";
    MessageLoopPoll::Cleanup();
    return false;
  }
  mask_saved_ = true;
  signal_fd_.reset(signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
  if (!signal_fd_.is_valid()) {
    if (error_message)
      *error_message = "Could not create signalfd";
    Cleanup();
    return false;
  }
  signal_fd_watch_ = WatchFD(WatchMode::kRead, signal_fd_.get(),
      [this](int, bool readable, bool, bool) {
        if (!readable)
          return;
        signalfd_siginfo info;
        while (HANDLE_EINTR(read(signal_fd_.get(), &info, sizeof(info))) == sizeof(info)) {}
        DrainChildSignals();
      });
  return true;
}

void MessageLoopLinux::Cleanup() {
  // Force unregister our signal watch before cleaning up current MessageLoop.
  signal_fd_watch_.StopWatching();
  signal_fd_.reset();
  if (mask_saved_) {
    sigprocmask(SIG_SETMASK, &original_mask_, nullptr);
    mask_saved_ = false;
  }

  MessageLoopPoll::Cleanup();
  child_scan_pending_ = false;
}

MessageLoop::WatchHandle MessageLoopLinux::WatchChildSignals(pid_t pid, SignalWatcher watcher) {
  // The dispatch code for watch callbacks requires this be called on the same thread as the message
  // loop is.
  FX_DCHECK(Current() == static_cast<MessageLoop*>(this));

  auto info = std::make_shared<SignalWatchInfo>();
  info->pid = pid;
  info->watcher = std::move(watcher);

  // The reason this function must be called on the message loop thread is that otherwise adding a
  // new watch would require synchronously breaking out of the existing poll() call to add the new
  // handle and then resuming it.
  int watch_id = GetNextWatchId();
  signal_watches_[watch_id] = std::move(info);

  // A stop may already be pending when its owner registers after a clone event.
  // Batch registrations share one scan instead of each scanning every watch.
  if (!child_scan_pending_) {
    child_scan_pending_ = true;
    PostTask(FROM_HERE, [this] {
      child_scan_pending_ = false;
      DrainChildSignals();
    });
  }
  return WatchHandle(this, watch_id);
}

pid_t MessageLoopLinux::WaitForChildEvent(pid_t pid, int* status) {
  return HANDLE_EINTR(waitpid(pid, status, __WALL | WUNTRACED | WNOHANG));
}

void MessageLoopLinux::DrainChildSignals() {
  // SIGCHLD coalesces. Probe every owned PID, without reaping unrelated children.
  // Callbacks can remove themselves or register new children, so retain the callback
  // and restart the scan after each delivered status.
  bool delivered;
  do {
    delivered = false;
    for (const auto& [id, info] : signal_watches_) {
      int status = 0;
      pid_t pid = WaitForChildEvent(info->pid, &status);
      if (pid > 0) {
        auto retained = info;
        retained->watcher(pid, status);
        delivered = true;
        break;
      }
    }
  } while (delivered);
}

void MessageLoopLinux::StopWatching(int id) {
  // The dispatch code for watch callbacks requires this be called on the
  // same thread as the message loop is.
  FX_DCHECK(MessageLoopLinux::Current() == this);

  {
    std::lock_guard<std::mutex> guard(mutex_);

    // Check for signal watches.
    auto found = signal_watches_.find(id);
    if (found != signal_watches_.end()) {
      signal_watches_.erase(found);
      return;
    }
  }

  // All other watches are removed by the base class.
  MessageLoopPoll::StopWatching(id);
}

}  // namespace debug
