// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"

#include <lib/syslog/cpp/macros.h>

#include <algorithm>
#include <limits>
#include <utility>

#include "src/developer/debug/zxdb/common/endian.h"
#include "src/developer/debug/zxdb/symbols/const_value.h"
#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/range_list_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf_binary.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"
#include "src/developer/debug/zxdb/symbols/dwarf_sections_llvm.h"
#include "src/developer/debug/zxdb/symbols/line_file_table.h"

namespace zxdb {

DwarfDieDecoder::DwarfDieDecoder(const DwarfBinary& binary, std::span<const DwarfAttr> attributes,
                                 UnitContextSource address_table)
    : binary_(binary), address_table_(address_table), attributes_(attributes) {
  FX_CHECK(attributes_.size() <= kMaxAttributes);
}

DwarfDieDecoder::~DwarfDieDecoder() = default;

bool DwarfDieDecoder::Decode(const DwarfDie& die) {
  // Keep the input alive before clearing the old sources: a caller can pass a previous value's
  // source. Reuse the table reference when scanning multiple DIEs in the same table.
  if (sources_[0].table_ == die.table_)
    sources_[0].index_ = die.index_;
  else
    sources_[0] = die;
  for (size_t i = 0; i < attributes_.size(); i++)
    values_[i].source_ = nullptr;
  for (size_t i = 1; i < source_count_; i++)
    sources_[i] = {};
  source_count_ = 1;

  Links links;
  if (!DecodeOneDie(sources_[0], true, &links))
    return false;

  // Most DIEs reference no other DIE. They don't need the bookkeeping below.
  if (!links.abstract_origin.is_valid() && !links.specification.is_valid())
    return true;
  return DecodeLinkedDies(std::move(links));
}

bool DwarfDieDecoder::DecodeLinkedDies(Links first_links) {
  // The DIEs to visit are a stack. Each DIE pushes its specification and then its abstract origin,
  // so the abstract origin and the DIEs it references come before the specification.
  std::array<DwarfDie, 2 * kMaxDiesToVisit> pending;
  size_t pending_count = 0;
  auto push_links = [&pending, &pending_count](Links& links) {
    for (DwarfDie* next : {&links.specification, &links.abstract_origin}) {
      if (*next && pending_count < pending.size())
        pending[pending_count++] = std::move(*next);
    }
  };
  push_links(first_links);

  while (pending_count > 0 && source_count_ < sources_.size()) {
    DwarfDie current = std::move(pending[--pending_count]);
    auto sources_end = sources_.begin() + source_count_;
    if (std::find(sources_.begin(), sources_end, current) != sources_end)
      continue;  // Already visited, the symbols have a cycle.

    DwarfDie& source = sources_[source_count_++];
    source = std::move(current);
    Links links;
    if (!DecodeOneDie(source, false, &links))
      return false;
    push_links(links);
  }
  return true;
}

bool DwarfDieDecoder::Has(DwarfAttr attribute) const { return GetValue(attribute) != nullptr; }

const DwarfDieDecoder::AttributeValue* DwarfDieDecoder::GetValue(DwarfAttr attribute) const {
  size_t index = FindIndex(static_cast<uint64_t>(attribute));
  FX_DCHECK(index < attributes_.size())
      << DwarfAttrToString(attribute, true) << " is not in the decoder's table.";
  if (index >= attributes_.size() || !values_[index].source_)
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
  return binary_.GetUnitContext(value.die(), address_table_);
}

std::optional<const char*> DwarfDieDecoder::GetCString(DwarfAttr attribute) const {
  if (const AttributeValue* value = GetValue(attribute))
    return DecodeString(binary_, value->die(), value->form, address_table_);
  return std::nullopt;
}

std::optional<const char*> DwarfDieDecoder::DecodeString(const DwarfBinary& binary,
                                                         const DwarfDie& die, const FormValue& form,
                                                         UnitContextSource address_table) {
  if (form.form_class != FormValue::Class::kString)
    return std::nullopt;
  if (form.form == DwarfForm::kString)
    return std::get<std::string_view>(form.value).data();

  if (auto context = binary.GetUnitContext(die, address_table)) {
    if (auto string = context.value()->ResolveString(form))
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

AddressRanges DwarfDieDecoder::GetCodeRanges() const {
  if (auto range = GetLowHighEncodedCodeRange())
    return std::move(*range);

  const auto* value = GetValue(DwarfAttr::kRanges);
  if (!value)
    return {};
  auto form = value->form.form;
  if (form != DwarfForm::kRnglistx && form != DwarfForm::kSecOffset && form != DwarfForm::kData4 &&
      form != DwarfForm::kData8) {
    return {};
  }
  auto offset = value->form.AsSectionOffset(value->params.version);
  if (!offset)
    return {};

  auto context = GetAttributeUnitContext(*value);
  if (context.has_error())
    return {};

  auto decoded = form == DwarfForm::kRnglistx
                     ? RangeListReader::ReadFromIndex(*context.value(), *offset)
                     : RangeListReader::ReadFromOffset(*context.value(), *offset);
  if (decoded.has_error())
    return {};

  auto ranges = decoded.take_value();
  // The linker can zero the start of a range for removed code.
  std::erase_if(ranges, [](const AddressRange& range) { return range.begin() == 0; });
  return AddressRanges(AddressRanges::kNonCanonical, std::move(ranges));
}

std::optional<AddressRanges> DwarfDieDecoder::GetLowHighEncodedCodeRange() const {
  const DwarfDieDecoder::AttributeValue* low = GetValue(DwarfAttr::kLowPc);
  const DwarfDieDecoder::AttributeValue* high = GetValue(DwarfAttr::kHighPc);
  // Note that the "high pc" has an offset form so don't check that it has an FC_Address form yet.
  if (!low || !high || !DwarfFormIsAddress(low->form.form)) {
    return std::nullopt;
  }

  // The two attributes describe one range, so they must come from the same DIE. A DIE with only
  // DW_AT_low_pc doesn't get DW_AT_high_pc from its abstract origin. LLDB has the same rule.
  if (low->source_ != high->source_)
    return std::nullopt;

  // The low address must be a valid address. The decoder resolves address indices in the address
  // table of the skeleton unit for .dwo files.
  std::optional<uint64_t> low_addr = GetAddress(DwarfAttr::kLowPc);
  if (!low_addr)
    return std::nullopt;

  // The high PC is an address, or an offset from the low address.
  std::optional<DwarfDieDecoder::HighPC> high_pc = GetHighPC();
  if (!high_pc)
    return std::nullopt;
  uint64_t max_address =
      std::numeric_limits<uint64_t>::max() >> (64 - low->params.address_size * 8);
  if (*low_addr == 0 || *low_addr > max_address)
    return AddressRanges();
  if (high_pc->is_constant && high_pc->value > max_address - *low_addr)
    return AddressRanges();
  uint64_t high_addr = high_pc->is_constant ? *low_addr + high_pc->value : high_pc->value;
  if (high_addr <= *low_addr || high_addr > max_address)
    return AddressRanges();
  return AddressRanges(AddressRange(*low_addr, high_addr));
}

DwarfDie DwarfDieDecoder::GetReference(DwarfAttr attribute) const {
  if (const AttributeValue* value = GetValue(attribute))
    return DecodeReference(binary_, value->die(), value->form);
  return DwarfDie();
}

std::optional<std::string> DwarfDieDecoder::GetFile(DwarfAttr attribute) const {
  const AttributeValue* value = GetValue(attribute);
  if (!value)
    return std::nullopt;
  std::optional<uint64_t> file_index = value->form.AsUnsignedConstant();
  if (!file_index)
    return std::nullopt;

  auto files = binary_.GetLineFileTable(value->die(), *file_index);
  if (!files)
    return std::nullopt;

  if (const std::string* name = files->GetFileName(*file_index))
    return *name;
  return std::nullopt;
}

DwarfDie DwarfDieDecoder::GetContextParent() const {
  return source_count_ ? sources_[source_count_ - 1].parent() : DwarfDie();
}

size_t DwarfDieDecoder::FindIndex(uint64_t attribute) const {
  // The tables are small, so a linear search is faster than a map lookup.
  for (size_t i = 0; i < attributes_.size(); i++) {
    if (static_cast<uint64_t>(attributes_[i]) == attribute)
      return i;
  }
  return attributes_.size();
}

bool DwarfDieDecoder::DecodeOneDie(const DwarfDie& die, bool is_first, Links* links) {
  if (!die.belongs_to(binary_))
    return false;

  auto attributes = die.attributes();
  if (!attributes)
    return false;

  FormParams params = die.unit_header().form_params();
  Endian endian = die.table_->endian;
  DataReader reader(die.encoded_entry(), endian);
  reader.Uleb();  // The attributes follow the already validated abbreviation code.

  for (const auto& spec : *attributes) {
    // A value that is already filled came from a DIE that the decoder visited earlier, or from an
    // earlier duplicate of the attribute in this DIE. That value wins.
    uint64_t attr = spec.attribute;
    size_t index = FindIndex(attr);
    bool applies = is_first || (attr != static_cast<uint64_t>(DwarfAttr::kDeclaration) &&
                                attr != static_cast<uint64_t>(DwarfAttr::kSibling));
    bool wanted = applies && index < attributes_.size() && !values_[index].source_;

    // The links are decoded so Decode() can follow them. The first resolved link of each kind in
    // the DIE is used.
    bool is_abstract_origin = follow_abstract_origins_ &&
                              attr == static_cast<uint64_t>(DwarfAttr::kAbstractOrigin) &&
                              !links->abstract_origin;
    bool is_specification = follow_specifications_ &&
                            attr == static_cast<uint64_t>(DwarfAttr::kSpecification) &&
                            !links->specification;

    if (!wanted && !is_abstract_origin && !is_specification) {
      if (!SkipForm(spec.form, params, reader))
        return false;
      continue;
    }

    auto form = ReadForm(spec, params, reader);
    if (!form)
      return false;
    if (is_abstract_origin)
      links->abstract_origin = DecodeReference(binary_, die, form.value());
    if (is_specification)
      links->specification = DecodeReference(binary_, die, form.value());
    if (wanted) {
      AttributeValue& value = values_[index];
      value.form = form.take_value();
      value.params = params;
      value.endian = endian;
      value.source_ = &die;
    }
  }
  return true;
}

DwarfDie DwarfDieDecoder::DecodeReference(const DwarfBinary& binary, const DwarfDie& die,
                                          const FormValue& form) {
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
      return binary.GetDieAtOffset(DwarfDieRef::Main(*ref_value), die.unit_is_dwo());
    case DwarfForm::kRefSig8:
      return binary.GetDieForSignature(*ref_value, die.unit_is_dwo());
    default:
      // Supplementary references require a separate symbol file.
      return {};
  }
}

std::optional<FormValue> DwarfDieDecoder::ReadDirectAttribute(const DwarfDie& die,
                                                              std::span<const DwarfAttr> attrs) {
  const auto attributes = die.attributes();
  if (!attributes)
    return std::nullopt;
  auto spec = attributes->end();
  for (DwarfAttr attr : attrs) {
    spec = std::ranges::find(*attributes, static_cast<uint64_t>(attr),
                             &AbbrevTable::AttributeSpec::attribute);
    if (spec != attributes->end())
      break;
  }
  if (spec == attributes->end())
    return std::nullopt;

  // The DIE table has already checked the entire entry. Read only as far as this attribute.
  const FormParams params = die.unit_header().form_params();
  DataReader reader(die.encoded_entry(), die.table_->endian);
  reader.Uleb();
  for (auto prefix = attributes->begin(); prefix != spec; ++prefix) {
    if (!SkipForm(prefix->form, params, reader))
      return std::nullopt;
  }
  auto form = ReadForm(*spec, params, reader);
  return form ? std::optional(form.take_value()) : std::nullopt;
}

}  // namespace zxdb
