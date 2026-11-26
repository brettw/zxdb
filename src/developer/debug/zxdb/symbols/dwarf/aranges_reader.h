// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_ARANGES_READER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_ARANGES_READER_H_

#include <stdint.h>

#include <span>
#include <vector>

#include "src/developer/debug/zxdb/common/address_ranges.h"
#include "src/developer/debug/zxdb/common/endian.h"
#include "src/developer/debug/zxdb/common/err_or.h"

namespace zxdb {

// Owns the ranges from one .debug_aranges set. A parsed set can be read from multiple threads.
class ArangesSet {
 public:
  // Reads a set at the given section offset. Returns an error for a bad header or missing final
  // terminator. Empty ranges and ranges with an overflowing end address are omitted.
  static ErrOr<ArangesSet> Parse(std::span<const uint8_t> section, uint64_t offset, Endian endian);

  uint64_t unit_offset() const { return unit_offset_; }
  uint64_t next_offset() const { return next_offset_; }
  std::span<const AddressRange> ranges() const { return ranges_; }

  // Some linkers emit zero tuples before the final terminator. Keep their offsets for warnings.
  std::span<const uint64_t> premature_terminators() const { return premature_terminators_; }

 private:
  ArangesSet(uint64_t unit_offset, uint64_t next_offset)
      : unit_offset_(unit_offset), next_offset_(next_offset) {}

  uint64_t unit_offset_;
  uint64_t next_offset_;
  AddressRanges::RangeVector ranges_;
  std::vector<uint64_t> premature_terminators_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_ARANGES_READER_H_
