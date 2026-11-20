// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/line_table_cache.h"

#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "src/developer/debug/zxdb/symbols/line_program.h"

namespace zxdb {

std::shared_ptr<const LineFileTable> LineTableCache::GetFileTable(llvm::DWARFUnit& unit,
                                                                  uint64_t file_index) {
  std::optional<uint64_t> offset = LineProgram::GetUnitLineTableOffset(unit);
  if (!offset)
    return nullptr;

  auto [iter, inserted] = entries_.try_emplace(Key(unit.getLineSection().Data.data(), *offset));
  Entry& entry = iter->second;
  bool needs_more = !entry.complete && entry.files && file_index >= entry.files->size();
  if (!inserted && !needs_more)
    return entry.files;

  ErrOr<LineProgram> program = LineProgram::ParseForUnit(unit, *offset);
  if (program.has_error()) {
    entry.complete = true;  // Don't try again.
    return entry.files;
  }

  if (inserted) {
    entry.files =
        std::make_shared<LineFileTable>(LineFileTable::FromPrologue(program.value().prologue()));
  }
  if (file_index >= entry.files->size())
    Complete(program.value(), entry);
  return entry.files;
}

LineTable LineTableCache::DecodeLineTable(llvm::DWARFUnit& unit) {
  std::optional<uint64_t> offset = LineProgram::GetUnitLineTableOffset(unit);
  if (!offset)
    return LineTable();

  Entry& entry = entries_[Key(unit.getLineSection().Data.data(), *offset)];
  ErrOr<LineProgram> program = LineProgram::ParseForUnit(unit, *offset);
  if (program.has_error()) {
    entry.complete = true;
    return LineTable();
  }

  std::vector<LineRow> rows = Complete(program.value(), entry);
  return LineTable(entry.files, std::move(rows));
}

std::vector<LineRow> LineTableCache::Complete(LineProgram& program, Entry& entry) {
  // A corrupt program still gives the rows before the error. LineTable keeps the complete
  // sequences from them.
  std::vector<LineRow> rows;
  program.DecodeRows(&rows);

  // The file table changes only if the program added files, which also makes the LLVM fallback
  // replace the prologue. Otherwise keep the table that is already resolved.
  if (!entry.files || entry.files->size() != program.GetFileCount())
    entry.files = std::make_shared<LineFileTable>(LineFileTable::FromPrologue(program.prologue()));
  entry.complete = true;
  return rows;
}

}  // namespace zxdb
