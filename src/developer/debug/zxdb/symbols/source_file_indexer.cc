// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/source_file_indexer.h"

#include <lib/syslog/cpp/macros.h>

#include <algorithm>

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "src/developer/debug/zxdb/symbols/dwarf_binary.h"
#include "src/developer/debug/zxdb/symbols/line_file_table.h"
#include "src/developer/debug/zxdb/symbols/line_program.h"

namespace zxdb {

namespace {

// The work runs during DIE indexing, which usually takes much longer. One worker is then enough to
// hide it, and more workers only add startup cost. Large split-DWARF binaries have little DIE work
// in the main binary, so they need more workers. The decoder handles about 300 bytes per
// microsecond.
//
// Below this many bytes of line tables (about 1 ms of work), the work runs on the calling thread.
// Starting one thread costs tens of microseconds.
constexpr uint64_t kMinBytesForThreads = 256 * 1024;

// Each worker after the first is for this many more bytes (about 25 ms of work).
constexpr uint64_t kBytesPerExtraThread = 8 * 1024 * 1024;

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

// Returns the size of the line table at the offset from its unit length, or 0 if the section is
// too short. This assumes little-endian data. It is only for the thread count.
uint64_t GetLineTableSize(const llvm::DWARFSection& section, uint64_t offset) {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(section.Data.data());
  uint64_t section_size = section.Data.size();
  auto read = [&](uint64_t at, size_t bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; i++)
      value |= static_cast<uint64_t>(data[at + i]) << (8 * i);
    return value;
  };

  if (offset > section_size || section_size - offset < 4)
    return 0;
  uint64_t length = read(offset, 4);
  uint64_t header = 4;
  if (length == 0xffffffff) {
    if (section_size - offset < 12)
      return 0;
    length = read(offset + 4, 8);
    header = 12;
  }
  return std::min(length, section_size - offset - header) + header;
}

bool FormNeedsUnit(const llvm::DWARFFormValue& form) {
  switch (form.getForm()) {
    case llvm::dwarf::DW_FORM_strx:
    case llvm::dwarf::DW_FORM_strx1:
    case llvm::dwarf::DW_FORM_strx2:
    case llvm::dwarf::DW_FORM_strx3:
    case llvm::dwarf::DW_FORM_strx4:
    case llvm::dwarf::DW_FORM_GNU_str_index:
      return true;
    default:
      return false;
  }
}

// Returns the string addresses that determine the name of a file table entry, or nullopt if the
// entry has no name. This makes the same choices as
// llvm::DWARFDebugLine::Prologue::getFileNameByIndex() for relative names:
//   - A name that decodes to a null pointer is an empty name. That isn't the same as a name that
//     can't be decoded, which means there is no name.
//   - A directory that is absent or can't be decoded is an empty directory. Both have a null
//     directory in the key.
std::optional<std::pair<const char*, const char*>> GetFileNameKey(
    const llvm::DWARFDebugLine::Prologue& prologue, uint64_t file_index) {
  if (!prologue.hasFileAtIndex(file_index))
    return std::nullopt;
  const llvm::DWARFDebugLine::FileNameEntry& entry = prologue.getFileNameEntry(file_index);

  std::optional<const char*> name = llvm::dwarf::toString(entry.Name);
  if (!name)
    return std::nullopt;

  const auto& dirs = prologue.IncludeDirectories;
  std::optional<llvm::DWARFFormValue> dir_form;
  if (prologue.getVersion() >= 5) {
    // Directory 0 is the compilation directory, which relative names don't include.
    if (entry.DirIdx != 0 && entry.DirIdx < dirs.size())
      dir_form = dirs[entry.DirIdx];
  } else {
    if (entry.DirIdx > 0 && entry.DirIdx <= dirs.size())
      dir_form = dirs[entry.DirIdx - 1];
  }

  const char* dir = dir_form ? llvm::dwarf::toString(dir_form, nullptr) : nullptr;
  return std::make_pair(dir, *name);
}

}  // namespace

