// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_H_

#include <optional>
#include <string>

#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_ref.h"
#include "src/developer/debug/zxdb/symbols/function_address_map.h"
#include "src/developer/debug/zxdb/symbols/line_table.h"
#include "src/developer/debug/zxdb/symbols/unit_index.h"
#include "src/lib/fxl/memory/ref_counted.h"
#include "src/lib/fxl/memory/ref_ptr.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace zxdb {

class DwarfBinary;
class LazySymbol;

// Represents a DWARF unit and provides access to its DIEs and line table.
// CompileUnit is the decoded symbol for a compilation unit's root DIE.
class DwarfUnit final : public fxl::RefCountedThreadSafe<DwarfUnit> {
 public:
  // Creates a weak pointer to this class. The units can get removed when modules or process are
  // unloaded so if you need to keep a pointer, either keep a weak ptr or an owning refptr.
  fxl::WeakPtr<DwarfUnit> GetWeakPtr() const { return weak_factory_.GetWeakPtr(); }

  // Returns the binary this unit is a member of. May return null if the binary has been destroyed
  // out from under the reference-counted unit pointer.
  DwarfBinary* GetBinary() const;

  // Returns the DWARF specification version of this unit (e.g. "5").
  int GetDwarfVersion() const;

  // By default, load the full tree so callers can walk the children of the root.
  DwarfDie GetUnitDie(DwarfDie::Load load = DwarfDie::Load::kAllDies) const;

  // Loads the full tree once and walks its entries in section order, including null entries.
  // Copy an entry to keep it after advancing or destroying the iterator.
  auto GetDies() const { return GetUnitDie().unit_dies(); }

  // Returns the innermost function covering the given module-relative address, if any.
  // Returns an invalid LazySymbol on failure.
  LazySymbol FunctionForRelativeAddress(uint64_t relative_address) const;

  // Returns this unit's index within the normal or split unit list.
  UnitIndex GetUnitIndex() const { return index_; }

  // Returns the offset of the beginning of this unit within its DWARF section.
  uint64_t GetOffset() const;

  // The compilation directory is what the compiler decides to write. In normal usage this will be
  // an absolute directory on the current computer. In the Fuchsia in-tree build this will be
  // relative.
  std::string GetCompilationDir() const;

  // The line table maps addresses to line numbers. Returns an empty table if the unit has no line
  // table or if the header can't be parsed.
  const LineTable& GetLineTable() const;

  // Returns the number of DIEs in this unit.
  uint64_t GetDieCount() const;

  // Releases the unit's cached DIE table. Existing handles keep their snapshot alive.
  void ClearDieCache() const;

  // Looks up a DIE in this unit by index or byte offset.
  //
  // The byte offset is the absolute offset within the entire binary, but it must be a member of
  // this unit. See DwarfBinary::GetDieAtOffset() to query across units.
  // An out-of-range index returns an empty handle.
  DwarfDie GetDieAtIndex(uint64_t index) const;
  DwarfDie GetDieAtOffset(DwarfDieRef die_ref) const;

  // Returns the array index if the DIE belongs to this unit.
  std::optional<uint32_t> GetIndexForDie(const DwarfDie& die) const;

  // Returns a reference to the unit header or to an offset in the unit's section.
  DwarfDieRef GetDieRef() const { return GetDieRef(GetOffset()); }
  DwarfDieRef GetDieRef(uint64_t offset) const;

 private:
  FRIEND_REF_COUNTED_THREAD_SAFE(DwarfUnit);
  FRIEND_MAKE_REF_COUNTED(DwarfUnit);

  DwarfUnit(DwarfBinary* binary, UnitIndex index, UnitSection section, const UnitHeader& header);
  ~DwarfUnit() = default;

  // Backs GetWeakPtr().
  mutable fxl::WeakPtrFactory<DwarfUnit> weak_factory_;

  // The binary that owns us.
  fxl::WeakPtr<DwarfBinary> binary_;

  const UnitIndex index_;
  const UnitSection section_;
  const UnitHeader header_;

  // Built on the first address lookup. Entries store DIE offsets so the DIE table can be evicted.
  mutable std::optional<FunctionAddressMap> function_address_map_;

  // The line table. Computed lazily.
  mutable std::optional<LineTable> line_table_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_H_
