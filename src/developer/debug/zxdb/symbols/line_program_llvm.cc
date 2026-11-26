// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/line_program_llvm.h"

#include <lib/syslog/cpp/macros.h>

#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"
#include "src/developer/debug/zxdb/symbols/dwarf_sections_llvm.h"

namespace zxdb {

LineStringSections GetLineStringSections(const DwarfSections& sections, llvm::DWARFUnit& unit) {
  LineStringSections result;
  // DWO units also use the main .debug_line_str section.
  result.line_str = AsStringView(sections.line_str);
  result.str = AsStringView(GetUnitStrSection(sections, unit));
  result.str_offsets = AsStringView(GetUnitStrOffsetsSection(sections, unit));
  if (unit.getVersion() < 5 && !unit.isDWOUnit())
    return result;
  if (auto context = ReadUnitContext(sections, unit)) {
    if (auto contribution = context.value().string_offsets()) {
      result.str_offsets = result.str_offsets.substr(0, contribution->end);
      result.str_offsets_base = contribution->base;
      result.str_offset_size = contribution->entry_size;
    }
  }
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

ErrOr<LineProgram> ParseLineProgramForUnit(const DwarfSections& sections, llvm::DWARFUnit& unit) {
  std::optional<uint64_t> offset = GetUnitLineTableOffset(unit);
  if (!offset)
    return Err("The unit has no line table.");
  return ParseLineProgramForUnit(sections, unit, *offset);
}

ErrOr<LineProgram> ParseLineProgramForUnit(const DwarfSections& sections, llvm::DWARFUnit& unit,
                                           uint64_t offset) {
  FX_DCHECK(sections.little_endian == unit.isLittleEndian());
  return LineProgram::Parse(AsStringView(GetUnitLineSection(sections, unit)), offset,
                            sections.little_endian, GetLineStringSections(sections, unit));
}

}  // namespace zxdb
