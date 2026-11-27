// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_die.h"

namespace zxdb {

DwarfDie DwarfDie::FromUnitRoot(llvm::DWARFUnit* unit, Load load) {
  return unit ? DwarfDie(unit->getUnitDIE(load == Load::kRootOnly)) : DwarfDie();
}

DwarfDie DwarfDie::FromSubroutineAddress(llvm::DWARFUnit& unit, uint64_t relative_address) {
  return DwarfDie(unit.getSubroutineForAddress(relative_address));
}

DwarfDie DwarfDie::unit_root() const { return FromUnitRoot(die_.getDwarfUnit()); }

DwarfDie DwarfDie::GetUnitRelativeDie(uint64_t offset) const {
  if (!is_valid())
    return {};

  auto* unit = die_.getDwarfUnit();
  // Like LLDB, reject references outside the DIE data. Check before adding the offset so
  // malformed values cannot wrap into the unit.
  uint64_t unit_offset = unit->getOffset();
  uint64_t unit_end = unit->getNextUnitOffset();
  if (unit_end <= unit_offset || offset < unit->getHeaderSize() || offset >= unit_end - unit_offset)
    return {};
  return FromOffset(unit, unit_offset + offset);
}

const char* DwarfDie::short_name() const { return die_.getShortName(); }

const char* DwarfDie::linkage_name() const { return die_.getLinkageName(); }

}  // namespace zxdb
