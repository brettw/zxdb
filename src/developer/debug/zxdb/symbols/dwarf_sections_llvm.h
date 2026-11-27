// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_SECTIONS_LLVM_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_SECTIONS_LLVM_H_

#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_sections.h"

namespace llvm {
class DWARFObject;
}  // namespace llvm

namespace zxdb {

// Converts the object loader's sections to views for the byte readers.
DwarfSections GetDwarfSections(const llvm::DWARFObject& object);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_SECTIONS_LLVM_H_
