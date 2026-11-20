// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_TABLE_CACHE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_TABLE_CACHE_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <utility>

#include "src/developer/debug/zxdb/symbols/line_file_table.h"
#include "src/developer/debug/zxdb/symbols/line_table.h"

namespace llvm {
class DWARFUnit;
}  // namespace llvm

namespace zxdb {

class LineProgram;

// Caches the file tables of the line tables of one binary, so that each file table is resolved
// once. Use it only on the thread that owns the binary's DWARFContext.
//
// The header of a line table has most file entries. A DW_LNE_define_file opcode in the program can
// add more. Decoding the program is much slower than parsing the header, so the cache first keeps
// the header's table. It decodes the program only when a caller asks for an entry past the
// header's entries, or when it decodes the whole table for DecodeLineTable(). After that the
// cached file table is complete.
class LineTableCache {
 public:
  // Returns a file table of the unit's line table that has the given file index if the table has
  // that index at all. Returns null if the unit has no line table or its header can't be parsed.
  std::shared_ptr<const LineFileTable> GetFileTable(llvm::DWARFUnit& unit, uint64_t file_index);

  // Decodes the whole line table of the unit. The result uses the cached file table. Returns an
  // empty table if the unit has no line table or its header can't be parsed.
  LineTable DecodeLineTable(llvm::DWARFUnit& unit);

  void Clear() { entries_.clear(); }

 private:
  struct Entry {
    std::shared_ptr<const LineFileTable> files;

    // Set when the program was decoded, so the file table has the files that the program adds.
    bool complete = false;
  };

  // Identifies a line table. Units of a main binary and DWO units use different line sections.
  using Key = std::pair<const void*, uint64_t>;

  // Decodes the parsed program and makes the entry complete. Returns the rows.
  std::vector<LineRow> Complete(LineProgram& program, Entry& entry);

  std::map<Key, Entry> entries_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_TABLE_CACHE_H_
