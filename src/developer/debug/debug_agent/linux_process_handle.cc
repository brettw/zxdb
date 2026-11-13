// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/linux_process_handle.h"

#include <algorithm>

#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <zircon/errors.h>

#include <fbl/unique_fd.h>

#include "src/developer/debug/debug_agent/elf_utils.h"
#include "src/developer/debug/debug_agent/linux_exception_handle.h"
#include "src/developer/debug/debug_agent/linux_suspend_handle.h"
#include "src/developer/debug/debug_agent/linux_thread_handle.h"
#include "src/developer/debug/debug_agent/linux_utils.h"
#include "src/developer/debug/debug_agent/process_handle_observer.h"
#include "src/developer/debug/ipc/records.h"
#include "src/developer/debug/shared/logging/logging.h"
#include "src/developer/debug/shared/message_loop.h"
#include "src/lib/fxl/strings/join_strings.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace debug_agent {

namespace {

// Word size for memory operations with ptrace.
using PtraceWord = uintptr_t;
constexpr size_t kPtraceWordSize = sizeof(PtraceWord);

// Wrapper around ptrace PEEK/POKE for LinuxProcessHandle's WriteMemoryByWord().
debug::Status PtraceReadWrapper(pid_t pid, uintptr_t word_address, uintptr_t* data_out) {
  FX_DCHECK(word_address % kPtraceWordSize == 0);

  errno = 0;
  uint64_t word = ptrace(PTRACE_PEEKTEXT, pid, word_address, 0);
  if (errno)
    return debug::ErrnoStatus(errno);

  *data_out = word;
  return debug::Status();
}

debug::Status PtraceWriteWrapper(pid_t pid, uintptr_t word_address, uintptr_t data) {
  if (ptrace(PTRACE_POKETEXT, pid, word_address, data) == -1)
    return debug::ErrnoStatus(errno);
  return debug::Status();
}

}  // namespace

LinuxProcessHandle::LinuxProcessHandle(fxl::RefPtr<LinuxTask> task) : task_(std::move(task)) {
  tasks_[task_->pid()] = task_;
  task_->SetObserver(this);
}

LinuxProcessHandle::~LinuxProcessHandle() {
  Detach();
  for (auto& [pid, task] : tasks_)
    task->SetObserver(nullptr);
}

std::string LinuxProcessHandle::GetName() const {
  // Use only the module name. This process name will be used as the module name for the main
  // module so must not include the command line.
  if (auto cmdline = linux::GetCommandLine(task_->pid()); !cmdline.empty())
    return cmdline[0];
  return linux::GetExe(task_->pid());
}

std::vector<std::unique_ptr<ThreadHandle>> LinuxProcessHandle::GetChildThreads() const {
  std::vector<std::unique_ptr<ThreadHandle>> result;

  if (observer_) {
    for (const auto& [pid, task] : tasks_) {
      if (!task->is_exiting())
        result.push_back(std::make_unique<LinuxThreadHandle>(task));
    }
  } else {
    for (int pid : linux::GetDirectoryPids(fxl::StringPrintf("/proc/%d/task", task_->pid()))) {
      auto found = tasks_.find(pid);
      result.push_back(std::make_unique<LinuxThreadHandle>(
          found == tasks_.end() ? fxl::MakeRefCounted<LinuxTask>(pid) : found->second));
    }
  }

  return result;
}

debug::Status LinuxProcessHandle::Kill() {
  if (kill(task_->pid(), SIGKILL) != 0)
    return debug::ErrnoStatus(errno);
  if (!observer_) {
    // A failed fork setup has no frontend owner to consume terminal events.
    int status;
    pid_t result;
    do {
      result = waitpid(task_->pid(), &status, __WALL);
      if (result < 0 && errno == EINTR)
        continue;
      if (result > 0 && WIFSTOPPED(status))
        ptrace(PTRACE_CONT, task_->pid(), nullptr, nullptr);
      else
        break;
    } while (true);
    task_->Forget();
  }
  return debug::Status();
}

