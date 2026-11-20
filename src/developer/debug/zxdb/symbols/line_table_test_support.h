// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_TABLE_TEST_SUPPORT_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_TABLE_TEST_SUPPORT_H_

#include <map>
#include <string>
#include <vector>

#include "src/developer/debug/zxdb/symbols/find_line.h"
#include "src/developer/debug/zxdb/symbols/line_table.h"
#include "src/developer/debug/zxdb/symbols/symbol.h"

namespace zxdb {

// Helpers to construct line table rows. The file is the raw file register value. The rows have no
// flags except:
//   - is_stmt for MakeStatementRow(), MakePrologueEndRow(), and MakeEndSequenceRow().
//   - prologue_end for MakePrologueEndRow().
//   - end_sequence for MakeEndSequenceRow().
LineRow MakeStatementRow(uint64_t address, uint32_t file, uint32_t line);
LineRow MakeNonStatementRow(uint64_t address, uint32_t file, uint32_t line);
LineRow MakePrologueEndRow(uint64_t address, uint32_t file, uint32_t line);
LineRow MakeEndSequenceRow(uint64_t address, uint32_t file, uint32_t line);

// Makes a line table with DWARF 4 file numbering: files[0] is the name of file 1.
LineTable MakeLineTable(std::vector<std::string> files, std::vector<LineRow> rows);

// Maps row addresses to functions for GetAllLineTableMatchesInUnit().
class MockFunctionMap {
 public:
  void SetForRow(const LineRow& row, fxl::RefPtr<Symbol> symbol) {
    functions_[row.address] = std::move(symbol);
  }

  // The returned callback references this object.
  FunctionForAddress GetCallback() const;

 private:
  std::map<uint64_t, fxl::RefPtr<Symbol>> functions_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_TABLE_TEST_SUPPORT_H_
