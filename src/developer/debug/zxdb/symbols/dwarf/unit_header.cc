// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"

#include <inttypes.h>

#include <algorithm>

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"

namespace zxdb {

ErrOr<UnitHeader> ReadUnitHeader(std::span<const uint8_t> section, uint64_t offset,
                                 bool little_endian, UnitSection kind) {
  auto error = [offset](const char* reason) {
    return Err("DWARF unit at offset 0x%" PRIx64 ": %s", offset, reason);
  };

  DataReader reader(section, little_endian);
  reader.Seek(offset);
  auto initial_length = reader.InitialLength();
  if (!reader.ok())
    return error("invalid or truncated initial length");
  if (initial_length.length > reader.remaining())
    return error("length extends past the section end");
  // Bound the header reads to this unit. LLVM can read a short unit's header into the next unit.
  reader.set_limit(reader.offset() + initial_length.length);

  UnitHeader header{};
  header.offset = offset;
  header.length = initial_length.length;
  header.offset_size = initial_length.offset_size;
  header.version = reader.Fixed(2);
  if (!reader.ok())
    return error("truncated header");
  if (header.version < 2 || header.version > 5)
    return error("unsupported version");

  if (header.version == 5) {
    header.unit_type = reader.Fixed(1);
    header.address_size = reader.Fixed(1);
    header.abbrev_offset = reader.Offset(header.offset_size);
  } else {
    header.unit_type = kind == UnitSection::kTypes ? kUnitTypeType : kUnitTypeCompile;
    header.abbrev_offset = reader.Offset(header.offset_size);
    header.address_size = reader.Fixed(1);
  }

  // LLVM and LLDB use the compile unit layout for unknown types. The declared length still
  // locates the next unit, so keep the unknown type and continue scanning.
  if (header.is_type_unit()) {
    header.type_signature = reader.Fixed(8);
    header.type_offset = reader.Offset(header.offset_size);
  } else if (header.unit_type == kUnitTypeSkeleton || header.unit_type == kUnitTypeSplitCompile) {
    header.dwo_id = reader.Fixed(8);
  }
  if (!reader.ok())
    return error("truncated header");
  header.header_size = reader.offset() - offset;

  // Match LLVM's supported address sizes and leave abbreviation checks to DIE parsing.
  // LLDB also checks that an abbreviation table exists before it creates a unit.
  if (header.address_size != 2 && header.address_size != 4 && header.address_size != 8)
    return error("unsupported address size");
  if (header.type_offset && (*header.type_offset < header.header_size ||
                             *header.type_offset >= header.next_unit_offset() - offset)) {
    return error("type offset is outside the DIE data");
  }
  return header;
}

UnitList UnitList::Parse(std::span<const uint8_t> section, bool little_endian, UnitSection kind) {
  UnitList list;
  for (uint64_t offset = 0; offset < section.size();) {
    auto header = ReadUnitHeader(section, offset, little_endian, kind);
    if (header.has_error()) {
      list.error_ = header.err();
      break;
    }
    offset = header.value().next_unit_offset();
    list.units_.push_back(header.take_value());
  }
  return list;
}

std::optional<size_t> UnitList::FindIndexForOffset(uint64_t offset) const {
  auto found = std::partition_point(units_.begin(), units_.end(), [offset](const UnitHeader& unit) {
    return unit.next_unit_offset() <= offset;
  });
  if (found == units_.end() || offset < found->offset)
    return std::nullopt;
  return found - units_.begin();
}

}  // namespace zxdb
