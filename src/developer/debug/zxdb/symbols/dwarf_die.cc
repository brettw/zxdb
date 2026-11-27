// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_die.h"

#include <cstddef>
#include <limits>
#include <memory_resource>
#include <set>
#include <type_traits>
#include <vector>

#include "src/developer/debug/zxdb/symbols/dwarf_attr.h"
#include "src/developer/debug/zxdb/symbols/dwarf_binary.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"

namespace zxdb {

bool DwarfDie::operator==(const DwarfDie& other) const {
  if (table_ == other.table_)
    return !table_ || index_ == other.index_;
  return table_ && other.table_ && table_->binary == other.table_->binary &&
         table_->unit == other.table_->unit && offset() == other.offset();
}

DwarfDie DwarfDie::die_at_index(uint64_t index) const {
  return table_ && index < table_->size() ? DwarfDie(table_, index) : DwarfDie();
}

DwarfDie DwarfDie::die_at_offset(uint64_t offset) const {
  if (table_) {
    if (auto index = table_->Find(offset))
      return DwarfDie(table_, *index);
  }
  return {};
}

DwarfDie DwarfDie::GetUnitRelativeDie(uint64_t offset) const {
  if (!table_)
    return {};
  const UnitHeader& header = unit_header();
  if (offset < header.header_size || offset >= header.next_unit_offset() - header.offset)
    return {};

  uint64_t target = header.offset + offset;
  if (DwarfDie die = die_at_offset(target); die || table_->load == Load::kAllDies)
    return die;

  auto ref = table_->section_kind == UnitSection::kInfo ? DwarfDieRef::Main(target)
                                                        : DwarfDieRef::Type(target);
  return table_->binary->GetDieAtOffset(ref, unit_is_dwo());
}

DwarfDieRef DwarfDie::ref() const {
  if (!table_)
    return {};
  return table_->section_kind == UnitSection::kInfo ? DwarfDieRef::Main(offset())
                                                    : DwarfDieRef::Type(offset());
}

DwarfTag DwarfDie::tag() const {
  using TagValue = std::underlying_type_t<DwarfTag>;
  if (!table_ || !entry().abbrev || entry().abbrev->tag > std::numeric_limits<TagValue>::max())
    return DwarfTag::kNone;
  return static_cast<DwarfTag>(entry().abbrev->tag);
}

uint64_t DwarfDie::offset() const { return entry().offset; }

const char* DwarfDie::short_name() const {
  constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName};
  return ReadName(kAttrs);
}

const char* DwarfDie::linkage_name() const {
  constexpr DwarfAttr kAttrs[] = {DwarfAttr::kMipsLinkageName, DwarfAttr::kLinkageName};
  return ReadName(kAttrs);
}

DwarfDie DwarfDie::abstract_origin() const {
  constexpr DwarfAttr kAttrs[] = {DwarfAttr::kAbstractOrigin};
  if (auto form = DwarfDieDecoder::ReadDirectAttribute(*this, kAttrs))
    return DwarfDieDecoder::DecodeReference(*table_->binary, *this, *form);
  return {};
}

const char* DwarfDie::ReadName(std::span<const DwarfAttr> attrs) const {
  if (!table_)
    return nullptr;
  const DwarfBinary& binary = *table_->binary;
  if (auto form = DwarfDieDecoder::ReadDirectAttribute(*this, attrs))
    return DwarfDieDecoder::DecodeString(binary, *this, *form).value_or(nullptr);

  // Follow chains using LLDB's reference priority at each hop. The stack visits the specification
  // first, then the abstract origin and finally the signature. Common short chains fit inline.
  using Position = std::pair<UnitIndex, uint32_t>;
  alignas(std::max_align_t) std::byte storage[512];
  std::pmr::monotonic_buffer_resource workspace(storage, sizeof(storage));
  std::pmr::set<Position> visited(&workspace);
  std::pmr::vector<DwarfDie> pending(&workspace);
  visited.emplace(unit_index(), index());
  auto push_links = [&](const DwarfDie& die) {
    for (DwarfAttr attr :
         {DwarfAttr::kSignature, DwarfAttr::kAbstractOrigin, DwarfAttr::kSpecification}) {
      auto form = DwarfDieDecoder::ReadDirectAttribute(die, std::span(&attr, 1));
      if (!form)
        continue;
      DwarfDie next = DwarfDieDecoder::DecodeReference(binary, die, *form);
      if (next)
        pending.push_back(std::move(next));
    }
  };
  push_links(*this);
  while (!pending.empty()) {
    DwarfDie die = std::move(pending.back());
    pending.pop_back();
    // A higher-priority path can reach a DIE already on the stack.
    if (!visited.emplace(die.unit_index(), die.index()).second)
      continue;
    if (auto form = DwarfDieDecoder::ReadDirectAttribute(die, attrs))
      return DwarfDieDecoder::DecodeString(binary, die, *form).value_or(nullptr);
    push_links(die);
  }
  return nullptr;
}

DwarfDie DwarfDie::parent() const {
  if (auto index = parent_index())
    return DwarfDie(table_, *index);
  return {};
}

std::optional<uint32_t> DwarfDie::parent_index() const { return parent_index_at(index_); }

std::optional<uint32_t> DwarfDie::parent_index_at(uint64_t index) const {
  if (table_) {
    if (const auto* entry = table_->Get(index); entry && entry->parent != DieTable::kNoIndex)
      return entry->parent;
  }
  return std::nullopt;
}

DwarfDie DwarfDie::first_child() const {
  if (table_) {
    if (auto index = table_->FirstChild(index_))
      return DwarfDie(table_, *index);
  }
  return {};
}

DwarfDie DwarfDie::sibling() const {
  if (!table_ || entry().sibling == DieTable::kNoIndex)
    return {};
  return DwarfDie(table_, entry().sibling);
}

std::optional<std::span<const AbbrevTable::AttributeSpec>> DwarfDie::attributes() const {
  if (!table_ || !entry().abbrev)
    return std::nullopt;
  return entry().abbrev->attributes;
}

std::span<const uint8_t> DwarfDie::encoded_entry() const {
  return table_->section().subspan(offset(), *table_->EntryEnd(index_) - offset());
}

}  // namespace zxdb
