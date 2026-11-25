// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_ARCH_LINUX_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_ARCH_LINUX_H_

#include <signal.h>

#include "src/developer/debug/ipc/records.h"

// Older C libraries don't define this SIGTRAP code.
#ifndef TRAP_HWBKPT
#define TRAP_HWBKPT 4
#endif

namespace debug_agent {
namespace arch {

// Converts a siginfo_t.si_code value to an ExceptionType.
debug_ipc::ExceptionType DecodeExceptionType(int signal, int sig_code);

}  // namespace arch
}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_ARCH_LINUX_H_
