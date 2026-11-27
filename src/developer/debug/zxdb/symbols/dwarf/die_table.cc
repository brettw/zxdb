// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/die_table.h"

#include <inttypes.h>

#include <algorithm>
#include <utility>

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"

namespace zxdb {

DieTable::DieTable(std::span<const uint8_t> section, const UnitHeader& header,
                   std::shared_ptr<const AbbrevTable> abbrevs)
    : section_(section), header_(header), abbrevs_(std::move(abbrevs)) {}

DieTable DieTable::Parse(std::span<const uint8_t> section, const UnitHeader& header, Endian endian,
                         std::shared_ptr<const AbbrevTable> abbrevs, Load load) {
  DieTable table(section, header, std::move(abbrevs));
  auto set_error = [&](uint64_t offset, const char* reason) {
    table.error_ = Err("DWARF DIE at offset 0x%" PRIx64 " in unit at 0x%" PRIx64 ": %s", offset,
                       header.offset, reason);
  };

  const uint64_t length_size = header.offset_size == 8 ? 12 : 4;
  if (header.offset > section.size() || length_size > section.size() - header.offset ||
      header.length > section.size() - header.offset - length_size ||
      header.header_size < length_size || header.header_size - length_size > header.length) {
    set_error(header.offset, "invalid unit bounds");
    return table;
  }

  DataReader reader(section, endian);
  reader.set_limit(header.next_unit_offset());
  reader.Seek(header.die_offset());
  const FormParams params = header.form_params();

  Index parent = kNoIndex;
  Index previous_sibling = kNoIndex;
  for (;;) {
    const uint64_t offset = reader.offset();
    if (reader.remaining() == 0) {
      set_error(offset, table.entries_.empty() ? "missing root DIE" : "unterminated child list");
      break;
    }
    const uint64_t code = reader.Uleb();
    if (!reader.ok()) {
      set_error(offset, "invalid abbreviation code");
      break;
    }

    const AbbrevTable::Declaration* abbrev = nullptr;
    if (code != 0) {
      abbrev = table.abbrevs_ ? table.abbrevs_->Find(code) : nullptr;
      if (!abbrev) {
        set_error(offset, "unknown abbreviation code");
        break;
      }
      // LLVM 19 can skip past the unit end. Keep a DIE only after all its attributes fit.
      if (!abbrev->SkipAttributes(params, reader)) {
        set_error(offset, "invalid or truncated attributes");
        break;
      }
    }
    if (table.entries_.size() == kNoIndex) {
      set_error(offset, "too many DIEs");
      break;
    }

    // DW_AT_sibling is an optional reference. The tree structure comes from the children flags
    // and null entries, so an incorrect reference can't hide entries or change the tree.
    const Index index = static_cast<Index>(table.entries_.size());
    if (previous_sibling != kNoIndex)
      table.entries_[previous_sibling].sibling = index;
    // Grow from observed entries and limit spare capacity in large cached units.
    if (index >= 64 && index == table.entries_.capacity())
      table.entries_.reserve(std::min<uint64_t>(uint64_t{index} + index / 2, kNoIndex));
    table.entries_.push_back({offset, abbrev, parent, kNoIndex});
    table.last_entry_end_ = reader.offset();
    if (load == Load::kRootOnly)
      break;

    if (abbrev && abbrev->has_children) {
      parent = index;
      previous_sibling = kNoIndex;
      continue;
    }
    if (parent == kNoIndex)
      break;  // A root without children has no child list.

    if (abbrev) {
      previous_sibling = index;
    } else {
      // The parent link lets the scan ascend even at great depth. The parent of the
      // closed list is the last sibling at the outer level.
      previous_sibling = parent;
      parent = table.entries_[parent].parent;
      if (parent == kNoIndex)
        break;  // The root's child list has closed.
    }
  }
  return table;
}

std::optional<DieTable::Index> DieTable::Find(uint64_t offset) const {
  auto found = std::ranges::lower_bound(entries_, offset, {}, &Entry::offset);
  if (found == entries_.end() || found->offset != offset)
    return std::nullopt;
  return static_cast<Index>(found - entries_.begin());
}

std::optional<DieTable::Index> DieTable::FirstChild(Index index) const {
  const uint64_t child_index = static_cast<uint64_t>(index) + 1;
  const auto* child = Get(child_index);
  if (!child || child->parent != index)
    return std::nullopt;
  return static_cast<Index>(child_index);
}

std::optional<uint64_t> DieTable::EntryEnd(Index index) const {
  if (index >= entries_.size())
    return std::nullopt;
  if (const auto* next = Get(static_cast<uint64_t>(index) + 1))
    return next->offset;
  return last_entry_end_;
}

}  // namespace zxdb
