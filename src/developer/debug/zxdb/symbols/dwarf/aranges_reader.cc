// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/aranges_reader.h"

#include <inttypes.h>

#include <limits>

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"

namespace zxdb {

ErrOr<ArangesSet> ArangesSet::Parse(std::span<const uint8_t> section, uint64_t offset,
                                    Endian endian) {
  auto error = [offset](const char* reason) {
    return Err("DWARF address range set at offset 0x%" PRIx64 ": %s", offset, reason);
  };
  DataReader reader(section, endian);
  reader.Seek(offset);
  auto length = reader.InitialLength();
  if (!reader.ok() || length.length > reader.remaining() || length.length < length.offset_size + 4u)
    return error("invalid set length");
  uint64_t end_offset = reader.offset() + length.length;
  reader.set_limit(end_offset);

  uint64_t version = reader.Fixed(2);
  uint64_t unit_offset = reader.Offset(length.offset_size);
  uint64_t address_size = reader.Fixed(1);
  uint64_t segment_size = reader.Fixed(1);
  // LLDB 19 accepts versions 2 through 5. The standard specifies version 2 and LLVM 19
  // accepts any version with this layout.
  if (version < 2 || version > 5)
    return error("unsupported version");
  if (address_size != 2 && address_size != 4 && address_size != 8)
    return error("unsupported address size");
  if (segment_size != 0)
    return error("unsupported segment selector size");

  // LLVM and LLDB align the first tuple relative to the start of this set. The padding
  // bytes can have any value. LLVM requires the set to end on a tuple boundary.
  uint64_t tuple_size = 2 * address_size;
  uint64_t header_size = reader.offset() - offset;
  uint64_t padding = (tuple_size - header_size % tuple_size) % tuple_size;
  if ((end_offset - offset) % tuple_size != 0 || padding > reader.remaining() ||
      tuple_size > reader.remaining() - padding)
    return error("invalid tuple alignment or missing tuples");
  reader.Skip(padding);

  uint64_t max_address = std::numeric_limits<uint64_t>::max() >> (64 - address_size * 8);
  ArangesSet result(unit_offset, end_offset);
  while (reader.remaining() != 0) {
    uint64_t tuple_offset = reader.offset();
    uint64_t address = reader.Address(address_size);
    uint64_t size = reader.Address(address_size);
    if (address == 0 && size == 0) {
      // LLVM and LLDB continue through early zero tuples and require a final terminator.
      if (reader.remaining() == 0)
        return result;
      result.premature_terminators_.push_back(tuple_offset);
    } else if (size != 0 && size <= max_address - address) {
      result.ranges_.emplace_back(address, address + size);
    }
  }
  return error("missing final terminator");
}

}  // namespace zxdb
