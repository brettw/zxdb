// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_HEADER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_HEADER_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <span>
#include <vector>

#include "src/developer/debug/zxdb/common/err_or.h"

namespace zxdb {

// The DWARF version and field sizes used to decode forms.
struct FormParams {
  uint16_t version = 0;
  uint8_t offset_size = 0;
  uint8_t address_size = 0;
};

// The section that contains the unit, including the corresponding .dwo section.
enum class UnitSection { kInfo, kTypes };

// DW_UT_* values from DWARF 5.
inline constexpr uint8_t kUnitTypeCompile = 0x01;
inline constexpr uint8_t kUnitTypeType = 0x02;
inline constexpr uint8_t kUnitTypePartial = 0x03;
inline constexpr uint8_t kUnitTypeSkeleton = 0x04;
inline constexpr uint8_t kUnitTypeSplitCompile = 0x05;
inline constexpr uint8_t kUnitTypeSplitType = 0x06;

// The header fields and bounds of a DWARF unit.
struct UnitHeader {
  uint64_t offset;         // Unit start relative to the section start.
  uint64_t length;         // Unit bytes after the initial length field.
  uint8_t offset_size;     // Bytes per DWARF offset, 4 for DWARF32 or 8 for DWARF64.
  uint16_t version;        // DWARF version.
  uint8_t unit_type;       // DW_UT_* value, inferred from the section for DWARF 2 through 4.
  uint8_t address_size;    // Bytes per target address.
  uint64_t abbrev_offset;  // Offset in the abbreviation section.
  std::optional<uint64_t> type_signature;  // Signature of a type or split type unit.
  std::optional<uint64_t> type_offset;     // Type DIE offset relative to the unit start.
  std::optional<uint64_t> dwo_id;          // ID of a skeleton or split compile unit.
  uint8_t header_size;                     // Bytes from the unit start through the header.

  bool is_type_unit() const {
    return unit_type == kUnitTypeType || unit_type == kUnitTypeSplitType;
  }
  FormParams form_params() const { return {version, offset_size, address_size}; }

  // Offset just after the header, relative to the section start.
  uint64_t die_offset() const { return offset + header_size; }
  // Offset just after the unit, relative to the section start.
  uint64_t next_unit_offset() const { return offset + (offset_size == 8 ? 12 : 4) + length; }
};

// Reads the header at offset in section using the given byte order and section kind.
// Checks the initial length, unit and header bounds, version, address size, and type offset.
// Returns an error with the unit offset and cause when validation fails.
ErrOr<UnitHeader> ReadUnitHeader(std::span<const uint8_t> section, uint64_t offset,
                                 bool little_endian, UnitSection kind);

// Owns the headers of one section. After parsing, callers can read it from multiple threads.
class UnitList {
 public:
  // Stops at the first error and keeps the headers that precede it.
  static UnitList Parse(std::span<const uint8_t> section, bool little_endian, UnitSection kind);

  size_t size() const { return units_.size(); }
  const UnitHeader& operator[](size_t index) const { return units_[index]; }
  auto begin() const { return units_.begin(); }
  auto end() const { return units_.end(); }

  const Err& error() const { return error_; }

  // The range of a unit includes its header and excludes the start of the next unit.
  std::optional<size_t> FindIndexForOffset(uint64_t offset) const;

 private:
  std::vector<UnitHeader> units_;
  Err error_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_HEADER_H_
