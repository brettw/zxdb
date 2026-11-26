// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/source_file_indexer.h"

#include <lib/syslog/cpp/macros.h>

#include <algorithm>

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf_binary.h"
#include "src/developer/debug/zxdb/symbols/dwarf_sections_llvm.h"
#include "src/developer/debug/zxdb/symbols/line_file_table.h"
#include "src/developer/debug/zxdb/symbols/line_program.h"
#include "src/developer/debug/zxdb/symbols/line_program_llvm.h"

namespace zxdb {

namespace {

// For most binaries the work runs during DIE indexing, which takes much longer, so the thread count
// doesn't change the index time. A split-DWARF main binary has only skeleton units, so the index
// waits for this work and more workers make it faster. On the split-DWARF build of zxdb (11 MB of
// line tables), the work took 42 ms on the calling thread, 24 ms with two workers, and 16 ms with
// six. The decoder handles about 300 bytes per microsecond on one thread.
//
// Below this many bytes of line tables (about 1 ms of work), the work runs on the calling thread.
// Starting threads costs more than the work.
constexpr uint64_t kMinBytesForThreads = 256 * 1024;

// Each worker after the first is for this many more bytes (about 7 ms of work).
constexpr uint64_t kBytesPerExtraThread = 2 * 1024 * 1024;

constexpr size_t kMaxThreads = 6;

size_t GetDefaultThreadCount(const std::vector<LineTableJob>& jobs) {
  uint64_t total = 0;
  for (const LineTableJob& job : jobs)
    total += job.size;
  if (total < kMinBytesForThreads)
    return 0;
  size_t hardware = std::max<size_t>(std::thread::hardware_concurrency(), 1);
  return std::min<size_t>({1 + total / kBytesPerExtraThread, kMaxThreads, hardware});
}

// Returns the size of the line table at the offset from its unit length, or 0 if it is invalid.
// This is only for the thread count.
uint64_t GetLineTableSize(std::string_view section, uint64_t offset, bool little_endian) {
  ErrOr<LineTableExtent> extent = ReadLineTableExtent(section, offset, little_endian);
  return extent.ok() ? extent.value().end - offset : 0;
}

// Returns the string addresses that determine the name of a file table entry, or nullopt if the
// entry has no name. A directory that is absent or can't be decoded is an empty directory for
// ResolveLineFileName(), so both have a null directory in the key. The strings are null-terminated
// in the section data, so the address determines the string.
std::optional<std::pair<const char*, const char*>> GetFileNameKey(LineFileEntries files,
                                                                  uint64_t file_index) {
  const LineFileEntry* entry = files.GetFileEntry(file_index);
  if (!entry || !entry->name)
    return std::nullopt;
  std::optional<std::string_view> dir = files.GetFileDirectory(*entry);
  return std::make_pair(dir ? dir->data() : nullptr, entry->name->data());
}

}  // namespace

void AddLineTableJob(const DwarfSections& sections, UnitIndex unit_index, llvm::DWARFUnit& unit,
                     uint64_t first_code_address, std::vector<LineTableJob>* jobs) {
  std::optional<uint64_t> offset = GetUnitLineTableOffset(unit);
  if (!offset)
    return;

  LineTableJob& job = jobs->emplace_back();
  job.unit_index = unit_index;
  job.section = AsStringView(GetUnitLineSection(sections, unit));
  job.offset = *offset;
  job.little_endian = sections.little_endian;
  job.size = GetLineTableSize(job.section, job.offset, job.little_endian);
  job.strings = GetLineStringSections(sections, unit);
  job.first_code_address = first_code_address;
}

std::vector<LineTableJob> CollectLineTableJobs(DwarfBinary& binary) {
  std::vector<LineTableJob> jobs;
  const DwarfSections& sections = binary.GetSections();

  auto add_units = [&](bool is_dwo, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
      UnitIndex unit_index(is_dwo, i);
      fxl::RefPtr<DwarfUnit> unit = binary.GetUnitAtIndex(unit_index);
      if (llvm::DWARFUnit* llvm_unit = unit ? unit->GetLLVMUnit() : nullptr)
        AddLineTableJob(sections, unit_index, *llvm_unit, binary.GetFirstCodeAddress(), &jobs);
    }
  };
  add_units(false, binary.GetNormalUnitCount());
  add_units(true, binary.GetDWOUnitCount());

  return jobs;
}

const std::string* LineFilePathResolver::Resolve(LineFileEntries files, uint64_t file_index) {
  std::optional<std::pair<const char*, const char*>> key = GetFileNameKey(files, file_index);
  if (!key)
    return nullptr;

  auto [iter, inserted] = by_key_.try_emplace(*key);
  if (inserted)
    iter->second = ResolveLineFileName(files, file_index).value_or(std::string());
  // An empty name means no file, like in LineFileTable.
  return iter->second.empty() ? nullptr : &iter->second;
}

SourceFileIndexer::SourceFileIndexer(std::vector<LineTableJob> jobs, Options options)
    : jobs_(std::move(jobs)), results_(jobs_.size()) {
  size_t thread_count = options.thread_count ? *options.thread_count : GetDefaultThreadCount(jobs_);
  resolvers_.resize(thread_count);
  for (LineFilePathResolver& resolver : resolvers_)
    threads_.emplace_back([this, &resolver]() { RunJobs(resolver); });
}

SourceFileIndexer::~SourceFileIndexer() {
  for (std::thread& thread : threads_)
    thread.join();
}

void SourceFileIndexer::FinishInto(FileIndex* files) {
  FX_CHECK(!finished_) << "SourceFileIndexer::FinishInto() can be called only once.";
  finished_ = true;

  // Resolves the names of the jobs that this thread runs. It lives until the merge below.
  LineFilePathResolver resolver;
  if (threads_.empty()) {
    RunJobs(resolver);
  } else {
    for (std::thread& thread : threads_)
      thread.join();
    threads_.clear();
  }

  for (size_t i = 0; i < jobs_.size(); i++) {
    UnitIndex unit_index = jobs_[i].unit_index;
    for (const std::string* name : results_[i]) {
      std::vector<UnitIndex>& units = (*files)[*name];
      // Two file entries in one unit can have the same name.
      if (units.empty() || units.back() != unit_index)
        units.push_back(unit_index);
    }
  }

  jobs_.clear();
  results_.clear();
  resolvers_.clear();
}

void SourceFileIndexer::RunJobs(LineFilePathResolver& resolver) {
  for (;;) {
    size_t index = next_job_.fetch_add(1, std::memory_order_relaxed);
    if (index >= jobs_.size())
      return;
    RunJob(index, resolver);
  }
}

void SourceFileIndexer::RunJob(size_t index, LineFilePathResolver& resolver) {
  const LineTableJob& job = jobs_[index];
  ErrOr<LineProgram> program =
      LineProgram::Parse(job.section, job.offset, job.little_endian, job.strings);
  if (program.has_error())
    return;

  // Keep the complete live sequences found before any decode error.
  LineProgram::FileScanResult scan = program.value().ScanFiles(job.first_code_address);
  LineFileEntries files = program.value().files(scan.defined_files);

  JobResult& result = results_[index];
  for (uint32_t file_index : scan.file_indices) {
    if (const std::string* name = resolver.Resolve(files, file_index))
      result.push_back(name);
  }
}

}  // namespace zxdb