debug::Status LinuxProcessHandle::Attach(ProcessHandleObserver* observer, AttachConfig config) {
  if (!config.claim_exception_channel)
    return debug::Status("Linux requires a full ptrace attach; observation-only attach is unsupported.");
  std::vector<std::unique_ptr<LinuxSuspendHandle>> stops;
  // Freeze every attached task before enumerating again. Attached parents cannot
  // create another thread, and a new kernel child remains in its initial stop.
  for (;;) {
    bool added = false;
    auto pids = linux::GetDirectoryPids(fxl::StringPrintf("/proc/%d/task", task_->pid()));
    if (pids.empty())
      return debug::Status("Cannot enumerate process threads.");
    for (int pid : pids) {
      auto found = tasks_.find(pid);
      if (found != tasks_.end() && found->second->is_attached() && found->second->is_suspended())
        continue;
      auto task = found == tasks_.end() ? fxl::MakeRefCounted<LinuxTask>(pid) : found->second;
      auto status = task->Attach();
      if (status.has_error()) {
        if (linux::GetStatus(pid).empty())
          continue;  // Thread exited during enumeration.
        Detach();
        return status;
      }
      tasks_[pid] = task;
      task->SetObserver(this);
      stops.push_back(std::make_unique<LinuxSuspendHandle>(task));
      if (!task->is_suspended()) {
        auto error = task->last_error();
        Detach();
        return error;
      }
      if (!linux::ReadGeneralRegisters(pid)) {
        Detach();
        return debug::Status("Cannot read native x86-64 registers; inferior ABI is unsupported or task exited.");
      }
      added = true;
    }
    if (!added)
      break;
  }
  observer_ = observer;
  return debug::Status();
}

bool LinuxProcessHandle::IsAttached() const {
  return std::any_of(tasks_.begin(), tasks_.end(),
                     [](const auto& entry) { return entry.second->is_attached(); });
}

debug::Status LinuxProcessHandle::Detach() {
  for (auto& [pid, exception] : unsupported_children_) {
    if (auto status = exception->task()->Detach(); status.has_error())
      return status;
    exception->task()->SetObserver(nullptr);
  }
  unsupported_children_.clear();
  for (auto& [pid, child] : vfork_children_) {
    if (auto status = child->Detach(); status.has_error())
      return status;
    child->SetObserver(nullptr);
  }
  vfork_children_.clear();
  for (auto& [pid, task] : tasks_) {
    auto status = task->Detach();
    if (status.has_error())
      return status;
  }
  return debug::Status();
}

uint64_t LinuxProcessHandle::GetLoaderBreakpointAddress() {
  return linux::GetLdSoBreakpointAddress(task_->pid());
}

std::vector<debug_ipc::AddressRegion> LinuxProcessHandle::GetAddressSpace(uint64_t address) const {
  std::vector<debug_ipc::AddressRegion> result;
  for (auto& entry : linux::GetMaps(task_->pid())) {
    if (address && !entry.range.InRange(address))
      continue;  // Asking for a specific range this address isn't in.

    debug_ipc::AddressRegion& region = result.emplace_back();
    region.name = std::move(entry.path);
    region.base = entry.range.begin();
    region.size = entry.range.size();
    region.depth = 0;  // Linux mappings aren't nested.
    region.vmo_offset = entry.offset;
    region.read = entry.read;
    region.write = entry.write;
    region.execute = entry.execute;
    region.shared = entry.shared;
  }
  return result;
}

std::optional<bool> LinuxProcessHandle::IsAddressMapped(uint64_t address) const {
  auto maps = linux::GetMapsChecked(task_->pid());
  if (maps.has_error())
    return std::nullopt;
  for (const auto& entry : maps.value()) {
    if (entry.range.InRange(address))
      return true;
  }
  return false;
}

std::optional<debug_ipc::AddressRegion> LinuxProcessHandle::GetSharedAddressSpace() const {
  // Not implemented.
  return std::nullopt;
}

std::vector<debug_ipc::Module> LinuxProcessHandle::GetModules() const {
  std::vector<std::unique_ptr<LinuxSuspendHandle>> stops;
  for (auto& [pid, task] : tasks_) {
    if (!task->is_attached() || task->is_exiting())
      continue;
    stops.push_back(std::make_unique<LinuxSuspendHandle>(task));
    if (!task->is_suspended()) {
      LOGS(Error) << "Cannot stop task for module inspection: " << task->last_error().message();
      return {};
    }
  }
  return GetElfModulesForProcess(*this, linux::GetLdSoDebugAddress(task_->pid()));
}

fit::result<debug::Status, std::vector<debug_ipc::InfoHandle>> LinuxProcessHandle::GetHandles()
    const {
  // This does not exist on linux.
  return fit::error(debug::Status("No handles on Linux"));
}

