// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/line_table_test_support.h"

#include "src/developer/debug/zxdb/symbols/lazy_symbol.h"

namespace zxdb {

LineRow MakeStatementRow(uint64_t address, uint32_t file, uint32_t line) {
  return LineRow(address, file, line, 0, LineRow::kIsStmt);
}

LineRow MakeNonStatementRow(uint64_t address, uint32_t file, uint32_t line) {
  return LineRow(address, file, line, 0, 0);
}

LineRow MakePrologueEndRow(uint64_t address, uint32_t file, uint32_t line) {
  return LineRow(address, file, line, 0, LineRow::kIsStmt | LineRow::kPrologueEnd);
}

LineRow MakeEndSequenceRow(uint64_t address, uint32_t file, uint32_t line) {
  return LineRow(address, file, line, 0, LineRow::kIsStmt | LineRow::kEndSequence);
}

LineTable MakeLineTable(std::vector<std::string> files, std::vector<LineRow> rows) {
  files.insert(files.begin(), std::string());  // File 0 has no name in DWARF 4.
  return LineTable(std::make_shared<LineFileTable>(std::move(files)), std::move(rows));
}

FunctionForAddress MockFunctionMap::GetCallback() const {
  return [this](uint64_t address) -> LazySymbol {
    if (auto found = functions_.find(address); found != functions_.end())
      return found->second->GetLazySymbol();
    return LazySymbol();
  };
}

}  // namespace zxdb
