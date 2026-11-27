// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DIE_TABLE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DIE_TABLE_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/symbols/dwarf/abbrev_table.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"

namespace zxdb {

// The DIEs of one unit in section order, including null entries that end child lists. The table
// owns its entries and shares the abbreviations. The caller keeps the section bytes alive.
class DieTable {
 public:
  enum class Load { kRootOnly, kAllDies };

  using Index = uint32_t;
  static constexpr Index kNoIndex = UINT32_MAX;

  struct Entry {
    uint64_t offset;                         // Relative to the section start.
    const AbbrevTable::Declaration* abbrev;  // Null for a child list terminator.
    Index parent;
    Index sibling;
  };

  // Takes a parsed header and the corresponding abbreviation table. On malformed DIE data,
  // retains complete entries and records the first error. A root-only parse reads one entry.
  static DieTable Parse(std::span<const uint8_t> section, const UnitHeader& header, Endian endian,
                        std::shared_ptr<const AbbrevTable> abbrevs, Load load = Load::kAllDies);

  const UnitHeader& header() const { return header_; }
  std::span<const uint8_t> section() const { return section_; }
  const Err& error() const { return error_; }

  size_t size() const { return entries_.size(); }
  const Entry* Get(uint64_t index) const {
    return index < entries_.size() ? &entries_[index] : nullptr;
  }

  // Finds an exact entry start, including a null entry.
  std::optional<Index> Find(uint64_t offset) const;

  // The first child can be the null entry for an empty child list.
  std::optional<Index> FirstChild(Index index) const;

  // Exclusive end offset of this entry's bytes, before any children.
  std::optional<uint64_t> EntryEnd(Index index) const;

 private:
  DieTable(std::span<const uint8_t> section, const UnitHeader& header,
           std::shared_ptr<const AbbrevTable> abbrevs);

  std::span<const uint8_t> section_;
  UnitHeader header_;
  std::shared_ptr<const AbbrevTable> abbrevs_;
  std::vector<Entry> entries_;
  uint64_t last_entry_end_ = 0;
  Err error_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DIE_TABLE_H_
