// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_BINARY_IMPL_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_BINARY_IMPL_H_

#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/common/err_or.h"
#include "src/developer/debug/zxdb/symbols/dwarf/debug_names_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/package_index.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"
#include "src/developer/debug/zxdb/symbols/dwarf_binary.h"
#include "src/developer/debug/zxdb/symbols/dwarf_symbol_factory.h"
#include "src/developer/debug/zxdb/symbols/line_table_cache.h"
#include "src/developer/debug/zxdb/symbols/unit_context_cache.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace llvm {

class DWARFContext;
class MemoryBuffer;

namespace object {
class Binary;
}  // namespace object

}  // namespace llvm

namespace zxdb {

class DwarfBinaryImpl final : public DwarfBinary {
 public:
  // Callers must call Load() to complete initialization (which can fail).
  DwarfBinaryImpl(const std::string& name, const std::string& binary_name,
                  const std::string& build_id);
  // For byte fixtures. The section buffers must outlive the binary and its readers.
  explicit DwarfBinaryImpl(DwarfSections sections);
  ~DwarfBinaryImpl();

  fxl::WeakPtr<DwarfBinaryImpl> GetWeakPtr();

  Err Load(fxl::WeakPtr<DwarfSymbolFactory::Delegate> delegate,
           DwarfSymbolFactory::FileType file_type);

  // DwarfBinary implementation.
  std::string GetName() const override;
  std::string GetBuildID() const override;
  std::time_t GetModificationTime() const override;
  const DwarfSymbolFactory* GetSymbolFactory() const override;
  bool HasBinary() const override;
  const DwarfSections& GetSections() const override;
  UnitContextResult GetUnitContext(UnitIndex index, UnitContextSource skeleton = {}) const override;
  using DwarfBinary::GetUnitContext;
  std::optional<LineProgramSource> GetLineProgramSource(UnitIndex index) const override;
  uint64_t GetMappedLength() const override;
  const std::map<std::string, ElfTableSymbol>& GetELFSymbols() const override;
  const std::map<std::string, uint64_t> GetPLTSymbols() const override;
  uint32_t GetNormalUnitCount() const override;
  uint32_t GetDWOUnitCount() const override;
  fxl::RefPtr<DwarfUnit> GetUnitAtIndex(UnitIndex i) override;
  fxl::RefPtr<DwarfUnit> UnitForRelativeAddress(uint64_t relative_address) override;
  std::optional<uint64_t> GetDebugAddrEntry(uint64_t addr_base, uint64_t index) const override;
  DwarfDie GetDieAtOffset(DwarfDieRef die_ref) const override;
  DwarfDie GetDieAtOffset(DwarfDieRef die_ref, bool is_dwo) const override;
  DwarfDieRef GetDieRefForSignature(uint64_t signature) const override;
  DwarfDie GetDieForSignature(uint64_t signature, bool is_dwo) const override;
  const DebugNamesReader* GetDebugNames() const override;
  std::shared_ptr<const LineFileTable> GetLineFileTable(UnitIndex unit,
                                                        uint64_t file_index) const override;
  uint64_t GetFirstCodeAddress() const override { return first_code_address_; }
  using DwarfBinary::GetLineFileTable;
  void ClearDwarfCaches() override;

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
  friend class DwarfUnitImpl;

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

  std::unique_ptr<llvm::MemoryBuffer> binary_buffer_;  // Backing for binary_.
  std::unique_ptr<llvm::object::Binary> binary_;
  std::unique_ptr<llvm::DWARFContext> context_;  // binary_ must outlive this.
  DwarfSections sections_;                       // Borrows bytes from the context or the fixture.
  std::optional<DebugNamesReader> debug_names_;
  UnitList info_units_;
  UnitList types_units_;
  UnitList info_dwo_units_;
  UnitList types_dwo_units_;

  class DebugAranges;
  std::unique_ptr<DebugAranges> debug_aranges_;  // Replaces context_->Aranges.

  std::time_t modification_time_ = 0;  // Set when the file is loaded.

  std::map<std::string, ElfTableSymbol> elf_symbols_;
  std::map<std::string, uint64_t> plt_symbols_;

  fxl::RefPtr<DwarfSymbolFactory> symbol_factory_;

  // The binary's owner accesses the wrappers and LLVM context.
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

  fxl::WeakPtrFactory<DwarfBinaryImpl> weak_factory_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_BINARY_IMPL_H_
