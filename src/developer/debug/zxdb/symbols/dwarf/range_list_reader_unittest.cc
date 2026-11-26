// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/range_list_reader.h"

#include <initializer_list>
#include <thread>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"
#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_sections.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_context.h"
#include "src/developer/debug/zxdb/symbols/dwarf_attr.h"
#include "src/developer/debug/zxdb/symbols/dwarf_tag.h"

namespace zxdb {

namespace {

constexpr uint8_t kEndOfList = 0;
constexpr uint8_t kBaseAddressx = 1;
constexpr uint8_t kStartxEndx = 2;
constexpr uint8_t kStartxLength = 3;
constexpr uint8_t kOffsetPair = 4;
constexpr uint8_t kBaseAddress = 5;
constexpr uint8_t kStartEnd = 6;
constexpr uint8_t kStartLength = 7;

struct UnitBytes {
  struct Attribute {
    DwarfAttr attr;
    DwarfForm form;
    uint64_t value;
  };

  void WriteUnit(std::initializer_list<Attribute> attributes) {
    info.clear();
    abbrev.clear();
    std::vector<AbbrevTable::AttributeSpec> specs;
    for (const auto& attr : attributes)
      specs.push_back({static_cast<uint64_t>(attr.attr), static_cast<uint64_t>(attr.form), {}});
    DwarfSectionBuilder abbrev_builder(abbrev, endian);
    abbrev_builder.AbbrevDeclaration(1, static_cast<uint64_t>(DwarfTag::kCompileUnit), false,
                                     specs);
    abbrev_builder.EndAbbrevTable();

    DwarfSectionBuilder builder(info, endian);
    auto marker = builder.BeginUnit(header);
    builder.Uleb(1);
    for (const auto& attr : attributes) {
      switch (attr.form) {
        case DwarfForm::kAddr:
          builder.Address(attr.value, header.address_size);
          break;
        case DwarfForm::kSecOffset:
          builder.Offset(attr.value, header.offset_size);
          break;
        case DwarfForm::kData4:
          builder.Fixed(attr.value, 4);
          break;
        case DwarfForm::kSdata:
          builder.Sleb(attr.value);
          break;
        default:
          builder.Uleb(attr.value);
          break;
      }
    }
    builder.EndUnit(marker);
  }

  ErrOr<UnitContext> Parse(UnitContext::Options options = {}) const {
    DwarfSections sections;
    sections.endian = endian;
    sections.addr = addr;
    sections.ranges = ranges;
    if (options.is_dwo) {
      sections.info_dwo = info;
      sections.abbrev_dwo = abbrev;
      sections.rnglists_dwo = rnglists;
    } else {
      sections.info = info;
      sections.abbrev = abbrev;
      sections.rnglists = rnglists;
    }
    auto parsed_header = ReadUnitHeader(info, 0, endian, UnitSection::kInfo);
    if (parsed_header.has_error())
      return parsed_header.err();
    return UnitContext::Parse(sections, parsed_header.value(), UnitSection::kInfo, options);
  }

  DwarfSectionBuilder::LengthMarker BeginRnglists(uint32_t count, uint16_t version = 5,
                                                  uint8_t segment_size = 0) {
    auto builder = DwarfSectionBuilder(rnglists, endian);
    auto marker = builder.BeginInitialLength(header.offset_size);
    builder.Fixed(version, 2);
    builder.Fixed(header.address_size, 1);
    builder.Fixed(segment_size, 1);
    builder.Fixed(count, 4);
    return marker;
  }

