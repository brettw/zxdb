// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DWARF_SECTION_BUILDER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DWARF_SECTION_BUILDER_H_

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
  LengthMarker BeginInitialLength(size_t offset_size);
  void EndInitialLength(LengthMarker marker);

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
