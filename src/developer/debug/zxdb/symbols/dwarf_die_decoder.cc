// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"

#include <lib/syslog/cpp/macros.h>

#include <algorithm>
#include <limits>

#include "src/developer/debug/zxdb/common/endian.h"
#include "src/developer/debug/zxdb/symbols/const_value.h"
#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf_binary.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"
#include "src/developer/debug/zxdb/symbols/dwarf_sections_llvm.h"
#include "src/developer/debug/zxdb/symbols/line_file_table.h"

namespace zxdb {

namespace {

// The maximum number of DIEs that one decode visits: the decoded DIE and up to 8 DIEs that it
// references directly or indirectly. Valid symbols rarely need more than 3. The limit also bounds
// the work for corrupt symbols.
constexpr size_t kMaxDiesToVisit = 9;

}  // namespace

DwarfDieDecoder::DwarfDieDecoder(const DwarfBinary& binary, std::span<const DwarfAttr> attributes,
                                 UnitContextSource address_table)
    : binary_(binary), address_table_(address_table), attributes_(attributes) {
  FX_CHECK(attributes_.size() <= kMaxAttributes);
}

DwarfDieDecoder::~DwarfDieDecoder() = default;

bool DwarfDieDecoder::Decode(DwarfDie die) {
  for (size_t i = 0; i < attributes_.size(); i++)
    values_[i] = AttributeValue();

  Links links;
  if (!DecodeOneDie(die, true, &links))
    return false;
  context_die_ = die;

  // Most DIEs reference no other DIE. They don't need the bookkeeping below.
  if (!links.abstract_origin.is_valid() && !links.specification.is_valid())
    return true;
  return DecodeLinkedDies(die, links);
}

bool DwarfDieDecoder::DecodeLinkedDies(DwarfDie first, const Links& first_links) {
  // The DIEs to visit are a stack. Each DIE pushes its specification and then its abstract origin,
  // so the abstract origin and the DIEs it references come before the specification.
  std::array<DwarfDie, kMaxDiesToVisit> visited;
  size_t visited_count = 0;
  visited[visited_count++] = first;

  std::array<DwarfDie, 2 * kMaxDiesToVisit> pending;
  size_t pending_count = 0;
  auto push_links = [&pending, &pending_count](const Links& links) {
    for (const DwarfDie& next : {links.specification, links.abstract_origin}) {
      if (next.is_valid() && pending_count < pending.size())
        pending[pending_count++] = next;
    }
  };
  push_links(first_links);

  while (pending_count > 0 && visited_count < kMaxDiesToVisit) {
    DwarfDie cur = pending[--pending_count];
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
  size_t index = FindIndex(static_cast<uint64_t>(attribute));
  FX_DCHECK(index < attributes_.size())
      << DwarfAttrToString(attribute, true) << " is not in the decoder's table.";
  if (index >= attributes_.size() || !values_[index].die)
    return nullptr;
  return &values_[index];
}

std::optional<bool> DwarfDieDecoder::GetBool(DwarfAttr attribute) const {
  if (const AttributeValue* value = GetValue(attribute)) {
    if (std::optional<uint64_t> as_unsigned = value->form.AsUnsignedConstant())
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

  if (value.form.form_class != FormValue::Class::kAddress)
    return std::nullopt;

  std::optional<uint64_t> address;
  if (value.form.form == DwarfForm::kAddr) {
    address = std::get<uint64_t>(value.form.value);
  } else {
    // An explicitly supplied address table takes precedence over the unit's table. A DWO
    // without an explicit provider uses its linked skeleton, when present.
    auto context = address_table_.unit
                       ? GetUnitContext(*address_table_.unit,
                                        address_table_.binary ? address_table_.binary : &binary_)
                       : GetAttributeUnitContext(value);
    if (context)
      address = context.value()->ResolveAddress(value.form);
  }
  if (address == kTombstone)
    return std::nullopt;
  return address;
}

UnitContextResult DwarfDieDecoder::GetAttributeUnitContext(const AttributeValue& value) const {
  return binary_.GetUnitContext(*value.die.llvm_unit(), address_table_);
}

std::optional<const char*> DwarfDieDecoder::GetCString(DwarfAttr attribute) const {
  const AttributeValue* value = GetValue(attribute);
  if (!value || value->form.form_class != FormValue::Class::kString)
    return std::nullopt;
  if (value->form.form == DwarfForm::kString)
    return std::get<std::string_view>(value->form.value).data();

  if (auto context = GetAttributeUnitContext(*value)) {
    if (auto string = context.value()->ResolveString(value->form))
      return string->data();
  }
  return std::nullopt;
}

std::optional<uint64_t> DwarfDieDecoder::GetSectionOffset(DwarfAttr attribute) const {
  if (const AttributeValue* value = GetValue(attribute))
    return value->form.AsSectionOffset(value->params.version);
  return std::nullopt;
}

std::optional<std::vector<uint8_t>> DwarfDieDecoder::GetBlock(DwarfAttr attribute) const {
  const AttributeValue* value = GetValue(attribute);
  if (!value)
    return std::nullopt;
  if (value->form.form_class != FormValue::Class::kBlock &&
      value->form.form_class != FormValue::Class::kExprloc)
    return std::nullopt;

  auto block = *value->form.AsBlock();
  return std::vector<uint8_t>(block.begin(), block.end());
}

std::optional<int128_t> DwarfDieDecoder::GetInteger(DwarfAttr attribute,
                                                    bool data_is_signed) const {
  if (const AttributeValue* value = GetValue(attribute))
    return value->form.AsInteger(data_is_signed);
  return std::nullopt;
}

ConstValue DwarfDieDecoder::GetConstValue(DwarfAttr attribute) const {
  const AttributeValue* value = GetValue(attribute);
  if (!value)
    return ConstValue();

  const FormValue& form = value->form;
  DwarfForm form_code = form.form;
  if (form_code == DwarfForm::kUdata) {
    return ConstValue(std::get<uint64_t>(form.value));
  } else if (form_code == DwarfForm::kSdata || form_code == DwarfForm::kImplicitConst) {
    return ConstValue(std::get<int64_t>(form.value));
  } else if (form_code == DwarfForm::kData1 || form_code == DwarfForm::kData2 ||
             form_code == DwarfForm::kData4 || form_code == DwarfForm::kData8) {
    // Write the integer's bytes in target byte order.
    uint64_t bits = std::get<uint64_t>(form.value);
    size_t size = form_code == DwarfForm::kData1   ? 1
                  : form_code == DwarfForm::kData2 ? 2
                  : form_code == DwarfForm::kData4 ? 4
                                                   : 8;
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; i++)
      bytes[value->endian == Endian::kLittle ? i : size - 1 - i] =
          static_cast<uint8_t>(bits >> (8 * i));
    return ConstValue(std::move(bytes));
  } else if (form_code == DwarfForm::kData16) {
    auto block = form.AsBlock();
    if (block && block->size() == 16)
      return ConstValue(std::vector<uint8_t>(block->begin(), block->end()));
  } else if (DwarfFormIsBlock(form_code)) {
    auto block = form.AsBlock();
    if (block && !block->empty())
      return ConstValue(std::vector<uint8_t>(block->begin(), block->end()));
  }
  return ConstValue();
}

std::optional<DwarfDieDecoder::HighPC> DwarfDieDecoder::GetHighPC() const {
  const AttributeValue* value = GetValue(DwarfAttr::kHighPc);
  if (!value)
    return std::nullopt;

  if (value->form.form_class == FormValue::Class::kConstant) {
    if (std::optional<uint64_t> as_constant = GetUnsignedConstant(DwarfAttr::kHighPc))
      return HighPC(true, *as_constant);
  } else if (value->form.form_class == FormValue::Class::kAddress) {
    if (std::optional<uint64_t> as_addr = DecodeAddress(*value))
      return HighPC(false, *as_addr);
  }
  return std::nullopt;
}

DwarfDie DwarfDieDecoder::GetReference(DwarfAttr attribute) const {
  if (const AttributeValue* value = GetValue(attribute))
    return DecodeReference(value->die, value->form);
  return DwarfDie();
}

std::optional<std::string> DwarfDieDecoder::GetFile(DwarfAttr attribute) const {
  const AttributeValue* value = GetValue(attribute);
  if (!value)
    return std::nullopt;
  std::optional<uint64_t> file_index = value->form.AsUnsignedConstant();
  if (!file_index)
    return std::nullopt;

  auto files = binary_.GetLineFileTable(value->die.llvm_unit(), *file_index);
  if (!files)
    return std::nullopt;

  if (const std::string* name = files->GetFileName(*file_index))
    return *name;
  return std::nullopt;
}

DwarfDie DwarfDieDecoder::GetContextParent() const {
  if (!context_die_.is_valid())
    return DwarfDie();
  return context_die_.parent();
}

size_t DwarfDieDecoder::FindIndex(uint64_t attribute) const {
  // The tables are small, so a linear search is faster than a map lookup.
  for (size_t i = 0; i < attributes_.size(); i++) {
    if (static_cast<uint64_t>(attributes_[i]) == attribute)
      return i;
  }
  return attributes_.size();
}

bool DwarfDieDecoder::DecodeOneDie(DwarfDie die, bool is_first, Links* links) {
  if (!die)
    return false;

  const UnitAttributeData* data = binary_.GetUnitAttributeData(die);
  if (!data || die.offset() < data->first_die_offset)
    return false;

  // The view stops at the unit end. The attributes follow the DIE's abbreviation code.
  DataReader reader(data->info, data->endian);
  reader.Seek(die.offset() - data->first_die_offset);
  uint64_t code = reader.Uleb();
  if (!reader.ok() || !code)
    return false;
  const auto* abbrev = data->abbrevs->Find(code);
  if (!abbrev)
    return false;

  for (const auto& spec : abbrev->attributes) {
    // A value that is already filled came from a DIE that the decoder visited earlier, or from an
    // earlier duplicate of the attribute in this DIE. That value wins.
    uint64_t attr = spec.attribute;
    size_t index = FindIndex(attr);
    bool applies = is_first || (attr != static_cast<uint64_t>(DwarfAttr::kDeclaration) &&
                                attr != static_cast<uint64_t>(DwarfAttr::kSibling));
    bool wanted = applies && index < attributes_.size() && !values_[index].die;

    // The links are decoded so Decode() can follow them. The first resolved link of each kind in
    // the DIE is used.
    bool is_abstract_origin = follow_abstract_origins_ &&
                              attr == static_cast<uint64_t>(DwarfAttr::kAbstractOrigin) &&
                              !links->abstract_origin;
    bool is_specification = follow_specifications_ &&
                            attr == static_cast<uint64_t>(DwarfAttr::kSpecification) &&
                            !links->specification;

    if (!wanted && !is_abstract_origin && !is_specification) {
      if (!SkipForm(spec.form, data->params, reader))
        return false;
      continue;
    }

    auto form = ReadForm(spec, data->params, reader);
    if (!form)
      return false;
    if (is_abstract_origin)
      links->abstract_origin = DecodeReference(die, form.value());
    if (is_specification)
      links->specification = DecodeReference(die, form.value());
    if (wanted)
      values_[index] = {die, form.take_value(), data->params, data->endian};
  }
  return true;
}

DwarfDie DwarfDieDecoder::DecodeReference(DwarfDie die, const FormValue& form) const {
  auto ref_value = form.AsReference();
  if (!ref_value)
    return {};

  switch (form.form) {
    case DwarfForm::kRef1:
    case DwarfForm::kRef2:
    case DwarfForm::kRef4:
    case DwarfForm::kRef8:
    case DwarfForm::kRefUdata:
      return die.GetUnitRelativeDie(*ref_value);
    case DwarfForm::kRefAddr:
      // Section-relative references can cross units in the source file's info section.
      return binary_.GetDieAtOffset(DwarfDieRef::Main(*ref_value), die.unit_is_dwo());
    case DwarfForm::kRefSig8:
      return binary_.GetDieForSignature(*ref_value, die.unit_is_dwo());
    default:
      // Supplementary references require a separate symbol file.
      return {};
  }
}

DwarfDie GetAbstractOrigin(DwarfDie die, const DwarfBinary& binary) {
  if (!die)
    return {};

  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kAbstractOrigin};
  DwarfDieDecoder decoder(binary, kAttrs);
  decoder.set_follow_abstract_origins(false);
  decoder.set_follow_specifications(false);
  if (!decoder.Decode(die))
    return {};
  return decoder.GetReference(DwarfAttr::kAbstractOrigin);
}

}  // namespace zxdb
