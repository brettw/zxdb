// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_BINARY_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_BINARY_H_

#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <string>

#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_sections.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_ref.h"
#include "src/developer/debug/zxdb/symbols/dwarf_unit.h"
#include "src/developer/debug/zxdb/symbols/line_program.h"
#include "src/developer/debug/zxdb/symbols/symbol_context.h"
#include "src/developer/debug/zxdb/symbols/unit_context_source.h"
#include "src/developer/debug/zxdb/symbols/unit_index.h"

namespace zxdb {

class DebugNamesReader;
class DwarfSymbolFactory;
class LineFileTable;
class SymbolContext;

// Represents one DWARF file or one set of in-memory sections. ModuleSymbols connects a
// main binary to the separate .dwo files that its skeleton units reference.
//
// Call on the binary's owning thread except for the raw DIE and type signature lookups below.
// Immutable section views and DIE snapshots can be read from other threads while the binary
// stays alive.
class DwarfBinary {
 public:
  // A symbol from the ELF symbol tables. It has the fields that the symbol index uses. It doesn't
  // use the ELF structure because this header must not include the system ELF header while LLVM
  // object headers are also in use. The macros in the system header break the LLVM header.
  struct ElfTableSymbol {
    enum class Type { kOther, kObject, kFunction, kTls };

    Type type = Type::kOther;

    // False for a symbol that this file references but doesn't define.
    bool is_defined = false;

    uint64_t value = 0;
    uint64_t size = 0;
  };

  virtual ~DwarfBinary() = default;

  // Getters for basic binary information.
  virtual std::string GetName() const = 0;
  virtual std::string GetBuildID() const = 0;
  virtual std::time_t GetModificationTime() const = 0;

  virtual const DwarfSymbolFactory* GetSymbolFactory() const = 0;

  // Return whether this module has been given the opportunity to include symbols from the binary
  // itself, such as PLT entries.
  virtual bool HasBinary() const = 0;

  // Returns views of this binary's DWARF sections. The views stay valid while this DwarfBinary
  // exists. A view is empty when its section is absent.
  virtual const DwarfSections& GetSections() const = 0;

  // Returns cached metadata for a unit of this binary. Call on its owning thread.
  virtual UnitContextResult GetUnitContext(UnitIndex index,
                                           UnitContextSource skeleton = {}) const = 0;

  // Use the native unit index only after checking that the DIE belongs to this binary.
  UnitContextResult GetUnitContext(const DwarfDie& die, UnitContextSource skeleton = {}) const {
    return die.belongs_to(*this) ? GetUnitContext(die.unit_index(), skeleton)
                                 : UnitContextResult(Err("Unknown DWARF unit."));
  }

  // Returns the section views and offset for the unit's line program, if present.
  virtual std::optional<LineProgramSource> GetLineProgramSource(UnitIndex index) const = 0;

  // Returns the extent of the mapped segments in memory.
  virtual uint64_t GetMappedLength() const = 0;

  // Returns symbols from the ELF File.
  virtual const std::map<std::string, ElfTableSymbol>& GetELFSymbols() const = 0;
  virtual const std::map<std::string, uint64_t> GetPLTSymbols() const = 0;

  // Allows access to compile units in this binary. There are two categories, the "normal" units in
  // .debug_info and the DWO units in .debug_info.dwo. Normally there are only one or the other,
  // but this isn't guaranteed.
  virtual uint32_t GetNormalUnitCount() const = 0;
  virtual uint32_t GetDWOUnitCount() const = 0;
  virtual fxl::RefPtr<DwarfUnit> GetUnitAtIndex(UnitIndex i) = 0;

  // Returns the DwarfUnit covering the given absolute address location. Can be null if there's
  // no unit that covers this area.
  fxl::RefPtr<DwarfUnit> UnitForAddress(const SymbolContext& symbol_context,
                                        TargetPointer absolute_address) {
    return UnitForRelativeAddress(symbol_context.AbsoluteToRelative(absolute_address));
  }

  // Like UnitForAddress but takes an address relative to the load address of the binary.
  virtual fxl::RefPtr<DwarfUnit> UnitForRelativeAddress(uint64_t relative_address) = 0;

  // Returns the 64-bit value from the .debug_addr section.
  //
  // The addr_base is an attribute on the compilation unit. This is the byte offset of the address
  // table in the .debug_addr section of that compilation unit's address table.
  //
  // The index is the address index within that compilation unit's table. This is an index into the
  // table (of address-sized entries) rather than a byte index.
  virtual std::optional<uint64_t> GetDebugAddrEntry(uint64_t addr_base, uint64_t index) const = 0;

  // Returns the main unit in a .dwo file. When using DWARF "debug fission" the main binary will
  // have "skeleton units" that reference .dwo files containing the real unit information. These
  // files will normally have exactly one unit in them. This getter returns that unit.
  //
  // It's theoretically possible for a .dwo file to have more than one unit. If that happens, it
  // would be possible to disambiguate them by name. Currently this does not happen so this just
  // returns the first DWO unit.
  fxl::RefPtr<DwarfUnit> GetDwoUnit() { return GetUnitAtIndex(UnitIndex(true, 0)); }

  // Looks up a DIE across units by its DIE reference. Both overloads can run on any thread.
  virtual DwarfDie GetDieAtOffset(DwarfDieRef die_ref) const = 0;

  // Restricts the lookup to normal or split DWARF sections.
  virtual DwarfDie GetDieAtOffset(DwarfDieRef die_ref, bool is_dwo) const = 0;

  // Looks up a DIE reference by its 64-bit DWARF type signature (DW_FORM_ref_sig8 /
  // DW_AT_signature). Can run on any thread.
  virtual DwarfDieRef GetDieRefForSignature(uint64_t signature) const = 0;

  // Looks up a type signature, preferring the source DIE's normal or split section set. Can run
  // on any thread.
  virtual DwarfDie GetDieForSignature(uint64_t signature, bool is_dwo) const = 0;

  // Returns the .debug_names reader, or nullptr if there are no readable contributions.
  virtual const DebugNamesReader* GetDebugNames() const { return nullptr; }

  // Returns the resolved file name table of the line table of the given unit of this binary, or
  // null if the unit has no line table. The table has the given file index if the line table has
  // it. Usually this parses only the line table header. See LineTableCache.
  virtual std::shared_ptr<const LineFileTable> GetLineFileTable(UnitIndex unit,
                                                                uint64_t file_index) const = 0;

  std::shared_ptr<const LineFileTable> GetLineFileTable(const DwarfDie& die,
                                                        uint64_t file_index) const {
    return die.belongs_to(*this) ? GetLineFileTable(die.unit_index(), file_index) : nullptr;
  }

  // Returns the lowest address of a code section of the binary, or 0 if it has none. Line table
  // sequences that start below it are for code that the linker removed. See
  // IsStrippedSequenceStart().
  virtual uint64_t GetFirstCodeAddress() const = 0;

  // Releases cached DIE tables, line tables, unit wrappers, unit contexts, abbreviation tables,
  // and type signature maps. Later requests rebuild them on demand.
  // Call on the context's owning thread. Existing DIE handles remain valid.
  virtual void ClearDwarfCaches() = 0;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_BINARY_H_
