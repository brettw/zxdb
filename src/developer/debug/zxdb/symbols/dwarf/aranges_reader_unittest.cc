// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/aranges_reader.h"

#include <initializer_list>
#include <utility>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"

namespace zxdb {

namespace {

struct Header {
  uint8_t offset_size = 4;
  uint8_t address_size = 8;
  uint16_t version = 2;
  uint8_t segment_size = 0;
  uint64_t unit_offset = 0;
};

void WriteSet(std::vector<uint8_t>& bytes, Endian endian, const Header& header,
              std::initializer_list<std::pair<uint64_t, uint64_t>> tuples) {
  uint64_t start = bytes.size();
  DwarfSectionBuilder builder(bytes, endian);
  auto marker = builder.BeginInitialLength(header.offset_size);
  builder.Fixed(header.version, 2);
  builder.Offset(header.unit_offset, header.offset_size);
  builder.Fixed(header.address_size, 1);
  builder.Fixed(header.segment_size, 1);
  uint64_t tuple_size = 2 * header.address_size;
  while ((bytes.size() - start) % tuple_size != 0)
    builder.Fixed(0xab, 1);
  for (const auto& [address, length] : tuples) {
    builder.Address(address, header.address_size);
    builder.Address(length, header.address_size);
  }
  builder.EndInitialLength(marker);
}

}  // namespace

TEST(ArangesReader, FormatsAndByteOrders) {
  for (Endian endian : {Endian::kLittle, Endian::kBig}) {
    for (uint8_t width : {4, 8}) {
      for (uint8_t address_size : {2, 4, 8}) {
        SCOPED_TRACE(testing::Message()
                     << "offset size " << +width << ", address size " << +address_size);
        std::vector<uint8_t> bytes;
        // This 20-byte set puts the next set off a section-relative 8- or 16-byte boundary.
        WriteSet(bytes, endian, {.address_size = 2}, {{10, 2}, {0, 0}});
        auto first = ArangesSet::Parse(bytes, 0, endian);
        ASSERT_TRUE(first.ok()) << first.err().msg();
        EXPECT_EQ(0u, first.value().unit_offset());
        EXPECT_EQ(20u, first.value().next_offset());
        ASSERT_EQ(1u, first.value().ranges().size());
        EXPECT_EQ(AddressRange(10, 12), first.value().ranges()[0]);

        uint64_t offset = bytes.size();
        uint64_t cu = width == 8 ? 0x1020304050 : 0x10203040;
        uint64_t address = address_size == 8 ? 0x100000000 : 0x1020;
        WriteSet(bytes, endian, {width, address_size, 2, 0, cu}, {{address, 0x34}, {0, 0}});
        auto second = ArangesSet::Parse(bytes, offset, endian);
        ASSERT_TRUE(second.ok()) << second.err().msg();
        EXPECT_EQ(cu, second.value().unit_offset());
        EXPECT_EQ(bytes.size(), second.value().next_offset());
        ASSERT_EQ(1u, second.value().ranges().size());
        EXPECT_EQ(AddressRange(address, address + 0x34), second.value().ranges()[0]);
        EXPECT_TRUE(second.value().premature_terminators().empty());
        EXPECT_TRUE(ArangesSet::Parse(bytes, bytes.size(), endian).has_error());
      }
    }
  }
}

TEST(ArangesReader, TuplesAndPrematureTerminators) {
  for (uint8_t size : {4, 8}) {
    std::vector<uint8_t> bytes;
    uint64_t max_address = UINT64_MAX >> (64 - 8 * size);
    WriteSet(bytes, Endian::kLittle, {.address_size = size},
             {{0, 0},
              {0, 0x34},
              {0x100, 0},
              {max_address - 1, 2},
              {0, 0},
              {max_address - 1, 1},
              {0, 0}});
    auto result = ArangesSet::Parse(bytes, 0, Endian::kLittle);
    ASSERT_TRUE(result.ok()) << result.err().msg();
    auto ranges = result.value().ranges();
    ASSERT_EQ(2u, ranges.size());
    EXPECT_EQ(AddressRange(0, 0x34), ranges[0]);
    EXPECT_EQ(AddressRange(max_address - 1, max_address), ranges[1]);
    auto warnings = result.value().premature_terminators();
    ASSERT_EQ(2u, warnings.size());
    EXPECT_EQ(16u, warnings[0]);
    EXPECT_EQ(16u + 8 * size, warnings[1]);
  }
}

TEST(ArangesReader, HeaderAndTerminatorValidation) {
  constexpr Endian kEndian = Endian::kLittle;
  for (Header header : {Header{.version = 1}, Header{.version = 6}, Header{.segment_size = 1}}) {
    std::vector<uint8_t> bytes;
    WriteSet(bytes, kEndian, header, {{1, 2}, {0, 0}});
    EXPECT_TRUE(ArangesSet::Parse(bytes, 0, kEndian).has_error());
  }
  for (uint16_t version : {2, 3, 4, 5}) {
    std::vector<uint8_t> bytes;
    WriteSet(bytes, kEndian, {.version = version}, {{0, 0}});
    auto empty = ArangesSet::Parse(bytes, 0, kEndian);
    ASSERT_TRUE(empty.ok()) << empty.err().msg();
    EXPECT_TRUE(empty.value().ranges().empty());
  }

  for (uint8_t width : {4, 8}) {
    std::vector<uint8_t> bytes;
    WriteSet(bytes, kEndian, {.offset_size = width}, {{0, 0}, {1, 2}});
    EXPECT_TRUE(ArangesSet::Parse(bytes, 0, kEndian).has_error());  // Last tuple must terminate.
    bytes.clear();
    WriteSet(bytes, kEndian, {.offset_size = width}, {{1, 2}, {0, 0}});
    for (size_t size = 0; size < bytes.size(); size++) {
      EXPECT_TRUE(ArangesSet::Parse(std::span(bytes).first(size), 0, kEndian).has_error()) << size;
    }
  }

  std::vector<uint8_t> bytes;
  WriteSet(bytes, kEndian, {}, {{0, 0}});
  bytes[10] = 1;  // Unsupported address size.
  EXPECT_TRUE(ArangesSet::Parse(bytes, 0, kEndian).has_error());
  bytes[10] = 8;
  bytes[0]--;  // The declared length leaves a partial tuple.
  EXPECT_TRUE(ArangesSet::Parse(bytes, 0, kEndian).has_error());
  bytes[0] = 4;  // The declared length ends inside the header.
  EXPECT_TRUE(ArangesSet::Parse(bytes, 0, kEndian).has_error());
  EXPECT_TRUE(ArangesSet::Parse(bytes, UINT64_MAX, kEndian).has_error());
}

}  // namespace zxdb
