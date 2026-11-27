// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_unit.h"

#include "src/developer/debug/zxdb/symbols/dwarf_binary.h"
#include "src/developer/debug/zxdb/symbols/dwarf_symbol_factory.h"
#include "src/developer/debug/zxdb/symbols/lazy_symbol.h"

namespace zxdb {

DwarfUnit::DwarfUnit(DwarfBinary* binary, UnitIndex index, UnitSection section,
                     const UnitHeader& header)
    : weak_factory_(this),
      binary_(binary->GetWeakPtr()),
      index_(index),
      section_(section),
      header_(header) {}

DwarfBinary* DwarfUnit::GetBinary() const { return binary_.get(); }

int DwarfUnit::GetDwarfVersion() const { return header_.version; }

DwarfDie DwarfUnit::GetUnitDie(DwarfDie::Load load) const {
  return binary_ ? binary_->GetDieAtIndex(index_, 0, load) : DwarfDie();
}

LazySymbol DwarfUnit::FunctionForRelativeAddress(uint64_t relative_address) const {
  if (!binary_)
    return {};

  const auto* factory = binary_->GetSymbolFactory();
  if (!factory)
    return {};

  if (!function_address_map_)
    function_address_map_.emplace(GetUnitDie(), *binary_.get(), factory->GetSkeletonUnit());
  if (auto offset = function_address_map_->Lookup(relative_address))
    return factory->MakeLazy(GetDieRef(*offset));
  return {};
}

uint64_t DwarfUnit::GetOffset() const { return header_.offset; }

std::string DwarfUnit::GetCompilationDir() const {
  if (!binary_)
    return std::string();

  auto context = binary_->GetUnitContext(index_);
  return context ? std::string(context.value()->compilation_dir().value_or(std::string_view()))
                 : std::string();
}

const LineTable& DwarfUnit::GetLineTable() const {
  if (!line_table_) {
    if (binary_)
      line_table_.emplace(binary_->DecodeLineTable(index_));
    else
      line_table_.emplace();
  }
  return *line_table_;
}

uint64_t DwarfUnit::GetDieCount() const { return GetUnitDie().unit_die_count(); }

void DwarfUnit::ClearDieCache() const {
  if (binary_)
    binary_->ClearDieCache(index_);
}

DwarfDie DwarfUnit::GetDieAtIndex(uint64_t index) const {
  return binary_ ? binary_->GetDieAtIndex(index_, index, DwarfDie::Load::kAllDies) : DwarfDie();
}

DwarfDie DwarfUnit::GetDieAtOffset(DwarfDieRef die_ref) const {
  return binary_ && die_ref && die_ref.section() == GetDieRef(GetOffset()).section()
             ? binary_->GetDieAtOffset(index_, die_ref.offset())
             : DwarfDie();
}

std::optional<uint32_t> DwarfUnit::GetIndexForDie(const DwarfDie& die) const {
  return binary_ && die.belongs_to(*binary_) && die.unit_index() == index_
             ? std::optional(die.index())
             : std::nullopt;
}

DwarfDieRef DwarfUnit::GetDieRef(uint64_t offset) const {
  return section_ == UnitSection::kInfo ? DwarfDieRef::Main(offset) : DwarfDieRef::Type(offset);
}

}  // namespace zxdb
