// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DEBUG_NAMES_READER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DEBUG_NAMES_READER_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "lib/fit/function.h"
#include "src/developer/debug/zxdb/common/err_or.h"
#include "src/developer/debug/zxdb/symbols/dwarf/abbrev_table.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"
#include "src/developer/debug/zxdb/symbols/dwarf_tag.h"

namespace zxdb {

class DataReader;
struct DwarfSections;

// Reads DWARF 5 name indexes. The tables and strings borrow the section bytes. All lookups
// are read-only and can run concurrently while those bytes stay alive.
class DebugNamesReader {
 public:
  struct Entry {
    DwarfTag tag = DwarfTag::kNone;
    std::optional<uint64_t> die_unit_offset;

    // For a type unit, the related CU can identify the skeleton for a foreign TU.
    std::optional<uint64_t> related_cu_offset;
    std::optional<uint64_t> cu_offset;
    std::optional<uint64_t> local_tu_offset;
    std::optional<uint64_t> foreign_tu_signature;
    std::optional<uint64_t> type_hash;

    bool has_parent_information = false;
    std::optional<uint64_t> parent_offset;  // Relative to this index's entry pool.

   private:
    friend class DebugNamesReader;
    size_t index_ = 0;
  };

  // The spelling borrows the string section. Copy the entry to keep it after the call.
  using EntryVisitor = fit::function<void(std::string_view, const Entry&)>;
  using NameVisitor = fit::function<void(std::string_view)>;

  // For a relocatable object, returns the relocated value at a section offset of the given
  // width. Parse calls this only for CU, local TU, and string offset fields.
  using RelocateOffset = fit::function<uint64_t(uint64_t, uint8_t)>;

  // Stops at the first bad contribution and keeps the earlier indexes and the error.
  static DebugNamesReader Parse(const DwarfSections& sections, const RelocateOffset& relocate = {});

  size_t index_count() const { return indexes_.size(); }
  const Err& error() const { return error_; }

  // Visits the entry list for the exact name in each index. A bad entry ends its list.
  void LookupName(std::string_view name, const EntryVisitor& visit) const;

  // Visits the lists for all names with this stored hash. Skips indexes without a hash table.
  void LookupHash(uint32_t hash, const EntryVisitor& visit) const;

  // Visits each name that starts with the prefix, in name table order across indexes.
  // A name can occur in more than one index. An empty prefix matches nothing.
  void VisitNamesWithPrefix(std::string_view prefix, const NameVisitor& visit) const;

  // Takes an entry from this reader. Returns an empty optional if the parent is not indexed
  // or the entry has no parent information. A malformed parent record returns an error.
  ErrOr<std::optional<Entry>> ReadParent(const Entry& entry) const;

 private:
  struct Abbreviation {
    DwarfTag tag;
    std::vector<AbbrevTable::AttributeSpec> attributes;
  };

  struct NameIndex {
    FormParams forms;
    std::span<const uint8_t> cus;
    std::span<const uint8_t> local_tus;
    std::span<const uint8_t> foreign_tus;
    std::span<const uint8_t> buckets;
    std::span<const uint8_t> hashes;
    std::span<const uint8_t> strings;
    std::span<const uint8_t> entries;
    std::span<const uint8_t> pool;
    std::map<uint64_t, Abbreviation> abbreviations;
    // Sparse overrides in section offset order. Empty for linked objects.
    std::vector<std::pair<uint64_t, uint64_t>> relocations;
  };

  DebugNamesReader(std::span<const uint8_t> names, std::span<const uint8_t> strings, Endian endian)
      : names_(names), strings_(strings), endian_(endian) {}

  ErrOr<NameIndex> ReadIndex(DataReader& reader, const RelocateOffset& relocate) const;
  static Err ReadAbbreviations(DataReader& reader, NameIndex& index);
  std::optional<uint64_t> ReadTableOffset(const NameIndex& index, std::span<const uint8_t> table,
                                          uint64_t element) const;
  std::span<const uint8_t> StringBytes(const NameIndex& index, uint64_t row) const;
  void VisitEntries(size_t index, uint64_t row, std::string_view name,
                    const EntryVisitor& visit) const;
  ErrOr<std::optional<Entry>> ReadEntry(size_t index, DataReader& reader) const;
  void Lookup(std::optional<std::string_view> name, std::optional<uint32_t> hash,
              const EntryVisitor& visit) const;

  std::span<const uint8_t> names_;
  std::span<const uint8_t> strings_;
  Endian endian_;
  std::vector<NameIndex> indexes_;
  Err error_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DEBUG_NAMES_READER_H_
