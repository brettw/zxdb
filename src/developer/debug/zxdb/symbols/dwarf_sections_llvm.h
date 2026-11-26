// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_SECTIONS_LLVM_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_SECTIONS_LLVM_H_

#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_sections.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_context.h"

namespace llvm {
class DWARFContext;
class DWARFObject;
class DWARFUnit;
}  // namespace llvm

namespace zxdb {

// This is the adapter from LLVM objects to DwarfSections. Phase 3 removes it.
DwarfSections GetDwarfSections(const llvm::DWARFObject& object);
DwarfSections GetDwarfSections(const llvm::DWARFContext& context);

// LLVM gives normal units the main sections and DWO units the corresponding .dwo sections.
std::span<const uint8_t> GetUnitLineSection(const DwarfSections& sections,
                                            const llvm::DWARFUnit& unit);
std::span<const uint8_t> GetUnitStrSection(const DwarfSections& sections,
                                           const llvm::DWARFUnit& unit);
std::span<const uint8_t> GetUnitStrOffsetsSection(const DwarfSections& sections,
                                                  const llvm::DWARFUnit& unit);

// Uses LLVM's section identity, DWP index, and split-unit link. The byte reader decodes
// the unit header and root attributes. Call on the LLVM context thread.
ErrOr<UnitContext> ReadUnitContext(const DwarfSections& sections, llvm::DWARFUnit& unit,
                                   llvm::DWARFUnit* skeleton = nullptr);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_SECTIONS_LLVM_H_