debug::Status LinuxProcessHandle::ReadMemory(uintptr_t address, void* buffer, size_t len,
                                             size_t* actual) const {
  *actual = 0;
  if (len > UINTPTR_MAX - address)
    return debug::Status("Memory read address overflow.");
  iovec local_iov;
  local_iov.iov_base = buffer;
  local_iov.iov_len = len;

  iovec remote_iov;
  remote_iov.iov_base = reinterpret_cast<void*>(address);
  remote_iov.iov_len = len;

  auto read_result = process_vm_readv(task_->pid(), &local_iov, 1, &remote_iov, 1, 0);
  int read_error = read_result < 0 ? errno : 0;
  if (read_result > 0)
    *actual = static_cast<size_t>(read_result);
  if (*actual == len)
    return debug::Status();

  // Ptrace can read mapped PROT_NONE memory and executable-only pages. Only
  // attempt it for an owned task with a confirmed stop.
  for (const auto& [pid, task] : tasks_) {
    if (!task->is_attached() || task->is_exiting())
      continue;
    LinuxSuspendHandle stop(task);
    if (!task->is_suspended())
      continue;
    auto bytes = static_cast<uint8_t*>(buffer);
    while (*actual < len) {
      uintptr_t current = address + *actual;
      uintptr_t aligned = current & ~(kPtraceWordSize - 1);
      uintptr_t word;
      if (auto status = PtraceReadWrapper(pid, aligned, &word); status.has_error())
        break;
      size_t offset = current - aligned;
      size_t count = std::min(len - *actual, kPtraceWordSize - offset);
      memcpy(bytes + *actual, reinterpret_cast<const uint8_t*>(&word) + offset, count);
      *actual += count;
    }
    break;
  }
  return *actual || !read_error ? debug::Status() : debug::ErrnoStatus(read_error);
}

debug::Status LinuxProcessHandle::WriteMemory(uintptr_t address, const void* buffer, size_t len,
                                              size_t* actual) {
  *actual = 0;

  // These ptrace functions require a stopped process which isn't guaranteed by our infrastructure.
  // Linux suspensions are synchronous so we don't need to wait for this to complete.
  std::vector<std::unique_ptr<LinuxSuspendHandle>> stops;
  fxl::RefPtr<LinuxTask> memory_task;
  for (auto& [pid, task] : tasks_) {
    if (task->is_exiting())
      continue;
    stops.push_back(std::make_unique<LinuxSuspendHandle>(task));
    if (!task->is_suspended())
      return task->last_error();
    memory_task = task;
  }
  if (!memory_task)
    return debug::Status("No stopped thread available for memory write.");

  // Unlike for reading, we can't use process_vm_writev() because that won't let us write to
  // read-only memory which is needed for breakpoints. PTRACE_POKETEXT allows us to do this but the
  // API is much less convenient.
  return WriteMemoryByWord(memory_task->pid(), address, buffer, len, &PtraceReadWrapper,
                           &PtraceWriteWrapper, actual);
}

std::vector<debug_ipc::MemoryBlock> LinuxProcessHandle::ReadMemoryBlocks(uint64_t address,
                                                                         uint32_t size) const {
  auto read_block = [this](uint64_t begin, uint32_t length) {
    debug_ipc::MemoryBlock block;
    block.address = begin;
    block.size = length;
    block.data.resize(length);
    size_t actual = 0;
    if (ReadMemory(begin, block.data.data(), length, &actual).has_error())
      actual = 0;
    if (actual == 0) {
      // Invalid blocks describe a range; they must not retain a data allocation.
      std::vector<uint8_t>().swap(block.data);
    } else if (actual < length) {
      std::vector<uint8_t>(block.data.begin(), block.data.begin() + actual).swap(block.data);
    }
    block.valid = actual == length;
    return block;
  };
  if (size > UINT64_MAX - address) {
    debug_ipc::MemoryBlock invalid;
    invalid.address = address;
    invalid.size = size;
    return {std::move(invalid)};
  }
  std::vector<debug_ipc::MemoryBlock> result;
  {
    auto block = read_block(address, size);
    if (block.valid) {
      result.push_back(std::move(block));
      return result;
    }
  }  // Release the initial read buffer before allocating fallback blocks.

  // A failed read says nothing about later mappings. Retry at each mapping
  // boundary so a hole cannot hide readable memory after it.
  uint64_t end = address + size;
  std::vector<uint64_t> boundaries;
  for (const auto& map : linux::GetMaps(task_->pid())) {
    if (map.range.begin() > address && map.range.begin() < end)
      boundaries.push_back(map.range.begin());
    if (map.range.end() > address && map.range.end() < end)
      boundaries.push_back(map.range.end());
  }
  boundaries.push_back(end);
  std::sort(boundaries.begin(), boundaries.end());
  boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

  for (uint64_t boundary : boundaries) {
    auto part = read_block(address, boundary - address);
    if (!part.valid && !part.data.empty()) {
      // Mappings can change during the read. Preserve any successfully read
      // prefix, but never label unread bytes as valid.
      debug_ipc::MemoryBlock unread;
      unread.address = address + part.data.size();
      unread.size = boundary - unread.address;
      part.size = part.data.size();
      part.valid = true;
      result.push_back(std::move(part));
      result.push_back(std::move(unread));
    } else {
      result.push_back(std::move(part));
    }
    address = boundary;
  }
  return result;
}

