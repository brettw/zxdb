// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_BINARY_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_BINARY_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/common/err_or.h"
#include "src/developer/debug/zxdb/symbols/arch.h"
#include "src/developer/debug/zxdb/symbols/dwarf/debug_names_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_sections.h"
#include "src/developer/debug/zxdb/symbols/dwarf/package_index.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_ref.h"
#include "src/developer/debug/zxdb/symbols/dwarf_symbol_factory.h"
#include "src/developer/debug/zxdb/symbols/line_program.h"
#include "src/developer/debug/zxdb/symbols/line_table_cache.h"
#include "src/developer/debug/zxdb/symbols/unit_context_cache.h"
#include "src/developer/debug/zxdb/symbols/unit_context_source.h"
#include "src/developer/debug/zxdb/symbols/unit_index.h"
#include "src/lib/fxl/memory/ref_ptr.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace elflib {
class ElfLib;
}  // namespace elflib

namespace zxdb {

class DwarfUnit;
class LineFileTable;
class LineTable;
class SymbolContext;

// Represents one DWARF file or one set of in-memory sections. ModuleSymbols connects a
// main binary to the separate .dwo files that its skeleton units reference.
//
// Call on the binary's owning thread except for the raw DIE and type signature lookups below.
// Immutable section views and DIE snapshots can be read from other threads while the binary
// stays alive.
class DwarfBinary final {
 public:
  // The fields from an ELF symbol table entry that the symbol index uses.
  struct ElfTableSymbol {
    enum class Type { kOther, kObject, kFunction, kTls };

    Type type = Type::kOther;

    // False for a symbol that this file references but doesn't define.
    bool is_defined = false;

    uint64_t value = 0;
    uint64_t size = 0;
  };

  // Callers must call Load() to complete initialization (which can fail).
  DwarfBinary(const std::string& name, const std::string& binary_name, const std::string& build_id);
  // For byte fixtures. The section buffers must outlive the binary and its readers.
  explicit DwarfBinary(DwarfSections sections);
  ~DwarfBinary();

  fxl::WeakPtr<DwarfBinary> GetWeakPtr();

  Err Load(fxl::WeakPtr<DwarfSymbolFactory::Delegate> delegate,
           DwarfSymbolFactory::FileType file_type);

  // Getters for basic binary information.
  std::string GetName() const;
  std::string GetBuildID() const;
  std::time_t GetModificationTime() const;

  const DwarfSymbolFactory* GetSymbolFactory() const;

  // Return whether this module has been given the opportunity to include symbols from the binary
  // itself, such as PLT entries.
  bool HasBinary() const;

  // Returns views of this binary's DWARF sections. The views stay valid while this DwarfBinary
  // exists. A view is empty when its section is absent.
  const DwarfSections& GetSections() const;

  // Returns cached metadata for a unit of this binary. Call on its owning thread.
  UnitContextResult GetUnitContext(UnitIndex index, UnitContextSource skeleton = {}) const;

  // Use the native unit index only after checking that the DIE belongs to this binary.
  UnitContextResult GetUnitContext(const DwarfDie& die, UnitContextSource skeleton = {}) const {
    return die.belongs_to(*this) ? GetUnitContext(die.unit_index(), skeleton)
                                 : UnitContextResult(Err("Unknown DWARF unit."));
  }

  // Returns the section views and offset for the unit's line program, if present.
  std::optional<LineProgramSource> GetLineProgramSource(UnitIndex index) const;

  // Returns the extent of the mapped segments in memory.
  uint64_t GetMappedLength() const;

  // Returns symbols from the ELF File.
  const std::map<std::string, ElfTableSymbol>& GetELFSymbols() const;
  const std::map<std::string, uint64_t> GetPLTSymbols() const;

