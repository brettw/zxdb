// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"

#include <lib/syslog/cpp/macros.h>

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
#include "llvm/DebugInfo/DWARF/DWARFTypeUnit.h"
#include "src/developer/debug/zxdb/common/file_util.h"
#include "src/developer/debug/zxdb/symbols/const_value.h"
#include "src/developer/debug/zxdb/symbols/dwarf_binary.h"
#include "src/developer/debug/zxdb/symbols/line_file_table.h"
#include "src/developer/debug/zxdb/symbols/line_table_cache.h"

namespace zxdb {

namespace {

// The maximum nesting of abstract origin references we'll follow recursively
// before givin up. Prevents blowing out the stack for corrupt symbols.
constexpr int kMaxAbstractOriginRefsToFollow = 8;

llvm::DWARFDie GetDIEForSignature(llvm::DWARFContext* context, uint64_t signature) {
  if (!context)
    return llvm::DWARFDie();

  auto search_units = [signature](const llvm::DWARFUnitVector& units) -> llvm::DWARFDie {
    for (const std::unique_ptr<llvm::DWARFUnit>& unit : units) {
      if (!unit->isTypeUnit())
        continue;
      auto* type_unit = static_cast<llvm::DWARFTypeUnit*>(unit.get());
      if (type_unit->getTypeHash() != signature)
        continue;
      uint64_t die_offset = unit->getOffset() + type_unit->getTypeOffset();
      return unit->getDIEForOffset(die_offset);
    }
    return llvm::DWARFDie();
  };

  if (llvm::DWARFDie die = search_units(context->getNormalUnitsVector()))
    return die;
  return search_units(context->getDWOUnitsVector());
}

}  // namespace

DwarfDieDecoder::DwarfDieDecoder(llvm::DWARFContext* context,
                                 std::span<const llvm::dwarf::Attribute> attributes)
    : context_(context), attributes_(attributes) {
  FX_CHECK(attributes_.size() <= kMaxAttributes);
}

DwarfDieDecoder::DwarfDieDecoder(DwarfBinary& binary,
                                 std::span<const llvm::dwarf::Attribute> attributes)
    : context_(binary.GetLLVMContext()), binary_(&binary), attributes_(attributes) {
  FX_CHECK(attributes_.size() <= kMaxAttributes);
}

DwarfDieDecoder::~DwarfDieDecoder() = default;

bool DwarfDieDecoder::Decode(const llvm::DWARFDie& die) {
  for (size_t i = 0; i < attributes_.size(); i++)
    values_[i] = AttributeValue();
  deepest_die_ = llvm::DWARFDie();
  return DecodeInternal(die, kMaxAbstractOriginRefsToFollow);
}

bool DwarfDieDecoder::Has(llvm::dwarf::Attribute attribute) const {
  return GetValue(attribute) != nullptr;
}

const DwarfDieDecoder::AttributeValue* DwarfDieDecoder::GetValue(
    llvm::dwarf::Attribute attribute) const {
  size_t index = FindIndex(attribute);
  FX_DCHECK(index < attributes_.size()) << "The attribute is not in the decoder's table.";
  if (index >= attributes_.size() || !values_[index].unit)
    return nullptr;
  return &values_[index];
}

std::optional<bool> DwarfDieDecoder::GetBool(llvm::dwarf::Attribute attribute) const {
  if (const AttributeValue* value = GetValue(attribute)) {
    if (std::optional<uint64_t> as_unsigned = value->form.getAsUnsignedConstant())
      return *as_unsigned != 0;
  }
  return std::nullopt;
}

std::optional<uint64_t> DwarfDieDecoder::GetUnsignedConstant(
    llvm::dwarf::Attribute attribute) const {
  if (const AttributeValue* value = GetValue(attribute))
    return value->form.getAsUnsignedConstant();
  return std::nullopt;
}

std::optional<int64_t> DwarfDieDecoder::GetSignedConstant(llvm::dwarf::Attribute attribute) const {
  if (const AttributeValue* value = GetValue(attribute))
    return value->form.getAsSignedConstant();
  return std::nullopt;
}

std::optional<uint64_t> DwarfDieDecoder::GetAddress(llvm::dwarf::Attribute attribute) const {
  if (const AttributeValue* value = GetValue(attribute))
    return value->form.getAsAddress();
  return std::nullopt;
}

std::optional<const char*> DwarfDieDecoder::GetCString(llvm::dwarf::Attribute attribute) const {
  if (const AttributeValue* value = GetValue(attribute)) {
    if (llvm::Expected<const char*> result = value->form.getAsCString())
      return *result;
    else
      llvm::consumeError(result.takeError());
  }
  return std::nullopt;
}

std::optional<uint64_t> DwarfDieDecoder::GetSectionOffset(llvm::dwarf::Attribute attribute) const {
  // The getAsSectionOffset() call returns nullopt if the form class doesn't match, so this doesn't
  // need to check it.
  if (const AttributeValue* value = GetValue(attribute))
    return value->form.getAsSectionOffset();
  return std::nullopt;
}

std::optional<std::vector<uint8_t>> DwarfDieDecoder::GetBlock(
    llvm::dwarf::Attribute attribute) const {
  const AttributeValue* value = GetValue(attribute);
  if (!value)
    return std::nullopt;
  if (!value->form.isFormClass(llvm::DWARFFormValue::FC_Block) &&
      !value->form.isFormClass(llvm::DWARFFormValue::FC_Exprloc))
    return std::nullopt;

  std::optional<llvm::ArrayRef<uint8_t>> block = value->form.getAsBlock();
  if (!block)
    return std::nullopt;
  return std::vector<uint8_t>(block->begin(), block->end());
}

ConstValue DwarfDieDecoder::GetConstValue(llvm::dwarf::Attribute attribute) const {
  const AttributeValue* value = GetValue(attribute);
  if (!value)
    return ConstValue();

  const llvm::DWARFFormValue& form = value->form;
  if (form.getForm() == llvm::dwarf::DW_FORM_udata) {
    if (std::optional<uint64_t> opt_unsigned = form.getAsUnsignedConstant())
      return ConstValue(*opt_unsigned);
  } else if (form.getForm() == llvm::dwarf::DW_FORM_sdata) {
    if (std::optional<int64_t> opt_signed = form.getAsSignedConstant())
      return ConstValue(static_cast<int64_t>(*opt_signed));
  } else if (form.isFormClass(llvm::DWARFFormValue::FC_Block)) {
    std::optional<llvm::ArrayRef<uint8_t>> block = form.getAsBlock();
    if (block && !block->empty())
      return ConstValue(std::vector<uint8_t>(block->begin(), block->end()));
  }
  return ConstValue();
}

std::optional<DwarfDieDecoder::HighPC> DwarfDieDecoder::GetHighPC() const {
  const AttributeValue* value = GetValue(llvm::dwarf::DW_AT_high_pc);
  if (!value)
    return std::nullopt;

  const llvm::DWARFFormValue& form = value->form;
  if (form.isFormClass(llvm::DWARFFormValue::FC_Constant)) {
    if (std::optional<uint64_t> as_constant = form.getAsUnsignedConstant())
      return HighPC(true, *as_constant);
  } else if (form.isFormClass(llvm::DWARFFormValue::FC_Address)) {
    if (std::optional<uint64_t> as_addr = form.getAsAddress())
      return HighPC(false, *as_addr);
  }
  return std::nullopt;
}

llvm::DWARFDie DwarfDieDecoder::GetReference(llvm::dwarf::Attribute attribute) const {
  if (const AttributeValue* value = GetValue(attribute))
    return DecodeReference(value->unit, value->form);
  return llvm::DWARFDie();
}

std::optional<std::string> DwarfDieDecoder::GetFile(llvm::dwarf::Attribute attribute) const {
  const AttributeValue* value = GetValue(attribute);
  if (!value)
    return std::nullopt;
  std::optional<uint64_t> file_index = value->form.getAsUnsignedConstant();
  if (!file_index)
    return std::nullopt;

  // The binary caches the file tables. Without a binary, use a cache for this lookup only.
  std::shared_ptr<const LineFileTable> files;
  if (binary_) {
    files = binary_->GetLineFileTable(value->unit, *file_index);
  } else {
    files = LineTableCache().GetFileTable(*value->unit, *file_index);
  }
  if (!files)
    return std::nullopt;

  if (const std::string* name = files->GetFileName(*file_index))
    return *name;
  return std::nullopt;
}

llvm::DWARFDie DwarfDieDecoder::GetAbstractParent() const {
  if (!deepest_die_.isValid())
    return llvm::DWARFDie();
  return deepest_die_.getParent();
}

size_t DwarfDieDecoder::FindIndex(llvm::dwarf::Attribute attribute) const {
  // The tables are small, so a linear search is faster than a map lookup.
  for (size_t i = 0; i < attributes_.size(); i++) {
    if (attributes_[i] == attribute)
      return i;
  }
  return attributes_.size();
}

bool DwarfDieDecoder::DecodeInternal(const llvm::DWARFDie& die,
                                     int abstract_origin_refs_to_follow) {
  // This indicates the abbreviation. Each DIE starts with an abbreviation
  // code.  The is the number that the DWARFAbbreviationDeclaration was derived
  // from above. We have to read it again to skip the offset over the number.
  //
  //  - A zero abbreviation code indicates a null DIE which is used to mark
  //    the end of a sequence of siblings.
  //
  //  - Otherwise this is a tag of an entry in the .debug_abbrev table (each
  //    entry in that table declares its own tag so it's not an index or an
  //    offset). The abbreviation entry indicates the attributes that this
  //    type of DIE contains, plus the data format for each.
  const llvm::DWARFAbbreviationDeclaration* abbrev = die.getAbbreviationDeclarationPtr();
  if (!abbrev)
    return false;

  llvm::DWARFUnit* unit = die.getDwarfUnit();
  llvm::DWARFDataExtractor extractor = unit->getDebugInfoExtractor();
  uint64_t offset = die.getOffset();

  // Skip over the abbreviationcode. We don't actually need this (the abbrev
  // pointer above is derived from this) but we need to move offset past it.
  uint64_t abbr_code = extractor.getULEB128(&offset);
  if (!abbr_code) {
    FX_NOTREACHED();  // Should have gotten a null abbrev for this above.
    return false;
  }

  // Set when we encounter an abstract origin attribute.
  llvm::DWARFDie abstract_origin;

  for (const llvm::DWARFAbbreviationDeclaration::AttributeSpec& spec : abbrev->attributes()) {
    // A value that is already filled came from a DIE nearer in the abstract origin chain, or from
    // an earlier duplicate of the attribute in this DIE. That value wins.
    size_t index = FindIndex(spec.Attr);
    bool wanted = index < attributes_.size() && !values_[index].unit;

    // Abstract origins are always decoded so they can be followed after the loop. This doesn't
    // check for duplicates so the decoder can follow more than one link in the chain.
    bool is_abstract_origin = spec.Attr == llvm::dwarf::DW_AT_abstract_origin;

    if (!wanted && !is_abstract_origin) {
      // Skip over the attribute to go to the next.
      llvm::DWARFFormValue::skipValue(spec.Form, extractor, &offset, unit->getFormParams());
      continue;
    }

    llvm::DWARFFormValue form_value(spec.Form);
    if (spec.isImplicitConst()) {
      // In the "implicit const" form, the value is stored in the abbreviation declaration which is
      // not passed in to the DWARFFormValue constructor or its extractValue() member. So this form
      // has to be specially decoded.
      form_value = llvm::DWARFFormValue::createFromSValue(spec.Form, spec.getImplicitConstValue());
    } else {
      form_value.extractValue(extractor, &offset, unit->getFormParams(), unit);
    }

    if (is_abstract_origin)
      abstract_origin = DecodeReference(unit, form_value);
    if (wanted)
      values_[index] = AttributeValue{.unit = unit, .form = form_value};
  }

  // Recursively decode abstract origins. The attributes on the abstract origin
  // DIE "underlay" any attributes present on the current one.
  if (abstract_origin.isValid() && abstract_origin_refs_to_follow > 0)
    return DecodeInternal(abstract_origin, abstract_origin_refs_to_follow - 1);

  // The deepest DIE in the abstract origin chain was found (which will be the original DIE itself
  // if there was no abstract origin).
  deepest_die_ = die;
  return true;
}

llvm::DWARFDie DwarfDieDecoder::DecodeReference(llvm::DWARFUnit* unit,
                                                const llvm::DWARFFormValue& form) const {
  switch (form.getForm()) {
    case llvm::dwarf::DW_FORM_ref1:
    case llvm::dwarf::DW_FORM_ref2:
    case llvm::dwarf::DW_FORM_ref4:
    case llvm::dwarf::DW_FORM_ref8:
    case llvm::dwarf::DW_FORM_ref_udata: {
      // A DWARF "form" is the way a value is encoded in the file. These
      // are all relative location of DIEs within the same unit.
      auto ref_value = form.getAsReferenceUVal();
      if (ref_value)
        return unit->getDIEForOffset(unit->getOffset() + *ref_value);
      break;
    }
    case llvm::dwarf::DW_FORM_ref_addr: {
      // This is an absolute DIE address which can be used across units.
      auto ref_value = form.getAsReferenceUVal();
      if (ref_value)
        return context_->getDIEForOffset(*ref_value);
      break;
    }
    case llvm::dwarf::DW_FORM_ref_sig8: {
      auto ref_value = form.getAsReferenceUVal();
      if (ref_value) {
        if (binary_) {
          // Prefer fast O(log N) cached signature lookup and section-aware DIE resolution
          // via DwarfBinary.
          if (DwarfDieRef die_ref = binary_->GetDieRefForSignature(*ref_value)) {
            return binary_->GetLLVMDieAtOffset(die_ref);
          }
        } else {
          // Fallback for tests or contexts where only a bare llvm::DWARFContext* was provided.
          return GetDIEForSignature(context_, *ref_value);
        }
      }
      break;
    }
    default:
      // Note that we don't handle DW_FORM_ref_sup4, or DW_FORM_ref_sup8.
      // The "sup4/8" ones require a shared separate symbol file we don't use.
      break;
  }
  return llvm::DWARFDie();
}

}  // namespace zxdb
