// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/unit_context.h"

#include <initializer_list>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"
#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_sections.h"
#include "src/developer/debug/zxdb/symbols/dwarf_attr.h"
#include "src/developer/debug/zxdb/symbols/dwarf_tag.h"

namespace zxdb {

namespace {

AbbrevTable::AttributeSpec Attr(DwarfAttr attr, DwarfForm form) {
  return {static_cast<uint64_t>(attr), static_cast<uint64_t>(form), std::nullopt};
}

struct UnitBytes {
  template <typename Write>
  void WriteUnit(std::initializer_list<AbbrevTable::AttributeSpec> attributes, Write write) {
    DwarfSectionBuilder abbrev_builder(abbrev, endian);
    options.abbrev_offset = abbrev.size();
    abbrev_builder.AbbrevDeclaration(1, static_cast<uint64_t>(tag), false, attributes);
    abbrev_builder.EndAbbrevTable();

    unit_offset = info.size();
    DwarfSectionBuilder info_builder(info, endian);
    auto marker = info_builder.BeginUnit(options);
    info_builder.Uleb(1);
    write(info_builder);
    info_builder.EndUnit(marker);
  }

  DwarfSections sections() const {
    DwarfSections result;
    result.endian = endian;
    if (is_dwo) {
      (options.section == UnitSection::kTypes ? result.types_dwo : result.info_dwo) = info;
      result.abbrev_dwo = abbrev;
      result.str_dwo = str;
      result.str_offsets_dwo = str_offsets;
      result.rnglists_dwo = rnglists;
      result.loclists_dwo = loclists;
    } else {
      (options.section == UnitSection::kTypes ? result.types : result.info) = info;
      result.abbrev = abbrev;
      result.str = str;
      result.str_offsets = str_offsets;
      result.rnglists = rnglists;
      result.loclists = loclists;
    }
    result.addr = addr;
    result.line_str = line_str;
    return result;
  }

  ErrOr<UnitContext> Parse(UnitContext::Options parse_options = {}) const {
    auto header = ReadUnitHeader(info, unit_offset, endian, options.section);
    if (!header)
      return header.err();
    parse_options.is_dwo = is_dwo;
    return UnitContext::Parse(sections(), header.value(), options.section, parse_options);
  }

