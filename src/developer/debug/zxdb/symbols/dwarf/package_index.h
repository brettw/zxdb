// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_PACKAGE_INDEX_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_PACKAGE_INDEX_H_

#include <stdint.h>

#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "src/developer/debug/zxdb/common/endian.h"
#include "src/developer/debug/zxdb/common/err_or.h"

namespace zxdb {

// Reads either index in a DWARF package. The index borrows the section bytes.
class PackageIndex {
 public:
  enum class Kind { kCu, kTu };
  enum class Section {
    kInfo,
    kTypes,
    kAbbrev,
    kLine,
    kLoclists,
    kStrOffsets,
    kRnglists,
  };

  struct Contribution {
    uint64_t offset;
    uint64_t size;

    // Returns an empty view if the contribution extends outside the section.
    std::span<const uint8_t> Slice(std::span<const uint8_t> section) const;
  };

  // An absent section is an empty index.
  static ErrOr<PackageIndex> Parse(std::span<const uint8_t> bytes, Endian endian, Kind kind);

  // A row number is the one-based index stored in an occupied hash slot.
  std::optional<uint32_t> FindByHash(uint64_t signature) const;
  std::optional<uint32_t> FindByOffset(uint64_t unit_offset) const;
  std::optional<Contribution> GetContribution(uint32_t row, Section section) const;

 private:
  PackageIndex() = default;

  std::optional<size_t> FindColumn(Section section) const;
  Contribution ReadContribution(uint32_t row, size_t column) const;

  Endian endian_;
  bool is_v5_;
  uint32_t unit_count_;
  std::span<const uint8_t> hashes_;
  std::span<const uint8_t> rows_;
  std::span<const uint8_t> columns_;
  std::span<const uint8_t> offsets_;
  std::span<const uint8_t> sizes_;
  std::vector<std::pair<uint64_t, uint32_t>> by_offset_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_PACKAGE_INDEX_H_