  // Allows access to compile units in this binary. There are two categories, the "normal" units in
  // .debug_info and the DWO units in .debug_info.dwo. Normally there are only one or the other,
  // but this isn't guaranteed.
  uint32_t GetNormalUnitCount() const;
  uint32_t GetDWOUnitCount() const;
  fxl::RefPtr<DwarfUnit> GetUnitAtIndex(UnitIndex i);

  // Returns the DwarfUnit covering the given absolute address location. Can be null if there's
  // no unit that covers this area.
  fxl::RefPtr<DwarfUnit> UnitForAddress(const SymbolContext& symbol_context,
                                        TargetPointer absolute_address);

  // Like UnitForAddress but takes an address relative to the load address of the binary.
  fxl::RefPtr<DwarfUnit> UnitForRelativeAddress(uint64_t relative_address);

  // Returns the 64-bit value from the .debug_addr section.
  //
  // The addr_base is an attribute on the compilation unit. This is the byte offset of the address
  // table in the .debug_addr section of that compilation unit's address table.
  //
  // The index is the address index within that compilation unit's table. This is an index into the
  // table (of address-sized entries) rather than a byte index.
  std::optional<uint64_t> GetDebugAddrEntry(uint64_t addr_base, uint64_t index) const;

  // Returns the main unit in a .dwo file. When using DWARF "debug fission" the main binary will
  // have "skeleton units" that reference .dwo files containing the real unit information. These
  // files will normally have exactly one unit in them. This getter returns that unit.
  //
  // It's theoretically possible for a .dwo file to have more than one unit. If that happens, it
  // would be possible to disambiguate them by name. Currently this does not happen so this just
  // returns the first DWO unit.
  fxl::RefPtr<DwarfUnit> GetDwoUnit();

  // Looks up a DIE across units by its DIE reference. Both overloads can run on any thread.
  DwarfDie GetDieAtOffset(DwarfDieRef die_ref) const;

  // Restricts the lookup to normal or split DWARF sections.
  DwarfDie GetDieAtOffset(DwarfDieRef die_ref, bool is_dwo) const;

  // Looks up a DIE reference by its 64-bit DWARF type signature (DW_FORM_ref_sig8 /
  // DW_AT_signature). Can run on any thread.
  DwarfDieRef GetDieRefForSignature(uint64_t signature) const;

  // Looks up a type signature, preferring the source DIE's normal or split section set. Can run
  // on any thread.
  DwarfDie GetDieForSignature(uint64_t signature, bool is_dwo) const;

  // Returns the .debug_names reader, or nullptr if there are no readable contributions.
  const DebugNamesReader* GetDebugNames() const;

  // Returns the resolved file name table of the line table of the given unit of this binary, or
  // null if the unit has no line table. The table has the given file index if the line table has
  // it. Usually this parses only the line table header. See LineTableCache.
  std::shared_ptr<const LineFileTable> GetLineFileTable(UnitIndex unit, uint64_t file_index) const;

  std::shared_ptr<const LineFileTable> GetLineFileTable(const DwarfDie& die,
                                                        uint64_t file_index) const {
    return die.belongs_to(*this) ? GetLineFileTable(die.unit_index(), file_index) : nullptr;
  }

  // Returns the lowest address of a code section of the binary, or 0 if it has none. Line table
  // sequences that start below it are for code that the linker removed. See
  // IsStrippedSequenceStart().
  uint64_t GetFirstCodeAddress() const { return first_code_address_; }

  // Releases cached DIE tables, line tables, unit wrappers, unit contexts, abbreviation tables,
  // and type signature maps. Later requests rebuild them on demand.
  // Call on the context's owning thread. Existing DIE handles remain valid.
  void ClearDwarfCaches();

  // Decodes the whole line table of the unit. The file table is shared with GetLineFileTable().
  LineTable DecodeLineTable(UnitIndex unit) const;

  // Restrict lookup to the selected file sections. These lookups do not create a unit wrapper.
  DwarfDie GetUnitDieForRef(DwarfDieRef die_ref, bool is_dwo) const;