  DwarfSectionBuilder::UnitHeaderOptions header;
  Endian endian = Endian::kLittle;
  std::vector<uint8_t> info, abbrev, addr, ranges, rnglists;
};

void WriteOperands(DwarfSectionBuilder& builder, uint8_t opcode,
                   std::initializer_list<uint64_t> operands) {
  builder.Fixed(opcode, 1);
  for (uint64_t value : operands)
    builder.Uleb(value);
}

void WriteAddressPair(DwarfSectionBuilder& builder, uint8_t address_size, uint64_t low,
                      uint64_t high) {
  builder.Address(low, address_size);
  builder.Address(high, address_size);
}

void ExpectRanges(const ErrOr<AddressRanges::RangeVector>& result,
                  const AddressRanges::RangeVector& expected) {
  ASSERT_TRUE(result.ok()) << result.err().msg();
  EXPECT_EQ(expected, result.value());
}

}  // namespace

TEST(RangeListReader, RnglistEncodings) {
  for (Endian endian : {Endian::kLittle, Endian::kBig}) {
    for (uint8_t width : {4, 8}) {
      SCOPED_TRACE(width);
      UnitBytes unit;
      unit.endian = endian;
      unit.header.offset_size = width;
      unit.header.address_size = width;
      uint64_t address = width == 8 ? 0x100000000 : 0x10000;
      DwarfSectionBuilder addr(unit.addr, endian);
      addr.Address(address + 0x100, width);
      addr.Address(address + 0x200, width);
      addr.Address(address + 0x220, width);

      // Start the unit's contribution after another valid table.
      DwarfSectionBuilder lists(unit.rnglists, endian);
      auto first = unit.BeginRnglists(0);
      lists.Fixed(kEndOfList, 1);
      lists.EndInitialLength(first);

      auto marker = unit.BeginRnglists(2);
      uint64_t base = unit.rnglists.size();
      lists.Offset(2 * width, width);      // First list immediately follows the array.
      lists.Offset(2 * width + 1, width);  // Second list follows an empty first list.
      lists.Fixed(kEndOfList, 1);
      uint64_t second_list = unit.rnglists.size();

      WriteOperands(lists, kOffsetPair, {1, 3});
      WriteOperands(lists, kBaseAddressx, {0});
      WriteOperands(lists, kOffsetPair, {4, 8});
      WriteOperands(lists, kStartxEndx, {1, 2});
      WriteOperands(lists, kStartxLength, {1, 0x120});
      lists.Fixed(kBaseAddress, 1);
      lists.Address(address + 0x400, width);
      WriteOperands(lists, kOffsetPair, {0x20, 0x24});
      lists.Fixed(kStartEnd, 1);
      WriteAddressPair(lists, width, address + 0x600, address + 0x640);
      lists.Fixed(kStartLength, 1);
      lists.Address(address + 0x500, width);
      lists.Uleb(0x50);
      lists.Fixed(kEndOfList, 1);
      lists.Fixed(0xff, 1);  // A reader must stop at the first end-of-list.
      lists.EndInitialLength(marker);

      unit.WriteUnit({{DwarfAttr::kLowPc, DwarfForm::kAddr, address},
                      {DwarfAttr::kAddrBase, DwarfForm::kSecOffset, 0},
                      {DwarfAttr::kRnglistsBase, DwarfForm::kSecOffset, base},
                      {DwarfAttr::kRanges, DwarfForm::kRnglistx, 1}});
      auto context = unit.Parse();
      ASSERT_TRUE(context.ok()) << context.err().msg();
      const auto& cu = context.value();
      AddressRanges::RangeVector expected{
          {address + 1, address + 3},         {address + 0x104, address + 0x108},
          {address + 0x200, address + 0x220}, {address + 0x200, address + 0x320},
          {address + 0x420, address + 0x424}, {address + 0x600, address + 0x640},
          {address + 0x500, address + 0x550},
      };
      ExpectRanges(RangeListReader::ReadFromIndex(cu, 0), {});
      ExpectRanges(RangeListReader::ReadFromIndex(cu, 1), expected);
      ExpectRanges(RangeListReader::ReadFromOffset(cu, second_list), expected);
      ExpectRanges(RangeListReader::ReadUnitRanges(cu), expected);
      EXPECT_TRUE(RangeListReader::ReadFromOffset(cu, base).has_error());
      EXPECT_TRUE(RangeListReader::ReadFromIndex(cu, 2).has_error());
      EXPECT_TRUE(RangeListReader::ReadFromIndex(cu, uint64_t{1} << 32).has_error());
    }
  }
}

TEST(RangeListReader, LegacyBaseSelection) {
  for (Endian endian : {Endian::kLittle, Endian::kBig}) {
    for (uint8_t size : {2, 4, 8}) {
      UnitBytes unit;
      unit.endian = endian;
      unit.header.version = 4;
      unit.header.address_size = size;
      unit.WriteUnit({{DwarfAttr::kLowPc, DwarfForm::kAddr, 0x100},
                      {DwarfAttr::kRanges, DwarfForm::kSecOffset, 0}});

      uint64_t marker = UINT64_MAX >> (64 - 8 * size);
      DwarfSectionBuilder ranges(unit.ranges, endian);
      WriteAddressPair(ranges, size, 0x20, 0x28);
      WriteAddressPair(ranges, size, 0x2a, 0x2a);        // Empty.
      WriteAddressPair(ranges, size, 0x2a, 0x29);        // Reversed.
      WriteAddressPair(ranges, size, marker - 1, 0x30);  // Tombstone start.
      WriteAddressPair(ranges, size, marker, marker - 1);
      WriteAddressPair(ranges, size, 0x10, 0x20);  // Tombstone base.
      WriteAddressPair(ranges, size, marker, 0x200);
      WriteAddressPair(ranges, size, 4, 12);
      WriteAddressPair(ranges, size, marker, 0);
      WriteAddressPair(ranges, size, 0, 3);
      WriteAddressPair(ranges, size, 0, 0);
      ranges.Fixed(0xff, 1);

      auto context = unit.Parse();
      ASSERT_TRUE(context.ok()) << context.err().msg();
      ExpectRanges(RangeListReader::ReadUnitRanges(context.value()),
                   {{0x120, 0x128}, {0x204, 0x20c}, {0, 3}});
      EXPECT_TRUE(RangeListReader::ReadFromOffset(context.value(), unit.ranges.size()).has_error());
      EXPECT_TRUE(RangeListReader::ReadFromIndex(context.value(), 0).has_error());
    }
  }
}

TEST(RangeListReader, LegacyMalformedAndOverflow) {
  UnitBytes unit;
  unit.header.version = 4;
  unit.WriteUnit({});
  DwarfSectionBuilder builder(unit.ranges, unit.endian);

  for (auto bytes :
       {std::vector<uint8_t>{1}, std::vector<uint8_t>(8, 0), std::vector<uint8_t>(16, 1)}) {
    unit.ranges = std::move(bytes);
    auto context = unit.Parse();
    ASSERT_TRUE(context.ok()) << context.err().msg();
    EXPECT_TRUE(RangeListReader::ReadFromOffset(context.value(), 0).has_error());
  }
  for (uint8_t size : {4, 8}) {
    unit.header.address_size = size;
    unit.WriteUnit({});
    unit.ranges.clear();
    uint64_t marker = UINT64_MAX >> (64 - 8 * size);
    WriteAddressPair(builder, size, marker, marker - 2);
    WriteAddressPair(builder, size, 1, 3);
    WriteAddressPair(builder, size, 0, 0);
    auto context = unit.Parse();
    ASSERT_TRUE(context.ok()) << context.err().msg();
    EXPECT_TRUE(RangeListReader::ReadFromOffset(context.value(), 0).has_error());
  }
}

TEST(RangeListReader, DirectRnglistWithoutBase) {
  UnitBytes unit;
  DwarfSectionBuilder builder(unit.rnglists, unit.endian);
  auto marker = unit.BeginRnglists(0);
  uint64_t offset = unit.rnglists.size();
  WriteOperands(builder, kOffsetPair, {0, 4});  // Missing unit base defaults to address zero.
  WriteOperands(builder, kOffsetPair, {8, 8});
  WriteOperands(builder, kOffsetPair, {8, 7});
  builder.Fixed(kStartLength, 1);
  builder.Address(UINT64_MAX, 8);  // Tombstone.
  builder.Uleb(5);
  builder.Fixed(kBaseAddress, 1);
  builder.Address(UINT64_MAX, 8);
  WriteOperands(builder, kOffsetPair, {1, 2});
  builder.Fixed(kStartEnd, 1);
  WriteAddressPair(builder, 8, 0x21, 0x20);
  builder.Fixed(kEndOfList, 1);
  builder.EndInitialLength(marker);

  unit.WriteUnit({{DwarfAttr::kRanges, DwarfForm::kSecOffset, offset}});
  auto context = unit.Parse();
  ASSERT_TRUE(context.ok()) << context.err().msg();
  ExpectRanges(RangeListReader::ReadUnitRanges(context.value()), {{0, 4}});
  EXPECT_TRUE(RangeListReader::ReadFromIndex(context.value(), 0).has_error());
  EXPECT_TRUE(RangeListReader::ReadFromOffset(context.value(), unit.rnglists.size()).has_error());
}

TEST(RangeListReader, RnglistTableValidation) {
  UnitBytes unit;
  DwarfSectionBuilder builder(unit.rnglists, unit.endian);
  for (const auto& bytes : {
           std::vector<uint8_t>{0xff, 0xff, 0xff, 0xff},              // Truncated DWARF64 length.
           std::vector<uint8_t>{0xf0, 0xff, 0xff, 0xff},              // Reserved length.
           std::vector<uint8_t>{7, 0, 0, 0, 5, 0, 8, 0, 0, 0, 0, 0},  // Header exceeds length.
           std::vector<uint8_t>{0xff, 0xee, 0xdd, 0x10},              // Length exceeds section.
           std::vector<uint8_t>{8, 0, 0, 0, 4, 0, 8, 0, 0, 0, 0, 0},  // Version 4.
           std::vector<uint8_t>{8, 0, 0, 0, 5, 0, 4, 0, 0, 0, 0, 0},  // Wrong address size.
           std::vector<uint8_t>{8, 0, 0, 0, 5, 0, 8, 1, 0, 0, 0, 0},  // Segment selector.
           std::vector<uint8_t>{8, 0, 0, 0, 5, 0, 8, 0, 0xff, 0xff, 0xff, 0xff},  // Count.
       }) {
    unit.rnglists = bytes;
    unit.WriteUnit({{DwarfAttr::kRnglistsBase, DwarfForm::kSecOffset, 12}});
    auto context = unit.Parse();
    ASSERT_TRUE(context.ok()) << context.err().msg();
    EXPECT_TRUE(RangeListReader::ReadFromIndex(context.value(), 0).has_error());
  }

  // Reject offsets into the offset array, just past the contribution, or too large to add.
  for (uint32_t relative : {0u, 3u, 5u, UINT32_MAX}) {
    unit.rnglists.clear();
    auto marker = unit.BeginRnglists(1);
    builder.Offset(relative, 4);
    builder.Fixed(kEndOfList, 1);
    builder.EndInitialLength(marker);
    auto context = unit.Parse();
    ASSERT_TRUE(context.ok()) << context.err().msg();
    EXPECT_TRUE(RangeListReader::ReadFromIndex(context.value(), 0).has_error());
  }

  unit.rnglists.clear();
  auto marker = unit.BeginRnglists(1);
  builder.Offset(4, 4);
  builder.Fixed(kEndOfList, 1);
  builder.EndInitialLength(marker);
  unit.WriteUnit({});
  auto implicit_base = unit.Parse();
  ASSERT_TRUE(implicit_base.ok()) << implicit_base.err().msg();
  ExpectRanges(RangeListReader::ReadFromIndex(implicit_base.value(), 0), {});

  unit.WriteUnit({{DwarfAttr::kRnglistsBase, DwarfForm::kSecOffset, 0}});
  auto invalid_base = unit.Parse();
  ASSERT_TRUE(invalid_base.ok()) << invalid_base.err().msg();
  EXPECT_TRUE(RangeListReader::ReadFromIndex(invalid_base.value(), 0).has_error());
}

TEST(RangeListReader, RnglistErrorsAreLocal) {
  for (const auto& invalid : {
           std::vector<uint8_t>{0xff},                  // Unknown opcode.
           std::vector<uint8_t>{kBaseAddressx, 9},      // Missing address index.
           std::vector<uint8_t>{kStartxEndx, 0, 9},     // Missing end address index.
           std::vector<uint8_t>{kStartxLength, 9, 4},   // Missing start address index.
           std::vector<uint8_t>{kOffsetPair, 1, 0x80},  // Truncated ULEB.
           std::vector<uint8_t>{kStartEnd, 0, 1},       // Truncated address.
           std::vector<uint8_t>{kOffsetPair, 1, 3},     // Missing end-of-list.
           std::vector<uint8_t>{kOffsetPair, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                                2, 0},  // ULEB overflow.
       }) {
    UnitBytes unit;
    unit.header.address_size = 4;
    unit.WriteUnit({{DwarfAttr::kRnglistsBase, DwarfForm::kSecOffset, 12},
                    {DwarfAttr::kAddrBase, DwarfForm::kSecOffset, 0}});
    DwarfSectionBuilder addr(unit.addr, unit.endian);
    addr.Address(0x100, 4);
    DwarfSectionBuilder builder(unit.rnglists, unit.endian);
    auto marker = unit.BeginRnglists(2);
    builder.Offset(8, 4);
    builder.Offset(9, 4);
    builder.Fixed(kEndOfList, 1);
    WriteOperands(builder, kOffsetPair, {0, 1});  // Valid prefix must be discarded on error.
    builder.Bytes(invalid);
    builder.EndInitialLength(marker);
    // A byte in the next contribution cannot finish a list or a truncated operand.
    marker = unit.BeginRnglists(0);
    builder.Fixed(kEndOfList, 1);
    builder.EndInitialLength(marker);

    auto context = unit.Parse();
    ASSERT_TRUE(context.ok()) << context.err().msg();
    ExpectRanges(RangeListReader::ReadFromIndex(context.value(), 0), {});
    EXPECT_TRUE(RangeListReader::ReadFromIndex(context.value(), 1).has_error());
    EXPECT_TRUE(RangeListReader::ReadFromOffset(context.value(), 21).has_error());
  }
}

TEST(RangeListReader, RnglistAddressOverflow) {
  for (uint8_t size : {4, 8}) {
    for (uint8_t encoding : {kStartLength, kStartxLength, kOffsetPair}) {
      UnitBytes unit;
      unit.header.address_size = size;
      uint64_t base = (UINT64_MAX >> (64 - 8 * size)) - 2;
      DwarfSectionBuilder addr(unit.addr, unit.endian);
      addr.Address(base, size);
      unit.WriteUnit({{DwarfAttr::kLowPc, DwarfForm::kAddr, base},
                      {DwarfAttr::kAddrBase, DwarfForm::kSecOffset, 0}});
      DwarfSectionBuilder builder(unit.rnglists, unit.endian);
      auto marker = unit.BeginRnglists(1);
      builder.Offset(4, 4);
      builder.Fixed(encoding, 1);
      if (encoding == kStartLength)
        builder.Address(base, size);
      else
        builder.Uleb(0);
      builder.Uleb(3);
      builder.Fixed(kEndOfList, 1);
      builder.EndInitialLength(marker);

      auto context = unit.Parse();
      ASSERT_TRUE(context.ok()) << context.err().msg();
      EXPECT_TRUE(RangeListReader::ReadFromIndex(context.value(), 0).has_error());
    }
  }
}

TEST(RangeListReader, LegacySplitUsesSkeletonRangesAndBase) {
  UnitBytes skeleton;
  skeleton.header.version = 4;
  skeleton.WriteUnit({{DwarfAttr::kLowPc, DwarfForm::kAddr, 0x100},
                      {DwarfAttr::kGnuRangesBase, DwarfForm::kSecOffset, 32},
                      {DwarfAttr::kRanges, DwarfForm::kSecOffset, 0}});
  DwarfSectionBuilder builder(skeleton.ranges, skeleton.endian);
  WriteAddressPair(builder, 8, 1, 2);
  WriteAddressPair(builder, 8, 0, 0);
  WriteAddressPair(builder, 8, 0x20, 0x30);
  WriteAddressPair(builder, 8, 0, 0);
  auto main = skeleton.Parse();
  ASSERT_TRUE(main.ok()) << main.err().msg();
  ExpectRanges(RangeListReader::ReadUnitRanges(main.value()), {{0x101, 0x102}});

  UnitBytes split;
  split.header.version = 4;
  split.WriteUnit({{DwarfAttr::kRanges, DwarfForm::kSecOffset, 0}});
  auto context = split.Parse({.is_dwo = true, .skeleton = &main.value()});
  ASSERT_TRUE(context.ok()) << context.err().msg();
  ExpectRanges(RangeListReader::ReadUnitRanges(context.value()), {{0x120, 0x130}});
  EXPECT_TRUE(RangeListReader::ReadFromOffset(context.value(), UINT64_MAX).has_error());
}

TEST(RangeListReader, SplitRnglistsUseSkeletonAddresses) {
  UnitBytes skeleton;
  DwarfSectionBuilder addr(skeleton.addr, skeleton.endian);
  addr.Address(0x100, 8);
  skeleton.WriteUnit({{DwarfAttr::kAddrBase, DwarfForm::kSecOffset, 0},
                      {DwarfAttr::kLowPc, DwarfForm::kAddr, 0x200}});
  auto main = skeleton.Parse();
  ASSERT_TRUE(main.ok()) << main.err().msg();

  for (uint64_t start : {0, 11}) {
    UnitBytes split;
    split.header.unit_type = kUnitTypeSplitCompile;
    split.WriteUnit({{DwarfAttr::kRanges, DwarfForm::kRnglistx, 0}});
    split.rnglists.resize(start);
    DwarfSectionBuilder builder(split.rnglists, split.endian);
    auto marker = split.BeginRnglists(1);
    builder.Offset(4, 4);
    WriteOperands(builder, kOffsetPair, {2, 5});
    WriteOperands(builder, kStartxLength, {0, 4});
    builder.Fixed(kEndOfList, 1);
    builder.EndInitialLength(marker);

    UnitContext::Options options{.is_dwo = true, .skeleton = &main.value()};
    if (start != 0) {
      options.package = UnitContext::PackageContributions{
          .rnglists = UnitContext::Contribution{start, split.rnglists.size() - start}};
    }
    auto context = split.Parse(options);
    ASSERT_TRUE(context.ok()) << context.err().msg();
    AddressRanges::RangeVector expected{{0x202, 0x205}, {0x100, 0x104}};
    std::thread index_reader(
        [&] { ExpectRanges(RangeListReader::ReadUnitRanges(context.value()), expected); });
    auto from_offset = RangeListReader::ReadFromOffset(context.value(), 16);
    index_reader.join();
    ExpectRanges(from_offset, expected);
    if (options.package) {
      options.package->rnglists->size--;
      auto truncated = split.Parse(options);
      ASSERT_TRUE(truncated.ok()) << truncated.err().msg();
      EXPECT_TRUE(RangeListReader::ReadFromIndex(truncated.value(), 0).has_error());
    }
  }
}

TEST(RangeListReader, UnitLowAndHighPC) {
  UnitBytes unit;
  unit.header.address_size = 4;
  unit.WriteUnit({{DwarfAttr::kLowPc, DwarfForm::kAddr, 0x1200},
                  {DwarfAttr::kHighPc, DwarfForm::kData4, 0x20},
                  {DwarfAttr::kRanges, DwarfForm::kRnglistx, 99}});
  auto context = unit.Parse();
  ASSERT_TRUE(context.ok()) << context.err().msg();
  ExpectRanges(RangeListReader::ReadUnitRanges(context.value()), {{0x1200, 0x1220}});

  unit.WriteUnit({{DwarfAttr::kLowPc, DwarfForm::kAddr, 0x1200},
                  {DwarfAttr::kHighPc, DwarfForm::kSdata, 0x80}});
  context = unit.Parse();
  ASSERT_TRUE(context.ok()) << context.err().msg();
  ExpectRanges(RangeListReader::ReadUnitRanges(context.value()), {{0x1200, 0x1280}});

  for (uint64_t high : {0x22, 0x21, 0x20}) {
    unit.WriteUnit({{DwarfAttr::kLowPc, DwarfForm::kAddr, 0x21},
                    {DwarfAttr::kHighPc, DwarfForm::kAddr, high}});
    context = unit.Parse();
    ASSERT_TRUE(context.ok()) << context.err().msg();
    AddressRanges::RangeVector expected =
        high > 0x21 ? AddressRanges::RangeVector{{0x21, high}} : AddressRanges::RangeVector{};
    ExpectRanges(RangeListReader::ReadUnitRanges(context.value()), expected);
  }
  unit.WriteUnit({{DwarfAttr::kLowPc, DwarfForm::kAddr, UINT32_MAX - 1},
                  {DwarfAttr::kHighPc, DwarfForm::kUdata, 2}});
  context = unit.Parse();
  ASSERT_TRUE(context.ok()) << context.err().msg();
  EXPECT_TRUE(RangeListReader::ReadUnitRanges(context.value()).has_error());

  DwarfSectionBuilder builder(unit.rnglists, unit.endian);
  auto marker = unit.BeginRnglists(1);
  builder.Offset(4, 4);
  builder.Fixed(kStartEnd, 1);
  WriteAddressPair(builder, 4, 0x80, 0x90);
  builder.Fixed(kEndOfList, 1);
  builder.EndInitialLength(marker);
  unit.WriteUnit({{DwarfAttr::kLowPc, DwarfForm::kAddr, UINT32_MAX},
                  {DwarfAttr::kHighPc, DwarfForm::kUdata, 2},
                  {DwarfAttr::kRanges, DwarfForm::kRnglistx, 0}});
  context = unit.Parse();
  ASSERT_TRUE(context.ok()) << context.err().msg();
  ExpectRanges(RangeListReader::ReadUnitRanges(context.value()), {{0x80, 0x90}});
}

}  // namespace zxdb
