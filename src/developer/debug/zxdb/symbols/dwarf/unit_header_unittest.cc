// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"

#include <string>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"

namespace zxdb {
namespace {

using UnitHeaderOptions = DwarfSectionBuilder::UnitHeaderOptions;

// Appends a header and one byte of DIE data.
void AppendUnit(DwarfSectionBuilder& builder, const UnitHeaderOptions& options) {
  auto marker = builder.BeginUnit(options);
  builder.Fixed(1, 1);
  builder.EndUnit(marker);
}

TEST(UnitHeader, LegacyInfo) {
  for (uint16_t version : {2, 3, 4}) {
    SCOPED_TRACE(version);
    std::vector<uint8_t> bytes;
    DwarfSectionBuilder builder(bytes);
    AppendUnit(builder, {.version = version, .address_size = 4, .abbrev_offset = 0x01020304});

    auto result = ReadUnitHeader(bytes, 0, true, UnitSection::kInfo);
    ASSERT_TRUE(result.ok()) << result.err().msg();
    const auto& header = result.value();
    EXPECT_EQ(0u, header.offset);
    EXPECT_EQ(8u, header.length);
    EXPECT_EQ(1u, header.unit_type);
    EXPECT_EQ(0x01020304u, header.abbrev_offset);
    EXPECT_EQ(11u, header.header_size);
    EXPECT_EQ(11u, header.die_offset());
    EXPECT_EQ(12u, header.next_unit_offset());
    EXPECT_FALSE(header.is_type_unit());
    EXPECT_EQ(std::nullopt, header.type_signature);
    EXPECT_EQ(std::nullopt, header.type_offset);
    EXPECT_EQ(std::nullopt, header.dwo_id);
    FormParams params = header.form_params();
    EXPECT_EQ(version, params.version);
    EXPECT_EQ(4u, params.offset_size);
    EXPECT_EQ(4u, params.address_size);
  }
}

TEST(UnitHeader, Dwarf4Types) {
  std::vector<uint8_t> bytes;
  DwarfSectionBuilder builder(bytes);
  AppendUnit(builder, {.section = UnitSection::kTypes, .version = 4, .type_offset = 23});

  auto result = ReadUnitHeader(bytes, 0, true, UnitSection::kTypes);
  ASSERT_TRUE(result.ok()) << result.err().msg();
  const auto& header = result.value();
  EXPECT_EQ(0u, header.offset);
  EXPECT_EQ(20u, header.length);
  EXPECT_EQ(4u, header.version);
  EXPECT_EQ(2u, header.unit_type);
  EXPECT_EQ(0u, header.abbrev_offset);
  EXPECT_TRUE(header.is_type_unit());
  EXPECT_EQ(0u, header.type_signature);
  EXPECT_EQ(23u, header.type_offset);
  EXPECT_EQ(std::nullopt, header.dwo_id);
  EXPECT_EQ(23u, header.die_offset());
  EXPECT_EQ(24u, header.next_unit_offset());
}

TEST(UnitHeader, Dwarf5UnitTypes) {
  struct TestCase {
    uint8_t unit_type;
    uint8_t die_offset;
    std::optional<uint64_t> signature;
    std::optional<uint64_t> dwo_id;
  };
  constexpr uint64_t kSignature = 0x0123456789abcdef;
  constexpr uint64_t kDwoId = 0xfedcba9876543210;
  const TestCase cases[] = {
      {1, 12, {}, {}},          // DW_UT_compile
      {2, 24, kSignature, {}},  // DW_UT_type
      {3, 12, {}, {}},          // DW_UT_partial
      {4, 20, {}, kDwoId},      // DW_UT_skeleton
      {5, 20, {}, kDwoId},      // DW_UT_split_compile
      {6, 24, kSignature, {}},  // DW_UT_split_type
  };
  for (const TestCase& test : cases) {
    SCOPED_TRACE(test.unit_type);
    std::vector<uint8_t> bytes;
    DwarfSectionBuilder builder(bytes);
    AppendUnit(builder, {.unit_type = test.unit_type,
                         .address_size = 2,
                         .abbrev_offset = 0x01020304,
                         .type_signature = kSignature,
                         .type_offset = test.die_offset,
                         .dwo_id = kDwoId});

    auto result = ReadUnitHeader(bytes, 0, true, UnitSection::kInfo);
    ASSERT_TRUE(result.ok()) << result.err().msg();
    const auto& header = result.value();
    EXPECT_EQ(5u, header.version);
    EXPECT_EQ(test.unit_type, header.unit_type);
    EXPECT_EQ(2u, header.address_size);
    EXPECT_EQ(4u, header.offset_size);
    EXPECT_EQ(0x01020304u, header.abbrev_offset);
    EXPECT_EQ(test.signature.has_value(), header.is_type_unit());
    EXPECT_EQ(test.signature, header.type_signature);
    EXPECT_EQ(test.signature ? std::optional<uint64_t>(test.die_offset) : std::nullopt,
              header.type_offset);
    EXPECT_EQ(test.dwo_id, header.dwo_id);
    EXPECT_EQ(test.die_offset, header.die_offset());
    EXPECT_EQ(test.die_offset + 1u, header.next_unit_offset());
  }
}

TEST(UnitHeader, Dwarf64BigEndian) {
  constexpr uint64_t kAbbrevOffset = 0x0102030405060708;
  constexpr uint64_t kSignature = 0xfedcba9876543210;
  std::vector<uint8_t> bytes(3, 0);
  DwarfSectionBuilder builder(bytes, false);
  AppendUnit(builder, {.offset_size = 8,
                       .unit_type = kUnitTypeSplitType,
                       .abbrev_offset = kAbbrevOffset,
                       .type_signature = kSignature,
                       .type_offset = 40});

  const std::vector<uint8_t> expected = {
      0,    0,    0,                                   // Bytes before the unit.
      0xff, 0xff, 0xff, 0xff,                          // DWARF64 marker.
      0,    0,    0,    0,    0,    0,    0,    29,    // Length.
      0,    5,    6,    8,                             // Version, unit type, and address size.
      1,    2,    3,    4,    5,    6,    7,    8,     // Abbreviation offset.
      0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,  // Signature.
      0,    0,    0,    0,    0,    0,    0,    40,    // Type offset.
      1,                                               // DIE data.
  };
  EXPECT_EQ(expected, bytes);

  auto result = ReadUnitHeader(bytes, 3, false, UnitSection::kInfo);
  ASSERT_TRUE(result.ok()) << result.err().msg();
  const auto& header = result.value();
  EXPECT_EQ(3u, header.offset);
  EXPECT_EQ(29u, header.length);
  EXPECT_EQ(8u, header.offset_size);
  EXPECT_EQ(5u, header.version);
  EXPECT_EQ(6u, header.unit_type);
  EXPECT_EQ(8u, header.address_size);
  EXPECT_EQ(kAbbrevOffset, header.abbrev_offset);
  EXPECT_EQ(kSignature, header.type_signature);
  EXPECT_EQ(40u, header.type_offset);
  EXPECT_EQ(43u, header.die_offset());
  EXPECT_EQ(44u, header.next_unit_offset());
}

TEST(UnitHeader, HeaderOnly) {
  std::vector<uint8_t> bytes;
  DwarfSectionBuilder builder(bytes);
  builder.EndUnit(builder.BeginUnit({.unit_type = kUnitTypeSplitCompile}));

  auto result = ReadUnitHeader(bytes, 0, true, UnitSection::kInfo);
  ASSERT_TRUE(result.ok()) << result.err().msg();
  EXPECT_EQ(0u, result.value().dwo_id);
  EXPECT_EQ(20u, result.value().die_offset());
  EXPECT_EQ(20u, result.value().next_unit_offset());
}

TEST(UnitHeader, TruncatedFields) {
  struct TestCase {
    UnitHeaderOptions options;
    size_t length_size;
    size_t header_size;
  };
  const TestCase cases[] = {
      {{.version = 2}, 4, 11},
      {{.section = UnitSection::kTypes, .version = 4}, 4, 23},
      {{.offset_size = 8, .unit_type = kUnitTypeType}, 12, 40},
      {{.unit_type = kUnitTypeSkeleton}, 4, 20},
  };

  for (const TestCase& test : cases) {
    // Set the length to the number of bytes present so parsing reaches the truncated field.
    for (size_t size = 0; size < test.header_size; ++size) {
      SCOPED_TRACE(test.header_size);
      SCOPED_TRACE(size);
      std::vector<uint8_t> bytes;
      DwarfSectionBuilder builder(bytes);
      auto marker = builder.BeginUnit(test.options);
      builder.EndUnit(marker, size < test.length_size ? 0 : size - test.length_size);
      bytes.resize(size);

      auto result = ReadUnitHeader(bytes, 0, true, test.options.section);
      ASSERT_TRUE(result.has_error());
      EXPECT_NE(
          std::string::npos,
          result.err().msg().find(size < test.length_size ? "initial length" : "truncated header"));
    }
  }
}

TEST(UnitHeader, InvalidLength) {
  // There are eight bytes after the initial length field in this buffer.
  for (uint64_t length : {0u, 7u, 9u, 0xfffffff0u, 0xfffffffeu}) {
    SCOPED_TRACE(length);
    std::vector<uint8_t> bytes;
    DwarfSectionBuilder builder(bytes);
    auto marker = builder.BeginUnit({});
    builder.EndUnit(marker, length);
    EXPECT_TRUE(ReadUnitHeader(bytes, 0, true, UnitSection::kInfo).has_error());
  }

  std::vector<uint8_t> bytes = {0};
  DwarfSectionBuilder builder(bytes);
  auto marker = builder.BeginUnit({.offset_size = 8});
  builder.EndUnit(marker, UINT64_MAX);
  EXPECT_TRUE(ReadUnitHeader(bytes, 1, true, UnitSection::kInfo).has_error());
  EXPECT_TRUE(ReadUnitHeader(bytes, UINT64_MAX, true, UnitSection::kInfo).has_error());
}

TEST(UnitHeader, InvalidVersionAndAddressSize) {
  for (const UnitHeaderOptions& options :
       {UnitHeaderOptions{.version = 1}, UnitHeaderOptions{.version = 6},
        UnitHeaderOptions{.address_size = 0}, UnitHeaderOptions{.address_size = 1},
        UnitHeaderOptions{.address_size = 3}}) {
    SCOPED_TRACE(options.version);
    SCOPED_TRACE(options.address_size);
    std::vector<uint8_t> bytes;
    DwarfSectionBuilder builder(bytes);
    AppendUnit(builder, options);
    EXPECT_TRUE(ReadUnitHeader(bytes, 0, true, UnitSection::kInfo).has_error());
  }
}

TEST(UnitHeader, InvalidTypeOffset) {
  for (uint64_t type_offset : {0u, 23u, 25u, 26u}) {
    SCOPED_TRACE(type_offset);
    std::vector<uint8_t> bytes;
    DwarfSectionBuilder builder(bytes);
    AppendUnit(builder, {.unit_type = kUnitTypeType, .type_offset = type_offset});
    EXPECT_TRUE(ReadUnitHeader(bytes, 0, true, UnitSection::kInfo).has_error());
  }
}

TEST(UnitList, MixedFormats) {
  std::vector<uint8_t> bytes;
  DwarfSectionBuilder builder(bytes);
  AppendUnit(builder, {.version = 2});
  AppendUnit(builder, {.offset_size = 8});
  AppendUnit(builder, {.unit_type = kUnitTypeSkeleton});

  const auto list = UnitList::Parse(bytes, true, UnitSection::kInfo);
  ASSERT_EQ(3u, list.size());
  EXPECT_FALSE(list.error().has_error());
  const uint64_t offsets[] = {0, 12, 37, 58};
  const uint8_t offset_sizes[] = {4, 8, 4};
  size_t index = 0;
  for (const UnitHeader& unit : list) {
    EXPECT_EQ(offsets[index], unit.offset);
    EXPECT_EQ(offset_sizes[index], unit.offset_size);
    EXPECT_EQ(offsets[index + 1], unit.next_unit_offset());
    EXPECT_EQ(index, list.FindIndexForOffset(offsets[index]));
    EXPECT_EQ(index, list.FindIndexForOffset(offsets[index + 1] - 1));
    ++index;
  }
  EXPECT_EQ(std::nullopt, list.FindIndexForOffset(58));
  EXPECT_EQ(std::nullopt, list.FindIndexForOffset(59));
  EXPECT_EQ(std::nullopt, list.FindIndexForOffset(UINT64_MAX));
}

TEST(UnitList, VendorUnitType) {
  constexpr uint8_t kVendorUnitType = 0x80;  // DW_UT_lo_user.
  std::vector<uint8_t> bytes;
  DwarfSectionBuilder builder(bytes);
  AppendUnit(builder, {.unit_type = kVendorUnitType});
  AppendUnit(builder, {});

  const auto list = UnitList::Parse(bytes, true, UnitSection::kInfo);
  ASSERT_EQ(2u, list.size());
  EXPECT_FALSE(list.error().has_error());
  EXPECT_EQ(kVendorUnitType, list[0].unit_type);
  EXPECT_FALSE(list[0].is_type_unit());
  EXPECT_EQ(12u, list[0].die_offset());
  EXPECT_EQ(13u, list[0].next_unit_offset());
  EXPECT_EQ(13u, list[1].offset);
  EXPECT_EQ(kUnitTypeCompile, list[1].unit_type);
}

TEST(UnitList, StopsAtFirstError) {
  std::vector<uint8_t> bytes;
  DwarfSectionBuilder builder(bytes);
  AppendUnit(builder, {});
  AppendUnit(builder, {.offset_size = 8});
  builder.Fixed(0, 4);  // A unit with length zero.
  AppendUnit(builder, {});

  auto list = UnitList::Parse(bytes, true, UnitSection::kInfo);
  ASSERT_EQ(2u, list.size());
  EXPECT_EQ(0u, list[0].offset);
  EXPECT_EQ(13u, list[1].offset);
  EXPECT_EQ(1u, list.FindIndexForOffset(37));
  EXPECT_EQ(std::nullopt, list.FindIndexForOffset(38));
  EXPECT_EQ(std::nullopt, list.FindIndexForOffset(42));
  ASSERT_TRUE(list.error().has_error());
  EXPECT_EQ("DWARF unit at offset 0x26: truncated header", list.error().msg());
}

TEST(UnitList, EmptySection) {
  auto list = UnitList::Parse({}, true, UnitSection::kTypes);
  EXPECT_EQ(0u, list.size());
  EXPECT_EQ(list.begin(), list.end());
  EXPECT_FALSE(list.error().has_error());
  EXPECT_EQ(std::nullopt, list.FindIndexForOffset(0));
}

}  // namespace
}  // namespace zxdb
