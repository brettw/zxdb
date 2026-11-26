// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_RANGE_LIST_READER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_RANGE_LIST_READER_H_

#include <stdint.h>

#include "src/developer/debug/zxdb/common/address_ranges.h"
#include "src/developer/debug/zxdb/common/err_or.h"

namespace zxdb {

class UnitContext;

// Reads one range list using immutable unit and section data. Results are in list order and can
// overlap. Empty and reversed ranges are ignored. Malformed lists return an error.
class RangeListReader {
 public:
  // Takes a DW_AT_ranges offset in .debug_ranges or .debug_rnglists. For a DWARF 5 package,
  // this offset is relative to the start of the unit's contribution.
  static ErrOr<AddressRanges::RangeVector> ReadFromOffset(const UnitContext& unit, uint64_t offset);

  // Takes a DW_FORM_rnglistx index in the DWARF 5 offset array.
  static ErrOr<AddressRanges::RangeVector> ReadFromIndex(const UnitContext& unit, uint64_t index);

  // Reads the root DIE's low/high PC pair or range list.
  static ErrOr<AddressRanges::RangeVector> ReadUnitRanges(const UnitContext& unit);
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_RANGE_LIST_READER_H_
