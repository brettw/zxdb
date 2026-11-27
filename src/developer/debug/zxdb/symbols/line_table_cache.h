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

namespace zxdb {

class LineProgram;
struct LineProgramSource;

// Caches the file tables of the line tables of one binary, so that each file table is resolved
// once. Use it on the thread that owns the binary.
//
// The header of a line table has most file entries. A DW_LNE_define_file opcode in the program can
// add more. Decoding the program is much slower than parsing the header, so the cache first keeps
// the header's table. It decodes the program only when a caller asks for an entry past the
// header's entries, or when it decodes the whole table for DecodeLineTable(). After that the
// cached file table is complete.
class LineTableCache {
 public:
  // Returns a file table of the unit's line table that has the given file index if the table has
  // that index at all. Returns null if its header can't be parsed.
  // Each result is an immutable snapshot. A later request can replace the cached snapshot with
  // one that includes program-defined files; earlier results remain valid.
  std::shared_ptr<const LineFileTable> GetFileTable(const LineProgramSource& source,
                                                    uint64_t file_index);

  // Decodes the whole line table of the unit. The result uses the cached file table. Returns an
  // empty table if its header can't be parsed. first_code_address is
  // the lowest address of a code section of the binary, which finds the stripped sequences (see
  // LineTable).
  LineTable DecodeLineTable(const LineProgramSource& source, uint64_t first_code_address);

  void Clear() { entries_.clear(); }

 private:
  struct Entry {
    std::shared_ptr<const LineFileTable> files;

    // Set after a decode attempt. No more files can be obtained, even if parsing failed.
    bool exhausted = false;
  };

  // Identifies a line table. Units of a main binary and DWO units use different line sections.
  using Key = std::pair<const void*, uint64_t>;

  // Decodes the parsed program and makes the entry complete. Returns the rows.
  std::vector<LineRow> Complete(const LineProgram& program, Entry& entry);

  std::map<Key, Entry> entries_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_TABLE_CACHE_H_
