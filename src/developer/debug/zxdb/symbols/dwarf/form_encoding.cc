// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/form_encoding.h"

#include <limits>

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"

namespace zxdb {

using Encoding = FormEncoding::Kind;

// LLVM 19's skipValue and extractValue accept known encodings without checking when a form
// was introduced. LLDB also selects the encoding from the form code.
FormDescription DescribeForm(uint64_t form, bool indirect) {
  if (form > std::numeric_limits<int>::max())
    return {{Encoding::kInvalid}};

  using Class = FormClass;
  switch (static_cast<DwarfForm>(form)) {
    case DwarfForm::kAddr:
      return {{Encoding::kAddress}, Class::kAddress};
    case DwarfForm::kRefAddr:
      return {{Encoding::kRefAddr}, Class::kReference};
    case DwarfForm::kStrp:
    case DwarfForm::kLineStrp:
    case DwarfForm::kStrpSup:
    case DwarfForm::kGnuStrpAlt:
      return {{Encoding::kOffset}, Class::kString};
    case DwarfForm::kSecOffset:
      return {{Encoding::kOffset}, Class::kSectionOffset};
    case DwarfForm::kGnuRefAlt:
      return {{Encoding::kOffset}, Class::kReference};
    case DwarfForm::kRef1:
      return {{Encoding::kFixed, 1}, Class::kReference};
    case DwarfForm::kRef2:
      return {{Encoding::kFixed, 2}, Class::kReference};
    case DwarfForm::kRef4:
    case DwarfForm::kRefSup4:
      return {{Encoding::kFixed, 4}, Class::kReference};
    case DwarfForm::kRef8:
    case DwarfForm::kRefSig8:
    case DwarfForm::kRefSup8:
      return {{Encoding::kFixed, 8}, Class::kReference};
    case DwarfForm::kRefUdata:
      return {{Encoding::kUleb}, Class::kReference};
    case DwarfForm::kData1:
      return {{Encoding::kFixed, 1}, Class::kConstant};
    case DwarfForm::kData2:
      return {{Encoding::kFixed, 2}, Class::kConstant};
    // In DWARF 2 and 3, the attribute can also give these forms the meaning of a section offset.
    case DwarfForm::kData4:
      return {{Encoding::kFixed, 4}, Class::kConstant};
    case DwarfForm::kData8:
      return {{Encoding::kFixed, 8}, Class::kConstant};
    case DwarfForm::kData16:
      return {{Encoding::kFixed, 16}, Class::kConstant};
    case DwarfForm::kUdata:
      return {{Encoding::kUleb}, Class::kConstant};
    case DwarfForm::kSdata:
      return {{Encoding::kSleb}, Class::kConstant};
    case DwarfForm::kImplicitConst:
      // DWARF 5 errata for section 7.5.3 puts the SLEB128 in the DIE after an indirect form code.
      // LLVM 19 always takes the constant from the abbreviation, which only handles the direct
      // case.
      return {{indirect ? Encoding::kSleb : Encoding::kZeroWidth}, Class::kConstant};
    case DwarfForm::kBlock1:
      return {{Encoding::kBlockFixed, 1}, Class::kBlock};
    case DwarfForm::kBlock2:
      return {{Encoding::kBlockFixed, 2}, Class::kBlock};
    case DwarfForm::kBlock4:
      return {{Encoding::kBlockFixed, 4}, Class::kBlock};
    case DwarfForm::kBlock:
      return {{Encoding::kBlockUleb}, Class::kBlock};
    case DwarfForm::kExprloc:
      return {{Encoding::kBlockUleb}, Class::kExprloc};
    case DwarfForm::kFlag:
      return {{Encoding::kFixed, 1}, Class::kFlag};
    case DwarfForm::kFlagPresent:
      return {{Encoding::kZeroWidth}, Class::kFlag};
    case DwarfForm::kString:
      return {{Encoding::kCString}, Class::kString};
    case DwarfForm::kStrx:
    case DwarfForm::kGnuStrIndex:
      return {{Encoding::kUleb}, Class::kString};
    case DwarfForm::kStrx1:
      return {{Encoding::kFixed, 1}, Class::kString};
    case DwarfForm::kStrx2:
      return {{Encoding::kFixed, 2}, Class::kString};
    case DwarfForm::kStrx3:
      return {{Encoding::kFixed, 3}, Class::kString};
    case DwarfForm::kStrx4:
      return {{Encoding::kFixed, 4}, Class::kString};
    case DwarfForm::kAddrx:
    case DwarfForm::kGnuAddrIndex:
      return {{Encoding::kUleb}, Class::kAddress};
    case DwarfForm::kAddrx1:
      return {{Encoding::kFixed, 1}, Class::kAddress};
    case DwarfForm::kAddrx2:
      return {{Encoding::kFixed, 2}, Class::kAddress};
    case DwarfForm::kAddrx3:
      return {{Encoding::kFixed, 3}, Class::kAddress};
    case DwarfForm::kAddrx4:
      return {{Encoding::kFixed, 4}, Class::kAddress};
    case DwarfForm::kLoclistx:
      return {{Encoding::kUleb}, Class::kLoclist};
    case DwarfForm::kRnglistx:
      return {{Encoding::kUleb}, Class::kRnglist};
    case DwarfForm::kLlvmAddrxOffset:
      return {{Encoding::kAddressIndexOffset}, Class::kAddress};
    case DwarfForm::kIndirect:
      return {{Encoding::kIndirect}};
    default:
      return {{Encoding::kInvalid}};
  }
}

std::optional<uint64_t> ReadIndirectForm(DataReader& reader) {
  // LLVM 19 follows indirect forms without a depth limit. LLDB's skip recurses. Limit the
  // work here and keep each full form code so it cannot narrow to a different form.
  for (unsigned depth = 0; depth < 8; ++depth) {
    const uint64_t form = reader.Uleb();
    if (!reader.ok())
      return std::nullopt;
    if (form != static_cast<uint64_t>(DwarfForm::kIndirect))
      return form;
  }
  return std::nullopt;
}

// DataReader checks payload lengths against the remaining bytes, including lengths that
// could wrap when added to the current offset. LLVM and LLDB skip fixed and block payloads
// by advancing the offset directly.
bool SkipForm(FormEncoding encoding, const FormParams& params, DataReader& reader) {
  if (encoding.kind == Encoding::kIndirect) {
    const auto form = ReadIndirectForm(reader);
    if (!form)
      return false;
    encoding = DescribeForm(*form, true).encoding;
  }

  switch (encoding.kind) {
    case Encoding::kFixed:
      reader.Skip(encoding.width);
      break;
    case Encoding::kAddress:
      reader.Address(params.address_size);
      break;
    case Encoding::kOffset:
      reader.Offset(params.offset_size);
      break;
    case Encoding::kRefAddr:
      if (params.version == 2)
        reader.Address(params.address_size);
      else
        reader.Offset(params.offset_size);
      break;
    case Encoding::kUleb:
      reader.SkipUleb();
      break;
    case Encoding::kSleb:
      reader.SkipSleb();
      break;
    case Encoding::kCString:
      reader.CString();
      break;
    case Encoding::kBlockFixed:
      reader.Skip(reader.Fixed(encoding.width));
      break;
    case Encoding::kBlockUleb:
      reader.Skip(reader.Uleb());
      break;
    case Encoding::kAddressIndexOffset:
      reader.SkipUleb();
      reader.Skip(4);
      break;
    case Encoding::kZeroWidth:
      break;
    case Encoding::kIndirect:
    case Encoding::kInvalid:
      return false;
  }
  return reader.ok();
}

bool SkipForm(uint64_t form, const FormParams& params, DataReader& reader) {
  return SkipForm(DescribeForm(form).encoding, params, reader);
}

}  // namespace zxdb
