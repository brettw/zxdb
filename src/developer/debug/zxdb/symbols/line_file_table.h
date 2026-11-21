// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_FILE_TABLE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_FILE_TABLE_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "src/developer/debug/zxdb/symbols/line_program.h"

namespace zxdb {

// Resolves one entry of a line table's file name table. Returns the normalized name relative to
// the compilation directory, or nullopt if the index has no file or its name can't be decoded.
//
// The result is the same as llvm::DWARFDebugLine::Prologue::getFileNameByIndex() with a relative
// path kind, then NormalizePath(). All file names that the symbol code computes from line tables
// come from this function, so file names from line rows, DW_AT_decl_file, and the file index always
// compare equal.
std::optional<std::string> ResolveLineFileName(LineFileEntries files, uint64_t file_index);

// The resolved file name table of one line table.
//
// The table is indexed by the raw value of the DWARF file register. DWARF 5 file indices start at
// 0. DWARF 2 to 4 file indices start at 1, so entry 0 of this table is empty for them. An empty
// name means that the entry has no file. That is entry 0 for DWARF 2 to 4, and entries whose name
// can't be decoded.
class LineFileTable {
 public:
  LineFileTable() = default;
  explicit LineFileTable(std::vector<std::string> names) : names_(std::move(names)) {}

  // Resolves all entries of the effective file table.
  static LineFileTable FromEntries(LineFileEntries files);

  // Valid raw file register values are in [0, size()).
  size_t size() const { return names_.size(); }

  // Returns null if the index is out of range or has no name. The pointer is valid for the
  // lifetime of this object.
  const std::string* GetFileName(uint64_t file_index) const {
    if (file_index >= names_.size() || names_[file_index].empty())
      return nullptr;
    return &names_[file_index];
  }

 private:
  std::vector<std::string> names_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_FILE_TABLE_H_
