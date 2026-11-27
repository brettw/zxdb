// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DIE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DIE_H_

#include <lib/syslog/cpp/macros.h>
#include <stdint.h>

#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <utility>

#include "src/developer/debug/zxdb/symbols/dwarf/die_table.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_ref.h"
#include "src/developer/debug/zxdb/symbols/dwarf_tag.h"
#include "src/developer/debug/zxdb/symbols/unit_index.h"

namespace zxdb {

class DwarfBinary;
class DwarfBinaryImpl;
enum class DwarfAttr : int;

// Keeps an immutable DIE table alive through cache eviction. The owning binary must stay alive
// while reading the encoded attributes. A root-only handle keeps its original view of the tree
// even after the binary loads all entries.
class DwarfDie {
 public:
  using Load = DieTable::Load;

  class ChildIterator;
  class UnitIterator;

  DwarfDie() = default;

  bool is_valid() const { return table_ != nullptr; }
  explicit operator bool() const { return is_valid(); }
  bool operator==(const DwarfDie& other) const;

  bool belongs_to(const DwarfBinary& binary) const { return table_ && table_->binary == &binary; }
  UnitIndex unit_index() const {
    FX_DCHECK(is_valid());
    return table_->unit;
  }
  bool unit_is_dwo() const { return unit_index().is_dwo; }
  const UnitHeader& unit_header() const {
    FX_DCHECK(is_valid());
    return table_->header();
  }
  DwarfDie unit_root() const { return die_at_index(0); }
  DwarfDie die_at_index(uint64_t index) const;
  DwarfDie die_at_offset(uint64_t offset) const;
  uint64_t unit_die_count() const { return table_ ? table_->size() : 0; }
  uint32_t index() const { return index_; }

  // Walks the entries in this handle's snapshot from its root, including null entries.
  // Copy an entry to keep it after advancing or destroying the iterator.
  auto unit_dies() const;

  // Resolves a unit-relative offset in the handle's table or loads the full tree if needed.
  DwarfDie GetUnitRelativeDie(uint64_t offset) const;

  DwarfDieRef ref() const;
  DwarfTag tag() const;
  uint64_t offset() const;  // Requires a valid DIE.

  // Names borrow section bytes. Call on the binary's owning thread to resolve string forms.
  const char* short_name() const;
  const char* linkage_name() const;

  // Resolves this DIE's direct DW_AT_abstract_origin reference, if present.
  DwarfDie abstract_origin() const;

  DwarfDie parent() const;
  std::optional<uint32_t> parent_index() const;
  std::optional<uint32_t> parent_index_at(uint64_t index) const;
  DwarfDie first_child() const;
  DwarfDie sibling() const;

  // The child range excludes the null entry that ends a sibling chain.
  auto children() const;

  // A null entry has no abbreviation or attributes.
  std::optional<std::span<const AbbrevTable::AttributeSpec>> attributes() const;

 private:
  friend class DwarfBinaryImpl;
  friend class DwarfDieDecoder;

  struct UnitTable : DieTable {
    UnitTable(const DwarfBinary* binary, UnitIndex unit, UnitSection section, DieTable table,
              Endian endian, Load load)
        : DieTable(std::move(table)),
          binary(binary),
          unit(unit),
          section_kind(section),
          endian(endian),
          load(load) {}

    const DwarfBinary* binary;
    UnitIndex unit;
    UnitSection section_kind;
    Endian endian;
    Load load;
  };

  DwarfDie(std::shared_ptr<const UnitTable> table, uint32_t index)
      : table_(std::move(table)), index_(index) {}

  const DieTable::Entry& entry() const {
    FX_DCHECK(is_valid());
    return *table_->Get(index_);
  }

  const char* ReadName(std::span<const DwarfAttr> attrs) const;
  std::span<const uint8_t> encoded_entry() const;

  std::shared_ptr<const UnitTable> table_;
  uint32_t index_ = 0;
};

class DwarfDie::UnitIterator {
 public:
  using iterator_concept = std::input_iterator_tag;
  using value_type = DwarfDie;
  using difference_type = std::ptrdiff_t;

  const DwarfDie& operator*() const {
    FX_DCHECK(remaining_);
    return die_;
  }
  UnitIterator& operator++() {
    FX_DCHECK(remaining_);
    if (--remaining_)
      ++die_.index_;
    return *this;
  }
  void operator++(int) { ++*this; }
  bool operator==(std::default_sentinel_t) const { return remaining_ == 0; }

 private:
  friend class DwarfDie;

  explicit UnitIterator(DwarfDie root) : die_(std::move(root)), remaining_(die_.unit_die_count()) {}

  DwarfDie die_;
  uint64_t remaining_;
};

inline auto DwarfDie::unit_dies() const {
  return std::ranges::subrange(UnitIterator(unit_root()), std::default_sentinel);
}

class DwarfDie::ChildIterator {
 public:
  using iterator_category = std::input_iterator_tag;
  using value_type = DwarfDie;
  using difference_type = std::ptrdiff_t;

  ChildIterator() = default;
  explicit ChildIterator(DwarfDie die)
      : die_(die && die.entry().abbrev ? std::move(die) : DwarfDie()) {}

  const DwarfDie& operator*() const { return die_; }
  ChildIterator& operator++() {
    uint32_t sibling = die_.entry().sibling;
    if (sibling != DieTable::kNoIndex && die_.table_->Get(sibling)->abbrev)
      die_.index_ = sibling;
    else
      die_ = {};
    return *this;
  }
  ChildIterator operator++(int) {
    auto previous = *this;
    ++*this;
    return previous;
  }
  bool operator==(const ChildIterator&) const = default;

 private:
  DwarfDie die_;
};

inline auto DwarfDie::children() const {
  return std::ranges::subrange(ChildIterator(first_child()), ChildIterator());
}

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DIE_H_
