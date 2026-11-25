// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// clang-format off
// Included early because of conflicts.
#include "src/lib/elflib/elflib.h"
// clang-format on

#include "src/developer/debug/debug_agent/linux_utils.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <unistd.h>
#include <zircon/errors.h>

#include <charconv>
#include <cstddef>
#include <cstring>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

#include <fbl/unique_fd.h>
#include <linux/auxvec.h>
#include <linux/limits.h>

#include "src/developer/debug/debug_agent/elf_utils.h"
#include "src/developer/debug/debug_agent/process_handle.h"
#include "src/developer/debug/shared/logging/logging.h"
#include "src/lib/files/eintr_wrapper.h"
#include "src/lib/files/file.h"
#include "src/lib/fxl/strings/split_string.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace debug_agent {
namespace linux {

namespace {

// Syscall ID of the pidfd_open() function. From "man pidfd_open".
constexpr long kPidFDOpenSyscallID = 434;

// Reads "/proc/<pid>/<name>" and returns it as a single string. Returns the empty string on error.
std::string ReadProcFile(int pid, const char* name) {
  std::string file_name = fxl::StringPrintf("/proc/%d/%s", pid, name);

  std::string result;
  bool success = files::ReadFileToString(file_name, &result);
  if (!success && errno != ENOENT && errno != ESRCH) {
    DEBUG_LOG(Process) << "Cannot read " << file_name << ": " << strerror(errno);
  }
  return result;
}

// Returns if this path is composed only of numeric digits.
bool IsNumericPath(const std::filesystem::path& path) {
  for (char c : path.string()) {
    if (!isdigit(c))
      return false;
  }
  return true;
}

// Converts a /proc/* path to a PID if it references one. Otherwise returns nullopt.
std::optional<int> PathToPid(const std::filesystem::path& path) {
  auto last_part = path.filename();
  if (!IsNumericPath(last_part))
    return std::nullopt;

  return atoi(last_part.c_str());
}

}  // namespace

fbl::unique_fd OpenPidfd(int pid) { return fbl::unique_fd(syscall(kPidFDOpenSyscallID, pid, 0)); }

std::optional<user_regs_struct> ReadGeneralRegisters(int pid) {
  user_regs_struct regs{};

  iovec vec;
  vec.iov_base = &regs;
  vec.iov_len = sizeof(regs);
  if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &vec) == -1 || vec.iov_len != sizeof(regs)) {
    return std::nullopt;
  }
  return regs;
}

zx_status_t WriteGeneralRegisters(int pid, const user_regs_struct& regs) {
  iovec vec;
  vec.iov_base = const_cast<void*>(static_cast<const void*>(&regs));
  vec.iov_len = sizeof(regs);
  if (ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &vec) == -1)
    return ZX_ERR_INTERNAL;
  return ZX_OK;
}

