// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_LINUX_UTILS_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_LINUX_UTILS_H_

#include <elf.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <zircon/syscalls/debug.h>
#include <zircon/types.h>

#include <filesystem>
#include <map>
#include <optional>
#include <vector>

#include <fbl/unique_fd.h>

#include "src/developer/debug/debug_agent/thread_handle.h"
#include "src/developer/debug/shared/address_range.h"
#include "src/developer/debug/shared/result.h"

namespace debug_agent {
namespace linux {

// Wrapper around pidfd_open.
fbl::unique_fd OpenPidfd(int pid);

std::optional<user_regs_struct> ReadGeneralRegisters(int pid);
zx_status_t WriteGeneralRegisters(int pid, const user_regs_struct& regs);

// Reads the x86-64 debug registers DR0 to DR3, DR6, and DR7 of a stopped, traced task.
std::optional<zx_thread_state_debug_regs_t> ReadDebugRegisters(int pid);

// Checks the slots that DR7 enables. Each one must use a supported access type and length, and its
// address must be aligned to the length. The error names the first invalid slot.
debug::Status ValidateDebugRegisters(const zx_thread_state_debug_regs_t& regs);

// Returns the type of a hardware trap (TRAP_HWBKPT) from the debug registers after the trap. DR6
// gives the slot that triggered, and its DR7 access type tells a watchpoint from an execution
// breakpoint.
debug_ipc::ExceptionType DecodeHardwareTrap(const zx_thread_state_debug_regs_t& regs);

// Changes the debug registers of a stopped, traced task from |current| to |updated|. The kernel
// checks an address against the DR7 settings of its slot, so the function writes the registers in
// an order that the kernel accepts. On failure, it restores |current| and returns the error.
debug::Status WriteDebugRegisters(int pid, const zx_thread_state_debug_regs_t& current,
                                  const zx_thread_state_debug_regs_t& updated);

// Represents one line in the Linux /proc/.../maps file.
struct MapEntry {
  debug::AddressRange range;

  bool read = false;
  bool write = false;
  bool execute = false;
  bool shared = false;

  uint64_t offset = 0;
  std::string device;
  uint64_t inode = 0;
  std::string path;
};

// Parse/read the given Linux maps file.
std::vector<MapEntry> ParseMaps(const std::string& contents);
std::vector<MapEntry> GetMaps(int pid);
debug::Result<std::vector<MapEntry>, debug::Status> ParseMapsChecked(const std::string& contents);
debug::Result<std::vector<MapEntry>, debug::Status> GetMapsChecked(int pid);

// Entries from /proc/.../stat we care about. This is not all of them and more can be added here if
// needed.
struct ProcStat {
  int pid = 0;
  std::string name;
  char state = 0;
};

// Parse/read the given Linux status file (this is "status" with key/value pairs and not the "stat"
// file that's just numbers). Returns an empty may on failure (the process doesn't exist or
// something).
std::map<std::string, std::string> ParseStatus(const std::string& contents);
std::map<std::string, std::string> GetStatus(int pid);

// Parses or reads the Yama ptrace scope from /proc/sys/kernel/yama/ptrace_scope. Returns nullopt
// when the value isn't a number or Yama isn't enabled.
std::optional<int> ParsePtraceScope(const std::string& contents);
std::optional<int> GetPtraceScope();

// Returns an explanation of how the given Yama ptrace scope can prevent an attach, with a way to
// remove the restriction. The scope is a possible cause of a failed attach, not a certain one.
// Returns an empty string for scopes that don't restrict an attach.
std::string GetPtraceScopeNote(int ptrace_scope);

// Returns the executable file name as defined in /proc/<pid>/exe. Empty on failure.
std::string GetExe(int pid);

// Returns the command line for the given process. Will be empty on failure. Empty arguments are
// omitted.
std::vector<std::string> GetCommandLine(int pid);

// Returns the first command-line argument (argv[0]) of the process. It's empty when the process
// set an empty argv[0] and on failure.
std::string GetArgv0(int pid);

// Returns the load address of the ld.so for the given process ID. Returns 0 on failure.
uint64_t GetLdSoLoadAddress(int pid);

// Returns the load address and path for the ld.so associated with the given process ID. Returns
// nullopt on failure.
struct LdSoInfo {
  uint64_t load_address = 0;
  std::string path;
};
std::optional<LdSoInfo> GetLdSoInfo(int pid);

// Returns the ld.so debug address in memory for the given process. Returns 0 if not found.
uint64_t GetLdSoDebugAddress(int pid);

// Returns the address at which to set a breakpoint to observe load events for the given process.
// Returns 0 if not found.
uint64_t GetLdSoBreakpointAddress(int pid);

// Reads the contents of /proc/<pid>/auxv and returns the key/value store. The keys are AT_*
// constants from <linux/auxvec.h>. Returns the empty map on failure.
std::map<uint64_t, uint64_t> ReadAuxv(int pid);
// Parses Linux ELF64 entries. Requires native AT_PHENT, rejects truncated input,
// and stops at AT_NULL.
std::map<uint64_t, uint64_t> ParseAuxv(std::string_view contents);

// Decodes the value of a /proc/status "State" line. Returns nullopt if parsing failed.
std::optional<ThreadHandle::State> ThreadStateFromLinuxState(const std::string& linux_state);

// Returns a vector of all numeric subdirectories in the given path. The path is specifyable to
// support enumerating the "task" subdirectory of a process as well as the toplevel processes.
std::vector<int> GetDirectoryPids(const std::filesystem::path& path = "/proc");

// Returns the string for the given signal name. If include_number is set, this also includes the
// numeric signal number.
std::string SignalToString(int signal, bool include_number);

// Given a local path to the dynamic loader on disk, returns the address (relative to the beginning
// of the module in memory) of the debug address or load breakpoint address.
//
// The Fuchsia loader does not expose these symbols which is why they're in a Linux-specific file.
uint64_t GetLdSoDebugRelativeAddress(const std::string& ld_so_path);
uint64_t GetLdSoBreakpointRelativeAddress(const std::string& ld_so_path);

}  // namespace linux
}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_LINUX_UTILS_H_
