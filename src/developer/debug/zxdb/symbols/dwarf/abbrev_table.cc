// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/abbrev_table.h"

#include <inttypes.h>

#include <algorithm>
#include <utility>

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"

namespace zxdb {

bool AbbrevTable::Declaration::FixedSize::AddForm(FormEncoding encoding) {
  // Each stored specification takes at least as many bytes as the widest fixed form.
  static_assert(sizeof(AttributeSpec) >= 16);
  switch (encoding.kind) {
    case FormEncoding::Kind::kFixed:
      bytes += encoding.width;
      return true;
    case FormEncoding::Kind::kAddress:
      ++addresses;
      return true;
    case FormEncoding::Kind::kRefAddr:
      ++ref_addrs;
      return true;
    case FormEncoding::Kind::kOffset:
      ++offsets;
      return true;
    case FormEncoding::Kind::kZeroWidth:
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
  if (addresses == 0 && ref_addrs == 0 && offsets == 0)
    return bytes;
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
  DataReader reader(section, Endian::kLittle);
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
      if (decl.fixed_size_ && !decl.fixed_size_->AddForm(DescribeForm(spec.form).encoding))
        decl.fixed_size_.reset();
    }
    if (!decl.fixed_size_) {
      // DIE scans reuse this sequence for every entry with this declaration.
      auto& forms = decl.skip_forms_;
      for (const auto& spec : decl.attributes) {
        auto encoding = DescribeForm(spec.form).encoding;
        if (encoding.kind == FormEncoding::Kind::kZeroWidth)
          continue;
        if (encoding.kind == FormEncoding::Kind::kFixed && !forms.empty() &&
            forms.back().kind == FormEncoding::Kind::kFixed &&
            encoding.width <= UINT8_MAX - forms.back().width) {
          forms.back().width += encoding.width;
        } else {
          forms.push_back(encoding);
        }
      }
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

const AbbrevTable::Declaration* AbbrevTable::FindSparse(uint64_t code) const {
  auto found = std::ranges::lower_bound(declarations_, code, {}, &Declaration::code);
  return found != declarations_.end() && found->code == code ? &*found : nullptr;
}

bool AbbrevTable::Declaration::SkipAttributes(const FormParams& params, DataReader& reader) const {
  if (fixed_size_) {
    const auto size = FixedAttributeSize(params);
    if (!size)
      return false;  // The unit supplies an invalid fixed width.
    reader.Skip(*size);
    return reader.ok();
  }
  for (auto encoding : skip_forms_) {
    if (encoding.kind == FormEncoding::Kind::kFixed) {
      reader.Skip(encoding.width);
      if (!reader.ok())
        return false;
    } else if (!SkipForm(encoding, params, reader)) {
      return false;
    }
  }
  return true;
}

}  // namespace zxdb