namespace {

constexpr int kDebugStatusIndex = 6;
constexpr int kDebugControlIndex = 7;

// Returns the offset of the debug register with the given index in the ptrace user area.
uintptr_t DebugRegisterOffset(int index) {
  return offsetof(struct user, u_debugreg) + index * sizeof(unsigned long);
}

// Returns the length in bytes for a DR7 length code.
int DebugLengthFromCode(uint64_t code) {
  switch (code) {
    case 0:
      return 1;
    case 1:
      return 2;
    case 2:
      return 8;
    default:
      return 4;
  }
}

// Writes one debug register. The error explains what the kernel didn't accept.
debug::Status WriteDebugRegister(int pid, int index, uint64_t value) {
  if (ptrace(PTRACE_POKEUSER, pid, DebugRegisterOffset(index), value) != -1)
    return debug::Status();

  int error = errno;
  if (index == kDebugControlIndex && error == ENOSPC) {
    return debug::ErrnoStatus(
        error, fxl::StringPrintf("No hardware debug slot is free for DR7 = 0x%" PRIx64
                                 ". Another tool, such as perf, can use the slots.",
                                 value));
  }
  if (index == kDebugControlIndex) {
    return debug::ErrnoStatus(
        error, fxl::StringPrintf("The kernel didn't accept DR7 = 0x%" PRIx64 ": %s.", value,
                                 strerror(error)));
  }
  if (index < 4 && error == EINVAL) {
    return debug::ErrnoStatus(
        error, fxl::StringPrintf("DR%d = 0x%" PRIx64 " isn't a user-space address.", index, value));
  }
  return debug::ErrnoStatus(error,
                            fxl::StringPrintf("Can't write DR%d: %s.", index, strerror(error)));
}

// Writes the registers that differ between |from| and |to|. DR7 is cleared before the addresses
// change and set last, so the kernel doesn't check a new address against an old slot setting.
debug::Status ApplyDebugRegisters(int pid, const zx_thread_state_debug_regs_t& from,
                                  const zx_thread_state_debug_regs_t& to) {
  bool addresses_change = false;
  for (int i = 0; i < 4; i++)
    addresses_change |= from.dr[i] != to.dr[i];

  uint64_t dr7 = from.dr7;
  if (dr7 != 0 && (addresses_change || to.dr7 != dr7)) {
    if (debug::Status status = WriteDebugRegister(pid, kDebugControlIndex, 0); status.has_error())
      return status;
    dr7 = 0;
  }
  for (int i = 0; i < 4; i++) {
    if (from.dr[i] != to.dr[i]) {
      if (debug::Status status = WriteDebugRegister(pid, i, to.dr[i]); status.has_error())
        return status;
    }
  }
  if (from.dr6 != to.dr6) {
    if (debug::Status status = WriteDebugRegister(pid, kDebugStatusIndex, to.dr6);
        status.has_error())
      return status;
  }
  if (to.dr7 != dr7)
    return WriteDebugRegister(pid, kDebugControlIndex, to.dr7);
  return debug::Status();
}

}  // namespace

std::optional<zx_thread_state_debug_regs_t> ReadDebugRegisters(int pid) {
  zx_thread_state_debug_regs_t regs{};
  const std::pair<int, uint64_t*> fields[] = {{0, &regs.dr[0]},
                                              {1, &regs.dr[1]},
                                              {2, &regs.dr[2]},
                                              {3, &regs.dr[3]},
                                              {kDebugStatusIndex, &regs.dr6},
                                              {kDebugControlIndex, &regs.dr7}};
  for (auto [index, field] : fields) {
    // PTRACE_PEEKUSER returns the value, so only errno shows a failure.
    errno = 0;
    long value = ptrace(PTRACE_PEEKUSER, pid, DebugRegisterOffset(index), nullptr);
    if (errno != 0)
      return std::nullopt;
    *field = static_cast<uint64_t>(value);
  }
  return regs;
}

debug::Status ValidateDebugRegisters(const zx_thread_state_debug_regs_t& regs) {
  for (int slot = 0; slot < 4; slot++) {
    bool enabled = (regs.dr7 >> (slot * 2)) & 0b11;
    if (!enabled)
      continue;

    uint64_t access = (regs.dr7 >> (16 + slot * 4)) & 0b11;
    int length = DebugLengthFromCode((regs.dr7 >> (18 + slot * 4)) & 0b11);
    if (access == 0b10) {
      return debug::Status(
          fxl::StringPrintf("DR7 enables slot %d for I/O access, which isn't supported.", slot));
    }
    if (access == 0b00 && length != 1) {
      return debug::Status(fxl::StringPrintf(
          "DR7 enables slot %d for execution with a length of %d bytes. An execution slot needs a "
          "length of 1 byte.",
          slot, length));
    }
    if (regs.dr[slot] % length != 0) {
      return debug::Status(fxl::StringPrintf(
          "DR%d = 0x%" PRIx64 " isn't aligned to the %d-byte length that DR7 sets for slot %d.",
          slot, regs.dr[slot], length, slot));
    }
  }
  return debug::Status();
}

