// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_test_util.h"

#include "src/developer/debug/zxdb/symbols/dwarf_binary.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"

namespace zxdb {

namespace {

constexpr DwarfAttr kNameAttr[] = {DwarfAttr::kName};

std::string_view GetDieName(DwarfDieDecoder& decoder, DwarfDie die) {
  if (die && decoder.Decode(die))
    return decoder.GetCString(DwarfAttr::kName).value_or("");
  return {};
}

}  // namespace

fxl::RefPtr<DwarfUnit> GetUnitWithNameEndingIn(DwarfBinary& binary, bool is_dwo,
                                               std::string_view name) {
  DwarfDieDecoder decoder(binary, kNameAttr);
  uint32_t count = is_dwo ? binary.GetDWOUnitCount() : binary.GetNormalUnitCount();
  for (uint32_t i = 0; i < count; ++i) {
    auto unit = binary.GetUnitAtIndex(UnitIndex(is_dwo, i));
    if (unit && GetDieName(decoder, unit->GetUnitDie(DwarfDie::Load::kRootOnly)).ends_with(name))
      return unit;
  }
  return nullptr;
}

DwarfDie GetFirstDieOfTagAndName(const DwarfUnit& unit, DwarfTag tag, std::string_view name) {
  DwarfDieDecoder decoder(*unit.GetBinary(), kNameAttr);
  for (uint64_t i = 0, count = unit.GetDieCount(); i < count; ++i) {
    DwarfDie die = unit.GetDieAtIndex(i);
    if (die.tag() == tag && GetDieName(decoder, die) == name)
      return die;
  }
  return {};
}

}  // namespace zxdb
