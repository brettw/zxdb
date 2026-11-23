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
// INHERITED ATTRIBUTES
//
// A DIE can omit attributes that another DIE gives. The decoder follows the two kinds of links that
// allow this, and the referenced DIEs "underlay" the attributes of the decoded DIE:
//
//   - DW_AT_abstract_origin (DWARF 5 section 3.3.8): a concrete instance of an inlined or
//     out-of-line function, and its children, reference the abstract instance.
//   - DW_AT_specification (DWARF 5 section 2.13.2): a definition references the declaration that
//     it completes, for example an out-of-line member function definition and the declaration in
//     the class.
//
// A common chain is: concrete instance -> (abstract origin) abstract instance -> (specification)
// declaration in a class.
//
// An attribute on a DIE overrides the same attribute on the DIEs it references. So a concrete
// instance wins over its abstract instance, and a definition wins over its declaration. The decoder
// visits the abstract origin of a DIE, and the DIEs that it references, before the specification of
// that DIE. It visits each DIE at most once, so cycles in corrupt symbols end.
//
// DW_AT_declaration and DW_AT_sibling apply only to the decoded DIE. The decoder doesn't take them
// from the referenced DIEs (DWARF 5 section 2.13.2). This is the same as LLDB.
class DwarfDieDecoder {
 public:
  static constexpr size_t kMaxAttributes = 16;

  // One attribute that Decode() found.
  //
  // The die is the DIE that has the attribute. It is the decoded DIE, or a DIE that the decoder
  // reached through DW_AT_abstract_origin or DW_AT_specification. That DIE can be in a different
  // unit than the decoded DIE. Use it, for example, as the source of a DWARF expression, so the
  // expression uses the address table of the right unit.
  //
  // The unit is the unit of that DIE. llvm::DWARFFormValue::getUnit() exists but this won't always
  // be set (the implicit constant path doesn't set it). As a result, if the unit is needed, always
  // use the unit here rather than getUnit().
  struct AttributeValue {
    llvm::DWARFUnit* unit = nullptr;  // Null when the attribute isn't present.
    llvm::DWARFDie die;
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

  // Control whether Decode() follows DW_AT_abstract_origin and DW_AT_specification attributes. The
  // default for both is true. Turn one off when the caller doesn't need the attributes from those
  // DIEs, to save the time of decoding them, or when the attribute means something else for the
  // tag. For example, DW_AT_abstract_origin on DW_TAG_GNU_call_site references the called function.
  void set_follow_abstract_origins(bool follow) { follow_abstract_origins_ = follow; }
  void set_follow_specifications(bool follow) { follow_specifications_ = follow; }

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

  // Returns the parent of the last DIE that Decode() visited. This is the DIE that gives the
  // context of the decoded entity, such as the namespace or class that encloses it.
  //
  // The parent DIE is not an attribute so needs to be handled explicitly. For an inlined
  // subroutine, the parent of the DIE is the block it's inlined into. The parent of the abstract
  // instance is the namespace or class that lexically encloses the function. If the abstract
  // instance has a DW_AT_specification, the declaration it references gives the context: for a
  // member function, that is the class (DWARF 5 section 3.3.8.1).
  //
  // If the DIE references no other DIE, this returns the regular parent of the DIE. The only case
  // the result should be !isValid() is when decoding a toplevel DIE with no parent.
  llvm::DWARFDie GetContextParent() const;

 private:
  // Returns the index of the attribute in attributes_, or attributes_.size() if it isn't there.
  size_t FindIndex(DwarfAttr attribute) const;

  // The DIEs that one DIE references through the links that Decode() follows.
  struct Links {
    llvm::DWARFDie abstract_origin;
    llvm::DWARFDie specification;
  };

  // Decodes the attributes of one DIE into the values that aren't filled yet. The first DIE is the
  // DIE that the caller gave to Decode(). Returns false if the DIE is corrupt.
  bool DecodeOneDie(const llvm::DWARFDie& die, bool is_first, Links* links);

  // Visits the DIEs that the first DIE references, after Decode() decoded the first DIE.
  bool DecodeLinkedDies(const llvm::DWARFDie& first, const Links& first_links);

  // Decodes a cross-DIE reference. Return value will be !isValid() on failure.
  llvm::DWARFDie DecodeReference(llvm::DWARFUnit* unit, const llvm::DWARFFormValue& form) const;

  llvm::DWARFContext* context_;
  const DwarfBinary* binary_ = nullptr;

  std::span<const DwarfAttr> attributes_;

  // The values of the last decoded DIE, with the same indices as attributes_. Decode() fills the
  // value of an attribute only once, so the first DIE it visits with the attribute gives the value.
  std::array<AttributeValue, kMaxAttributes> values_;

  // The last DIE that Decode() visited. See GetContextParent().
  llvm::DWARFDie context_die_;

  bool follow_abstract_origins_ = true;
  bool follow_specifications_ = true;

  FXL_DISALLOW_COPY_AND_ASSIGN(DwarfDieDecoder);
};

// Returns the DIE that the DW_AT_abstract_origin attribute of the given DIE references. This
// doesn't follow the abstract origin of that DIE. Returns an invalid DIE if the DIE has no abstract
// origin or the reference is invalid.
llvm::DWARFDie GetAbstractOrigin(const llvm::DWARFDie& die);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DIE_DECODER_H_
