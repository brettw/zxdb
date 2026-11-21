// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_SEQUENCE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_SEQUENCE_H_

#include <stdint.h>

namespace zxdb {

// Returns true when a line sequence starting at the given address is for code that the linker
// removed. first_code_address is the lowest address of a code section of the binary (see
// GetFirstCodeAddress()). Linkers give removed code an address of 0, or 0 plus an offset into the
// removed section, which is below every code section. This matches LLDB, which drops the sequences
// that start below the first code address (ConvertLineTable() in SymbolFileDWARF.cpp).
//
// A tombstone address of all ones isn't checked here, because the decoder doesn't emit rows while
// the address is a tombstone.
inline bool IsStrippedSequenceStart(uint64_t address, uint64_t first_code_address) {
  return address < first_code_address;
}

// Validates a sequence without storing its rows. Both the file scan and LineTable use this
// state, so they agree on malformed and stripped sequences. A complete sequence can have zero
// extent; it remains useful for file/line searches but cannot cover an address.
//
// This is stricter than LLDB, which keeps a sequence whose addresses decrease. The address lookup
// does a binary search on the rows of a sequence, so it can't give correct results for those rows.
// See "Known differences from LLDB" in line_tables.md.
class LineSequenceValidator {
 public:
  void AddAddress(uint64_t address) {
    if (empty_) {
      first_ = address;
      empty_ = false;
    } else if (address < last_) {
      ordered_ = false;
    }
    last_ = address;
  }

  // Call only for an end_sequence row, after AddAddress(). An unfinished sequence is discarded
  // by the caller. Reset here so a bad sequence cannot affect the next one.
  bool Finish(uint64_t first_code_address) {
    bool live = !empty_ && ordered_ && !IsStrippedSequenceStart(first_, first_code_address);
    *this = LineSequenceValidator();
    return live;
  }

 private:
  uint64_t first_ = 0;
  uint64_t last_ = 0;
  bool empty_ = true;
  bool ordered_ = true;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_SEQUENCE_H_
