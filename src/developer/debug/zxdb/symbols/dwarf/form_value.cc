// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/form_value.h"

#include <inttypes.h>

#include <limits>
#include <optional>
#include <utility>

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"

namespace zxdb {

namespace {

enum class Encoding {
  kFixed,
  kAddress,
  kOffset,
  kUleb,
  kSleb,
  kCString,
  kBytes,
  kBlockFixed,
  kBlockUleb,
  kFlag,
  kFlagPresent,
  kImplicitConst,
  kAddressIndexOffset,
};

struct FormEncoding {
  Encoding encoding;
  FormValue::Class form_class;
  uint8_t width = 0;  // Byte width for fixed-size payloads and length fields.
};

// LLVM 19's skipValue and extractValue accept known encodings without checking when a form
// was introduced. LLDB also selects the encoding from the form code. Use the version for
// DW_FORM_ref_addr's width and leave attribute-specific interpretation to the caller.
std::optional<FormEncoding> GetFormEncoding(uint64_t form, uint16_t version, bool indirect) {
  if (form > std::numeric_limits<int>::max())
    return std::nullopt;

  using Class = FormValue::Class;
  switch (static_cast<DwarfForm>(form)) {
    case DwarfForm::kAddr:
      return FormEncoding{Encoding::kAddress, Class::kAddress};
    case DwarfForm::kRefAddr:
      return FormEncoding{version == 2 ? Encoding::kAddress : Encoding::kOffset, Class::kReference};
    case DwarfForm::kStrp:
    case DwarfForm::kLineStrp:
    case DwarfForm::kStrpSup:
    case DwarfForm::kGnuStrpAlt:
      return FormEncoding{Encoding::kOffset, Class::kString};
    case DwarfForm::kSecOffset:
      return FormEncoding{Encoding::kOffset, Class::kSectionOffset};
    case DwarfForm::kGnuRefAlt:
      return FormEncoding{Encoding::kOffset, Class::kReference};
    case DwarfForm::kRef1:
      return FormEncoding{Encoding::kFixed, Class::kReference, 1};
    case DwarfForm::kRef2:
      return FormEncoding{Encoding::kFixed, Class::kReference, 2};
    case DwarfForm::kRef4:
    case DwarfForm::kRefSup4:
      return FormEncoding{Encoding::kFixed, Class::kReference, 4};
    case DwarfForm::kRef8:
    case DwarfForm::kRefSig8:
    case DwarfForm::kRefSup8:
      return FormEncoding{Encoding::kFixed, Class::kReference, 8};
    case DwarfForm::kRefUdata:
      return FormEncoding{Encoding::kUleb, Class::kReference};
    case DwarfForm::kData1:
      return FormEncoding{Encoding::kFixed, Class::kConstant, 1};
    case DwarfForm::kData2:
      return FormEncoding{Encoding::kFixed, Class::kConstant, 2};
    // In DWARF 2 and 3, the attribute can also give these forms the meaning of a section offset.
    case DwarfForm::kData4:
      return FormEncoding{Encoding::kFixed, Class::kConstant, 4};
    case DwarfForm::kData8:
      return FormEncoding{Encoding::kFixed, Class::kConstant, 8};
    case DwarfForm::kData16:
      return FormEncoding{Encoding::kBytes, Class::kConstant, 16};
    case DwarfForm::kUdata:
      return FormEncoding{Encoding::kUleb, Class::kConstant};
    case DwarfForm::kSdata:
      return FormEncoding{Encoding::kSleb, Class::kConstant};
    case DwarfForm::kImplicitConst:
      // DWARF 5 errata for section 7.5.3 puts the SLEB128 in the DIE after an indirect form code.
      // LLVM 19 always takes the constant from the abbreviation, which only handles the direct
      // case.
      return FormEncoding{indirect ? Encoding::kSleb : Encoding::kImplicitConst, Class::kConstant};
    case DwarfForm::kBlock1:
      return FormEncoding{Encoding::kBlockFixed, Class::kBlock, 1};
    case DwarfForm::kBlock2:
      return FormEncoding{Encoding::kBlockFixed, Class::kBlock, 2};
    case DwarfForm::kBlock4:
      return FormEncoding{Encoding::kBlockFixed, Class::kBlock, 4};
    case DwarfForm::kBlock:
      return FormEncoding{Encoding::kBlockUleb, Class::kBlock};
    case DwarfForm::kExprloc:
      return FormEncoding{Encoding::kBlockUleb, Class::kExprloc};
    case DwarfForm::kFlag:
      return FormEncoding{Encoding::kFlag, Class::kFlag, 1};
    case DwarfForm::kFlagPresent:
      return FormEncoding{Encoding::kFlagPresent, Class::kFlag};
    case DwarfForm::kString:
      return FormEncoding{Encoding::kCString, Class::kString};
    case DwarfForm::kStrx:
    case DwarfForm::kGnuStrIndex:
      return FormEncoding{Encoding::kUleb, Class::kString};
    case DwarfForm::kStrx1:
      return FormEncoding{Encoding::kFixed, Class::kString, 1};
    case DwarfForm::kStrx2:
      return FormEncoding{Encoding::kFixed, Class::kString, 2};
    case DwarfForm::kStrx3:
      return FormEncoding{Encoding::kFixed, Class::kString, 3};
    case DwarfForm::kStrx4:
      return FormEncoding{Encoding::kFixed, Class::kString, 4};
    case DwarfForm::kAddrx:
    case DwarfForm::kGnuAddrIndex:
      return FormEncoding{Encoding::kUleb, Class::kAddress};
    case DwarfForm::kAddrx1:
      return FormEncoding{Encoding::kFixed, Class::kAddress, 1};
    case DwarfForm::kAddrx2:
      return FormEncoding{Encoding::kFixed, Class::kAddress, 2};
    case DwarfForm::kAddrx3:
      return FormEncoding{Encoding::kFixed, Class::kAddress, 3};
    case DwarfForm::kAddrx4:
      return FormEncoding{Encoding::kFixed, Class::kAddress, 4};
    case DwarfForm::kLoclistx:
      return FormEncoding{Encoding::kUleb, Class::kLoclist};
    case DwarfForm::kRnglistx:
      return FormEncoding{Encoding::kUleb, Class::kRnglist};
    case DwarfForm::kLlvmAddrxOffset:
      return FormEncoding{Encoding::kAddressIndexOffset, Class::kAddress};
    default:
      return std::nullopt;
  }
}

std::optional<uint64_t> ResolveForm(uint64_t form, DataReader& reader) {
  // LLVM 19 follows indirect forms without a depth limit. LLDB's skip recurses. Limit the
  // work here and keep each full form code so it cannot narrow to a different form.
  constexpr unsigned kMaxIndirectDepth = 8;
  for (unsigned depth = 0; form == static_cast<uint64_t>(DwarfForm::kIndirect); ++depth) {
    if (depth == kMaxIndirectDepth)
      return std::nullopt;
    form = reader.Uleb();
    if (!reader.ok())
      return std::nullopt;
  }
  return form;
}

// DataReader checks payload lengths against the remaining bytes, including lengths that
// could wrap when added to the current offset. LLVM and LLDB skip fixed and block payloads
// by advancing the offset directly.
FormValue::Value ReadPayload(const FormEncoding& encoding, const FormParams& params,
                             DataReader& reader, int64_t implicit_const) {
  switch (encoding.encoding) {
    case Encoding::kFixed:
      return reader.Fixed(encoding.width);
    case Encoding::kAddress:
      return reader.Address(params.address_size);
    case Encoding::kOffset:
      return reader.Offset(params.offset_size);
    case Encoding::kUleb:
      return reader.Uleb();
    case Encoding::kSleb:
      return reader.Sleb();
    case Encoding::kCString:
      return reader.CString();
    case Encoding::kBytes:
      return reader.Bytes(encoding.width);
    case Encoding::kBlockFixed:
      return reader.Bytes(reader.Fixed(encoding.width));
    case Encoding::kBlockUleb:
      return reader.Bytes(reader.Uleb());
    case Encoding::kFlag:
      return reader.Fixed(encoding.width) != 0;
    case Encoding::kFlagPresent:
      return true;
    case Encoding::kImplicitConst:
      return implicit_const;
    case Encoding::kAddressIndexOffset:
      // LLVM 19 packs both fields into 64 bits. Keep the whole ULEB128 index separately.
      return FormValue::IndexedAddress{reader.Uleb(), static_cast<uint32_t>(reader.Fixed(4))};
  }
  std::unreachable();
}

}  // namespace

bool SkipForm(uint64_t form, const FormParams& params, DataReader& reader) {
  const auto resolved = ResolveForm(form, reader);
  if (!resolved)
    return false;
  const auto encoding = GetFormEncoding(*resolved, params.version, *resolved != form);
  if (!encoding)
    return false;

  switch (encoding->encoding) {
    case Encoding::kFixed:
    case Encoding::kBytes:
    case Encoding::kFlag:
      reader.Skip(encoding->width);
      break;
    case Encoding::kFlagPresent:
    case Encoding::kImplicitConst:
      break;
    default:
      ReadPayload(*encoding, params, reader, 0);
      break;
  }
  return reader.ok();
}

ErrOr<FormValue> ReadForm(const AbbrevTable::AttributeSpec& spec, const FormParams& params,
                          DataReader& reader) {
  const uint64_t start = reader.offset();
  auto error = [start](uint64_t form, const char* reason) {
    return Err("DWARF form 0x%" PRIx64 " at offset 0x%" PRIx64 ": %s", form, start, reason);
  };
  const auto resolved = ResolveForm(spec.form, reader);
  if (!resolved) {
    return error(spec.form, reader.ok() ? "too many indirect forms" : "invalid indirect form code");
  }

  const auto encoding = GetFormEncoding(*resolved, params.version, *resolved != spec.form);
  if (!encoding)
    return error(*resolved, "unknown form");
  if (encoding->encoding == Encoding::kImplicitConst && !spec.implicit_const)
    return error(*resolved, "missing implicit constant in abbreviation");

  auto value = ReadPayload(*encoding, params, reader, spec.implicit_const.value_or(0));
  if (!reader.ok())
    return error(*resolved, "invalid or truncated value");
  return FormValue{static_cast<DwarfForm>(*resolved), encoding->form_class, value};
}

}  // namespace zxdb
