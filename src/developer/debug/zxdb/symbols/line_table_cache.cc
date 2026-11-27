// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/line_table_cache.h"

#include "src/developer/debug/zxdb/symbols/line_program.h"

namespace zxdb {

std::shared_ptr<const LineFileTable> LineTableCache::GetFileTable(const LineProgramSource& source,
                                                                  uint64_t file_index) {
  auto [iter, inserted] = entries_.try_emplace(Key(source.section.data(), source.offset));
  Entry& entry = iter->second;
  bool needs_more = !entry.exhausted && entry.files && file_index >= entry.files->size();
  if (!inserted && !needs_more)
    return entry.files;

  ErrOr<LineProgram> program = source.Parse();
  if (program.has_error()) {
    entry.exhausted = true;  // Don't try again.
    return entry.files;
  }

  if (inserted) {
    entry.files =
        std::make_shared<LineFileTable>(LineFileTable::FromEntries(program.value().header_files()));
  }
  if (file_index >= entry.files->size())
    Complete(program.value(), entry);
  return entry.files;
}

LineTable LineTableCache::DecodeLineTable(const LineProgramSource& source,
                                          uint64_t first_code_address) {
  Entry& entry = entries_[Key(source.section.data(), source.offset)];
  ErrOr<LineProgram> program = source.Parse();
  if (program.has_error()) {
    entry.exhausted = true;
    return LineTable();
  }

  std::vector<LineRow> rows = Complete(program.value(), entry);
  return LineTable(entry.files, std::move(rows), first_code_address);
}

std::vector<LineRow> LineTableCache::Complete(const LineProgram& program, Entry& entry) {
  // A corrupt program still gives the rows before the error. LineTable keeps the complete
  // sequences from them.
  LineProgram::DecodeResult decoded = program.DecodeRows();
  LineFileEntries files = program.files(decoded.defined_files);

  // The file table changes only if the program added files with DW_LNE_define_file. Otherwise keep
  // the table that is already resolved.
  if (!entry.files || entry.files->size() != files.size())
    entry.files = std::make_shared<LineFileTable>(LineFileTable::FromEntries(files));
  entry.exhausted = true;
  return std::move(decoded.rows);
}

}  // namespace zxdb
