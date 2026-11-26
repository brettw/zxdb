// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"

#include <lib/syslog/cpp/macros.h>

#include "src/developer/debug/zxdb/symbols/dwarf_form.h"

namespace zxdb {

namespace {

void WriteInteger(std::span<uint8_t> out, uint64_t value, Endian endian) {
  for (size_t i = 0; i < out.size(); i++) {
    size_t shift = endian == Endian::kLittle ? i : out.size() - 1 - i;
    out[i] = static_cast<uint8_t>(value >> (8 * shift));
  }
}

}  // namespace

void AppendInteger(std::vector<uint8_t>& out, uint64_t value, size_t size, Endian endian) {
  FX_DCHECK(size >= 1 && size <= 8);
  size_t offset = out.size();
  out.resize(offset + size);
  WriteInteger(std::span(out).subspan(offset), value, endian);
}

void AppendUleb(std::vector<uint8_t>& out, uint64_t value) {
  do {
    uint8_t byte = value & 0x7f;
    value >>= 7;
    if (value)
      byte |= 0x80;
    out.push_back(byte);
  } while (value);
}

void AppendUleb(std::vector<uint8_t>& out, DwarfForm form) {
  AppendUleb(out, static_cast<uint64_t>(form));
}

void AppendSleb(std::vector<uint8_t>& out, int64_t value) {
  bool more = true;
  while (more) {
    uint8_t byte = value & 0x7f;
    value >>= 7;
    if ((value == 0 && !(byte & 0x40)) || (value == -1 && (byte & 0x40)))
      more = false;
    else
      byte |= 0x80;
    out.push_back(byte);
  }
}

void AppendCString(std::vector<uint8_t>& out, std::string_view value) {
  if (!value.empty())
    out.insert(out.end(), value.begin(), value.end());
  out.push_back(0);
}

void DwarfSectionBuilder::Fixed(uint64_t value, size_t size) {
  AppendInteger(out_, value, size, endian_);
}

void DwarfSectionBuilder::Uleb(uint64_t value) { AppendUleb(out_, value); }

void DwarfSectionBuilder::Sleb(int64_t value) { AppendSleb(out_, value); }

void DwarfSectionBuilder::CString(std::string_view value) { AppendCString(out_, value); }

void DwarfSectionBuilder::Bytes(std::span<const uint8_t> value) {
  if (!value.empty())
    out_.insert(out_.end(), value.begin(), value.end());
}

void DwarfSectionBuilder::Offset(uint64_t value, size_t offset_size) {
  FX_DCHECK(offset_size == 4 || offset_size == 8);
  Fixed(value, offset_size);
}

void DwarfSectionBuilder::Address(uint64_t value, size_t address_size) {
  FX_DCHECK(address_size == 1 || address_size == 2 || address_size == 4 || address_size == 8);
  Fixed(value, address_size);
}

DwarfSectionBuilder::LengthMarker DwarfSectionBuilder::BeginInitialLength(size_t offset_size) {
  FX_DCHECK(offset_size == 4 || offset_size == 8);
  if (offset_size == 8)
    Fixed(0xffffffff, 4);
  size_t offset = out_.size();
  Fixed(0, offset_size);
  return LengthMarker(offset, offset_size);
}

void DwarfSectionBuilder::EndInitialLength(LengthMarker marker,
                                           std::optional<uint64_t> raw_length) {
  FX_DCHECK(marker.offset_ <= out_.size() && marker.offset_size_ <= out_.size() - marker.offset_);
  auto bytes = std::span(out_).subspan(marker.offset_);
  size_t length = bytes.size() - marker.offset_size_;
  FX_DCHECK(raw_length || marker.offset_size_ == 8 || length < 0xfffffff0);
  WriteInteger(bytes.first(marker.offset_size_), raw_length.value_or(length), endian_);
}

DwarfSectionBuilder::LengthMarker DwarfSectionBuilder::BeginUnit(const UnitHeaderOptions& options) {
  auto marker = BeginInitialLength(options.offset_size);
  Fixed(options.version, 2);
  uint8_t unit_type = options.unit_type;
  if (options.version >= 5) {
    Fixed(unit_type, 1);
    Fixed(options.address_size, 1);
    Offset(options.abbrev_offset, options.offset_size);
  } else {
    Offset(options.abbrev_offset, options.offset_size);
    Fixed(options.address_size, 1);
    unit_type = options.section == UnitSection::kTypes ? kUnitTypeType : kUnitTypeCompile;
  }

  if (unit_type == kUnitTypeType || unit_type == kUnitTypeSplitType) {
    Fixed(options.type_signature, 8);
    Offset(options.type_offset, options.offset_size);
  } else if (unit_type == kUnitTypeSkeleton || unit_type == kUnitTypeSplitCompile) {
    Fixed(options.dwo_id, 8);
  }
  return marker;
}

void DwarfSectionBuilder::AbbrevDeclaration(
    uint64_t code, uint64_t tag, bool has_children,
    std::span<const AbbrevTable::AttributeSpec> attributes) {
  FX_DCHECK(code != 0 && tag != 0);
  Uleb(code);
  Uleb(tag);
  Fixed(has_children ? 1 : 0, 1);
  for (const auto& spec : attributes) {
    FX_DCHECK(spec.attribute != 0 && spec.form != 0);
    FX_DCHECK(spec.implicit_const.has_value() ==
              (spec.form == static_cast<uint64_t>(DwarfForm::kImplicitConst)));
    Uleb(spec.attribute);
    Uleb(spec.form);
    if (spec.implicit_const)
      Sleb(*spec.implicit_const);
  }
  Uleb(0);
  Uleb(0);
}

uint64_t DwarfStringSectionBuilder::AddString(std::string_view value) {
  if (auto found = offsets_.find(value); found != offsets_.end())
    return found->second;
  uint64_t offset = bytes_.size();
  AppendCString(bytes_, value);
  offsets_.emplace(value, offset);
  return offset;
}

}  // namespace zxdb