debug_ipc::ExceptionType DecodeHardwareTrap(const zx_thread_state_debug_regs_t& regs) {
  for (int slot = 0; slot < 4; slot++) {
    if ((regs.dr6 >> slot) & 1) {
      uint64_t access = (regs.dr7 >> (16 + slot * 4)) & 0b11;
      return access == 0 ? debug_ipc::ExceptionType::kHardwareBreakpoint
                         : debug_ipc::ExceptionType::kWatchpoint;
    }
  }
  // The kernel reports TRAP_HWBKPT only for a slot hit, so this is unexpected.
  return debug_ipc::ExceptionType::kHardwareBreakpoint;
}

debug::Status WriteDebugRegisters(int pid, const zx_thread_state_debug_regs_t& current,
                                  const zx_thread_state_debug_regs_t& updated) {
  if (debug::Status status = ValidateDebugRegisters(updated); status.has_error())
    return status;

  debug::Status status = ApplyDebugRegisters(pid, current, updated);
  if (status.ok())
    return status;

  // A failed write can leave some registers changed, so restore the previous values.
  if (std::optional<zx_thread_state_debug_regs_t> partial = ReadDebugRegisters(pid)) {
    if (ApplyDebugRegisters(pid, *partial, current).has_error())
      LOGS(Warn) << "Could not restore the debug registers of task " << pid << ".";
  }
  return status;
}

std::optional<user_fpregs_struct> ReadFloatingPointRegisters(int pid) {
  user_fpregs_struct regs{};
  iovec vec{.iov_base = &regs, .iov_len = sizeof(regs)};
  if (ptrace(PTRACE_GETREGSET, pid, NT_PRFPREG, &vec) == -1 || vec.iov_len != sizeof(regs))
    return std::nullopt;
  return regs;
}

debug::Status WriteFloatingPointRegisters(int pid, const user_fpregs_struct& regs) {
  iovec vec{.iov_base = const_cast<user_fpregs_struct*>(&regs), .iov_len = sizeof(regs)};
  if (ptrace(PTRACE_SETREGSET, pid, NT_PRFPREG, &vec) == -1) {
    return debug::ErrnoStatus(
        errno, fxl::StringPrintf("Can't write the floating-point registers of thread %d: %s.", pid,
                                 strerror(errno)));
  }
  return debug::Status();
}

debug::Result<std::vector<MapEntry>, debug::Status> ParseMapsChecked(const std::string& contents) {
  std::vector<MapEntry> result;

  std::istringstream lines(contents);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream in(line);
    MapEntry entry;

    // <address>-<address>
    uint64_t begin = 0, end = 0;
    char hyphen = 0;
    in >> std::hex >> begin >> hyphen >> std::hex >> end;
    if (!in.good() || hyphen != '-' || end < begin)
      return debug::Status("Malformed procfs mapping.");
    entry.range = debug::AddressRange(begin, end);

    // Flags.
    char r_bit, w_bit, x_bit, s_bit;
    in >> r_bit >> w_bit >> x_bit >> s_bit;
    if (!in.good() || (r_bit != 'r' && r_bit != '-') || (w_bit != 'w' && w_bit != '-') ||
        (x_bit != 'x' && x_bit != '-') || (s_bit != 's' && s_bit != 'p'))
      return debug::Status("Malformed procfs mapping.");
    entry.read = r_bit == 'r';
    entry.write = w_bit == 'w';
    entry.execute = x_bit == 'x';
    entry.shared = s_bit == 's';

    // Offset, device, inode.
    in >> std::hex >> entry.offset >> entry.device >> std::dec >> entry.inode;

    if (in.fail())
      return debug::Status("Malformed procfs mapping.");

    // Everything else to the end is the path (could be empty).
    while (in.good() && in.peek() == ' ')
      in.get();  // Skip whitespace before.
    std::getline(in, entry.path);

    result.push_back(std::move(entry));
  }
  return result;
}

std::vector<MapEntry> ParseMaps(const std::string& contents) {
  return ParseMapsChecked(contents).take_value_or_empty();
}

debug::Result<std::vector<MapEntry>, debug::Status> GetMapsChecked(int pid) {
  std::string contents;
  if (!files::ReadFileToString(fxl::StringPrintf("/proc/%d/maps", pid), &contents))
    return debug::ErrnoStatus(errno, "Cannot read process mappings.");
  return ParseMapsChecked(contents);
}

