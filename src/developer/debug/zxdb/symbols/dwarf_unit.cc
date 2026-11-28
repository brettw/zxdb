// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_unit.h"

#include "src/developer/debug/zxdb/symbols/dwarf_binary.h"
#include "src/developer/debug/zxdb/symbols/lazy_symbol.h"
#include "src/developer/debug/zxdb/symbols/line_table.h"

namespace zxdb {

DwarfUnit::DwarfUnit(DwarfBinary& binary, UnitIndex index)
    : binary_(binary.GetWeakPtr()), index_(index) {}

DwarfBinary* DwarfUnit::GetBinary() const { return binary_.get(); }

DwarfDie DwarfUnit::GetUnitDie(DwarfDie::Load load) const {
  if (auto* binary = GetBinary())
    return binary->GetDieAtIndex(index_, 0, load);
  return {};
}

LazySymbol DwarfUnit::FunctionForRelativeAddress(uint64_t relative_address) const {
  if (auto* binary = GetBinary())
    return binary->FunctionForRelativeAddress(index_, relative_address);
  return {};
}

std::optional<uint64_t> DwarfUnit::GetOffset() const {
  if (DwarfDieRef ref = GetDieRef())
    return ref.offset();
  return std::nullopt;
}

std::string DwarfUnit::GetCompilationDir() const {
  if (auto* binary = GetBinary()) {
    if (auto context = binary->GetUnitContext(index_))
      return std::string(context.value()->compilation_dir().value_or(std::string_view()));
  }
  return {};
}

const LineTable& DwarfUnit::GetLineTable() const {
  if (auto* binary = GetBinary())
    return binary->GetLineTable(index_);
  static const auto& empty = *new LineTable();
  return empty;
}

uint64_t DwarfUnit::GetDieCount() const { return GetUnitDie().unit_die_count(); }

void DwarfUnit::ClearDieCache() const {
  if (auto* binary = GetBinary())
    binary->ClearDieCache(index_);
}

DwarfDie DwarfUnit::GetDieAtIndex(uint64_t index) const {
  if (auto* binary = GetBinary())
    return binary->GetDieAtIndex(index_, index, DwarfDie::Load::kAllDies);
  return {};
}

DwarfDie DwarfUnit::GetDieAtOffset(DwarfDieRef die_ref) const {
  if (auto* binary = GetBinary(); binary && die_ref) {
    if (die_ref.section() == binary->GetUnitRef(index_).section())
      return binary->GetDieAtOffset(index_, die_ref.offset());
  }
  return {};
}

std::optional<uint32_t> DwarfUnit::GetIndexForDie(const DwarfDie& die) const {
  if (auto* binary = GetBinary(); binary && die.belongs_to(*binary) && die.unit_index() == index_)
    return die.index();
  return std::nullopt;
}

DwarfDieRef DwarfUnit::GetDieRef() const {
  if (auto* binary = GetBinary())
    return binary->GetUnitRef(index_);
  return {};
}

DwarfDieRef DwarfUnit::GetDieRef(uint64_t offset) const {
  if (auto* binary = GetBinary())
    return binary->GetUnitRef(index_, offset);
  return {};
}

}  // namespace zxdb
