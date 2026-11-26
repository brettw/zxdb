// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_PROGRAM_LLVM_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_PROGRAM_LLVM_H_

#include "src/developer/debug/zxdb/symbols/line_program.h"

namespace llvm {
class DWARFUnit;
}  // namespace llvm

namespace zxdb {

class DwarfBinary;

// Supplies the unit's line table offset and string offsets contribution to the line parser.
// Call these functions on the thread that owns the LLVM context. Unit access can extract DIEs
// and change LLVM caches. Workers can read the resulting views while the section bytes are alive.
LineStringSections GetLineStringSections(const DwarfSections& sections, llvm::DWARFUnit& unit,
                                         const DwarfBinary* binary);

// Includes the contribution offset for split DWARF. Offset 0 is valid; nullopt means no table.
std::optional<uint64_t> GetUnitLineTableOffset(llvm::DWARFUnit& unit);

// Test helper for a standalone LLVM context. Reads the unit's line table offset.
ErrOr<LineProgram> ParseLineProgramForUnit(const DwarfSections& sections, llvm::DWARFUnit& unit);
ErrOr<LineProgram> ParseLineProgramForUnit(const DwarfSections& sections, llvm::DWARFUnit& unit,
                                           uint64_t offset, const DwarfBinary* binary);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_PROGRAM_LLVM_H_
