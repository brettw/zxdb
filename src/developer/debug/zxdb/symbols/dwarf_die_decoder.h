// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DIE_DECODER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DIE_DECODER_H_

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "llvm/DebugInfo/DWARF/DWARFDie.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
#include "src/developer/debug/zxdb/symbols/const_value.h"
#include "src/developer/debug/zxdb/symbols/dwarf_attr.h"
#include "src/lib/fxl/macros.h"

namespace llvm {
class DWARFUnit;
class DWARFContext;
}  // namespace llvm

namespace zxdb {

class DwarfBinary;

// Decodes the desired attributes of a given DWARF Debug Info Entry ("DIE").
//
// The caller gives a table of the attributes it wants when it creates the decoder. Decode() reads
// one DIE and keeps the raw form value of each attribute in the table. The Get*() functions then
// convert the kept values. Decode() clears the values of the previous DIE, so one decoder can
// decode many DIEs.
//
//   static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName, DwarfAttr::kType};
//   DwarfDieDecoder decoder(binary, kAttrs);
//   if (!decoder.Decode(die))
//     return Error();
//   std::optional<const char*> name = decoder.GetCString(DwarfAttr::kName);
//   llvm::DWARFDie type = decoder.GetReference(DwarfAttr::kType);
//
// The decoder doesn't allocate memory. It keeps the values in a fixed array, so a table can have
// at most kMaxAttributes attributes.
//
// This transparently follows DW_AT_abstract_origin attributes. This is used to implement
// "inheritance" of DIEs. An attribute on a DIE overrides the same attribute on its abstract origin.
class DwarfDieDecoder {
 public:
  static constexpr size_t kMaxAttributes = 16;

  // One attribute that Decode() found. The unit is the unit of the DIE that has the attribute. An
  // abstract origin can be in a different unit than the decoded DIE.
  //
  // llvm::DWARFFormValue::getUnit() exists but this won't always be set (the implicit constant
  // path doesn't set it). As a result, if the unit is needed, always use the unit here rather than
  // getUnit().
  struct AttributeValue {
    llvm::DWARFUnit* unit = nullptr;  // Null when the attribute isn't present.
    llvm::DWARFFormValue form;
  };

  // DW_AT_high_pc is special: If it is of class "address", it's an address, and if it's of class
  // "constant" it's an unsigned integer offset from the low PC. This struct encodes whether it was
  // a constant or not in the output. Use with GetHighPC().
  struct HighPC {
    HighPC() = default;
    HighPC(bool c, uint64_t v) : is_constant(c), value(v) {}

    bool is_constant = false;
    uint64_t value = 0;
  };

  // The context or binary, and the attribute table, must outlive this class. A static constexpr
  // array is a good attribute table. Prefer the DwarfBinary version when possible.
  DwarfDieDecoder(llvm::DWARFContext* context, std::span<const DwarfAttr> attributes);
  DwarfDieDecoder(DwarfBinary& binary, std::span<const DwarfAttr> attributes);
  ~DwarfDieDecoder();

  // Decodes one DIE. Returns true on success, false means the DIE was corrupt. After a failure, the
  // values are incomplete.
  bool Decode(const llvm::DWARFDie& die);

  // The functions below get the values of the last decoded DIE. The attribute must be in the table
  // given to the constructor.
  //
  // The functions that return an optional return nullopt when the attribute isn't present or when
  // its form can't be converted to the requested type.

  // Returns true when the attribute is present. This doesn't check the form of the attribute.
  bool Has(DwarfAttr attribute) const;

  // Returns the raw value, or null when the attribute isn't present. Use this for attributes that
  // can have more than one encoding.
  const AttributeValue* GetValue(DwarfAttr attribute) const;

  // These call the similarly-named function in llvm::DWARFFormValue.
  std::optional<bool> GetBool(DwarfAttr attribute) const;
  std::optional<uint64_t> GetUnsignedConstant(DwarfAttr attribute) const;
  std::optional<int64_t> GetSignedConstant(DwarfAttr attribute) const;
  std::optional<uint64_t> GetAddress(DwarfAttr attribute) const;
  std::optional<const char*> GetCString(DwarfAttr attribute) const;

  // The returned section offset is the raw value. The caller must find the ELF section that it
  // references and interpret it.
  std::optional<uint64_t> GetSectionOffset(DwarfAttr attribute) const;

  // Accepts the block and exprloc form classes.
  std::optional<std::vector<uint8_t>> GetBlock(DwarfAttr attribute) const;

  // Returns an unset ConstValue when the attribute isn't present or has an unsupported form.
  ConstValue GetConstValue(DwarfAttr attribute) const;

  // Requires DW_AT_high_pc in the attribute table.
  std::optional<HighPC> GetHighPC() const;

  // For cross-DIE references. Note that the resulting DIE may not be in the same unit. If the
  // attribute doesn't exist or is invalid, the returned DIE will be !isValid().
  llvm::DWARFDie GetReference(DwarfAttr attribute) const;

  // Gets a file name. File names (e.g. for DW_AT_decl_file) are not strings but rather indices
  // into the file name table for the corresponding unit. This function resolves the string.
  std::optional<std::string> GetFile(DwarfAttr attribute) const;

  // Returns the parent of the deepest abstract origin of the last decoded DIE.
  //
  // Most DIEs can have an "abstract origin" which is another DIE that underlays values.
  // Theoretically abstract origins can be linked into arbitrarily long chains. In the current Clang
  // this mostly happens for inlined functions, where the inlined instance references the actual
  // function definition as its abstract origin. But abstract origins can theoretically appear
  // almost anywhere.
  //
  // Normally this class handles abstract origins transparently when querying attributes. But the
  // parent DIE is not an attribute so needs to be handled explicitly. In the example of inlined
  // functions, the parent of the inlined subroutine DIE will be the block it's inlined into, but
  // the parent of the abstract origin will be the namespace or class that lexically encloses that
  // function.
  //
  // If there is no abstract origin, this returns the regular parent of the DIE. The only case the
  // result should be !isValid() is when decoding a toplevel DIE with no parent.
  llvm::DWARFDie GetAbstractParent() const;

 private:
  // Returns the index of the attribute in attributes_, or attributes_.size() if it isn't there.
  size_t FindIndex(DwarfAttr attribute) const;

  // Backend for Decode() above.
  //
  // Following abstract origins generates a recursive call. To prevent infinite recursion for
  // corrupt symbols, this function takes a maximum number of abstract origin references to follow
  // which is decremented each time a recursive call is made. When this gets to 0, no more abstract
  // origin references will be followed.
  bool DecodeInternal(const llvm::DWARFDie& die, int abstract_origin_refs_to_follow);

  // Decodes a cross-DIE reference. Return value will be !isValid() on failure.
  llvm::DWARFDie DecodeReference(llvm::DWARFUnit* unit, const llvm::DWARFFormValue& form) const;

  llvm::DWARFContext* context_;
  const DwarfBinary* binary_ = nullptr;

  std::span<const DwarfAttr> attributes_;

  // The values of the last decoded DIE, with the same indices as attributes_. Decode() fills the
  // value of an attribute only once, so the DIE nearest to the decoded DIE in the abstract origin
  // chain gives the value.
  std::array<AttributeValue, kMaxAttributes> values_;

  // The deepest DIE in the abstract origin chain of the last decoded DIE. See GetAbstractParent().
  llvm::DWARFDie deepest_die_;

  FXL_DISALLOW_COPY_AND_ASSIGN(DwarfDieDecoder);
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DIE_DECODER_H_