std::vector<MapEntry> GetMaps(int pid) {
  auto result = GetMapsChecked(pid);
  if (result.has_error()) {
    DEBUG_LOG(Process) << result.err().message();
  }
  return result.take_value_or_empty();
}

std::map<std::string, std::string> ParseStatus(const std::string& contents) {
  std::istringstream in(contents);

  std::map<std::string, std::string> result;
  do {
    std::string line;
    std::getline(in, line);

    // Split on the first colon.
    std::string key, value;
    for (size_t i = 0; i < line.size(); i++) {
      if (line[i] == ':') {
        key = line.substr(0, i);

        // Skip spaces.
        i++;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
          i++;

        // Everything after that is the value.
        value = line.substr(i);
        break;
      }
    }
    if (key.empty() && value.empty())
      break;

    result[key] = std::move(value);
  } while (in.good());

  return result;
}

std::map<std::string, std::string> GetStatus(int pid) {
  return ParseStatus(ReadProcFile(pid, "status"));
}

std::optional<int> ParsePtraceScope(const std::string& contents) {
  // The file contains one number and a newline.
  int value = 0;
  const char* begin = contents.data();
  const char* end = begin + contents.size();
  auto [ptr, error] = std::from_chars(begin, end, value);
  if (error != std::errc() || ptr == begin ||
      (ptr != end && std::string_view(ptr, end - ptr) != "\n"))
    return std::nullopt;
  return value;
}

std::optional<int> GetPtraceScope() {
  std::string contents;
  if (!files::ReadFileToString("/proc/sys/kernel/yama/ptrace_scope", &contents))
    return std::nullopt;
  return ParsePtraceScope(contents);
}

std::string GetPtraceScopeNote(int ptrace_scope) {
  constexpr char kOtherCauses[] =
      "This setting can be why the attach failed. Other causes are a target that belongs to "
      "another user or that isn't dumpable, and a change to the setting doesn't remove them.\n";
  constexpr char kSysctlFix[] =
      "To remove the Yama restriction, run:\n"
      "    sudo sysctl -w kernel.yama.ptrace_scope=0\n";
  constexpr char kMoreInfo[] =
      "For more information, see "
      "https://www.kernel.org/doc/Documentation/security/Yama.txt.";

  switch (ptrace_scope) {
    case 1:
      return std::string(
                 "The value of /proc/sys/kernel/yama/ptrace_scope is 1. With this value, a "
                 "process can attach only to its descendants, unless it has CAP_SYS_PTRACE or "
                 "the target allows it with prctl(PR_SET_PTRACER).\n") +
             kOtherCauses + kSysctlFix + kMoreInfo;
    case 2:
      return std::string(
                 "The value of /proc/sys/kernel/yama/ptrace_scope is 2. With this value, only a "
                 "process with CAP_SYS_PTRACE can attach.\n") +
             kOtherCauses +
             "To remove the Yama restriction, run the debugger with CAP_SYS_PTRACE, for "
             "example as root, or run:\n"
             "    sudo sysctl -w kernel.yama.ptrace_scope=0\n" +
             kMoreInfo;
    case 3:
      return std::string(
                 "The value of /proc/sys/kernel/yama/ptrace_scope is 3. With this value, no "
                 "process can attach. The value can't change until the system restarts.\n") +
             kMoreInfo;
    default:
      return std::string();
  }
}

std::string GetExe(int pid) {
  std::string link_path = fxl::StringPrintf("/proc/%d/exe", pid);

  char dest_path[PATH_MAX];  // Not null terminated!
  ssize_t result = readlink(link_path.c_str(), dest_path, PATH_MAX);
  if (result > 0)
    return std::string(dest_path, result);

  // Some links aren't set, so fall back on the name from the proc status.
  return GetStatus(pid)["Name"];
}

std::vector<std::string> GetCommandLine(int pid) {
  // /proc/<pid>/cmdline is a null-separated view of the command line.
  return fxl::SplitStringCopy(ReadProcFile(pid, "cmdline"), std::string_view("\0", 1),
                              fxl::kKeepWhitespace, fxl::kSplitWantNonEmpty);
}