  // Returns a new DwarfUnit wrapper without adding it to the wrapper cache.
  // Returns null if the DIE does not belong to this binary.
  fxl::RefPtr<DwarfUnit> CreateUncachedUnit(const DwarfDie& die);

 private:
  friend class UnitContextCache;
  friend class UnitContextCacheTest;
  friend class DwarfUnit;

  struct IndexedUnit {
    const UnitHeader& header;
    UnitSection section;
  };

  std::optional<IndexedUnit> GetUnitHeader(UnitIndex index) const;
  void InitUnitLists();
  const UnitList& GetUnitList(bool is_dwo, UnitSection section) const;

  std::optional<UnitIndex> FindUnitForRef(DwarfDieRef die_ref, bool is_dwo) const;
  std::shared_ptr<const DwarfDie::UnitTable> GetDieTable(UnitIndex index,
                                                         DwarfDie::Load load) const;
  DwarfDie GetDieAtIndex(UnitIndex index, uint64_t die_index, DwarfDie::Load load) const;
  DwarfDie GetDieAtOffset(UnitIndex index, uint64_t die_offset) const;
  void ClearDieCache(UnitIndex index) const;

  // Lazily initializes the signature maps.
  void EnsureSignatureMap() const;
  DwarfDieRef FindSignature(uint64_t signature, bool is_dwo) const;

  const std::string name_;
  const std::string binary_name_;
  const std::string build_id_;

  std::unique_ptr<elflib::ElfLib> object_;  // Owns the file-backed section bytes.
  DwarfSections sections_;                  // Borrows bytes from object_ or the fixture.
  std::optional<DebugNamesReader> debug_names_;
  UnitList info_units_;
  UnitList types_units_;
  UnitList info_dwo_units_;
  UnitList types_dwo_units_;

  class DebugAranges;
  std::unique_ptr<DebugAranges> debug_aranges_;

  std::time_t modification_time_ = 0;  // Set when the file is loaded.

  std::map<std::string, ElfTableSymbol> elf_symbols_;
  std::map<std::string, uint64_t> plt_symbols_;

  fxl::RefPtr<DwarfSymbolFactory> symbol_factory_;

  // The binary's owner accesses the unit wrappers.
  mutable std::map<UnitIndex, fxl::RefPtr<DwarfUnit>> unit_map_;

  struct PackageUnit {
    UnitContextCache::AbbrevSection abbrev;
    UnitContext::PackageContributions contributions;
    std::optional<PackageIndex::Contribution> line;
  };

  using PackageUnitResult = ErrOr<std::optional<PackageUnit>>;
  PackageUnitResult ReadPackageUnit(const ErrOr<PackageIndex>& package_index,
                                    const UnitHeader& header, UnitSection kind) const;

  // Split unit order. Entries hold contributions, an error, or nullopt for an unpackaged unit.
  // An empty vector means every split unit is unpackaged.
  std::vector<PackageUnitResult> package_units_;

  // Serializes table builds and eviction across units, and protects the signature maps. A
  // table build can take the abbreviation cache lock while holding this lock.
  mutable std::mutex die_cache_mutex_;
  mutable std::array<std::vector<std::shared_ptr<const DwarfDie::UnitTable>>, 2> die_tables_;

  // Separate maps for normal and DWO units preserve the section that owns the reference.
  using SignatureMap = std::map<uint64_t, DwarfDieRef>;
  mutable std::optional<std::array<SignatureMap, 2>> signature_to_die_;

  // The abbreviation cache is shared with DIE table parsing.
  mutable UnitContextCache unit_contexts_;
#ifndef NDEBUG
  mutable std::atomic_flag unit_context_cache_active_;
#endif

  // File tables of the line tables, computed lazily.
  mutable LineTableCache line_tables_;

  uint64_t mapped_length_ = 0;
  uint64_t first_code_address_ = 0;  // Set when the file is loaded.

  fxl::WeakPtrFactory<DwarfBinary> weak_factory_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_BINARY_H_