  DwarfSectionBuilder::UnitHeaderOptions options;
  DwarfTag tag = DwarfTag::kCompileUnit;
  bool is_dwo = false;
  Endian endian = Endian::kLittle;
  uint64_t unit_offset = 0;
  std::vector<uint8_t> info, abbrev, str, line_str, str_offsets, addr, rnglists, loclists;
};

void WriteStringOffsets(DwarfSectionBuilder builder, uint8_t width,
                        std::initializer_list<uint64_t> offsets) {
  auto marker = builder.BeginInitialLength(width);
  builder.Fixed(5, 2);
  builder.Fixed(0, 2);
  for (uint64_t offset : offsets)
    builder.Offset(offset, width);
  builder.EndInitialLength(marker);
}

void WriteListTable(DwarfSectionBuilder builder, uint8_t width, uint8_t address_size,
                    uint32_t count, std::initializer_list<uint64_t> offsets) {
  auto marker = builder.BeginInitialLength(width);
  builder.Fixed(5, 2);
  builder.Fixed(address_size, 1);
  builder.Fixed(0, 1);  // Segment selector size.
  builder.Fixed(count, 4);
  for (uint64_t offset : offsets)
    builder.Offset(offset, width);
  builder.Fixed(0, 1);  // DW_LLE_end_of_list or DW_RLE_end_of_list.
  builder.EndInitialLength(marker);
}

}  // namespace

TEST(UnitContext, NoAttributes) {
  for (uint16_t version : {4, 5}) {
    for (uint8_t width : {4, 8}) {
      UnitBytes unit;
      unit.options.version = version;
      unit.options.offset_size = width;
      unit.WriteUnit({}, [](auto&) {});
      auto context = unit.Parse();
      ASSERT_TRUE(context.ok()) << context.err().msg();
      EXPECT_EQ(std::nullopt, context.value().str_offsets_base());
      EXPECT_EQ(std::nullopt, context.value().addr_base());
      EXPECT_EQ(std::nullopt, context.value().rnglists_base());
      EXPECT_EQ(std::nullopt, context.value().loclists_base());
      EXPECT_EQ(std::nullopt, context.value().ranges_base());
      EXPECT_EQ(std::nullopt, context.value().compilation_dir());
      EXPECT_EQ(std::nullopt, context.value().base_address());
      EXPECT_EQ(std::nullopt, context.value().low_pc());
      EXPECT_FALSE(context.value().ranges());
      EXPECT_EQ(std::nullopt, context.value().stmt_list());
      EXPECT_EQ(std::nullopt, context.value().ReadAddress(0));
      EXPECT_EQ(std::nullopt, context.value().ReadLoclistOffset(0));
    }
  }
}

TEST(UnitContext, IndexedAttributesAndBases) {
  for (uint8_t width : {4, 8}) {
    for (Endian endian : {Endian::kLittle, Endian::kBig}) {
      SCOPED_TRACE(testing::Message()
                   << "width " << +width << ", "
                   << (endian == Endian::kLittle ? "little endian" : "big endian"));
      UnitBytes unit;
      unit.options.offset_size = width;
      unit.options.address_size = 4;
      unit.endian = endian;

      AppendCString(unit.str, "");
      AppendCString(unit.str, "/directory");
      AppendCString(unit.line_str, "/line-directory");
      WriteStringOffsets(DwarfSectionBuilder(unit.str_offsets, endian), width, {0, 1});
      uint64_t str_end = unit.str_offsets.size();
      WriteStringOffsets(DwarfSectionBuilder(unit.str_offsets, endian), width, {1});
      const uint64_t str_base = width == 4 ? 8 : 16;
      const uint64_t loc_base = width == 4 ? 12 : 20;
      const uint64_t addr_base = width == 4 ? 8 : 16;

      DwarfSectionBuilder addresses(unit.addr, endian);
      auto addr_length = addresses.BeginInitialLength(width);
      addresses.Fixed(5, 2);
      addresses.Fixed(unit.options.address_size, 1);
      addresses.Fixed(0, 1);
      addresses.Address(0, unit.options.address_size);
      addresses.Address(0x12345678, unit.options.address_size);
      addresses.EndInitialLength(addr_length);

      WriteListTable(DwarfSectionBuilder(unit.loclists, endian), width, 4, 1, {width});
      WriteListTable(DwarfSectionBuilder(unit.rnglists, endian), width, 4, 1, {width});
      unit.WriteUnit(
          {Attr(DwarfAttr::kCompDir, DwarfForm::kStrx2), Attr(DwarfAttr::kLowPc, DwarfForm::kAddrx),
           Attr(DwarfAttr::kEntryPc, DwarfForm::kAddr),
           Attr(DwarfAttr::kRanges, DwarfForm::kRnglistx),
           Attr(DwarfAttr::kStmtList, DwarfForm::kSecOffset),
           Attr(DwarfAttr::kStrOffsetsBase, DwarfForm::kSecOffset),
           Attr(DwarfAttr::kAddrBase, DwarfForm::kSecOffset),
           Attr(DwarfAttr::kRnglistsBase, DwarfForm::kSecOffset),
           Attr(DwarfAttr::kLoclistsBase, DwarfForm::kSecOffset)},
          [&](auto& die) {
            die.Fixed(1, 2);
            die.Uleb(0);
            die.Address(0x42, unit.options.address_size);
            die.Uleb(0);
            die.Offset(0, width);
            die.Offset(str_base, width);
            die.Offset(addr_base, width);
            die.Offset(loc_base, width);
            die.Offset(loc_base, width);
          });

      auto context = unit.Parse();
      ASSERT_TRUE(context.ok()) << context.err().msg();
      const auto& cu = context.value();
      EXPECT_EQ("/directory", cu.compilation_dir());
      EXPECT_EQ(str_base, cu.str_offsets_base());
      ASSERT_TRUE(cu.string_offsets());
      EXPECT_EQ(str_end, cu.string_offsets()->end);
      EXPECT_EQ(width, cu.string_offsets()->entry_size);
      EXPECT_EQ(addr_base, cu.addr_base());
      EXPECT_EQ(loc_base, cu.rnglists_base());
      EXPECT_EQ(loc_base, cu.loclists_base());
      EXPECT_EQ(0u, cu.stmt_list());
      EXPECT_EQ(0u, cu.base_address());
      EXPECT_EQ(0u, cu.low_pc());
      ASSERT_TRUE(cu.ranges());
      EXPECT_EQ(DwarfForm::kRnglistx, cu.ranges()->form);
      EXPECT_EQ(0u, std::get<uint64_t>(cu.ranges()->value));
      EXPECT_EQ(loc_base + width, cu.ReadLoclistOffset(0));
      EXPECT_EQ(std::nullopt, cu.ReadLoclistOffset(1));
      EXPECT_EQ(std::nullopt, cu.ReadLoclistOffset(UINT64_MAX));
      EXPECT_EQ(0x12345678u, cu.ReadAddress(1));
      EXPECT_EQ(std::nullopt, cu.ReadAddress(2));
      EXPECT_EQ(std::nullopt, cu.ReadAddress(UINT64_MAX));
      EXPECT_EQ("/line-directory",
                cu.ResolveString({DwarfForm::kLineStrp, FormValue::Class::kString, uint64_t{0}}));
      EXPECT_EQ("", cu.ResolveString({DwarfForm::kStrx, FormValue::Class::kString, uint64_t{0}}));
      EXPECT_EQ(std::nullopt,
                cu.ResolveString({DwarfForm::kStrx, FormValue::Class::kString, uint64_t{2}}));
      EXPECT_EQ(0x1234567au,
                cu.ResolveAddress({DwarfForm::kLlvmAddrxOffset, FormValue::Class::kAddress,
                                   FormValue::IndexedAddress{1, 2}}));
    }
  }
}

TEST(UnitContext, EntryPcAndDirectStrings) {
  UnitBytes unit;
  unit.options.version = 4;
  unit.WriteUnit(
      {Attr(DwarfAttr::kCompDir, DwarfForm::kStrp), Attr(DwarfAttr::kEntryPc, DwarfForm::kAddr),
       Attr(DwarfAttr::kRanges, DwarfForm::kSecOffset)},
      [](auto& die) {
        die.Offset(0, 4);
        die.Address(0x1234, 8);
        die.Offset(0, 4);
      });
  AppendCString(unit.str, "direct");
  auto context = unit.Parse();
  ASSERT_TRUE(context.ok()) << context.err().msg();
  EXPECT_EQ("direct", context.value().compilation_dir());
  EXPECT_EQ(0x1234u, context.value().base_address());
  EXPECT_EQ(std::nullopt, context.value().low_pc());
  ASSERT_TRUE(context.value().ranges());
  EXPECT_EQ(DwarfForm::kSecOffset, context.value().ranges()->form);
  EXPECT_EQ(0u, std::get<uint64_t>(context.value().ranges()->value));

  unit.str.pop_back();
  context = unit.Parse();
  ASSERT_TRUE(context.ok()) << context.err().msg();
  EXPECT_EQ(std::nullopt, context.value().compilation_dir());
  EXPECT_EQ(0x1234u, context.value().base_address());
}

TEST(UnitContext, LegacySectionOffsets) {
  UnitBytes unit;
  unit.options.version = 3;
  unit.WriteUnit(
      {Attr(DwarfAttr::kStmtList, DwarfForm::kData4), Attr(DwarfAttr::kRanges, DwarfForm::kData8)},
      [](auto& die) {
        die.Fixed(0, 4);
        die.Fixed(0x1234, 8);
      });
  auto context = unit.Parse();
  ASSERT_TRUE(context.ok()) << context.err().msg();
  EXPECT_EQ(0u, context.value().stmt_list());
  ASSERT_TRUE(context.value().ranges());
  EXPECT_EQ(0x1234u, std::get<uint64_t>(context.value().ranges()->value));
}

TEST(UnitContext, Dwarf4SplitUnit) {
  UnitBytes skeleton;
  skeleton.options.version = 4;
  DwarfSectionBuilder(skeleton.addr, Endian::kLittle).Address(0xabcd, 8);
  skeleton.WriteUnit({Attr(DwarfAttr::kGnuAddrBase, DwarfForm::kSecOffset),
                      Attr(DwarfAttr::kGnuRangesBase, DwarfForm::kSecOffset),
                      Attr(DwarfAttr::kLowPc, DwarfForm::kAddr)},
                     [](auto& die) {
                       die.Offset(0, 4);
                       die.Offset(0x123, 4);
                       die.Address(0x345, 8);
                     });
  auto main = skeleton.Parse();
  ASSERT_TRUE(main.ok()) << main.err().msg();
  EXPECT_EQ(0u, main.value().addr_base());
  EXPECT_EQ(std::nullopt, main.value().ranges_base());

  UnitBytes dwo;
  dwo.is_dwo = true;
  dwo.options.version = 4;
  dwo.options.offset_size = 8;
  AppendCString(dwo.str, "split-dir");
  DwarfSectionBuilder(dwo.str_offsets, Endian::kLittle).Offset(0, 8);
  dwo.WriteUnit({Attr(DwarfAttr::kCompDir, DwarfForm::kGnuStrIndex),
                 Attr(DwarfAttr::kLowPc, DwarfForm::kGnuAddrIndex),
                 Attr(DwarfAttr::kRanges, DwarfForm::kSecOffset)},
                [](auto& die) {
                  die.Uleb(0);
                  die.Uleb(0);
                  die.Offset(0, 8);
                });
  auto split = dwo.Parse({.skeleton = &main.value()});
  ASSERT_TRUE(split.ok()) << split.err().msg();
  const auto& cu = split.value();
  EXPECT_EQ("split-dir", cu.compilation_dir());
  EXPECT_EQ(0u, cu.str_offsets_base());
  ASSERT_TRUE(cu.string_offsets());
  EXPECT_EQ(8, cu.string_offsets()->entry_size);
  EXPECT_EQ(0u, cu.addr_base());
  EXPECT_EQ(0x123u, cu.ranges_base());
  EXPECT_EQ(std::nullopt, cu.rnglists_base());
  EXPECT_EQ(std::nullopt, cu.loclists_base());
  EXPECT_EQ(0x345u, cu.base_address());
  EXPECT_EQ(0xabcdu, cu.low_pc());
  ASSERT_TRUE(cu.ranges());
  EXPECT_EQ(0u, std::get<uint64_t>(cu.ranges()->value));

  auto unlinked = dwo.Parse();
  ASSERT_TRUE(unlinked.ok()) << unlinked.err().msg();
  EXPECT_EQ("split-dir", unlinked.value().compilation_dir());
  EXPECT_EQ(std::nullopt, unlinked.value().addr_base());
  EXPECT_EQ(std::nullopt, unlinked.value().ranges_base());
  EXPECT_EQ(std::nullopt, unlinked.value().base_address());

  auto addresses_only = dwo.Parse({.address_table = &main.value()});
  ASSERT_TRUE(addresses_only.ok()) << addresses_only.err().msg();
  EXPECT_EQ(0xabcdu, addresses_only.value().base_address());
  EXPECT_EQ(std::nullopt, addresses_only.value().ranges_base());
}

TEST(UnitContext, Dwarf5SplitUnit) {
  UnitBytes skeleton;
  skeleton.options.unit_type = kUnitTypeSkeleton;
  skeleton.tag = DwarfTag::kSkeletonUnit;
  skeleton.WriteUnit({Attr(DwarfAttr::kAddrBase, DwarfForm::kSecOffset),
                      Attr(DwarfAttr::kLoclistsBase, DwarfForm::kSecOffset),
                      Attr(DwarfAttr::kRnglistsBase, DwarfForm::kSecOffset),
                      Attr(DwarfAttr::kStrOffsetsBase, DwarfForm::kSecOffset),
                      Attr(DwarfAttr::kEntryPc, DwarfForm::kAddr)},
                     [](auto& die) {
                       die.Offset(8, 4);
                       die.Offset(500, 4);
                       die.Offset(600, 4);
                       die.Offset(700, 4);
                       die.Address(0x9988, 8);
                     });
  DwarfSectionBuilder(skeleton.addr, Endian::kLittle).Address(0, 8);
  DwarfSectionBuilder(skeleton.addr, Endian::kLittle).Address(0x11223344, 8);
  auto main = skeleton.Parse();
  ASSERT_TRUE(main.ok()) << main.err().msg();

  UnitBytes dwo;
  dwo.is_dwo = true;
  dwo.options.unit_type = kUnitTypeSplitCompile;
  dwo.WriteUnit(
      {Attr(DwarfAttr::kCompDir, DwarfForm::kStrx), Attr(DwarfAttr::kLowPc, DwarfForm::kAddrx1)},
      [](auto& die) {
        die.Uleb(0);
        die.Fixed(0, 1);
      });
  AppendCString(dwo.str, "split");
  // The string-offset contribution can use a different format than the unit.
  WriteStringOffsets(DwarfSectionBuilder(dwo.str_offsets, Endian::kLittle), 8, {0});
  WriteListTable(DwarfSectionBuilder(dwo.loclists, Endian::kLittle), 4, 8, 1, {4});
  WriteListTable(DwarfSectionBuilder(dwo.rnglists, Endian::kLittle), 4, 8, 1, {4});

  auto split = dwo.Parse({.skeleton = &main.value()});
  ASSERT_TRUE(split.ok()) << split.err().msg();
  const auto& cu = split.value();
  EXPECT_EQ("split", cu.compilation_dir());
  EXPECT_EQ(16u, cu.str_offsets_base());
  EXPECT_EQ(12u, cu.loclists_base());
  EXPECT_EQ(12u, cu.rnglists_base());
  EXPECT_EQ(16u, cu.ReadLoclistOffset(0));
  EXPECT_EQ(8u, cu.addr_base());
  EXPECT_EQ(0x11223344u, cu.low_pc());
  EXPECT_EQ(0x9988u, cu.base_address());

  std::optional<uint64_t> address, location;
  std::thread address_reader([&] { address = cu.ReadAddress(0); });
  std::thread location_reader([&] { location = cu.ReadLoclistOffset(0); });
  address_reader.join();
  location_reader.join();
  EXPECT_EQ(0x11223344u, address);
  EXPECT_EQ(16u, location);
}

TEST(UnitContext, PackageContributions) {
  UnitBytes dwo;
  dwo.is_dwo = true;
  dwo.options.unit_type = kUnitTypeSplitCompile;
  dwo.WriteUnit({Attr(DwarfAttr::kCompDir, DwarfForm::kStrx)}, [](auto& die) { die.Uleb(0); });
  AppendCString(dwo.str, "package");
  dwo.str_offsets.resize(4);
  dwo.rnglists.resize(10);
  dwo.loclists.resize(20);
  WriteStringOffsets(DwarfSectionBuilder(dwo.str_offsets, Endian::kLittle), 4, {0});
  WriteListTable(DwarfSectionBuilder(dwo.rnglists, Endian::kLittle), 4, 8, 1, {4});
  WriteListTable(DwarfSectionBuilder(dwo.loclists, Endian::kLittle), 4, 8, 1, {4});
  UnitContext::PackageContributions package{
      UnitContext::Contribution{4, 12},
      UnitContext::Contribution{20, 17},
      UnitContext::Contribution{10, 17},
  };
  auto context = dwo.Parse({.package = package});
  ASSERT_TRUE(context.ok()) << context.err().msg();
  EXPECT_EQ("package", context.value().compilation_dir());
  EXPECT_EQ(12u, context.value().str_offsets_base());
  EXPECT_EQ(22u, context.value().rnglists_base());
  EXPECT_EQ(12u, context.value().loclists_base());
  EXPECT_EQ(17u, context.value().loclists().size());
  EXPECT_EQ(16u, context.value().ReadLoclistOffset(0));

  package.str_offsets->size = UINT64_MAX;
  package.loclists->offset = UINT64_MAX;
  package.rnglists->size = 11;
  context = dwo.Parse({.package = package});
  ASSERT_TRUE(context.ok()) << context.err().msg();
  EXPECT_EQ(std::nullopt, context.value().str_offsets_base());
  EXPECT_EQ(std::nullopt, context.value().loclists_base());
  EXPECT_EQ(std::nullopt, context.value().rnglists_base());
}

TEST(UnitContext, Dwarf4TypesSection) {
  UnitBytes unit;
  unit.options.section = UnitSection::kTypes;
  unit.options.version = 4;
  unit.options.type_offset = 23;
  unit.tag = DwarfTag::kTypeUnit;
  unit.WriteUnit({Attr(DwarfAttr::kCompDir, DwarfForm::kString)},
                 [](auto& die) { die.CString("types"); });
  auto context = unit.Parse();
  ASSERT_TRUE(context.ok()) << context.err().msg();
  EXPECT_EQ("types", context.value().compilation_dir());
}

TEST(UnitContext, DuplicateAndInvalidForms) {
  UnitBytes unit;
  unit.WriteUnit(
      {Attr(DwarfAttr::kGnuAddrBase, DwarfForm::kSecOffset),
       Attr(DwarfAttr::kAddrBase, DwarfForm::kSecOffset),
       Attr(DwarfAttr::kAddrBase, DwarfForm::kSecOffset),
       Attr(DwarfAttr::kLowPc, DwarfForm::kData8), Attr(DwarfAttr::kEntryPc, DwarfForm::kAddr),
       Attr(DwarfAttr::kCompDir, DwarfForm::kString)},
      [](auto& die) {
        die.Offset(4, 4);
        die.Offset(0, 4);
        die.Offset(8, 4);
        die.Fixed(123, 8);
        die.Address(0x1234, 8);
        die.CString("");
      });
  auto context = unit.Parse();
  ASSERT_TRUE(context.ok()) << context.err().msg();
  EXPECT_EQ(0u, context.value().addr_base());
  EXPECT_EQ("", context.value().compilation_dir());
  EXPECT_EQ(std::nullopt, context.value().low_pc());
  EXPECT_EQ(std::nullopt, context.value().base_address());
}

TEST(UnitContext, MalformedRoot) {
  UnitBytes unit;
  unit.WriteUnit(
      {Attr(DwarfAttr::kCompDir, DwarfForm::kString), Attr(DwarfAttr::kLowPc, DwarfForm::kAddr)},
      [](auto& die) { die.CString("truncated-address"); });
  // The next unit has enough bytes for the address. They are outside the first unit.
  unit.WriteUnit({}, [](auto&) {});
  ASSERT_TRUE(unit.Parse().ok());
  unit.unit_offset = 0;
  EXPECT_TRUE(unit.Parse().has_error());

  UnitBytes bad_code;
  bad_code.WriteUnit({}, [](auto&) {});
  bad_code.info.back() = 0;
  EXPECT_TRUE(bad_code.Parse().has_error());
  bad_code.info.back() = 2;
  EXPECT_TRUE(bad_code.Parse().has_error());
  bad_code.info.back() = 0x80;
  EXPECT_TRUE(bad_code.Parse().has_error());
  bad_code.info.back() = 1;
  bad_code.abbrev.pop_back();
  EXPECT_TRUE(bad_code.Parse().has_error());

  UnitBytes unknown_form;
  unknown_form.WriteUnit({{static_cast<uint64_t>(DwarfAttr::kName), 0xffff, std::nullopt}},
                         [](auto&) {});
  EXPECT_TRUE(unknown_form.Parse().has_error());

  UnitBytes no_root;
  DwarfSectionBuilder builder(no_root.info, Endian::kLittle);
  builder.EndUnit(builder.BeginUnit(no_root.options));
  EXPECT_TRUE(no_root.Parse().has_error());
}

TEST(UnitContext, MalformedTablesStayLocal) {
  UnitBytes unit;
  unit.options.offset_size = 8;
  unit.WriteUnit({Attr(DwarfAttr::kCompDir, DwarfForm::kStrx),
                  Attr(DwarfAttr::kStrOffsetsBase, DwarfForm::kSecOffset),
                  Attr(DwarfAttr::kAddrBase, DwarfForm::kSecOffset),
                  Attr(DwarfAttr::kLoclistsBase, DwarfForm::kSecOffset),
                  Attr(DwarfAttr::kLowPc, DwarfForm::kAddr)},
                 [](auto& die) {
                   die.Uleb(0);
                   die.Offset(16, 8);
                   die.Offset(UINT64_MAX, 8);
                   die.Offset(20, 8);
                   die.Address(0x1234, 8);
                 });
  AppendCString(unit.str, "dir");
  WriteStringOffsets(DwarfSectionBuilder(unit.str_offsets, Endian::kLittle), 8, {0});
  unit.str_offsets.pop_back();
  // A count that would require more space than the contribution can hold.
  WriteListTable(DwarfSectionBuilder(unit.loclists, Endian::kLittle), 8, 8, UINT32_MAX, {8});

  auto context = unit.Parse();
  ASSERT_TRUE(context.ok()) << context.err().msg();
  EXPECT_EQ(16u, context.value().str_offsets_base());
  EXPECT_FALSE(context.value().string_offsets());
  EXPECT_EQ(std::nullopt, context.value().compilation_dir());
  EXPECT_EQ(UINT64_MAX, context.value().addr_base());
  EXPECT_EQ(std::nullopt, context.value().ReadAddress(0));
  EXPECT_EQ(0x1234u, context.value().base_address());
  EXPECT_EQ(20u, context.value().loclists_base());
  EXPECT_EQ(std::nullopt, context.value().ReadLoclistOffset(0));

  // A list offset must point past the offset array and stay in the contribution.
  for (uint64_t relative : {uint64_t{0}, uint64_t{9}, UINT64_MAX}) {
    unit.loclists.clear();
    WriteListTable(DwarfSectionBuilder(unit.loclists, Endian::kLittle), 8, 8, 1, {relative});
    auto bad_offset = unit.Parse();
    ASSERT_TRUE(bad_offset.ok()) << bad_offset.err().msg();
    EXPECT_EQ(std::nullopt, bad_offset.value().ReadLoclistOffset(0));
  }
}

}  // namespace zxdb
