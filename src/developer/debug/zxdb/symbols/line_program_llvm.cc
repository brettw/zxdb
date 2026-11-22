// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/line_program_llvm.h"

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"

namespace zxdb {

LineStringSections GetLineStringSections(llvm::DWARFUnit& unit) {
  LineStringSections result = GetLineStringSections(unit.getContext());
  result.str = unit.getStringSection();
  result.str_offsets = unit.getStringOffsetSection().Data;
  // This extracts the unit DIE, which has the string offsets base.
  if (const auto& contribution = unit.getStringOffsetsTableContribution()) {
    result.str_offsets_base = contribution->Base;
    result.str_offset_size = contribution->getDwarfOffsetByteSize();
  }
  return result;
}

LineStringSections GetLineStringSections(const llvm::DWARFContext& context) {
  LineStringSections result;
  result.line_str = context.getDWARFObj().getLineStrSection();
  result.str = context.getDWARFObj().getStrSection();
  return result;
}

std::optional<uint64_t> GetUnitLineTableOffset(llvm::DWARFUnit& unit) {
  llvm::DWARFDie unit_die = unit.getUnitDIE();
  if (!unit_die)
    return std::nullopt;
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kStmtList};
  DwarfDieDecoder decoder(&unit.getContext(), kAttrs);
  if (!decoder.Decode(unit_die))
    return std::nullopt;
  std::optional<uint64_t> stmt_list = decoder.GetSectionOffset(DwarfAttr::kStmtList);
  if (!stmt_list)
    return std::nullopt;
  return *stmt_list + unit.getLineTableOffset();
}

ErrOr<LineProgram> ParseLineProgramForUnit(llvm::DWARFUnit& unit) {
  std::optional<uint64_t> offset = GetUnitLineTableOffset(unit);
  if (!offset)
    return Err("The unit has no line table.");
  return ParseLineProgramForUnit(unit, *offset);
}

ErrOr<LineProgram> ParseLineProgramForUnit(llvm::DWARFUnit& unit, uint64_t offset) {
  llvm::StringRef section = unit.getLineSection().Data;
  return LineProgram::Parse(std::string_view(section.data(), section.size()), offset,
                            unit.isLittleEndian(), GetLineStringSections(unit));
}

}  // namespace zxdb