std::string GetArgv0(int pid) {
  // The arguments in /proc/<pid>/cmdline are null-terminated, so argv[0] ends at the first null.
  std::string cmdline = ReadProcFile(pid, "cmdline");
  return cmdline.substr(0, cmdline.find('\0'));
}

uint64_t GetLdSoLoadAddress(int pid) { return ReadAuxv(pid)[AT_BASE]; }

std::optional<LdSoInfo> GetLdSoInfo(int pid) {
  uint64_t load_addr = GetLdSoLoadAddress(pid);
  if (!load_addr)
    return std::nullopt;

  for (const MapEntry& entry : GetMaps(pid)) {
    if (entry.range.begin() == load_addr) {
      auto path = fxl::StringPrintf("/proc/%d/map_files/%lx-%lx", pid, entry.range.begin(),
                                    entry.range.end());
      if (access(path.c_str(), R_OK) != 0)
        path = fxl::StringPrintf("/proc/%d/root", pid) + entry.path;
      return LdSoInfo{.load_address = load_addr, .path = std::move(path)};
    }
  }
  return std::nullopt;
}

uint64_t GetLdSoDebugAddress(int pid) {
  auto info = GetLdSoInfo(pid);
  if (!info)
    return 0;

  if (uint64_t relative_addr = GetLdSoDebugRelativeAddress(info->path))
    return info->load_address + relative_addr;
  return 0;
}

uint64_t GetLdSoBreakpointAddress(int pid) {
  auto info = GetLdSoInfo(pid);
  if (!info)
    return 0;

  if (uint64_t relative_addr = GetLdSoBreakpointRelativeAddress(info->path))
    return info->load_address + relative_addr;
  return 0;
}

std::map<uint64_t, uint64_t> ReadAuxv(int pid) { return ParseAuxv(ReadProcFile(pid, "auxv")); }

std::map<uint64_t, uint64_t> ParseAuxv(std::string_view contents) {
  std::map<uint64_t, uint64_t> result;
  constexpr size_t kEntrySize = 2 * sizeof(uint64_t);
  if (contents.size() % kEntrySize)
    return result;
  for (size_t offset = 0; offset < contents.size(); offset += kEntrySize) {
    uint64_t key, value;
    memcpy(&key, contents.data() + offset, sizeof(key));
    memcpy(&value, contents.data() + offset + sizeof(key), sizeof(value));
    if (key == AT_NULL)
      break;
    result[key] = value;
  }
  // Linux supplies the ELF program-header size in auxv. Validate the native
  // layout without requiring permission to reopen the executable on disk.
  auto phent = result.find(AT_PHENT);
  if (phent == result.end() || phent->second != sizeof(Elf64_Phdr))
    return {};
  return result;
}

std::optional<ThreadHandle::State> ThreadStateFromLinuxState(const std::string& linux_state) {
  using State = debug_ipc::ThreadRecord::State;
  using BlockedReason = debug_ipc::ThreadRecord::BlockedReason;

  if (linux_state.empty())
    return std::nullopt;

  // The first character gives the state.
  // TODO(brettw) We should probably extend our thread state options to better cover the Linux ones.
  switch (linux_state[0]) {
    case 'S':  // Sleeping
      return ThreadHandle::State(BlockedReason::kSleeping);
    case 'D':  // Waiting in uninterruptible disk sleep
      return ThreadHandle::State(BlockedReason::kSleeping);
    case 'T':  // Stopped (on a signal) or (before Linux 2.6.33) trace stopped
      return ThreadHandle::State(BlockedReason::kException);
    case 't':  // Tracing stop (Linux 2.6.33 onward)
      return ThreadHandle::State(State::kSuspended);
    case 'K':  // Wakekill (Linux 2.6.33 to 3.13 only)
    case 'W':  // Waking (Linux 2.6.33 to 3.13 only)
      return ThreadHandle::State(State::kRunning);
    case 'P':  // Parked (Linux 3.9 to 3.13 only)
      return ThreadHandle::State(BlockedReason::kSleeping);
    case 'X':  // Dead (from Linux 2.6.0 onward)
    case 'x':  // Dead (Linux 2.6.33 to 3.13 only)
    case 'Z':  // Zombie
      return ThreadHandle::State(State::kDead);
    case 'R':  // Running
      return ThreadHandle::State(State::kRunning);
    default:
      return std::nullopt;
  }
}