debug::Status LinuxProcessHandle::SaveMinidump(const std::vector<DebuggedThread*>& threads,
                                               std::vector<uint8_t>* core_data) {
  return debug::Status("SaveMinidump is unimplemented on Linux.");
}

// static
debug::Status LinuxProcessHandle::WriteMemoryByWord(pid_t pid, uintptr_t address,
                                                    const void* buffer, size_t len,
                                                    ReadWordFunc read_fn, WriteWordFunc write_fn,
                                                    size_t* actual) {
  *actual = 0;
  if (len > UINTPTR_MAX - address)
    return debug::Status("Memory write address overflow.");
  if (len == 0)
    return debug::Status();

  const uint8_t* source_byte_buffer = static_cast<const uint8_t*>(buffer);

  uintptr_t word_first = address / kPtraceWordSize * kPtraceWordSize;
  uintptr_t word_last = (address + len - 1) / kPtraceWordSize * kPtraceWordSize;

  size_t begin_offset_in_word = address % kPtraceWordSize;

  // Using this buffer + memcpy is not as fast as doing bitwise register operations but it's easier
  // to write correctly and avoids any endian issues or undefined compiler behavior.

  // copy_target_to_word_buffer and copy_word_buffer_to_target transfers between the target memory
  // and our word buffer, converted through a register word as used in ptrace.
  uint8_t word_buffer[kPtraceWordSize];
  auto copy_target_to_word_buffer = [pid, word_buffer = word_buffer,
                                     read_fn](uintptr_t word_address) mutable {
    PtraceWord word;
    if (debug::Status status = read_fn(pid, word_address, &word); status.has_error())
      return status;
    memcpy(word_buffer, &word, kPtraceWordSize);
    return debug::Status();
  };
  auto copy_word_buffer_to_target = [pid, word_buffer = word_buffer,
                                     write_fn](uintptr_t word_address) {
    PtraceWord word;
    memcpy(&word, word_buffer, kPtraceWordSize);
    return write_fn(pid, word_address, word);
  };

  if (word_first == word_last) {
    // Handle masking on both ends.
    if (debug::Status status = copy_target_to_word_buffer(word_first); status.has_error())
      return status;
    memcpy(&word_buffer[begin_offset_in_word], source_byte_buffer, len);
    if (debug::Status status = copy_word_buffer_to_target(word_first); status.has_error())
      return status;

    *actual = len;
    return debug::Status();
  }

  // Writing more than one word.

  // Left side.
  size_t source_offset = 0;  // Index of the byte we're reading, updated as we go.
  uintptr_t first_full_word_address;
  if (address % kPtraceWordSize == 0) {
    // Beginning is word aligned.
    first_full_word_address = word_first;
  } else {
    // Need to read and combine the bytes on the left size.
    // Example of which bytes to write for begin_offset_in_word = 5:
    //               0 1 2 3 4 5 6 7
    //             [ . . . . . X X X ]
    if (debug::Status status = copy_target_to_word_buffer(word_first); status.has_error())
      return status;
    size_t left_bytes_to_write = kPtraceWordSize - begin_offset_in_word;  // = 3 for the example.
    memcpy(&word_buffer[begin_offset_in_word], source_byte_buffer, left_bytes_to_write);
    if (debug::Status status = copy_word_buffer_to_target(word_first); status.has_error())
      return status;

    (*actual) += left_bytes_to_write;
    source_offset += left_bytes_to_write;
    first_full_word_address = word_first + kPtraceWordSize;
  }

  // Set of full words.
  uintptr_t last_full_word_address = (address + len - 8) / kPtraceWordSize * kPtraceWordSize;
  if (last_full_word_address >= first_full_word_address) {
    size_t num_full_words =
        (last_full_word_address - first_full_word_address) / kPtraceWordSize + 1;
    for (size_t word_i = 0; word_i < num_full_words; word_i++) {
      PtraceWord word;
      memcpy(&word, &source_byte_buffer[source_offset], kPtraceWordSize);
      if (debug::Status status =
              write_fn(pid, first_full_word_address + word_i * kPtraceWordSize, word);
          status.has_error()) {
        return status;
      }

      (*actual) += kPtraceWordSize;
      source_offset += kPtraceWordSize;
    }
  }

  // Right side.
  if (source_offset < len) {
    FX_DCHECK(len - source_offset < kPtraceWordSize);  // Should have less than one word remaining.
    if (debug::Status status = copy_target_to_word_buffer(word_last); status.has_error())
      return status;
    memcpy(word_buffer, &source_byte_buffer[source_offset], len - source_offset);
    if (debug::Status status = copy_word_buffer_to_target(word_last); status.has_error())
      return status;

    (*actual) += len - source_offset;
  }

  return debug::Status();
}

