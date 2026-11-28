// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_H_

#include <optional>
#include <string>

#include "src/developer/debug/zxdb/symbols/dwarf_die.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_ref.h"
#include "src/developer/debug/zxdb/symbols/unit_index.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace zxdb {

class DwarfBinary;
class LazySymbol;
class LineTable;

// An index into a DWARF binary. Copying a unit shares the binary's caches.
// Use on the binary's owning thread. Lookups return empty results after the binary is destroyed.
class DwarfUnit {
 public:
  // Returns the binary this unit belongs to, or null after it has been destroyed.
  DwarfBinary* GetBinary() const;

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

  // Returns the offset of the beginning of this unit within its DWARF section, if still loaded.
  std::optional<uint64_t> GetOffset() const;

  // The compilation directory is what the compiler decides to write. In normal usage this will be
  // an absolute directory on the current computer. In the Fuchsia in-tree build this will be
  // relative.
  std::string GetCompilationDir() const;

  // The line table maps addresses to line numbers. Returns an empty table if the unit has no line
  // table or if the header can't be parsed. The reference is valid until ClearDwarfCaches() or
  // destruction of the binary.
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
  DwarfDieRef GetDieRef() const;
  DwarfDieRef GetDieRef(uint64_t offset) const;

 private:
  friend class DwarfBinary;

  DwarfUnit(DwarfBinary& binary, UnitIndex index);

  fxl::WeakPtr<DwarfBinary> binary_;
  UnitIndex index_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_H_
