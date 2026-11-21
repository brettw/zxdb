// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_PROGRAM_LLVM_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_PROGRAM_LLVM_H_

#include "src/developer/debug/zxdb/symbols/line_program.h"

namespace llvm {
class DWARFContext;
class DWARFUnit;
}  // namespace llvm

namespace zxdb {

// LLVM adaptation for the section-data-only parser. Call these functions only on the thread that
// owns the context. Unit access can extract DIEs and change LLVM caches. The returned views must
// not outlive the context's section data; workers can read those views without calling LLVM.
LineStringSections GetLineStringSections(llvm::DWARFUnit& unit);
LineStringSections GetLineStringSections(const llvm::DWARFContext& context);

// Includes the contribution offset for split DWARF. Offset 0 is valid; nullopt means no table.
std::optional<uint64_t> GetUnitLineTableOffset(llvm::DWARFUnit& unit);

ErrOr<LineProgram> ParseLineProgramForUnit(llvm::DWARFUnit& unit);
ErrOr<LineProgram> ParseLineProgramForUnit(llvm::DWARFUnit& unit, uint64_t offset);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_PROGRAM_LLVM_H_
