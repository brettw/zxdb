// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"

#include <lib/syslog/cpp/macros.h>

#include <algorithm>
#include <limits>

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
#include "llvm/DebugInfo/DWARF/DWARFTypeUnit.h"
#include "src/developer/debug/zxdb/common/checked_math.h"
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

DwarfDieDecoder::DwarfDieDecoder(llvm::DWARFContext* context, std::span<const DwarfAttr> attributes,
                                 llvm::DWARFUnit* address_table_unit)
    : context_(context), address_table_unit_(address_table_unit), attributes_(attributes) {
  FX_CHECK(attributes_.size() <= kMaxAttributes);
}

DwarfDieDecoder::DwarfDieDecoder(DwarfBinary& binary, std::span<const DwarfAttr> attributes,
                                 llvm::DWARFUnit* address_table_unit)
    : context_(binary.GetLLVMContext()),
      binary_(&binary),
      address_table_unit_(address_table_unit),
      attributes_(attributes) {
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
  std::optional<int128_t> value = GetInteger(attribute, false);
  if (!value || *value < 0 || *value > std::numeric_limits<uint64_t>::max())
    return std::nullopt;
  return static_cast<uint64_t>(*value);
}

std::optional<int64_t> DwarfDieDecoder::GetSignedConstant(DwarfAttr attribute) const {
  std::optional<int128_t> value = GetInteger(attribute, true);
  if (!value || *value < std::numeric_limits<int64_t>::min() ||
      *value > std::numeric_limits<int64_t>::max())
    return std::nullopt;
  return static_cast<int64_t>(*value);
}

std::optional<uint64_t> DwarfDieDecoder::GetAddress(DwarfAttr attribute) const {
  if (const AttributeValue* value = GetValue(attribute))
    return DecodeAddress(*value);
  return std::nullopt;
}

std::optional<uint64_t> DwarfDieDecoder::DecodeAddress(const AttributeValue& value) const {
  constexpr uint64_t kTombstone = std::numeric_limits<uint64_t>::max();

  DwarfForm form = static_cast<DwarfForm>(value.form.getForm());
  if (!DwarfFormIsAddress(form))
    return std::nullopt;

  uint64_t raw = value.form.getRawUValue();
  if (form == DwarfForm::kAddr)
    return raw == kTombstone ? std::nullopt : std::optional<uint64_t>(raw);

  // The other forms are indices into the address table. DW_FORM_LLVM_addrx_offset has the index in
  // the high 32 bits and an offset from that address in the low 32 bits.
  uint64_t index = raw;
  uint64_t offset = 0;
  if (form == DwarfForm::kLlvmAddrxOffset) {
    index = raw >> 32;
    offset = raw & 0xffffffff;
  }

  llvm::DWARFUnit* table_unit = address_table_unit_ ? address_table_unit_ : value.unit;
  std::optional<llvm::object::SectionedAddress> entry =
      table_unit->getAddrOffsetSectionItem(static_cast<uint32_t>(index));
  if (!entry || entry->Address == kTombstone)
    return std::nullopt;
  std::optional<uint64_t> address = CheckedAdd(entry->Address, offset);
  if (!address || *address == kTombstone)
    return std::nullopt;
  return address;
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

std::optional<int128_t> DwarfDieDecoder::GetInteger(DwarfAttr attribute,
                                                    bool data_is_signed) const {
  const AttributeValue* value = GetValue(attribute);
  if (!value)
    return std::nullopt;

  const llvm::DWARFFormValue& form = value->form;
  switch (static_cast<DwarfForm>(form.getForm())) {
    case DwarfForm::kData1:
    case DwarfForm::kData2:
    case DwarfForm::kData4:
    case DwarfForm::kData8:
      if (!data_is_signed)
        break;
      [[fallthrough]];
    case DwarfForm::kSdata:
    case DwarfForm::kImplicitConst:
      // LLVM sign-extends the data forms.
      if (std::optional<int64_t> as_signed = form.getAsSignedConstant())
        return *as_signed;
      return std::nullopt;
    case DwarfForm::kUdata:
      break;
    default:
      return std::nullopt;
  }

  if (std::optional<uint64_t> as_unsigned = form.getAsUnsignedConstant())
    return *as_unsigned;
  return std::nullopt;
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
  } else if (form_code == DwarfForm::kSdata || form_code == DwarfForm::kImplicitConst) {
    if (std::optional<int64_t> opt_signed = form.getAsSignedConstant())
      return ConstValue(static_cast<int64_t>(*opt_signed));
  } else if (form_code == DwarfForm::kData1 || form_code == DwarfForm::kData2 ||
             form_code == DwarfForm::kData4 || form_code == DwarfForm::kData8) {
    // LLVM gives the value as an integer. Write its bytes in target byte order.
    std::optional<uint64_t> bits = form.getAsUnsignedConstant();
    if (!bits)
      return ConstValue();
    size_t size = form_code == DwarfForm::kData1   ? 1
                  : form_code == DwarfForm::kData2 ? 2
                  : form_code == DwarfForm::kData4 ? 4
                                                   : 8;
    bool little_endian = value->unit->getContext().isLittleEndian();
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; i++)
      bytes[little_endian ? i : size - 1 - i] = static_cast<uint8_t>(*bits >> (8 * i));
    return ConstValue(std::move(bytes));
  } else if (form_code == DwarfForm::kData16) {
    // LLVM keeps the 16 bytes as a block, in the byte order of the file.
    std::optional<llvm::ArrayRef<uint8_t>> block = form.getAsBlock();
    if (block && block->size() == 16)
      return ConstValue(std::vector<uint8_t>(block->begin(), block->end()));
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
    if (std::optional<uint64_t> as_constant = GetUnsignedConstant(DwarfAttr::kHighPc))
      return HighPC(true, *as_constant);
  } else if (DwarfFormIsAddress(form_code)) {
    if (std::optional<uint64_t> as_addr = DecodeAddress(*value))
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

  // Each value must end inside the unit. LLVM's skipValue() and extractValue() don't check the
  // lengths of blocks against the data, so the decoder checks the offset after each attribute.
  const uint64_t unit_end = unit->getNextUnitOffset();

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
      if (!llvm::DWARFFormValue::skipValue(spec.Form, extractor, &offset, unit->getFormParams()) ||
          offset > unit_end)
        return false;
      continue;
    }

    llvm::DWARFFormValue form_value(spec.Form);
    if (spec.isImplicitConst()) {
      // In the "implicit const" form, the value is stored in the abbreviation declaration which is
      // not passed in to the DWARFFormValue constructor or its extractValue() member. So this form
      // has to be specially decoded.
      form_value = llvm::DWARFFormValue::createFromSValue(spec.Form, spec.getImplicitConstValue());
    } else if (!form_value.extractValue(extractor, &offset, unit->getFormParams(), unit) ||
               offset > unit_end) {
      return false;
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
      // This is an offset in the .debug_info section of the referencing unit, so it can be used
      // across units. DWARFContext::getDIEForOffset() searches only the normal units, so a unit in
      // a .dwo file looks in the units of .debug_info.dwo itself.
      auto ref_value = form.getAsReferenceUVal();
      if (!ref_value)
        break;
      if (unit->isDWOUnit()) {
        if (llvm::DWARFUnit* target =
                unit->getContext().getDWOUnitsVector().getUnitForOffset(*ref_value))
          return target->getDIEForOffset(*ref_value);
        break;
      }
      return context_->getDIEForOffset(*ref_value);
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
