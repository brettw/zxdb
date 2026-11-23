// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"

#include <lib/syslog/cpp/macros.h>

#include <algorithm>

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
#include "llvm/DebugInfo/DWARF/DWARFTypeUnit.h"
#include "src/developer/debug/zxdb/common/file_util.h"
#include "src/developer/debug/zxdb/symbols/const_value.h"
#include "src/developer/debug/zxdb/symbols/dwarf_binary.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"
#include "src/developer/debug/zxdb/symbols/line_file_table.h"
#include "src/developer/debug/zxdb/symbols/line_table_cache.h"

namespace zxdb {

namespace {

// The maximum number of DIEs that one decode visits: the decoded DIE and up to 8 DIEs that it
// references directly or indirectly. Valid symbols rarely need more than 3. The limit also bounds
// the work for corrupt symbols.
constexpr size_t kMaxDiesToVisit = 9;

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

DwarfDieDecoder::DwarfDieDecoder(llvm::DWARFContext* context, std::span<const DwarfAttr> attributes)
    : context_(context), attributes_(attributes) {
  FX_CHECK(attributes_.size() <= kMaxAttributes);
}

DwarfDieDecoder::DwarfDieDecoder(DwarfBinary& binary, std::span<const DwarfAttr> attributes)
    : context_(binary.GetLLVMContext()), binary_(&binary), attributes_(attributes) {
  FX_CHECK(attributes_.size() <= kMaxAttributes);
}

DwarfDieDecoder::~DwarfDieDecoder() = default;

bool DwarfDieDecoder::Decode(const llvm::DWARFDie& die) {
  for (size_t i = 0; i < attributes_.size(); i++)
    values_[i] = AttributeValue();

  Links links;
  if (!DecodeOneDie(die, true, &links))
    return false;
  context_die_ = die;

  // Most DIEs reference no other DIE. They don't need the bookkeeping below.
  if (!links.abstract_origin.isValid() && !links.specification.isValid())
    return true;
  return DecodeLinkedDies(die, links);
}

bool DwarfDieDecoder::DecodeLinkedDies(const llvm::DWARFDie& first, const Links& first_links) {
  // The DIEs to visit are a stack. Each DIE pushes its specification and then its abstract origin,
  // so the abstract origin and the DIEs it references come before the specification.
  std::array<llvm::DWARFDie, kMaxDiesToVisit> visited;
  size_t visited_count = 0;
  visited[visited_count++] = first;

  std::array<llvm::DWARFDie, 2 * kMaxDiesToVisit> pending;
  size_t pending_count = 0;
  auto push_links = [&pending, &pending_count](const Links& links) {
    for (const llvm::DWARFDie& next : {links.specification, links.abstract_origin}) {
      if (next.isValid() && pending_count < pending.size())
        pending[pending_count++] = next;
    }
  };
  push_links(first_links);

  while (pending_count > 0 && visited_count < kMaxDiesToVisit) {
    llvm::DWARFDie cur = pending[--pending_count];
    if (std::find(visited.begin(), visited.begin() + visited_count, cur) !=
        visited.begin() + visited_count)
      continue;  // Already visited, the symbols have a cycle.
    visited[visited_count++] = cur;

    Links links;
    if (!DecodeOneDie(cur, false, &links))
      return false;
    context_die_ = cur;
    push_links(links);
  }
  return true;
}

bool DwarfDieDecoder::Has(DwarfAttr attribute) const { return GetValue(attribute) != nullptr; }

const DwarfDieDecoder::AttributeValue* DwarfDieDecoder::GetValue(DwarfAttr attribute) const {
  size_t index = FindIndex(attribute);
  FX_DCHECK(index < attributes_.size())
      << DwarfAttrToString(attribute, true) << " is not in the decoder's table.";
  if (index >= attributes_.size() || !values_[index].unit)
    return nullptr;
  return &values_[index];
}

std::optional<bool> DwarfDieDecoder::GetBool(DwarfAttr attribute) const {
  if (const AttributeValue* value = GetValue(attribute)) {
    if (std::optional<uint64_t> as_unsigned = value->form.getAsUnsignedConstant())
      return *as_unsigned != 0;
  }
  return std::nullopt;
}

std::optional<uint64_t> DwarfDieDecoder::GetUnsignedConstant(DwarfAttr attribute) const {
  if (const AttributeValue* value = GetValue(attribute))
    return value->form.getAsUnsignedConstant();
  return std::nullopt;
}

std::optional<int64_t> DwarfDieDecoder::GetSignedConstant(DwarfAttr attribute) const {
  if (const AttributeValue* value = GetValue(attribute))
    return value->form.getAsSignedConstant();
  return std::nullopt;
}

std::optional<uint64_t> DwarfDieDecoder::GetAddress(DwarfAttr attribute) const {
  if (const AttributeValue* value = GetValue(attribute))
    return value->form.getAsAddress();
  return std::nullopt;
}

std::optional<const char*> DwarfDieDecoder::GetCString(DwarfAttr attribute) const {
  if (const AttributeValue* value = GetValue(attribute)) {
    if (llvm::Expected<const char*> result = value->form.getAsCString())
      return *result;
    else
      llvm::consumeError(result.takeError());
  }
  return std::nullopt;
}

std::optional<uint64_t> DwarfDieDecoder::GetSectionOffset(DwarfAttr attribute) const {
  // The getAsSectionOffset() call returns nullopt if the form class doesn't match, so this doesn't
  // need to check it.
  if (const AttributeValue* value = GetValue(attribute))
    return value->form.getAsSectionOffset();
  return std::nullopt;
}

std::optional<std::vector<uint8_t>> DwarfDieDecoder::GetBlock(DwarfAttr attribute) const {
  const AttributeValue* value = GetValue(attribute);
  if (!value)
    return std::nullopt;
  DwarfForm form = static_cast<DwarfForm>(value->form.getForm());
  if (!DwarfFormIsBlock(form) && form != DwarfForm::kExprloc)
    return std::nullopt;

  std::optional<llvm::ArrayRef<uint8_t>> block = value->form.getAsBlock();
  if (!block)
    return std::nullopt;
  return std::vector<uint8_t>(block->begin(), block->end());
}

ConstValue DwarfDieDecoder::GetConstValue(DwarfAttr attribute) const {
  const AttributeValue* value = GetValue(attribute);
  if (!value)
    return ConstValue();

  const llvm::DWARFFormValue& form = value->form;
  DwarfForm form_code = static_cast<DwarfForm>(form.getForm());
  if (form_code == DwarfForm::kUdata) {
    if (std::optional<uint64_t> opt_unsigned = form.getAsUnsignedConstant())
      return ConstValue(*opt_unsigned);
  } else if (form_code == DwarfForm::kSdata) {
    if (std::optional<int64_t> opt_signed = form.getAsSignedConstant())
      return ConstValue(static_cast<int64_t>(*opt_signed));
  } else if (DwarfFormIsBlock(form_code)) {
    std::optional<llvm::ArrayRef<uint8_t>> block = form.getAsBlock();
    if (block && !block->empty())
      return ConstValue(std::vector<uint8_t>(block->begin(), block->end()));
  }
  return ConstValue();
}

std::optional<DwarfDieDecoder::HighPC> DwarfDieDecoder::GetHighPC() const {
  const AttributeValue* value = GetValue(DwarfAttr::kHighPc);
  if (!value)
    return std::nullopt;

  const llvm::DWARFFormValue& form = value->form;
  DwarfForm form_code = static_cast<DwarfForm>(form.getForm());
  if (DwarfFormIsConstant(form_code)) {
    if (std::optional<uint64_t> as_constant = form.getAsUnsignedConstant())
      return HighPC(true, *as_constant);
  } else if (DwarfFormIsAddress(form_code)) {
    if (std::optional<uint64_t> as_addr = form.getAsAddress())
      return HighPC(false, *as_addr);
  }
  return std::nullopt;
}

llvm::DWARFDie DwarfDieDecoder::GetReference(DwarfAttr attribute) const {
  if (const AttributeValue* value = GetValue(attribute))
    return DecodeReference(value->unit, value->form);
  return llvm::DWARFDie();
}

std::optional<std::string> DwarfDieDecoder::GetFile(DwarfAttr attribute) const {
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

llvm::DWARFDie DwarfDieDecoder::GetContextParent() const {
  if (!context_die_.isValid())
    return llvm::DWARFDie();
  return context_die_.getParent();
}

size_t DwarfDieDecoder::FindIndex(DwarfAttr attribute) const {
  // The tables are small, so a linear search is faster than a map lookup.
  for (size_t i = 0; i < attributes_.size(); i++) {
    if (attributes_[i] == attribute)
      return i;
  }
  return attributes_.size();
}

bool DwarfDieDecoder::DecodeOneDie(const llvm::DWARFDie& die, bool is_first, Links* links) {
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

  for (const llvm::DWARFAbbreviationDeclaration::AttributeSpec& spec : abbrev->attributes()) {
    // A value that is already filled came from a DIE that the decoder visited earlier, or from an
    // earlier duplicate of the attribute in this DIE. That value wins.
    DwarfAttr attr = static_cast<DwarfAttr>(spec.Attr);
    size_t index = FindIndex(attr);
    bool applies = is_first || (attr != DwarfAttr::kDeclaration && attr != DwarfAttr::kSibling);
    bool wanted = applies && index < attributes_.size() && !values_[index].unit;

    // The links are decoded so Decode() can follow them. The first link of each kind in the DIE is
    // used.
    bool is_abstract_origin = follow_abstract_origins_ && attr == DwarfAttr::kAbstractOrigin &&
                              !links->abstract_origin.isValid();
    bool is_specification = follow_specifications_ && attr == DwarfAttr::kSpecification &&
                            !links->specification.isValid();

    if (!wanted && !is_abstract_origin && !is_specification) {
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
      links->abstract_origin = DecodeReference(unit, form_value);
    if (is_specification)
      links->specification = DecodeReference(unit, form_value);
    if (wanted)
      values_[index] = AttributeValue{.unit = unit, .die = die, .form = form_value};
  }
  return true;
}

llvm::DWARFDie DwarfDieDecoder::DecodeReference(llvm::DWARFUnit* unit,
                                                const llvm::DWARFFormValue& form) const {
  switch (static_cast<DwarfForm>(form.getForm())) {
    case DwarfForm::kRef1:
    case DwarfForm::kRef2:
    case DwarfForm::kRef4:
    case DwarfForm::kRef8:
    case DwarfForm::kRefUdata: {
      // A DWARF "form" is the way a value is encoded in the file. These
      // are all relative location of DIEs within the same unit.
      auto ref_value = form.getAsReferenceUVal();
      if (ref_value)
        return unit->getDIEForOffset(unit->getOffset() + *ref_value);
      break;
    }
    case DwarfForm::kRefAddr: {
      // This is an absolute DIE address which can be used across units.
      auto ref_value = form.getAsReferenceUVal();
      if (ref_value)
        return context_->getDIEForOffset(*ref_value);
      break;
    }
    case DwarfForm::kRefSig8: {
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

llvm::DWARFDie GetAbstractOrigin(const llvm::DWARFDie& die) {
  if (!die.isValid())
    return llvm::DWARFDie();

  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kAbstractOrigin};
  DwarfDieDecoder decoder(&die.getDwarfUnit()->getContext(), kAttrs);
  decoder.set_follow_abstract_origins(false);
  decoder.set_follow_specifications(false);
  if (!decoder.Decode(die))
    return llvm::DWARFDie();
  return decoder.GetReference(DwarfAttr::kAbstractOrigin);
}

}  // namespace zxdb
