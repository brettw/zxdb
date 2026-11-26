// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/range_list_reader.h"

#include <inttypes.h>

#include <limits>

#include "src/developer/debug/zxdb/common/checked_math.h"
#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_context.h"

namespace zxdb {

namespace {

using Ranges = AddressRanges::RangeVector;

// DWARF 5 section 7.28.
enum class RangeEncoding : uint8_t {
  kEndOfList = 0x00,
  kBaseAddressx = 0x01,
  kStartxEndx = 0x02,
  kStartxLength = 0x03,
  kOffsetPair = 0x04,
  kBaseAddress = 0x05,
  kStartEnd = 0x06,
  kStartLength = 0x07,
};

uint64_t MaxAddress(uint8_t address_size) {
  return std::numeric_limits<uint64_t>::max() >> (64 - address_size * 8);
}

std::optional<uint64_t> AddAddress(uint64_t address, uint64_t offset, uint64_t max_address) {
  if (address > max_address || offset > max_address - address)
    return std::nullopt;
  return address + offset;
}

Err ListError(uint64_t offset, const char* reason) {
  return Err("DWARF range list at offset 0x%" PRIx64 ": %s", offset, reason);
}

ErrOr<Ranges> ReadLegacyRanges(DataReader reader, uint8_t address_size, uint64_t base) {
  const uint64_t max_address = MaxAddress(address_size);
  // LLVM uses all ones minus one for discarded code in .debug_ranges. All ones selects a base.
  const uint64_t tombstone = max_address - 1;
  Ranges ranges;
  for (;;) {
    uint64_t offset = reader.offset();
    uint64_t start = reader.Address(address_size);
    uint64_t end = reader.Address(address_size);
    if (!reader.ok())
      return ListError(offset, "truncated range or missing terminator");
    if (start == 0 && end == 0)
      return ranges;
    if (start == max_address) {
      base = end;
      continue;
    }
    if (start == tombstone || base == tombstone || end <= start)
      continue;
    auto low = AddAddress(base, start, max_address);
    auto high = AddAddress(base, end, max_address);
    if (!low || !high)
      return ListError(offset, "address overflow");
    ranges.emplace_back(*low, *high);
  }
}

ErrOr<Ranges> ReadRnglist(DataReader reader, const UnitContext& unit, uint8_t address_size) {
  const uint64_t tombstone = MaxAddress(address_size);
  uint64_t base = unit.base_address().value_or(0);
  Ranges ranges;
  for (;;) {
    uint64_t offset = reader.offset();
    auto encoding = static_cast<RangeEncoding>(reader.Fixed(1));
    if (!reader.ok())
      return ListError(offset, "missing terminator");

    std::optional<uint64_t> low, high;
    switch (encoding) {
      case RangeEncoding::kEndOfList:
        return ranges;
      case RangeEncoding::kBaseAddressx: {
        auto address = unit.ReadAddress(reader.Uleb());
        // LLVM substitutes the index or zero after failed address lookups. A bad index
        // invalidates this list so it can't describe unrelated code.
        if (!reader.ok() || !address)
          return ListError(offset, "invalid base address index");
        base = *address;
        continue;
      }
      case RangeEncoding::kBaseAddress:
        base = reader.Address(address_size);
        if (!reader.ok())
          return ListError(offset, "truncated base address");
        continue;
      case RangeEncoding::kStartxEndx:
        low = unit.ReadAddress(reader.Uleb());
        high = unit.ReadAddress(reader.Uleb());
        if (!low || !high)
          return ListError(offset, "invalid address index");
        break;
      case RangeEncoding::kStartxLength: {
        low = unit.ReadAddress(reader.Uleb());
        if (!low)
          return ListError(offset, "invalid start address index");
        uint64_t length = reader.Uleb();
        if (reader.ok() && *low == tombstone)
          continue;
        high = AddAddress(*low, length, tombstone);
        break;
      }
      case RangeEncoding::kOffsetPair: {
        uint64_t start = reader.Uleb();
        uint64_t end = reader.Uleb();
        // LLVM skips ranges that use a tombstone base or start. Check the operands first.
        if (reader.ok() && (base == tombstone || start == tombstone || end <= start))
          continue;
        low = AddAddress(base, start, tombstone);
        high = AddAddress(base, end, tombstone);
        break;
      }
      case RangeEncoding::kStartEnd:
        low = reader.Address(address_size);
        high = reader.Address(address_size);
        break;
      case RangeEncoding::kStartLength: {
        low = reader.Address(address_size);
        uint64_t length = reader.Uleb();
        if (reader.ok() && *low == tombstone)
          continue;
        high = AddAddress(*low, length, tombstone);
        break;
      }
      default:
        return ListError(offset, "unknown range list entry encoding");
    }
    if (!reader.ok())
      return ListError(offset, "truncated operands or invalid LEB128");
    if (!low || !high || *low > tombstone || *high > tombstone)
      return ListError(offset, "address overflow");
    // LLDB's address-to-unit table and zxdb both omit empty and reversed ranges.
    if (*low < *high)
      ranges.emplace_back(*low, *high);
  }
}

struct RnglistsTable {
  uint64_t base;
  uint64_t list_data_start;
  uint64_t end;
  uint64_t count;
};

ErrOr<RnglistsTable> ReadRnglistsTable(std::span<const uint8_t> section, Endian endian,
                                       const FormParams& params, uint64_t base) {
  // LLDB's ParseListTableHeader finds the header immediately before DW_AT_rnglists_base.
  uint64_t prefix_size = (params.offset_size == 8 ? 12 : 4) + 8;
  if (base < prefix_size)
    return ListError(base, "invalid offset array base");
  uint64_t start = base - prefix_size;
  DataReader reader(section, endian);
  reader.Seek(start);
  auto length = reader.InitialLength();
  if (!reader.ok() || length.offset_size != params.offset_size || length.length < 8 ||
      length.length > reader.remaining()) {
    return ListError(start, "invalid range list table length or format");
  }
  uint64_t end = reader.offset() + length.length;
  uint64_t version = reader.Fixed(2);
  uint64_t address_size = reader.Fixed(1);
  uint64_t segment_size = reader.Fixed(1);
  uint64_t count = reader.Fixed(4);
  if (version != 5 || address_size != params.address_size || segment_size != 0)
    return ListError(start, "unsupported range list table version, address size, or segment size");
  if (count > (end - base) / params.offset_size)
    return ListError(start, "offset array exceeds the contribution");
  return RnglistsTable{base, base + count * params.offset_size, end, count};
}

}  // namespace

ErrOr<Ranges> RangeListReader::ReadFromOffset(const UnitContext& unit, uint64_t offset) {
  if (unit.params_.version < 5) {
    auto actual_offset = CheckedAdd(unit.ranges_base_.value_or(0), offset);
    if (!actual_offset || *actual_offset >= unit.ranges_section_.size())
      return ListError(offset, "offset is past .debug_ranges");
    DataReader reader(unit.ranges_section_, unit.endian_);
    reader.Seek(*actual_offset);
    return ReadLegacyRanges(reader, unit.params_.address_size, unit.base_address_.value_or(0));
  }

  DataReader reader(unit.rnglists_, unit.endian_);
  // Like LLDB, use the contribution bounds when the unit supplies a table base. Direct
  // offsets without a base use the section bounds, as LLVM and LLDB do.
  if (unit.rnglists_base_ || unit.is_dwo_) {
    if (!unit.rnglists_base_)
      return ListError(offset, "missing split range list table header");
    auto table = ReadRnglistsTable(unit.rnglists_, unit.endian_, unit.params_,
                                   *unit.rnglists_base_ - unit.rnglists_section_offset_);
    if (table.has_error())
      return table.err();
    if (offset < table.value().list_data_start || offset >= table.value().end)
      return ListError(offset, "offset is outside the range list data");
    reader.set_limit(table.value().end);
  }
  if (offset >= reader.remaining())
    return ListError(offset, "offset is past .debug_rnglists");
  reader.Seek(offset);
  return ReadRnglist(reader, unit, unit.params_.address_size);
}

ErrOr<Ranges> RangeListReader::ReadFromIndex(const UnitContext& unit, uint64_t index) {
  if (unit.params_.version < 5)
    return Err("DW_FORM_rnglistx requires DWARF 5.");

  // LLVM 19 defaults to the first table when no DW_AT_rnglists_base is present. Split units
  // derive their base from the header at the start of their contribution.
  uint64_t prefix_size = (unit.params_.offset_size == 8 ? 12 : 4) + 8;
  uint64_t base =
      unit.rnglists_base_ ? *unit.rnglists_base_ - unit.rnglists_section_offset_ : prefix_size;
  auto decoded = ReadRnglistsTable(unit.rnglists_, unit.endian_, unit.params_, base);
  if (decoded.has_error())
    return decoded.err();
  const auto& table = decoded.value();
  if (index >= table.count)
    return Err("DWARF range list index 0x%" PRIx64 " is outside the offset array.", index);

  DataReader reader(unit.rnglists_, unit.endian_);
  reader.set_limit(table.end);
  reader.Seek(table.base + index * unit.params_.offset_size);
  uint64_t relative = reader.Offset(unit.params_.offset_size);
  // The slot and its target must belong to this contribution. LLVM 19 checks only the
  // section bounds for these lookups.
  if (relative < table.list_data_start - table.base || relative >= table.end - table.base)
    return ListError(table.base, "offset array entry points outside the range list data");
  reader.Seek(table.base + relative);
  return ReadRnglist(reader, unit, unit.params_.address_size);
}

ErrOr<Ranges> RangeListReader::ReadUnitRanges(const UnitContext& unit) {
  // LLVM's collectAddressRanges reads only the root DIE and prefers its low/high PC pair.
  uint64_t max_address = MaxAddress(unit.params_.address_size);
  if (unit.low_pc_ && *unit.low_pc_ != max_address && unit.high_pc_) {
    const auto& high_form = *unit.high_pc_;
    auto high = unit.ResolveAddress(high_form);
    if (high_form.form_class == FormValue::Class::kConstant) {
      // LLDB accepts a signed constant here. Require a nonnegative offset.
      std::optional<uint64_t> length;
      if (auto* unsigned_value = std::get_if<uint64_t>(&high_form.value))
        length = *unsigned_value;
      else if (auto* signed_value = std::get_if<int64_t>(&high_form.value);
               signed_value && *signed_value >= 0)
        length = *signed_value;
      if (length) {
        high = AddAddress(*unit.low_pc_, *length, max_address);
        if (!high)
          return Err("DWARF unit high PC overflows the address size.");
      }
    }
    if (high) {
      if (*high > max_address)
        return Err("DWARF unit high PC exceeds the address size.");
      if (*unit.low_pc_ < *high)
        return Ranges{AddressRange(*unit.low_pc_, *high)};
      return Ranges{};
    }
  }

  if (const auto& ranges = unit.ranges()) {
    uint64_t value = std::get<uint64_t>(ranges->value);
    return ranges->form == DwarfForm::kRnglistx ? ReadFromIndex(unit, value)
                                                : ReadFromOffset(unit, value);
  }
  return Ranges{};
}

}  // namespace zxdb
