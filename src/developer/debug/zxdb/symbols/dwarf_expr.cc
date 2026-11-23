// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_expr.h"

#include "src/developer/debug/zxdb/symbols/compile_unit.h"

namespace zxdb {

std::optional<uint64_t> DwarfExpr::GetAddrBase() const {
  if (!source_)
    return std::nullopt;

  // Use the unit that contains the DIE of the source. Walking the parents of the source symbol can
  // reach another unit, because the parent of a concrete instance is the context of its abstract
  // origin. Synthetic symbols don't have a DIE, so they fall back to the parents.
  fxl::RefPtr<CompileUnit> compile_unit = source_.GetCompileUnit();
  if (!compile_unit) {
    fxl::RefPtr<Symbol> symbol = source_.Get();
    if (!symbol)
      return std::nullopt;
    compile_unit = symbol->GetCompileUnit();
  }
  if (!compile_unit)
    return std::nullopt;

  return compile_unit->addr_base();
}

}  // namespace zxdb
