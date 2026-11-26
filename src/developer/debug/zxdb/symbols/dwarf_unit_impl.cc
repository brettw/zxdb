// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_unit_impl.h"

#include <lib/syslog/cpp/macros.h>

#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "src/developer/debug/zxdb/common/ref_ptr_to.h"
#include "src/developer/debug/zxdb/symbols/address_range_map_builder.h"
#include "src/developer/debug/zxdb/symbols/dwarf_binary_impl.h"
#include "src/developer/debug/zxdb/symbols/dwarf_sections_llvm.h"
#include "src/developer/debug/zxdb/symbols/dwarf_tag.h"
#include "src/developer/debug/zxdb/symbols/function.h"
#include "src/developer/debug/zxdb/symbols/lazy_symbol.h"

namespace zxdb {

DwarfUnitImpl::DwarfUnitImpl(DwarfBinaryImpl* binary, llvm::DWARFUnit* unit,
                             const UnitHeader& header)
    : binary_(binary->GetWeakPtr()), unit_(unit), header_(header) {}

DwarfBinary* DwarfUnitImpl::GetBinary() const { return binary_.get(); }

int DwarfUnitImpl::GetDwarfVersion() const { return header_.version; }

DwarfDie DwarfUnitImpl::GetUnitDie(DwarfDie::Load load) const {
  return DwarfDie::FromUnitRoot(unit_, load);
}

LazySymbol DwarfUnitImpl::FunctionForRelativeAddress(uint64_t relative_address) const {
  if (!binary_) {
    return LazySymbol();
  }

  // Only create the FuncAddrMap when we're actually dealing with DWO files. This operation is quite
  // costly in addition to creating the LLVM DIE index. Note the FuncAddrMap will work in all cases,
  // it is purely an optimization to skip creating it for non-DWO binaries. The overhead may be
  // avoided by either not indexing the LLVM cache, or by skipping the creation of this mapping. We
  // are currently choosing to take the second of those options to support DWO while keeping the
  // non-DWO case fast.
  if (unit_->isDWOUnit()) {
    EnsureFuncAddrMap();
    uint64_t offset = func_addr_to_die_offset_.Lookup(relative_address);
    if (offset)
      return binary_->GetSymbolFactory()->MakeLazy(GetDieRef(offset));
  } else {
    if (DwarfDie die = DwarfDie::FromSubroutineAddress(*unit_, relative_address))
      return binary_->GetSymbolFactory()->MakeLazy(GetDieRef(die.offset()));
  }

  return LazySymbol();
}

uint64_t DwarfUnitImpl::GetOffset() const { return header_.offset; }

std::string DwarfUnitImpl::GetCompilationDir() const {
  if (!binary_)
    return std::string();

  auto context = binary_->GetUnitContext(*unit_);
  return context ? std::string(context.value()->compilation_dir().value_or(std::string_view()))
                 : std::string();
}

const LineTable& DwarfUnitImpl::GetLineTable() const {
  if (!line_table_) {
    if (binary_)
      line_table_.emplace(binary_->DecodeLineTable(unit_));
    else
      line_table_.emplace();
  }
  return *line_table_;
}

uint64_t DwarfUnitImpl::GetDieCount() const { return unit_->getNumDIEs(); }

void DwarfUnitImpl::ClearDieCache() const { unit_->clear(); }

DwarfDie DwarfUnitImpl::GetDieAtIndex(uint64_t index) const {
  return DwarfDie::FromIndex(*unit_, index);
}

DwarfDie DwarfUnitImpl::GetDieAtOffset(DwarfDieRef die_ref) const {
  if (!die_ref.is_valid())
    return DwarfDie();
  return DwarfDie::FromOffset(unit_, die_ref.offset());
}

std::optional<uint32_t> DwarfUnitImpl::GetIndexForDie(DwarfDie die) const {
  return die.index_in(*unit_);
}

DwarfDieRef DwarfUnitImpl::GetDieRef(uint64_t offset) const {
  if (header_.is_type_unit())
    return DwarfDieRef::ForTypeUnit(header_.version, offset);
  return DwarfDieRef::Main(offset);
}

void DwarfUnitImpl::EnsureFuncAddrMap() const {
  if (binary_ && func_addr_to_die_offset_.empty()) {
    // Recursively collect all DIEs and their begin ranges.
    AddressRangeMapBuilder builder;
    AddDieToFuncAddr(*binary_, GetUnitDie(), builder);
    func_addr_to_die_offset_ = builder.GetMap();
  }
}

void DwarfUnitImpl::AddDieToFuncAddr(const DwarfBinary& binary, DwarfDie die,
                                     AddressRangeMapBuilder& builder) const {
  // Add all functions and inlines to the map.
  DwarfTag tag = die.tag();
  if (tag == DwarfTag::kSubprogram || tag == DwarfTag::kInlinedSubroutine) {
    auto lazy_func = binary.GetSymbolFactory()->MakeLazy(GetDieRef(die.offset()));
    if (const Function* func = lazy_func.Get()->As<Function>()) {
      builder.AddRanges(func->code_ranges(), die.offset());
    }
  }

  // Recurse into children.
  for (DwarfDie child = die.first_child(); child; child = child.sibling())
    AddDieToFuncAddr(binary, child, builder);
}

}  // namespace zxdb