void AddLineTableJob(UnitIndex unit_index, llvm::DWARFUnit& unit, std::vector<LineTableJob>* jobs) {
  std::optional<uint64_t> offset = LineProgram::GetUnitLineTableOffset(unit);
  if (!offset)
    return;

  LineTableJob& job = jobs->emplace_back();
  job.unit_index = unit_index;
  job.section = &unit.getLineSection();
  job.offset = *offset;
  job.address_size = unit.getAddressByteSize();
  job.size = GetLineTableSize(*job.section, job.offset);
  job.unit = &unit;
}

std::vector<LineTableJob> CollectLineTableJobs(DwarfBinary& binary) {
  std::vector<LineTableJob> jobs;

  auto add_units = [&](bool is_dwo, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
      UnitIndex unit_index(is_dwo, i);
      fxl::RefPtr<DwarfUnit> unit = binary.GetUnitAtIndex(unit_index);
      if (llvm::DWARFUnit* llvm_unit = unit ? unit->GetLLVMUnit() : nullptr)
        AddLineTableJob(unit_index, *llvm_unit, &jobs);
    }
  };
  add_units(false, binary.GetNormalUnitCount());
  add_units(true, binary.GetDWOUnitCount());

  return jobs;
}

bool LineNamesNeedUnit(const llvm::DWARFDebugLine::Prologue& prologue) {
  for (const llvm::DWARFFormValue& dir : prologue.IncludeDirectories) {
    if (FormNeedsUnit(dir))
      return true;
  }
  for (const llvm::DWARFDebugLine::FileNameEntry& file : prologue.FileNames) {
    if (FormNeedsUnit(file.Name))
      return true;
  }
  return false;
}

const std::string* LineFilePathResolver::Resolve(const llvm::DWARFDebugLine::Prologue& prologue,
                                                 uint64_t file_index) {
  std::optional<std::pair<const char*, const char*>> key = GetFileNameKey(prologue, file_index);
  if (!key)
    return nullptr;

  auto [iter, inserted] = by_key_.try_emplace(*key);
  if (inserted)
    iter->second = ResolveLineFileName(prologue, file_index).value_or(std::string());
  // An empty name means no file, like in LineFileTable.
  return iter->second.empty() ? nullptr : &iter->second;
}

SourceFileIndexer::SourceFileIndexer(const llvm::DWARFContext& context,
                                     std::vector<LineTableJob> jobs, Options options)
    : context_(context), jobs_(std::move(jobs)), results_(jobs_.size()) {
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

  // The workers are done, so this thread can use the units for the jobs that need them.
  for (size_t i = 0; i < jobs_.size(); i++) {
    if (results_[i].needs_unit && jobs_[i].unit) {
      // The string offsets table comes from the unit DIE. Index::CreateIndex() can have freed the
      // unit's DIEs, so extract the unit DIE again.
      jobs_[i].unit->getUnitDIE();
      RunJob(i, resolver, jobs_[i].unit);
    }
  }

  for (size_t i = 0; i < jobs_.size(); i++) {
    UnitIndex unit_index = jobs_[i].unit_index;
    for (const std::string* name : results_[i].names) {
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
    RunJob(index, resolver, nullptr);
  }
}

void SourceFileIndexer::RunJob(size_t index, LineFilePathResolver& resolver,
                               const llvm::DWARFUnit* unit) {
  const LineTableJob& job = jobs_[index];
  JobResult& result = results_[index];

  // Workers don't pass the unit. The prologue parser uses it only for string forms that need the
  // unit's string offsets table, and those jobs run again with the unit in FinishInto().
  ErrOr<LineProgram> program =
      LineProgram::Parse(context_, *job.section, job.offset, job.address_size, unit);
  if (program.has_error())
    return;
  if (!unit && LineNamesNeedUnit(program.value().prologue())) {
    result.needs_unit = true;
    return;
  }

  std::vector<uint32_t> file_indices;
  program.value().ScanFiles(&file_indices);  // Keeps the files found before any error.

  result.names.clear();
  for (uint32_t file_index : file_indices) {
    if (const std::string* name = resolver.Resolve(program.value().prologue(), file_index))
      result.names.push_back(name);
  }
}

}  // namespace zxdb
