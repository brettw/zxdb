// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/package_index.h"

#include <algorithm>
#include <bit>

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"

namespace zxdb {

namespace {

// Section IDs from the GNU Debug Fission format and DWARF 5 section 7.3.5.
std::optional<uint32_t> SectionId(PackageIndex::Section section, bool is_v5) {
  using Section = PackageIndex::Section;
  switch (section) {
    case Section::kInfo:
      return 1;
    case Section::kTypes:
      return is_v5 ? std::nullopt : std::optional(2u);
    case Section::kAbbrev:
      return 3;
    case Section::kLine:
      return 4;
    case Section::kLoclists:
      return is_v5 ? std::optional(5u) : std::nullopt;
    case Section::kStrOffsets:
      return 6;
    case Section::kRnglists:
      return is_v5 ? std::optional(8u) : std::nullopt;
  }
  return std::nullopt;
}

constexpr size_t kWordSize = 4;
constexpr size_t kSignatureSize = 8;

}  // namespace

std::span<const uint8_t> PackageIndex::Contribution::Slice(std::span<const uint8_t> section) const {
  if (offset > section.size() || size > section.size() - offset)
    return {};
  return section.subspan(offset, size);
}

ErrOr<PackageIndex> PackageIndex::Parse(std::span<const uint8_t> bytes, Endian endian, Kind kind) {
  if (bytes.empty())
    return PackageIndex();

  DataReader reader(bytes, endian);
  // GNU v2 uses a 32-bit version. DWARF 5 uses a 16-bit version followed by padding.
  bool is_v5 = reader.Fixed(kWordSize) != 2;
  if (is_v5) {
    reader.Seek(0);
    if (reader.Fixed(2) != 5)
      return Err("Invalid DWARF package index version.");
    reader.Skip(2);
  }
  uint64_t columns = reader.Fixed(kWordSize);
  uint32_t units = reader.Fixed(kWordSize);
  uint32_t slots = reader.Fixed(kWordSize);
  if (!reader.ok())
    return Err("Truncated DWARF package index header.");
  if ((slots != 0 && !std::has_single_bit(slots)) || units > slots)
    return Err("Invalid DWARF package index hash table size.");

  PackageIndex result;
  result.endian_ = endian;
  result.is_v5_ = is_v5;
  result.unit_count_ = units;
  result.hashes_ = reader.Bytes(uint64_t{slots} * kSignatureSize);
  result.rows_ = reader.Bytes(uint64_t{slots} * kWordSize);
  result.columns_ = reader.Bytes(columns * kWordSize);
  // Each matrix has one word per column per unit. Bound both matrices before multiplying.
  if (!reader.ok() || (columns && units > reader.remaining() / 2 / kWordSize / columns))
    return Err("Truncated DWARF package index tables.");
  uint64_t matrix_size = columns * units * kWordSize;
  result.offsets_ = reader.Bytes(matrix_size);
  result.sizes_ = reader.Bytes(matrix_size);

  if (!units && !slots && !columns)
    return result;  // Producers can emit a header-only empty index.
  auto unit_column =
      result.FindColumn(!is_v5 && kind == Kind::kTu ? Section::kTypes : Section::kInfo);
  if (!unit_column)
    return Err("DWARF package index is missing its unit section column.");

  // DWARF 4 CUs store their DWO IDs in the root DIE. Find their contributions by the unit's
  // section offset so callers can get the abbreviation table before reading that DIE.
  for (size_t pos = 0; pos < result.rows_.size(); pos += kWordSize) {
    uint32_t row = ReadFixed(result.rows_.data() + pos, kWordSize, endian);
    if (row > units)
      return Err("Invalid DWARF package index row.");
    if (row != 0)
      result.by_offset_.emplace_back(result.ReadContribution(row, *unit_column).offset, row);
  }
  std::sort(result.by_offset_.begin(), result.by_offset_.end());
  return result;
}

std::optional<uint32_t> PackageIndex::FindByHash(uint64_t signature) const {
  size_t slots = rows_.size() / kWordSize;
  if (!slots)
    return std::nullopt;
  size_t mask = slots - 1;
  size_t slot = signature & mask;
  size_t stride = ((signature >> 32) & mask) | 1;
  // Bound probing even when a malformed hash table has no empty slot.
  for (size_t probes = 0; probes < slots; ++probes, slot = (slot + stride) & mask) {
    uint32_t row = ReadFixed(rows_.data() + slot * kWordSize, kWordSize, endian_);
    if (!row)
      break;
    if (ReadFixed(hashes_.data() + slot * kSignatureSize, kSignatureSize, endian_) == signature)
      return row;
  }
  return std::nullopt;
}

std::optional<uint32_t> PackageIndex::FindByOffset(uint64_t unit_offset) const {
  auto found =
      std::ranges::lower_bound(by_offset_, unit_offset, {}, &std::pair<uint64_t, uint32_t>::first);
  if (found != by_offset_.end() && found->first == unit_offset)
    return found->second;
  return std::nullopt;
}

std::optional<PackageIndex::Contribution> PackageIndex::GetContribution(uint32_t row,
                                                                        Section section) const {
  if (!row || row > unit_count_)
    return std::nullopt;
  if (auto column = FindColumn(section))
    return ReadContribution(row, *column);
  return std::nullopt;
}

std::optional<size_t> PackageIndex::FindColumn(Section section) const {
  auto id = SectionId(section, is_v5_);
  if (!id)
    return std::nullopt;
  for (size_t pos = 0; pos < columns_.size(); pos += kWordSize) {
    if (ReadFixed(columns_.data() + pos, kWordSize, endian_) == *id)
      return pos / kWordSize;
  }
  return std::nullopt;
}

PackageIndex::Contribution PackageIndex::ReadContribution(uint32_t row, size_t column) const {
  size_t pos = (row - 1) * columns_.size() + column * kWordSize;
  return {ReadFixed(offsets_.data() + pos, kWordSize, endian_),
          ReadFixed(sizes_.data() + pos, kWordSize, endian_)};
}

}  // namespace zxdb
