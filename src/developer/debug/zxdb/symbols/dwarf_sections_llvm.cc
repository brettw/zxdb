// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_sections_llvm.h"

#include "llvm/DebugInfo/DWARF/DWARFObject.h"
#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"

namespace zxdb {

namespace {

using SectionVisitor = llvm::function_ref<void(const llvm::DWARFSection&)>;
using VisitSections = void (llvm::DWARFObject::*)(SectionVisitor) const;

std::span<const uint8_t> GetSingleSection(const llvm::DWARFObject& object,
                                          VisitSections visit_sections) {
  std::span<const uint8_t> result;
  size_t count = 0;
  (object.*visit_sections)([&](const llvm::DWARFSection& section) {
    if (++count == 1)
      result = AsBytes(section.Data);
  });
  return count == 1 ? result : std::span<const uint8_t>();
}

}  // namespace

DwarfSections GetDwarfSections(const llvm::DWARFObject& object) {
  return {
      .info = GetSingleSection(object, &llvm::DWARFObject::forEachInfoSections),
      .abbrev = AsBytes(object.getAbbrevSection()),
      .str = AsBytes(object.getStrSection()),
      .str_offsets = AsBytes(object.getStrOffsetsSection().Data),
      .line = AsBytes(object.getLineSection().Data),
      .line_str = AsBytes(object.getLineStrSection()),
      .addr = AsBytes(object.getAddrSection().Data),
      .ranges = AsBytes(object.getRangesSection().Data),
      .rnglists = AsBytes(object.getRnglistsSection().Data),
      .loc = AsBytes(object.getLocSection().Data),
      .loclists = AsBytes(object.getLoclistsSection().Data),
      .aranges = AsBytes(object.getArangesSection()),
      .names = AsBytes(object.getNamesSection().Data),
      .types = GetSingleSection(object, &llvm::DWARFObject::forEachTypesSections),
      .info_dwo = GetSingleSection(object, &llvm::DWARFObject::forEachInfoDWOSections),
      .abbrev_dwo = AsBytes(object.getAbbrevDWOSection()),
      .str_dwo = AsBytes(object.getStrDWOSection()),
      .str_offsets_dwo = AsBytes(object.getStrOffsetsDWOSection().Data),
      .line_dwo = AsBytes(object.getLineDWOSection().Data),
      .loc_dwo = AsBytes(object.getLocDWOSection().Data),
      .loclists_dwo = AsBytes(object.getLoclistsDWOSection().Data),
      .rnglists_dwo = AsBytes(object.getRnglistsDWOSection().Data),
      .types_dwo = GetSingleSection(object, &llvm::DWARFObject::forEachTypesDWOSections),
      .cu_index = AsBytes(object.getCUIndexSection()),
      .tu_index = AsBytes(object.getTUIndexSection()),
      .endian = object.isLittleEndian() ? Endian::kLittle : Endian::kBig,
  };
}

}  // namespace zxdb
