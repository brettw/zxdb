// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_BINARY_IMPL_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_BINARY_IMPL_H_

#include <map>
#include <optional>

#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/symbols/dwarf/debug_names_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"
#include "src/developer/debug/zxdb/symbols/dwarf_binary.h"
#include "src/developer/debug/zxdb/symbols/dwarf_symbol_factory.h"
#include "src/developer/debug/zxdb/symbols/line_table_cache.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace llvm {

class DWARFCompileUnit;
class DWARFContext;
class MemoryBuffer;

namespace object {
class Binary;
class ObjectFile;
}  // namespace object

}  // namespace llvm

namespace zxdb {

// Returns the lowest address of the sections of the object file that have code, or 0 if there are
// none. This matches the first code address of LLDB (InitializeFirstCodeAddress() in
// SymbolFileDWARF.cpp). LLDB uses the code sections of the module. A separate debug file has the
// same addresses and flags for its code sections even though they have no data, so the result is
// the same for it.
uint64_t GetFirstCodeAddress(const llvm::object::ObjectFile& object);

class DwarfBinaryImpl final : public DwarfBinary {
 public:
  // Callers must call Load() to complete initialization (which can fail).
  DwarfBinaryImpl(const std::string& name, const std::string& binary_name,
                  const std::string& build_id);
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
  llvm::object::ObjectFile* GetLLVMObjectFile() override;
  llvm::DWARFContext* GetLLVMContext() const override;
  const DwarfSections& GetSections() const override;
  uint64_t GetMappedLength() const override;
  const std::map<std::string, ElfTableSymbol>& GetELFSymbols() const override;
  const std::map<std::string, uint64_t> GetPLTSymbols() const override;
  uint32_t GetNormalUnitCount() const override;
  uint32_t GetDWOUnitCount() const override;
  fxl::RefPtr<DwarfUnit> GetUnitAtIndex(UnitIndex i) override;
  fxl::RefPtr<DwarfUnit> UnitForRelativeAddress(uint64_t relative_address) override;
  std::optional<uint64_t> GetDebugAddrEntry(uint64_t addr_base, uint64_t index) const override;
  llvm::DWARFDie GetLLVMDieAtOffset(DwarfDieRef die_ref) const override;
  DwarfDieRef GetDieRefForSignature(uint64_t signature) const override;
  const DebugNamesReader* GetDebugNames() const override;
  std::shared_ptr<const LineFileTable> GetLineFileTable(llvm::DWARFUnit* unit,
                                                        uint64_t file_index) const override;
  uint64_t GetFirstCodeAddress() const override { return first_code_address_; }
  void ClearLLVMCache() override;

  // Decodes the whole line table of the unit. The file table is shared with GetLineFileTable().
  LineTable DecodeLineTable(llvm::DWARFUnit* unit) const;

  // Returns the LLVM unit containing the DIE reference in the normal or DWO sections.
  // Returns null for an invalid reference or a missing unit. The wrapper cache is untouched.
  llvm::DWARFUnit* GetLLVMUnitForDieRef(DwarfDieRef die_ref, bool is_dwo) const;

  // Returns a new DwarfUnit wrapper without adding it to the wrapper cache.
  // Returns null if the LLVM unit is null, belongs to another binary, or has no matching header.
  fxl::RefPtr<DwarfUnit> CreateUncachedUnit(llvm::DWARFUnit* llvm_unit);

 private:
  struct IndexedUnit {
    const UnitHeader& header;
    UnitSection section;
  };

  std::optional<IndexedUnit> GetUnitHeader(UnitIndex index) const;
  const UnitList& GetUnitList(bool is_dwo, UnitSection section) const;
  llvm::DWARFUnit* FindLLVMUnit(bool is_dwo, UnitSection section, uint64_t unit_offset) const;

  // Call every time before using signature_to_die_ to make sure it's lazily populated.
  void EnsureSignatureMap() const;

  const std::string name_;
  const std::string binary_name_;
  const std::string build_id_;

  std::unique_ptr<llvm::MemoryBuffer> binary_buffer_;  // Backing for binary_.
  std::unique_ptr<llvm::object::Binary> binary_;
  std::unique_ptr<llvm::DWARFContext> context_;  // binary_ must outlive this.
  DwarfSections sections_;                       // Views into the sections of context_.
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

  // Holds the mapping between LLVM units and our cached unit wrappers that reference them.
  mutable std::map<const llvm::DWARFUnit*, fxl::RefPtr<DwarfUnit>> unit_map_;

  // Maps 64-bit type unit signatures to their DIE references. Computed lazily by
  // EnsureSignatureMap() by scanning all type units in the normal and DWO unit lists.
  mutable std::optional<std::map<uint64_t, DwarfDieRef>> signature_to_die_;

  // File tables of the line tables, computed lazily.
  mutable LineTableCache line_tables_;

  uint64_t mapped_length_ = 0;
  uint64_t first_code_address_ = 0;  // Set when the file is loaded.

  fxl::WeakPtrFactory<DwarfBinaryImpl> weak_factory_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_BINARY_IMPL_H_