void LinuxProcessHandle::OnUnsupportedClone(std::unique_ptr<LinuxExceptionHandle> exception) {
  auto child = exception->task();
  child->SetObserver(this);
  unsupported_children_[child->pid()] = std::move(exception);
}

void LinuxProcessHandle::OnVforkStarting(std::unique_ptr<LinuxExceptionHandle> exception) {
  auto child = exception->task();
  vfork_children_[child->pid()] = child;
  child->SetObserver(this);
  if (auto status = observer_->OnVfork(true); status.has_error()) {
    LOGS(Error) << "Cannot prepare shared vfork image: " << status.message();
    kill(child->pid(), SIGKILL);
  }
}

void LinuxProcessHandle::OnVforkFinished(fxl::RefPtr<LinuxTask> child, bool exec) {
  auto status = observer_->OnVfork(false);
  if (status.has_error())
    LOGS(Error) << "Cannot restore parent after vfork: " << status.message();
  vfork_children_.erase(child->pid());
  if (exec)
    observer_->OnProcessStarting(std::make_unique<LinuxProcessHandle>(child), false);
  else
    child->SetObserver(nullptr);
}

void LinuxProcessHandle::OnExec(LinuxTask* task, int former_tid) {
  for (auto& [pid, old_task] : tasks_) {
    if (old_task.get() != task) {
      old_task->Forget();
      old_task->SetObserver(nullptr);
    }
  }
  tasks_.clear();
  tasks_[task->pid()] = task_;
  if (observer_)
    observer_->OnExec();
}

void LinuxProcessHandle::OnExited(LinuxTask* task,
                                  std::unique_ptr<LinuxExceptionHandle> exception) {
  if (unsupported_children_.erase(task->pid()))
    return;
  tasks_.erase(task->pid());
  if (!observer_)
    return;
  // Linux delays the leader's final wait status until all threads have exited.
  if (task->pid() == task_->pid()) {
    // Thread is the main thread of this process.
    observer_->OnProcessTerminated();
  } else {
    observer_->OnThreadExiting(std::move(exception));
  }
}

void LinuxProcessHandle::OnProcessStarting(std::unique_ptr<LinuxExceptionHandle> exception) {
  observer_->OnProcessStarting(std::make_unique<LinuxProcessHandle>(exception->task()));

  // Letting the exception handle go out of scope here may resume the new process. If the
  // observer wants it suspended it will have taken a new suspend handle to it.
}

void LinuxProcessHandle::OnThreadStarting(std::unique_ptr<LinuxExceptionHandle> exception) {
  // Register outselves as the watcher for the new task. All thread notifications are routed through
  // the process.
  tasks_[exception->task()->pid()] = exception->task();
  exception->task()->SetObserver(this);
  observer_->OnThreadStarting(std::move(exception));
}

void LinuxProcessHandle::OnTermSignal(int pid, int signal_number) {
  observer_->OnProcessTerminated();
}

void LinuxProcessHandle::OnStopSignal(LinuxTask* task,
                                      std::unique_ptr<LinuxExceptionHandle> exception) {
  observer_->OnException(std::move(exception));
}

void LinuxProcessHandle::OnContinued(int pid) {}

}  // namespace debug_agent
