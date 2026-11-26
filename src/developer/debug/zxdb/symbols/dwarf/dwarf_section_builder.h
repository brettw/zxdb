// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DWARF_SECTION_BUILDER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DWARF_SECTION_BUILDER_H_

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/developer/debug/zxdb/symbols/dwarf/abbrev_table.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"

namespace zxdb {

enum class DwarfForm : int;

void AppendInteger(std::vector<uint8_t>& out, uint64_t value, size_t size,
                   bool little_endian = true);
void AppendUleb(std::vector<uint8_t>& out, uint64_t value);
void AppendUleb(std::vector<uint8_t>& out, DwarfForm form);
void AppendSleb(std::vector<uint8_t>& out, int64_t value);
void AppendCString(std::vector<uint8_t>& out, std::string_view value);

// Appends DWARF bytes to a test buffer.
class DwarfSectionBuilder {
 public:
  class LengthMarker {
   private:
    friend class DwarfSectionBuilder;

    LengthMarker(size_t offset, size_t offset_size) : offset_(offset), offset_size_(offset_size) {}

    size_t offset_;
    size_t offset_size_;
  };

  struct UnitHeaderOptions {
    UnitSection section = UnitSection::kInfo;
    uint16_t version = 5;
    uint8_t offset_size = 4;
    uint8_t unit_type = kUnitTypeCompile;
    uint8_t address_size = 8;
    uint64_t abbrev_offset = 0;
    uint64_t type_signature = 0;
    uint64_t type_offset = 0;
    uint64_t dwo_id = 0;
  };

  explicit DwarfSectionBuilder(std::vector<uint8_t>& out, bool little_endian = true)
      : out_(out), little_endian_(little_endian) {}

  void Fixed(uint64_t value, size_t size);
  void Uleb(uint64_t value);
  void Sleb(int64_t value);
  void CString(std::string_view value);
  void Bytes(std::span<const uint8_t> value);
  void Offset(uint64_t value, size_t offset_size);
  void Address(uint64_t value, size_t address_size);

  // EndInitialLength patches the placeholder with the number of bytes written after it.
  // Pass the marker back to the same builder at the end of the contribution.
  // Tests can supply a raw length to write a malformed contribution.
  LengthMarker BeginInitialLength(size_t offset_size);
  void EndInitialLength(LengthMarker marker, std::optional<uint64_t> raw_length = std::nullopt);

  // Writes the header. Append DIE bytes before calling EndUnit.
  LengthMarker BeginUnit(const UnitHeaderOptions& options);
  void EndUnit(LengthMarker marker, std::optional<uint64_t> raw_length = std::nullopt) {
    EndInitialLength(marker, raw_length);
  }

  // Writes one declaration with its attribute list terminator.
  void AbbrevDeclaration(uint64_t code, uint64_t tag, bool has_children,
                         std::span<const AbbrevTable::AttributeSpec> attributes = {});
  void EndAbbrevTable() { Uleb(0); }

 private:
  std::vector<uint8_t>& out_;
  bool little_endian_;
};

// Builds a string section with one entry per distinct string.
class DwarfStringSectionBuilder {
 public:
  uint64_t AddString(std::string_view value);
  std::span<const uint8_t> bytes() const { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
  std::map<std::string, uint64_t, std::less<>> offsets_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DWARF_SECTION_BUILDER_H_
