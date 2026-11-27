// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/form_value.h"

#include <inttypes.h>

#include <optional>
#include <utility>

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/form_encoding.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"

namespace zxdb {

namespace {

using Encoding = FormEncoding::Kind;

FormValue::Value ReadPayload(DwarfForm form, FormEncoding encoding, const FormParams& params,
                             DataReader& reader, int64_t implicit_const) {
  switch (encoding.kind) {
    case Encoding::kFixed:
      if (form == DwarfForm::kData16)
        return reader.Bytes(encoding.width);
      return reader.Fixed(encoding.width);
    case Encoding::kAddress:
      return reader.Address(params.address_size);
    case Encoding::kOffset:
      return reader.Offset(params.offset_size);
    case Encoding::kRefAddr:
      return params.version == 2 ? reader.Address(params.address_size)
                                 : reader.Offset(params.offset_size);
    case Encoding::kUleb:
      return reader.Uleb();
    case Encoding::kSleb:
      return reader.Sleb();
    case Encoding::kCString:
      return reader.CString();
    case Encoding::kBlockFixed:
      return reader.Bytes(reader.Fixed(encoding.width));
    case Encoding::kBlockUleb:
      return reader.Bytes(reader.Uleb());
    case Encoding::kZeroWidth:
      if (form == DwarfForm::kFlagPresent)
        return uint64_t{1};
      return implicit_const;
    case Encoding::kAddressIndexOffset:
      // LLVM 19 packs both fields into 64 bits. Keep the whole ULEB128 index separately.
      return FormValue::IndexedAddress{reader.Uleb(), static_cast<uint32_t>(reader.Fixed(4))};
    case Encoding::kIndirect:
    case Encoding::kInvalid:
      std::unreachable();
  }
  std::unreachable();
}

}  // namespace

std::optional<int128_t> FormValue::AsInteger(bool data_is_signed) const {
  if (form_class != Class::kConstant || form == DwarfForm::kData16)
    return std::nullopt;
  if (form == DwarfForm::kSdata || form == DwarfForm::kImplicitConst)
    return std::get<int64_t>(value);

  uint64_t bits = std::get<uint64_t>(value);
  if (data_is_signed) {
    switch (form) {
      case DwarfForm::kData1:
        return static_cast<int8_t>(bits);
      case DwarfForm::kData2:
        return static_cast<int16_t>(bits);
      case DwarfForm::kData4:
        return static_cast<int32_t>(bits);
      case DwarfForm::kData8:
        return static_cast<int64_t>(bits);
      default:
        break;
    }
  }
  return bits;
}

std::optional<uint64_t> FormValue::AsUnsignedConstant() const {
  if (form_class != Class::kConstant && form_class != Class::kFlag)
    return std::nullopt;
  switch (form) {
    case DwarfForm::kSdata:
      return std::nullopt;
    case DwarfForm::kImplicitConst:
      return static_cast<uint64_t>(std::get<int64_t>(value));
    case DwarfForm::kData16:
      return std::get<std::span<const uint8_t>>(value).size();
    default:
      return std::get<uint64_t>(value);
  }
}

std::optional<uint64_t> FormValue::AsSectionOffset(uint16_t version) const {
  switch (form) {
    case DwarfForm::kSecOffset:
    case DwarfForm::kStrp:
    case DwarfForm::kLineStrp:
    case DwarfForm::kLoclistx:
    case DwarfForm::kRnglistx:
      return std::get<uint64_t>(value);
    case DwarfForm::kData4:
    case DwarfForm::kData8:
      if (version <= 3)
        return std::get<uint64_t>(value);
      break;
    default:
      break;
  }
  return std::nullopt;
}

std::optional<uint64_t> FormValue::AsReference() const {
  if (form_class == Class::kReference)
    return std::get<uint64_t>(value);
  return std::nullopt;
}

std::optional<std::span<const uint8_t>> FormValue::AsBlock() const {
  if (const auto* bytes = std::get_if<std::span<const uint8_t>>(&value))
    return *bytes;
  return std::nullopt;
}

ErrOr<FormValue> ReadForm(const AbbrevTable::AttributeSpec& spec, const FormParams& params,
                          DataReader& reader) {
  const uint64_t start = reader.offset();
  auto error = [start](uint64_t form, const char* reason) {
    return Err("DWARF form 0x%" PRIx64 " at offset 0x%" PRIx64 ": %s", form, start, reason);
  };
  uint64_t resolved = spec.form;
  const bool indirect = spec.form == static_cast<uint64_t>(DwarfForm::kIndirect);
  if (indirect) {
    const auto form = ReadIndirectForm(reader);
    if (!form) {
      return error(spec.form,
                   reader.ok() ? "too many indirect forms" : "invalid indirect form code");
    }
    resolved = *form;
  }

  const auto [encoding, form_class] = DescribeForm(resolved, indirect);
  if (encoding.kind == Encoding::kInvalid)
    return error(resolved, "unknown form");
  const auto form = static_cast<DwarfForm>(resolved);
  if (form == DwarfForm::kImplicitConst && !indirect && !spec.implicit_const)
    return error(resolved, "missing implicit constant in abbreviation");

  auto value = ReadPayload(form, encoding, params, reader, spec.implicit_const.value_or(0));
  if (!reader.ok())
    return error(resolved, "invalid or truncated value");
  return FormValue{form, form_class, value};
}

}  // namespace zxdb