std::vector<int> GetDirectoryPids(const std::filesystem::path& path) {
  std::vector<int> result;

  std::error_code ec;
  for (const auto& child : std::filesystem::directory_iterator(path, ec)) {
    if (std::filesystem::is_directory(child.path(), ec)) {
      if (auto parsed = PathToPid(child.path()))
        result.push_back(*parsed);
    }
  }
  return result;
}

std::string SignalToString(int signal, bool include_number) {
  // clang-format off
  const char* name = nullptr;
  switch (signal) {
    case SIGABRT:   name = "SIGABRT";          break;
    case SIGALRM:   name = "SIGALRM";          break;
    case SIGBUS:    name = "SIGBUS";           break;
    case SIGCHLD:   name = "SIGCHLD";          break;
    case SIGCONT:   name = "SIGCONT";          break;
    case SIGFPE:    name = "SIGFPE";           break;
    case SIGHUP:    name = "SIGHUP";           break;
    case SIGILL:    name = "SIGILL";           break;
    case SIGINT:    name = "SIGINT";           break;
    case SIGKILL:   name = "SIGKILL";          break;
    case SIGPIPE:   name = "SIGPIPE";          break;
    case SIGPOLL:   name = "SIGPOLL";          break;
    case SIGPROF:   name = "SIGPROF";          break;
    case SIGPWR:    name = "SIGPWR";           break;
    case SIGQUIT:   name = "SIGQUIT";          break;
    case SIGSEGV:   name = "SIGSEGV";          break;
    case SIGSTKFLT: name = "SIGSTKFLT";        break;
    case SIGSTOP:   name = "SIGSTOP";          break;
    case SIGSYS:    name = "SIGSYS";           break;
    case SIGTERM:   name = "SIGTERM";          break;
    case SIGTRAP:   name = "SIGTRAP";          break;
    case SIGTSTP:   name = "SIGTSTP";          break;
    case SIGTTIN:   name = "SIGTTIN";          break;
    case SIGTTOU:   name = "SIGTTOU";          break;
    case SIGURG:    name = "SIGURG";           break;
    case SIGUSR1:   name = "SIGUSR1";          break;
    case SIGUSR2:   name = "SIGUSR2";          break;
    case SIGVTALRM: name = "SIGVTALRM";        break;
    case SIGWINCH:  name = "SIGWINCH";         break;
    case SIGXCPU:   name = "SIGXCPU";          break;
    case SIGXFSZ:   name = "SIGXFSZ";          break;
    default:        name = "<Unknown signal>"; break;
  }
  // clang-format on

  if (include_number) {
    std::ostringstream out;
    out << name << " (" << signal << ")";
    return out.str();
  }
  return name;
}

uint64_t GetLdSoDebugRelativeAddress(const std::string& ld_so_path) {
  auto elf = elflib::ElfLib::Create(ld_so_path);
  if (!elf)
    return 0;

  // LLDB and GDB have a list of different symbols to look for for different dynamic loaders.
  // We're only currently concerned with the one loader so omit that for now.
  if (auto sym = elf->GetDynamicSymbol("_r_debug"))
    return sym->st_value;

  return 0;
}

uint64_t GetLdSoBreakpointRelativeAddress(const std::string& ld_so_path) {
  auto elf = elflib::ElfLib::Create(ld_so_path);
  if (!elf)
    return 0;

  // Different loaders use different symbol names. This is the list that everybody uses:
  static const char* kDebugStateNames[] = {
      "_dl_debug_state", "__dl_rtld_db_dlactivity", "_r_debug_state",
      "r_debug_state",   "rtld_db_dlactivity",      "_rtld_debug_state",
  };

  for (const char* name : kDebugStateNames) {
    if (auto sym = elf->GetDynamicSymbol(name))
      return sym->st_value;
  }

  return 0;
}

}  // namespace linux
}  // namespace debug_agent
