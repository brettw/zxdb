// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_ROW_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_ROW_H_

#include <stdint.h>

#include <limits>

namespace zxdb {

// One row of a decoded DWARF line table.
//
// The row keeps only the registers that the debugger reads. The discriminator, ISA, and op_index
// registers are decoded but not stored.
struct LineRow {
  enum Flags : uint8_t {
    kIsStmt = 1 << 0,
    kEndSequence = 1 << 1,
    kPrologueEnd = 1 << 2,
    kEpilogueBegin = 1 << 3,
    kBasicBlock = 1 << 4,
  };

  LineRow() = default;
  LineRow(uint64_t addr, uint32_t fl, uint32_t ln, uint16_t col, uint8_t f)
      : address(addr), line(ln), file(fl), column(col), flags(f) {}

  bool is_stmt() const { return flags & kIsStmt; }
  bool end_sequence() const { return flags & kEndSequence; }
  bool prologue_end() const { return flags & kPrologueEnd; }
  bool epilogue_begin() const { return flags & kEpilogueBegin; }
  bool basic_block() const { return flags & kBasicBlock; }

  // The line for a FileLine, which uses int. The line register wraps like in LLVM, so a corrupt
  // table can give a line above the int range. Such a line gives 0, which means no line.
  int line_number() const {
    return line <= static_cast<uint32_t>(std::numeric_limits<int>::max()) ? static_cast<int>(line)
                                                                          : 0;
  }

  bool operator==(const LineRow& other) const = default;

  // Module-relative address.
  uint64_t address = 0;

  // Line 0 means the code has no source line (it is compiler-generated).
  uint32_t line = 0;

  // Raw value of the DWARF file register. See LineFileTable for how to resolve it.
  uint32_t file = 0;

  uint16_t column = 0;
  uint8_t flags = 0;
};

static_assert(sizeof(LineRow) == 24);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_ROW_H_
