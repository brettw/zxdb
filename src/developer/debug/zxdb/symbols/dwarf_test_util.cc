// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_test_util.h"

#include "src/developer/debug/shared/string_util.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"

namespace zxdb {

namespace {

// Returns the DW_AT_Name for the given DIE or the empty string on failure.
std::string GetDIEName(llvm::DWARFContext* context, const llvm::DWARFDie& die) {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName};
  DwarfDieDecoder decoder(context, kAttrs);
  if (!decoder.Decode(die))
    return std::string();
  std::optional<const char*> name = decoder.GetCString(DwarfAttr::kName);
  return name ? std::string(*name) : std::string();
}

}  // namespace

llvm::DWARFUnit* GetUnitWithNameEndingIn(llvm::DWARFContext* context, llvm::DWARFUnitVector& units,
                                         const std::string& name) {
  for (unsigned i = 0; i < units.size(); i++) {
    llvm::DWARFUnit* unit = units[i].get();
    std::string unit_name = GetDIEName(context, unit->getUnitDIE());
    if (debug::StringEndsWith(unit_name, name))
      return unit;
  }
  return nullptr;
}

llvm::DWARFDie GetFirstDieOfTagAndName(llvm::DWARFContext* context, llvm::DWARFUnit* unit,
                                       DwarfTag tag, const std::string& name) {
  for (unsigned i = 0; i < unit->getNumDIEs(); i++) {
    llvm::DWARFDie die = unit->getDIEAtIndex(i);
    if (static_cast<DwarfTag>(die.getTag()) == tag) {
      if (GetDIEName(context, die) == name)
        return die;
    }
  }
  return llvm::DWARFDie();
}

}  // namespace zxdb
