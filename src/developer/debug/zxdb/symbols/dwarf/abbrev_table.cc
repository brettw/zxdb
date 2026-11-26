// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/abbrev_table.h"

#include <inttypes.h>

#include <algorithm>
#include <limits>
#include <utility>

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"

namespace zxdb {

bool AbbrevTable::Declaration::FixedSize::AddForm(uint64_t form) {
  if (form > std::numeric_limits<int>::max())
    return false;

  // The declaration has already stored this attribute. Each stored specification occupies
  // at least as many bytes as the largest fixed-width form (DW_FORM_data16).
  static_assert(sizeof(AttributeSpec) >= 16);
  switch (static_cast<DwarfForm>(form)) {
    case DwarfForm::kAddr:
      ++addresses;
      return true;
    case DwarfForm::kRefAddr:
      ++ref_addrs;
      return true;
    case DwarfForm::kStrp:
    case DwarfForm::kSecOffset:
    case DwarfForm::kStrpSup:
    case DwarfForm::kLineStrp:
    case DwarfForm::kGnuRefAlt:
    case DwarfForm::kGnuStrpAlt:
      ++offsets;
      return true;
    case DwarfForm::kFlagPresent:
    case DwarfForm::kImplicitConst:
      return true;
    case DwarfForm::kData1:
    case DwarfForm::kFlag:
    case DwarfForm::kRef1:
    case DwarfForm::kStrx1:
    case DwarfForm::kAddrx1:
      bytes += 1;
      return true;
    case DwarfForm::kData2:
    case DwarfForm::kRef2:
    case DwarfForm::kStrx2:
    case DwarfForm::kAddrx2:
      bytes += 2;
      return true;
    case DwarfForm::kStrx3:
    case DwarfForm::kAddrx3:
      bytes += 3;
      return true;
    case DwarfForm::kData4:
    case DwarfForm::kRef4:
    case DwarfForm::kRefSup4:
    case DwarfForm::kStrx4:
    case DwarfForm::kAddrx4:
      bytes += 4;
      return true;
    case DwarfForm::kData8:
    case DwarfForm::kRef8:
    case DwarfForm::kRefSig8:
    case DwarfForm::kRefSup8:
      bytes += 8;
      return true;
    case DwarfForm::kData16:
      bytes += 16;
      return true;
    default:
      return false;
  }
}

std::optional<uint64_t> AbbrevTable::Declaration::FixedAttributeSize(
    const FormParams& params) const {
  if (!fixed_size_)
    return std::nullopt;

  const auto& [bytes, addresses, ref_addrs, offsets] = *fixed_size_;
  const uint8_t ref_addr_size = params.version == 2 ? params.address_size : params.offset_size;
  uint64_t total = bytes;
  for (auto [count, width] :
       {std::pair{addresses, params.address_size}, std::pair{ref_addrs, ref_addr_size},
        std::pair{offsets, params.offset_size}}) {
    if (count == 0)
      continue;
    if (width == 0 || count > (UINT64_MAX - total) / width)
      return std::nullopt;
    total += count * width;
  }
  return total;
}

ErrOr<AbbrevTable> AbbrevTable::Parse(std::span<const uint8_t> section, uint64_t offset) {
  // Abbreviations contain only LEB128 values and single-byte flags.
  DataReader reader(section, true);
  reader.Seek(offset);
  auto error = [&reader, offset](const char* reason) {
    return Err("DWARF abbreviation table at offset 0x%" PRIx64 ", byte 0x%" PRIx64 ": %s", offset,
               reader.offset(), reason);
  };
  if (!reader.ok() || reader.remaining() == 0)
    return Err("DWARF abbreviation table offset 0x%" PRIx64 " is outside the section", offset);

  // Match LLVM 19's declaration and set extraction, which LLDB also uses. Allow unknown
  // nonzero tags, attributes, and forms. Only DW_CHILDREN_yes means children. Require both
  // zeros in the attribute terminator and require a zero code to end the table. Keep the first
  // declaration for duplicate codes. Keep the full LEB128 values to avoid aliasing on narrowing,
  // and check implicit constants for LEB128 errors just like the other fields.
  AbbrevTable table;
  for (;;) {
    if (reader.remaining() == 0)
      return error("missing table terminator");
    uint64_t code = reader.Uleb();
    if (!reader.ok())
      return error("invalid abbreviation code");
    if (code == 0)
      break;

    Declaration decl;
    decl.code = code;
    decl.tag = reader.Uleb();
    decl.has_children = reader.Fixed(1) == 1;  // DW_CHILDREN_yes.
    if (!reader.ok())
      return error("invalid or truncated declaration header");
    if (decl.tag == 0)
      return error("declaration has a zero tag");

    for (;;) {
      if (reader.remaining() == 0)
        return error("missing attribute list terminator");
      AttributeSpec spec{reader.Uleb(), reader.Uleb(), std::nullopt};
      if (!reader.ok())
        return error("invalid or truncated attribute specification");
      if (spec.attribute == 0 && spec.form == 0)
        break;
      if (spec.attribute == 0 || spec.form == 0)
        return error("attribute and form must both be nonzero");

      if (spec.form == static_cast<uint64_t>(DwarfForm::kImplicitConst)) {
        spec.implicit_const = reader.Sleb();
        if (!reader.ok())
          return error("invalid or truncated implicit constant");
      }
      decl.attributes.push_back(spec);
      if (decl.fixed_size_ && !decl.fixed_size_->AddForm(spec.form))
        decl.fixed_size_.reset();
    }
    table.declarations_.push_back(std::move(decl));
  }

  auto& declarations = table.declarations_;
  if (!std::ranges::is_sorted(declarations, {}, &Declaration::code))
    std::ranges::stable_sort(declarations, {}, &Declaration::code);
  auto duplicates = std::ranges::unique(declarations, {}, &Declaration::code);
  declarations.erase(duplicates.begin(), duplicates.end());
  table.consecutive_ =
      !declarations.empty() &&
      declarations.back().code - declarations.front().code == declarations.size() - 1;
  return table;
}

const AbbrevTable::Declaration* AbbrevTable::Find(uint64_t code) const {
  if (declarations_.empty())
    return nullptr;
  if (consecutive_) {
    if (code < declarations_.front().code)
      return nullptr;
    uint64_t index = code - declarations_.front().code;
    return index < declarations_.size() ? &declarations_[index] : nullptr;
  }
  auto found = std::ranges::lower_bound(declarations_, code, {}, &Declaration::code);
  return found != declarations_.end() && found->code == code ? &*found : nullptr;
}

}  // namespace zxdb
