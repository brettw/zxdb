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
#include "src/developer/debug/zxdb/symbols/dwarf_unit.h"
#include "src/developer/debug/zxdb/symbols/function_address_map.h"
#include "src/developer/debug/zxdb/symbols/line_program.h"
#include "src/developer/debug/zxdb/symbols/line_table.h"
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

class LineFileTable;

// Represents one DWARF file or one set of in-memory sections. ModuleSymbols connects a
// main binary to the separate .dwo files that its skeleton units reference.
//
// Call on the binary's owning thread except where noted.
// The owner builds the unit context, line table, and function address map caches on demand.
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

  // Call Load() to complete initialization, which can fail.
  DwarfBinary(const std::string& name, const std::string& binary_name, const std::string& build_id);

  // For byte fixtures. The section buffers must outlive the binary and its readers.
  explicit DwarfBinary(DwarfSections sections);

  ~DwarfBinary();

  // Loads ELF and DWARF data from the file.
  Err Load(fxl::WeakPtr<DwarfSymbolFactory::Delegate> delegate,
           DwarfSymbolFactory::FileType file_type);

  // Returns the symbol file path.
  const std::string& GetName() const;
  const std::string& GetBuildID() const;

  // Returns the symbol file's modification time.
  std::time_t GetModificationTime() const;

  // Returns the factory created by Load().
  const DwarfSymbolFactory* GetSymbolFactory() const;

  // Returns whether a program binary was supplied or the symbol file contains program bits.
  // This permits loading symbols from the program itself, such as PLT entries.
  bool HasBinary() const;

  // Returns views of this binary's DWARF sections. The views stay valid while
  // this DwarfBinary exists. A view is empty when its section is absent.
  const DwarfSections& GetSections() const;

  // Returns cached metadata for a unit of this binary.
  UnitContextResult GetUnitContext(UnitIndex index, UnitContextSource skeleton = {}) const;

  // Returns cached metadata for the DIE's unit. A DIE from another binary yields an error.
  UnitContextResult GetUnitContext(const DwarfDie& die, UnitContextSource skeleton = {}) const {
    return die.belongs_to(*this) ? GetUnitContext(die.unit_index(), skeleton)
                                 : UnitContextResult(Err("Unknown DWARF unit."));
  }

  // Returns the section views and offset for the unit's line program, if present.
  std::optional<LineProgramSource> GetLineProgramSource(UnitIndex index) const;

  // Returns the extent of the mapped segments in memory.
  uint64_t GetMappedLength() const;

  // Returns symbols from the ELF file.
  const std::map<std::string, ElfTableSymbol>& GetELFSymbols() const;

  // Returns PLT entries from the ELF file.
  const std::map<std::string, uint64_t>& GetPLTSymbols() const;

  // Normal units are in .debug_info and .debug_types. DWO units are in .debug_info.dwo and
  // .debug_types.dwo. A binary usually contains only one kind, but can contain both.
  uint32_t GetNormalUnitCount() const;
  uint32_t GetDWOUnitCount() const;

  // Returns the unit at the given index, with info units before types units
  // within each section set.
  std::optional<DwarfUnit> GetUnitAtIndex(UnitIndex index);

  // Finds the normal unit covering a module-relative address, if any.
  std::optional<DwarfUnit> UnitForRelativeAddress(uint64_t relative_address);

  // Returns the 64-bit value from the .debug_addr section.
  //
  // The addr_base is an attribute on the compilation unit. This is the byte offset of the address
  // table in the .debug_addr section of that compilation unit's address table.
  //
  // The index is the address index within that compilation unit's table. This is an index into the
  // table (of address-sized entries) rather than a byte index.
  std::optional<uint64_t> GetDebugAddrEntry(uint64_t addr_base, uint64_t index) const;

  // Any thread. Looks up the DIE in the selected normal or split section set.
  DwarfDie GetDieAtOffset(DwarfDieRef die_ref, bool is_dwo) const;

  // Any thread. Returns the root DIE of the unit containing the offset in the selected section
  // set. The offset can identify the unit header or a DIE.
  DwarfDie GetUnitDieForRef(DwarfDieRef die_ref, bool is_dwo) const;

  // Any thread. Looks up a type signature in the source DIE's section set. Searches the other
  // set only if the source set has no matching signature.
  DwarfDie GetDieForSignature(uint64_t signature, bool is_dwo) const;

  // Returns the .debug_names reader, or nullptr if there are no readable contributions.
  const DebugNamesReader* GetDebugNames() const;

  // Returns the line file table for the DIE's unit, or nullptr if the DIE
  // belongs to another binary or the unit has no line table. Usually this parses only the header.
  // The result includes file_index if it appears in the header or the program defines it.
  std::shared_ptr<const LineFileTable> GetLineFileTable(const DwarfDie& die,
                                                        uint64_t file_index) const {
    return die.belongs_to(*this) ? GetLineFileTable(die.unit_index(), file_index) : nullptr;
  }

  // Returns the lowest address of a code section of the binary, or 0 if it has none.
  // Line table sequences that start below it are for code that the linker removed. See
  // IsStrippedSequenceStart().
  uint64_t GetFirstCodeAddress() const { return first_code_address_; }

  // Releases cached DIE tables, line and file tables, function address maps,
  // unit contexts, abbreviation tables, and type signature maps. Later requests rebuild them on
  // demand. Existing DIE handles keep their snapshots. Borrowed line table references expire.
  void ClearDwarfCaches();

 private:
  friend class UnitContextCache;
  friend class UnitContextCacheTest;
  friend class DwarfUnit;

  struct IndexedUnit {
    const UnitHeader& header;
    UnitSection section;
  };

  fxl::WeakPtr<DwarfBinary> GetWeakPtr();

  std::optional<IndexedUnit> GetUnitHeader(UnitIndex index) const;
  DwarfDieRef GetUnitRef(UnitIndex index, std::optional<uint64_t> offset = std::nullopt) const;
  void InitUnitLists();
  const UnitList& GetUnitList(bool is_dwo, UnitSection section) const;

  std::optional<UnitIndex> FindUnitForRef(DwarfDieRef die_ref, bool is_dwo) const;
  std::shared_ptr<const DwarfDie::UnitTable> GetDieTable(UnitIndex index,
                                                         DwarfDie::Load load) const;
  DwarfDie GetDieAtIndex(UnitIndex index, uint64_t die_index, DwarfDie::Load load) const;
  DwarfDie GetDieAtOffset(UnitIndex index, uint64_t die_offset) const;
  void ClearDieCache(UnitIndex index) const;

  std::shared_ptr<const LineFileTable> GetLineFileTable(UnitIndex unit, uint64_t file_index) const;
  const LineTable& GetLineTable(UnitIndex index) const;
  LazySymbol FunctionForRelativeAddress(UnitIndex index, uint64_t relative_address) const;

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
  // These caches can request unit contexts while building an entry.
  mutable std::atomic_flag unit_caches_active_;
#endif

  // Slots follow the normal and DWO unit lists. The owner populates them on demand and resets
  // them without resizing. Function maps store DIE offsets and survive DIE table eviction.
  mutable std::array<std::vector<std::optional<FunctionAddressMap>>, 2> function_address_maps_;
  mutable std::array<std::vector<std::optional<LineTable>>, 2> line_tables_;

  // File tables are shared with DW_AT_decl_file lookups.
  mutable LineTableCache line_file_tables_;

  uint64_t mapped_length_ = 0;
  uint64_t first_code_address_ = 0;  // Set when the file is loaded.

  fxl::WeakPtrFactory<DwarfBinary> weak_factory_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_BINARY_H_
