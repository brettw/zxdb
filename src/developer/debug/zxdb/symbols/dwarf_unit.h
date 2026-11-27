// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_H_

#include <string>

#include "src/developer/debug/zxdb/symbols/arch.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_ref.h"
#include "src/developer/debug/zxdb/symbols/symbol_context.h"
#include "src/lib/fxl/memory/ref_counted.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace zxdb {

class DwarfBinary;
class LazySymbol;
class LineTable;

// Represents a DWARF unit in the binary file. The primary purpose of this class is to allow
// mocking the LLVM libraries. It corresponds to what we need from llvm::DWARFUnit and is consumed
// primarily by ModuleSymbolsImpl which provides the high-level symbol interface.
//
// This is a toplevel DWARF concept that contains all the different data types associated with the
// compilation unit like line tables and abbreviation tables. The main thing it contains is the
// Debug Information Entry (DIE) tree for the unit. This tree uses a root of DW_TAG_compilation_unit
// and is represented in our system by the CompileUnit class.
//
// These are similarly named so can be confusing, but this class is the higher-level construct.
// Usually when something ion DWARF is "relative to the compilation unit" it means this class and
// not the CompileUnit DIE.
class DwarfUnit : public fxl::RefCountedThreadSafe<DwarfUnit> {
 public:
  // Creates a weak pointer to this class. The units can get removed when modules or process are
  // unloaded so if you need to keep a pointer, either keep a weak ptr or an owning refptr.
  fxl::WeakPtr<DwarfUnit> GetWeakPtr() const { return weak_factory_.GetWeakPtr(); }

  // Returns the binary this unit is a member of. May return null if the binary has been destroyed
  // out from under the reference-counted unit pointer.
  virtual DwarfBinary* GetBinary() const = 0;

  // Returns the DWARF specification version of this unit (e.g. "5").
  virtual int GetDwarfVersion() const = 0;

  // By default, load the full tree so callers can walk the children of the root.
  virtual DwarfDie GetUnitDie(DwarfDie::Load load = DwarfDie::Load::kAllDies) const = 0;

  // Loads the full tree once and walks its entries in section order, including null entries.
  // Copy an entry to keep it after advancing or destroying the iterator.
  auto GetDies() const { return GetUnitDie().unit_dies(); }

  // Returns the innermost function covering the given module-relative address, if any.
  // Returns an invalid LazySymbol on failure.
  virtual LazySymbol FunctionForRelativeAddress(uint64_t relative_address) const = 0;

  // Returns this unit's index within the normal or split unit list.
  virtual UnitIndex GetUnitIndex() const = 0;

  // Returns the offset of the beginning of this unit within its DWARF section.
  virtual uint64_t GetOffset() const = 0;

  // The compilation directory is what the compiler decides to write. In normal usage this will be
  // an absolute directory on the current computer. In the Fuchsia in-tree build this will be
  // relative.
  virtual std::string GetCompilationDir() const = 0;

  // The line table maps addresses to line numbers. Returns an empty table if the unit has no line
  // table or if the header can't be parsed.
  virtual const LineTable& GetLineTable() const = 0;

  // Returns the number of DIEs in this unit.
  virtual uint64_t GetDieCount() const = 0;

  // Releases the unit's cached DIE table. Existing handles keep their snapshot alive.
  virtual void ClearDieCache() const = 0;

  // Looks up a DIE in this unit by index or byte offset.
  //
  // The byte offset is the absolute offset within the entire binary, but it must be a member of
  // this unit. See DwarfBinary::GetDieAtOffset() to query across units.
  // An out-of-range index returns an empty handle.
  virtual DwarfDie GetDieAtIndex(uint64_t index) const = 0;
  virtual DwarfDie GetDieAtOffset(DwarfDieRef die_ref) const = 0;

  // Returns the array index if the DIE belongs to this unit.
  virtual std::optional<uint32_t> GetIndexForDie(const DwarfDie& die) const = 0;

  // Returns a reference to the unit header or to an offset in the unit's section.
  DwarfDieRef GetDieRef() const { return GetDieRef(GetOffset()); }
  virtual DwarfDieRef GetDieRef(uint64_t offset) const = 0;

 protected:
  FRIEND_REF_COUNTED_THREAD_SAFE(DwarfUnit);

  DwarfUnit() : weak_factory_(this) {}
  virtual ~DwarfUnit() = default;

  // Backs GetWeakPtr().
  mutable fxl::WeakPtrFactory<DwarfUnit> weak_factory_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_H_
