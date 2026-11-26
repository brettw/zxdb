// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_SYMBOL_FACTORY_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_SYMBOL_FACTORY_H_

#include <span>

#include <llvm/DebugInfo/DWARF/DWARFAddressRange.h>
#include <llvm/Support/Error.h>  // For llvm::Expected.

#include "src/developer/debug/zxdb/common/address_range.h"
#include "src/developer/debug/zxdb/symbols/array_type.h"
#include "src/developer/debug/zxdb/symbols/dwarf_attr.h"
#include "src/developer/debug/zxdb/symbols/dwarf_lang.h"
#include "src/developer/debug/zxdb/symbols/dwarf_tag.h"
#include "src/developer/debug/zxdb/symbols/symbol_factory.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace llvm {
class DWARFContext;
class DWARFDie;
class DWARFFormValue;
class DWARFUnit;
}  // namespace llvm

namespace zxdb {

class AddressRanges;
class BaseType;
class CompileUnit;
class DwarfBinaryImpl;
class DwarfDieDecoder;
class LazySymbol;
class ModuleSymbols;
class UncachedLazySymbol;
class VariableLocation;

// Implementation of SymbolFactory that reads from the DWARF symbols in the given DwarfBinary.
//
// For Debug Fission (where each compilation unit has its symbols in a separate .dwo file), there
// will be one DwarfSymbolFactory for each binary (executable or .dwo file).
class DwarfSymbolFactory : public SymbolFactory {
 public:
  enum FileType {
    kMainBinary,  // The main executable or shared library binary.
    kDWO          // A separate DWO file.
  };

  // Provides information from the owner of this class without owning pointers to break the
  // reference cycle.
  class Delegate {
   public:
    virtual std::string GetBuildDirForSymbolFactory() = 0;
    virtual fxl::WeakPtr<ModuleSymbols> GetModuleSymbols() = 0;

    // When the current binary is a DWO, this returns the skeleton unit in the main binary
    // corresponding to this DWO. When this is not a DWO, returns null.
    virtual CompileUnit* GetSkeletonCompileUnit() = 0;
  };

  // SymbolFactory implementation.
  fxl::RefPtr<Symbol> CreateSymbol(DwarfDieRef die_ref) const override;
  fxl::RefPtr<CompileUnit> GetCompileUnitForDie(DwarfDieRef die_ref) const override;

  using SymbolFactory::MakeLazy;
  using SymbolFactory::MakeUncachedLazy;

 private:
  FRIEND_REF_COUNTED_THREAD_SAFE(DwarfSymbolFactory);
  FRIEND_MAKE_REF_COUNTED(DwarfSymbolFactory);

  DwarfSymbolFactory(fxl::WeakPtr<DwarfBinaryImpl> binary, fxl::WeakPtr<Delegate> delegate,
                     FileType file_type);
  ~DwarfSymbolFactory() override;

  LazySymbol MakeLazy(const llvm::DWARFDie& die) const;
  UncachedLazySymbol MakeUncachedLazy(const llvm::DWARFDie& die) const;

  llvm::DWARFContext* GetLLVMContext() const;

  VariableLocation DecodeLocation(const llvm::DWARFDie& die,
                                  const llvm::DWARFFormValue& form) const;

  // Returns the unit that contains the DIE, or null.
  llvm::DWARFUnit* GetUnitForDieRef(DwarfDieRef die_ref) const;

  // The size of one array dimension. At most one is set. Neither is set when the size is unknown.
  struct ArraySubrange {
    std::optional<size_t> size;
    std::optional<DynamicArrayBound> dynamic_bound;
  };

  // Reads an array subrange. The decoder must use kArraySubrangeAttrs. The default lower bound
  // depends on the language of the unit.
  ArraySubrange ReadArraySubrange(DwarfDieDecoder& range_decoder,
                                  const llvm::DWARFDie& subrange_die,
                                  int64_t default_lower_bound) const;

  // Reads a bound that is a reference or an expression (DWARF 5 section 2.19). The decoder must
  // have decoded the subrange.
  ArraySubrange ReadDynamicBound(const DwarfDieDecoder& range_decoder, DwarfAttr attribute,
                                 DynamicArrayBound::Kind kind, int64_t lower_bound) const;

  // Returns whether the integer type is signed, after typedefs, qualifiers, and the underlying type
  // of an enumeration. Returns nullopt if the type isn't an integer type or can't be decoded.
  std::optional<bool> IsSignedIntegerType(llvm::DWARFDie type) const;

  // Returns the byte size of the discriminant member of the variant part that contains the
  // variant, or nullopt if it's unknown.
  std::optional<uint64_t> GetDiscriminantByteSize(const llvm::DWARFDie& variant) const;

  // Returns the language of the unit that contains the DIE.
  DwarfLang GetLanguage(const llvm::DWARFDie& die) const;

  // Makes a decoder for the given attribute table. The decoder uses the binary, so it finds type
  // unit signatures and file tables in the binary's caches.
  DwarfDieDecoder MakeDecoder(std::span<const DwarfAttr> attributes) const;

  // Internal version that creates a symbol from a Die.
  fxl::RefPtr<Symbol> DecodeSymbol(const llvm::DWARFDie& die) const;

  // As with SymbolFactory::CreateSymbol, these should never return null but rather an empty Symbol
  // implementation on error.
  //
  // The tag is DW_TAG_subprogram or DW_TAG_inlined_subroutine.
  fxl::RefPtr<Symbol> DecodeFunction(const llvm::DWARFDie& die, DwarfTag tag) const;
  fxl::RefPtr<Symbol> DecodeArrayType(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeBaseType(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeCallSite(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeCallSiteParameter(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeCollection(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeCompileUnit(const llvm::DWARFDie& die, DwarfTag tag) const;
  fxl::RefPtr<Symbol> DecodeDataMember(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeEnum(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeFunctionType(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeImportedDeclaration(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeInheritedFrom(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeLexicalBlock(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeMemberPtr(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeModifiedType(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeNamespace(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeTemplateParameter(const llvm::DWARFDie& die, DwarfTag tag) const;
  fxl::RefPtr<Symbol> DecodeUnspecifiedType(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeVariable(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeVariant(const llvm::DWARFDie& die) const;
  fxl::RefPtr<Symbol> DecodeVariantPart(const llvm::DWARFDie& die) const;

  // Generates ranges for a CodeBlock from a decoded DIE. The decoder must have DW_AT_low_pc,
  // DW_AT_high_pc, and DW_AT_ranges in its attribute table. The attributes may be not present, this
  // function will compute what it can given the information (which may be an empty vector).
  AddressRanges GetCodeRanges(const DwarfDieDecoder& decoder) const;

  // Returns the single code range if it's encoded with DW_AT_low_pc and DW_AT_high_pc.
  std::optional<AddressRange> GetLowHighEncodedCodeRange(const DwarfDieDecoder& decoder) const;

  // Decodes the index for a DW_FORM_rnglistx encoding. The unit is the unit of the attribute.
  llvm::Expected<llvm::DWARFAddressRangesVector> GetCodeRangesFromRangelistXIndex(
      llvm::DWARFUnit* unit, uint64_t index) const;

  // These can be null if the module is unloaded but there are still some dangling type references
  // to them.
  fxl::WeakPtr<DwarfBinaryImpl> binary_;
  fxl::WeakPtr<Delegate> delegate_;

  FileType file_type_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_SYMBOL_FACTORY_H_
