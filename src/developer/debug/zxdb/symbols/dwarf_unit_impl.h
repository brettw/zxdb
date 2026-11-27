// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_IMPL_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_IMPL_H_

#include <optional>

#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"
#include "src/developer/debug/zxdb/symbols/dwarf_unit.h"
#include "src/developer/debug/zxdb/symbols/function_address_map.h"
#include "src/developer/debug/zxdb/symbols/line_table.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace zxdb {

class DwarfBinaryImpl;

class DwarfUnitImpl : public DwarfUnit {
 public:
  // Construct with fxl::MakeRefCounted<DwarfUnitImpl>().

  // DwarfUnit implementation.
  DwarfBinary* GetBinary() const override;
  int GetDwarfVersion() const override;
  DwarfDie GetUnitDie(DwarfDie::Load load = DwarfDie::Load::kAllDies) const override;
  UnitIndex GetUnitIndex() const override { return index_; }
  uint64_t GetOffset() const override;
  LazySymbol FunctionForRelativeAddress(uint64_t relative_address) const override;
  std::string GetCompilationDir() const override;
  const LineTable& GetLineTable() const override;
  uint64_t GetDieCount() const override;
  void ClearDieCache() const override;
  DwarfDie GetDieAtIndex(uint64_t index) const override;
  DwarfDie GetDieAtOffset(DwarfDieRef die_ref) const override;
  std::optional<uint32_t> GetIndexForDie(const DwarfDie& die) const override;
  DwarfDieRef GetDieRef(uint64_t offset) const override;

 private:
  FRIEND_REF_COUNTED_THREAD_SAFE(DwarfUnitImpl);
  FRIEND_MAKE_REF_COUNTED(DwarfUnitImpl);

  DwarfUnitImpl(DwarfBinaryImpl* binary, UnitIndex index, UnitSection section,
                const UnitHeader& header);

  // The binary that owns us.
  fxl::WeakPtr<DwarfBinaryImpl> binary_;

  const UnitIndex index_;
  const UnitSection section_;
  const UnitHeader header_;

  // Built on the first address lookup. Entries store DIE offsets so the DIE table can be evicted.
  mutable std::optional<FunctionAddressMap> function_address_map_;

  // The line table. Computed lazily.
  mutable std::optional<LineTable> line_table_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_IMPL_H_
