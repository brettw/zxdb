// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/unit_context.h"

#include <inttypes.h>

#include <algorithm>
#include <array>

#include "src/developer/debug/zxdb/common/checked_math.h"
#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_sections.h"
#include "src/developer/debug/zxdb/symbols/dwarf_attr.h"

namespace zxdb {

namespace {

constexpr uint8_t InitialLengthSize(uint8_t offset_size) { return offset_size == 8 ? 12 : 4; }

std::span<const uint8_t> SliceContribution(std::span<const uint8_t> section,
                                           std::optional<UnitContext::Contribution> contribution) {
  if (!contribution || contribution->offset > section.size() ||
      contribution->size > section.size() - contribution->offset) {
    return {};
  }
  return section.subspan(contribution->offset, contribution->size);
}

std::optional<uint64_t> ReadEntry(std::span<const uint8_t> section, uint64_t base, uint64_t index,
                                  uint8_t width, Endian endian) {
  // Check the index before multiplying or adding it. This also requires a complete entry.
  if (base > section.size() || index >= (section.size() - base) / width)
    return std::nullopt;
  return ReadFixed(section.data() + base + index * width, width, endian);
}

std::optional<uint64_t> SectionOffset(const std::optional<FormValue>& value, uint16_t version) {
  if (!value)
    return std::nullopt;
  // DWARF 2 and 3 also use fixed constants for section offsets. LLVM's
  // doesFormBelongToClass() makes the same distinction.
  if (value->form == DwarfForm::kSecOffset ||
      (version <= 3 && (value->form == DwarfForm::kData4 || value->form == DwarfForm::kData8))) {
    return std::get<uint64_t>(value->value);
  }
  return std::nullopt;
}

std::optional<UnitContext::StringOffsets> ReadStringOffsets(std::span<const uint8_t> section,
                                                            const FormParams& params, Endian endian,
                                                            const UnitContext::Options& options,
                                                            std::optional<uint64_t> base) {
  uint64_t start;
  uint64_t section_base = 0;
  if (options.is_dwo) {
    if (options.package) {
      auto contribution = options.package->str_offsets;
      section = SliceContribution(section, contribution);
      if (section.empty())
        return std::nullopt;
      section_base = contribution->offset;
    }
    if (section.empty())
      return std::nullopt;
    // DWARF 4 split contributions have no header, including at section offset zero.
    if (params.version < 5) {
      if (section.size() % params.offset_size != 0)
        return std::nullopt;
      return UnitContext::StringOffsets{section_base, section_base + section.size(),
                                        params.offset_size};
    }
    start = 0;
  } else {
    uint64_t prefix_size = InitialLengthSize(params.offset_size) + 4;
    if (params.version < 5 || !base || *base < prefix_size)
      return std::nullopt;
    start = *base - prefix_size;
  }

  DataReader reader(section, endian);
  reader.Seek(start);
  auto length = reader.InitialLength();
  if (!reader.ok() || length.length < 4 || length.length > reader.remaining())
    return std::nullopt;
  // LLVM 19 uses the unit format even in a DWO. LLDB reads the DWO's own length encoding.
  if (!options.is_dwo && length.offset_size != params.offset_size)
    return std::nullopt;
  uint64_t end = reader.offset() + length.length;
  uint64_t version = reader.Fixed(2);
  reader.Skip(2);  // Padding.
  if (version < 5 || (end - reader.offset()) % length.offset_size != 0)
    return std::nullopt;
  return UnitContext::StringOffsets{section_base + reader.offset(), section_base + end,
                                    length.offset_size};
}

}  // namespace

ErrOr<UnitContext> UnitContext::Parse(const DwarfSections& sections, const UnitHeader& header,
                                      UnitSection section, const Options& options,
                                      const AbbrevLookup& lookup_abbrevs) {
  auto error = [offset = header.offset](const char* reason) {
    return Err("DWARF unit DIE for unit at offset 0x%" PRIx64 ": %s", offset, reason);
  };
  auto info = options.is_dwo ? sections.info_dwo : sections.info;
  if (section == UnitSection::kTypes)
    info = options.is_dwo ? sections.types_dwo : sections.types;

  // LLVM's extractDIEsToVector() reads the root first. Bound even the root abbreviation code
  // and fixed-size attributes to this unit, so the next header cannot supply missing bytes.
  if (header.offset_size != 4 && header.offset_size != 8)
    return error("invalid offset size");
  if (header.address_size != 2 && header.address_size != 4 && header.address_size != 8)
    return error("invalid address size");
  uint64_t length_size = InitialLengthSize(header.offset_size);
  if (header.offset > info.size() || length_size > info.size() - header.offset ||
      header.length > info.size() - header.offset - length_size ||
      header.header_size < length_size || header.header_size - length_size >= header.length) {
    return error("invalid unit bounds or missing root");
  }
  DataReader reader(info, sections.endian);
  reader.set_limit(header.next_unit_offset());
  reader.Seek(header.die_offset());
  uint64_t code = reader.Uleb();
  if (!reader.ok() || code == 0)
    return error("invalid root abbreviation code");

  auto abbrev_section = options.is_dwo ? sections.abbrev_dwo : sections.abbrev;
  std::optional<ErrOr<AbbrevTable>> local_abbrevs;
  const auto& abbrevs =
      lookup_abbrevs
          ? lookup_abbrevs(abbrev_section, header.abbrev_offset)
          : local_abbrevs.emplace(AbbrevTable::Parse(abbrev_section, header.abbrev_offset));
  if (abbrevs.has_error())
    return abbrevs.err();
  const auto* declaration = abbrevs.value().Find(code);
  if (!declaration)
    return error("unknown root abbreviation");

  // Keep the first occurrence, as DWARFDie::find() does. Resolve indexed values only after
  // reading the bases, since the attributes can occur in either order.
  constexpr std::array kAttributes{
      DwarfAttr::kStrOffsetsBase, DwarfAttr::kAddrBase,      DwarfAttr::kGnuAddrBase,
      DwarfAttr::kRnglistsBase,   DwarfAttr::kGnuRangesBase, DwarfAttr::kLoclistsBase,
      DwarfAttr::kCompDir,        DwarfAttr::kLowPc,         DwarfAttr::kEntryPc,
      DwarfAttr::kRanges,         DwarfAttr::kStmtList,      DwarfAttr::kHighPc,
  };
  std::array<std::optional<FormValue>, kAttributes.size()> values;
  auto find = [&](DwarfAttr attr) -> std::optional<FormValue>& {
    return values[std::find(kAttributes.begin(), kAttributes.end(), attr) - kAttributes.begin()];
  };

  FormParams params = header.form_params();
  for (const auto& spec : declaration->attributes) {
    auto decoded = ReadForm(spec, params, reader);
    if (decoded.has_error())
      return decoded.err();
    auto attr = std::find_if(kAttributes.begin(), kAttributes.end(), [&](DwarfAttr attr) {
      return spec.attribute == static_cast<uint64_t>(attr);
    });
    if (attr != kAttributes.end()) {
      auto& value = values[attr - kAttributes.begin()];
      if (!value)
        value = decoded.take_value();
    }
  }

  UnitContext result;
  result.params_ = params;
  result.endian_ = sections.endian;
  result.is_dwo_ = options.is_dwo;
  result.str_ = options.is_dwo ? sections.str_dwo : sections.str;
  result.line_str_ = sections.line_str;
  result.str_offsets_ = options.is_dwo ? sections.str_offsets_dwo : sections.str_offsets;
  result.addr_ = sections.addr;
  result.addr_entry_size_ = header.address_size;
  result.addr_endian_ = sections.endian;
  result.loclists_ = options.is_dwo ? sections.loclists_dwo : sections.loclists;
  result.ranges_section_ = sections.ranges;
  result.rnglists_ = options.is_dwo ? sections.rnglists_dwo : sections.rnglists;

  auto offset = [&](DwarfAttr attr) { return SectionOffset(find(attr), header.version); };
  if (options.is_dwo) {
    // LLVM and LLDB use the split contributions for the DWARF 5 lists and strings.
    if (header.version >= 5) {
      uint64_t prefix_size = length_size + 8;
      if (options.package) {
        result.loclists_ = SliceContribution(result.loclists_, options.package->loclists);
        result.rnglists_ = SliceContribution(result.rnglists_, options.package->rnglists);
        if (options.package->rnglists)
          result.rnglists_section_offset_ = options.package->rnglists->offset;
      }
      if (result.loclists_.size() >= prefix_size)
        result.loclists_base_ = prefix_size;
      if (result.rnglists_.size() >= prefix_size)
        result.rnglists_base_ = result.rnglists_section_offset_ + prefix_size;
    }
    const UnitContext* address_table = options.skeleton ? options.skeleton : options.address_table;
    if (address_table) {
      result.addr_ = address_table->addr_;
      result.addr_base_ = address_table->addr_base_;
      result.addr_entry_size_ = address_table->addr_entry_size_;
      result.addr_endian_ = address_table->addr_endian_;
    }
    if (options.skeleton && header.version <= 4) {
      result.ranges_base_ = options.skeleton->gnu_ranges_base_;
      result.ranges_section_ = options.skeleton->ranges_section_;
    }
  } else {
    result.str_offsets_base_ = offset(DwarfAttr::kStrOffsetsBase);
    result.addr_base_ = offset(DwarfAttr::kAddrBase);
    if (!result.addr_base_)
      result.addr_base_ = offset(DwarfAttr::kGnuAddrBase);
    result.rnglists_base_ = offset(DwarfAttr::kRnglistsBase);
    result.loclists_base_ = offset(DwarfAttr::kLoclistsBase);
    // DW_AT_GNU_ranges_base applies to the split unit's ranges. LLDB and LLVM leave the
    // skeleton's own range offsets section-relative.
    result.gnu_ranges_base_ = offset(DwarfAttr::kGnuRangesBase);
  }

  result.string_offsets_ = ReadStringOffsets(result.str_offsets_, params, sections.endian, options,
                                             result.str_offsets_base_);
  if (result.string_offsets_)
    result.str_offsets_base_ = result.string_offsets_->base;
  if (const auto& dir = find(DwarfAttr::kCompDir))
    result.compilation_dir_ = result.ResolveString(*dir);
  if (const auto& low_pc = find(DwarfAttr::kLowPc))
    result.low_pc_ = result.ResolveAddress(*low_pc);
  result.high_pc_ = find(DwarfAttr::kHighPc);

  // LLVM's getBaseAddress() uses the linked skeleton's PC. Otherwise it prefers low_pc,
  // including zero. It tries entry_pc only when low_pc is absent.
  result.base_address_ = result.low_pc_;
  if (options.is_dwo && options.skeleton) {
    result.base_address_ = options.skeleton->base_address_;
  } else if (!find(DwarfAttr::kLowPc)) {
    if (const auto& entry_pc = find(DwarfAttr::kEntryPc))
      result.base_address_ = result.ResolveAddress(*entry_pc);
  }
  result.stmt_list_ = offset(DwarfAttr::kStmtList);
  const auto& ranges = find(DwarfAttr::kRanges);
  if (ranges && (ranges->form == DwarfForm::kRnglistx || SectionOffset(ranges, header.version)))
    result.ranges_ = ranges;
  return result;
}

std::optional<std::string_view> UnitContext::ResolveString(const FormValue& value) const {
  if (value.form_class != FormValue::Class::kString)
    return std::nullopt;
  if (value.form == DwarfForm::kString)
    return std::get<std::string_view>(value.value);

  auto strings = str_;
  uint64_t offset = std::get<uint64_t>(value.value);
  switch (value.form) {
    case DwarfForm::kLineStrp:
      strings = line_str_;
      break;
    case DwarfForm::kStrp:
      break;
    case DwarfForm::kStrx:
    case DwarfForm::kStrx1:
    case DwarfForm::kStrx2:
    case DwarfForm::kStrx3:
    case DwarfForm::kStrx4:
    case DwarfForm::kGnuStrIndex: {
      if (!string_offsets_)
        return std::nullopt;
      // LLVM's lookup checks the section end. Stay in this unit's contribution.
      auto entry = ReadEntry(str_offsets_.first(string_offsets_->end), string_offsets_->base,
                             offset, string_offsets_->entry_size, endian_);
      if (!entry)
        return std::nullopt;
      offset = *entry;
      break;
    }
    default:
      return std::nullopt;
  }
  DataReader reader(strings, endian_);
  reader.Seek(offset);
  auto string = reader.CString();
  if (!reader.ok())
    return std::nullopt;
  return string;
}

std::optional<uint64_t> UnitContext::ResolveAddress(const FormValue& value) const {
  if (value.form_class != FormValue::Class::kAddress)
    return std::nullopt;
  if (value.form == DwarfForm::kLlvmAddrxOffset) {
    auto indexed = std::get<FormValue::IndexedAddress>(value.value);
    if (auto address = ReadAddress(indexed.index))
      return CheckedAdd(*address, indexed.offset);
    return std::nullopt;
  }
  uint64_t raw = std::get<uint64_t>(value.value);
  if (value.form == DwarfForm::kAddr)
    return raw;
  return ReadAddress(raw);
}

std::optional<uint64_t> UnitContext::ReadAddress(uint64_t index) const {
  if (!addr_base_)
    return std::nullopt;
  // Like LLDB's ReadAddressFromDebugAddrSection(), use the address size and section bounds.
  return ReadEntry(addr_, *addr_base_, index, addr_entry_size_, addr_endian_);
}

std::optional<uint64_t> UnitContext::ReadLoclistOffset(uint64_t index) const {
  uint64_t prefix_size = InitialLengthSize(params_.offset_size) + 8;
  if (params_.version < 5 || !loclists_base_ || *loclists_base_ < prefix_size)
    return std::nullopt;
  uint64_t base = *loclists_base_;
  DataReader reader(loclists_, endian_);
  reader.Seek(base - prefix_size);
  auto length = reader.InitialLength();
  if (!reader.ok() || length.offset_size != params_.offset_size || length.length < 8 ||
      length.length > reader.remaining()) {
    return std::nullopt;
  }
  uint64_t end = reader.offset() + length.length;
  auto version = reader.Fixed(2);
  auto address_size = reader.Fixed(1);
  auto segment_size = reader.Fixed(1);
  auto count = reader.Fixed(4);
  // LLDB uses DWARFListTableHeader::extract() and checks the count. LLVM's unit getter
  // reads the slot directly, which can consume bytes from a list or another contribution.
  if (version != 5 || address_size != params_.address_size || segment_size != 0 ||
      count > (end - base) / params_.offset_size || index >= count) {
    return std::nullopt;
  }
  reader.Seek(base + index * params_.offset_size);
  uint64_t relative = reader.Offset(params_.offset_size);
  if (relative < count * params_.offset_size || relative >= end - base)
    return std::nullopt;
  return base + relative;
}

}  // namespace zxdb
