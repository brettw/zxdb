// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/function_address_map.h"

#include <algorithm>
#include <iterator>
#include <queue>
#include <utility>

#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"
#include "src/developer/debug/zxdb/symbols/dwarf_tag.h"

namespace zxdb {

namespace {

bool HasCodeRangeAttributes(const DwarfDie& die) {
  auto attrs = die.attributes();
  return attrs && std::ranges::any_of(*attrs, [](const AbbrevTable::AttributeSpec& spec) {
           return spec.attribute == static_cast<uint64_t>(DwarfAttr::kLowPc) ||
                  spec.attribute == static_cast<uint64_t>(DwarfAttr::kRanges);
         });
}

}  // namespace

FunctionAddressMap::FunctionAddressMap(DwarfDie die, const DwarfBinary& binary,
                                       UnitContextSource skeleton) {
  if (!die)
    return;

  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kLowPc, DwarfAttr::kHighPc, DwarfAttr::kRanges,
                                         DwarfAttr::kDeclaration};
  DwarfDieDecoder decoder(binary, kAttrs, skeleton);
  // Only a concrete DIE's own attributes describe its code.
  decoder.set_follow_abstract_origins(false);
  decoder.set_follow_specifications(false);

  std::vector<Entry> ranges;
  for (uint32_t i = 0, count = die.unit_die_count(); i < count; ++i) {
    die.SetIndexUnchecked(i);
    DwarfTag tag = die.tag();
    if (tag != DwarfTag::kSubprogram && tag != DwarfTag::kInlinedSubroutine)
      continue;
    // Abstract and declaration DIEs usually have no code range attributes.
    if (!HasCodeRangeAttributes(die))
      continue;
    if (!decoder.Decode(die) || decoder.GetBool(DwarfAttr::kDeclaration).value_or(false))
      continue;
    for (const AddressRange& range : decoder.GetCodeRanges()) {
      if (!range.empty())
        ranges.push_back({range, die.offset()});
    }
  }
  ResolveOverlaps(std::move(ranges));
}

void FunctionAddressMap::ResolveOverlaps(std::vector<Entry> input) {
  // LLVM 19 also inserts ranges in DIE order, but its split assumes that a new range fits
  // inside the previous one. The sweep preserves the tails of unrelated crossing ranges.
  std::ranges::sort(input, {}, [](const Entry& entry) { return entry.range.begin(); });
  auto order = [](const Entry* left, const Entry* right) { return left->offset < right->offset; };
  std::priority_queue<const Entry*, std::vector<const Entry*>, decltype(order)> active(order);

  // The highest-priority range stays the same until the next range starts or the winner ends.
  // Lower-priority expired ranges can stay in the queue until they reach the top.
  auto next = input.cbegin();
  uint64_t address = 0;
  while (next != input.cend() || !active.empty()) {
    while (!active.empty() && active.top()->range.end() <= address)
      active.pop();
    if (active.empty()) {
      if (next == input.cend())
        break;
      address = next->range.begin();
    }
    while (next != input.cend() && next->range.begin() <= address) {
      active.push(&*next);
      ++next;
    }

    const Entry& winner = *active.top();
    uint64_t end = winner.range.end();
    if (next != input.cend())
      end = std::min(end, next->range.begin());
    if (!entries_.empty() && entries_.back().range.end() == address &&
        entries_.back().offset == winner.offset) {
      entries_.back().range = AddressRange(entries_.back().range.begin(), end);
    } else {
      entries_.push_back({AddressRange(address, end), winner.offset});
    }
    address = end;
  }
}

std::optional<uint64_t> FunctionAddressMap::Lookup(uint64_t relative_address) const {
  auto after = std::ranges::upper_bound(entries_, relative_address, {},
                                        [](const Entry& entry) { return entry.range.begin(); });
  if (after == entries_.begin())
    return std::nullopt;
  const Entry& entry = *std::prev(after);
  if (entry.range.InRange(relative_address))
    return entry.offset;
  return std::nullopt;
}

}  // namespace zxdb
