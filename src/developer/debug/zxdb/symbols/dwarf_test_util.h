// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_TEST_UTIL_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_TEST_UTIL_H_

#include <string_view>

#include "src/developer/debug/zxdb/symbols/dwarf_unit.h"

namespace zxdb {

// A unit name usually ends with the source file name.
fxl::RefPtr<DwarfUnit> GetUnitWithNameEndingIn(DwarfBinary& binary, bool is_dwo,
                                               std::string_view name);

// Returns the first DIE with the given tag and DW_AT_name, or an invalid handle.
DwarfDie GetFirstDieOfTagAndName(const DwarfUnit& unit, DwarfTag tag, std::string_view name);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_TEST_UTIL_H_
