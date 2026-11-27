// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/die_table.h"

#include <array>
#include <utility>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"
#include "src/developer/debug/zxdb/symbols/dwarf_attr.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"
#include "src/developer/debug/zxdb/symbols/dwarf_tag.h"

namespace zxdb {
namespace {

using AttributeSpec = AbbrevTable::AttributeSpec;
using Index = DieTable::Index;
constexpr Index kNone = DieTable::kNoIndex;

constexpr AttributeSpec Attribute(DwarfAttr attr, DwarfForm form,
                                  std::optional<int64_t> constant = std::nullopt) {
  return {static_cast<uint64_t>(attr), static_cast<uint64_t>(form), constant};
}

void Abbrev(DwarfSectionBuilder& builder, uint64_t code, DwarfTag tag, bool has_children,
            std::span<const AttributeSpec> attributes = {}) {
  builder.AbbrevDeclaration(code, static_cast<uint64_t>(tag), has_children, attributes);
}

std::shared_ptr<const AbbrevTable> ParseAbbrevs(std::span<const uint8_t> bytes) {
  auto abbrevs = AbbrevTable::Parse(bytes, 0);
  EXPECT_TRUE(abbrevs.ok()) << abbrevs.err().msg();
  if (!abbrevs.ok())
    return nullptr;
  return std::make_shared<const AbbrevTable>(abbrevs.take_value());
}

TEST(DieTable, TreeAndSiblingReferences) {
  constexpr Endian kEndian = Endian::kLittle;
  std::vector<uint8_t> abbrev_bytes;
  DwarfSectionBuilder abbrev(abbrev_bytes, kEndian);
  Abbrev(abbrev, 1, DwarfTag::kCompileUnit, true);
  Abbrev(abbrev, 2, DwarfTag::kNamespace, true,
         std::array{Attribute(DwarfAttr::kSibling, DwarfForm::kRef4)});
  Abbrev(abbrev, 3, DwarfTag::kVariable, false,
         std::array{Attribute(DwarfAttr::kName, DwarfForm::kString)});
  Abbrev(abbrev, 4, DwarfTag::kLexicalBlock, true);
  abbrev.EndAbbrevTable();
  auto abbrevs = ParseAbbrevs(abbrev_bytes);
  ASSERT_TRUE(abbrevs);

  std::vector<uint8_t> info_bytes;
  DwarfSectionBuilder info(info_bytes, kEndian);
  auto unit = info.BeginUnit({});
  info.Uleb(1);       // 12: compile unit.
  info.Uleb(2);       // 13: namespace.
  info.Fixed(18, 4);  // Incorrect sibling reference to its child.
  info.Uleb(3);       // 18: variable.
  info.CString("v");
  info.Uleb(0);       // 21: end of first namespace.
  info.Uleb(2);       // 22: second namespace.
  info.Fixed(28, 4);  // Correct sibling reference to the lexical block.
  info.Uleb(0);       // 27: end of second namespace.
  info.Uleb(4);       // 28: lexical block.
  info.Uleb(3);       // 29: variable.
  info.CString("w");
  info.Uleb(0);  // 32: end of lexical block.
  info.Uleb(0);  // 33: end of compile unit.
  info.Uleb(3);  // 34: bytes after the tree.
  info.EndUnit(unit);

  auto header = ReadUnitHeader(info_bytes, 0, kEndian, UnitSection::kInfo);
  ASSERT_TRUE(header.ok()) << header.err().msg();
  auto root =
      DieTable::Parse(info_bytes, header.value(), kEndian, abbrevs, DieTable::Load::kRootOnly);
  ASSERT_TRUE(root.error().ok()) << root.error().msg();
  ASSERT_EQ(1u, root.size());
  EXPECT_EQ(0u, root.Find(12));
  EXPECT_EQ(13u, root.EntryEnd(0));
  EXPECT_EQ(std::nullopt, root.FirstChild(0));
  EXPECT_EQ(std::nullopt, root.Find(13));

  auto table = DieTable::Parse(info_bytes, header.value(), kEndian, std::move(abbrevs));
  ASSERT_TRUE(table.error().ok()) << table.error().msg();
  // DWARF gives each child the enclosing entry as its parent. Offsets count bytes after
  // the 12-byte DWARF 5 header. The null entries mark the end of each list.
  struct Expected {
    uint64_t offset;
    uint64_t end;
    uint64_t code;
    Index parent;
    Index sibling;
    std::optional<Index> first_child;
  };
  const Expected expected[] = {
      {12, 13, 1, kNone, kNone, 1},
      {13, 18, 2, 0, 4, 2},
      {18, 21, 3, 1, 3, std::nullopt},
      {21, 22, 0, 1, kNone, std::nullopt},
      {22, 27, 2, 0, 6, 5},
      {27, 28, 0, 4, kNone, std::nullopt},
      {28, 29, 4, 0, 9, 7},
      {29, 32, 3, 6, 8, std::nullopt},
      {32, 33, 0, 6, kNone, std::nullopt},
      {33, 34, 0, 0, kNone, std::nullopt},
  };
  ASSERT_EQ(std::size(expected), table.size());
  for (Index i = 0; i < table.size(); ++i) {
    SCOPED_TRACE(i);
    const auto& want = expected[i];
    const auto& entry = *table.Get(i);
    EXPECT_EQ(want.offset, entry.offset);
    EXPECT_EQ(want.code, entry.abbrev ? entry.abbrev->code : 0);
    EXPECT_EQ(want.parent, entry.parent);
    EXPECT_EQ(want.sibling, entry.sibling);
    EXPECT_EQ(want.first_child, table.FirstChild(i));
    EXPECT_EQ(i, table.Find(want.offset));
    EXPECT_EQ(want.end, table.EntryEnd(i));
  }
  EXPECT_EQ(nullptr, table.Get(table.size()));
  EXPECT_EQ(nullptr, table.Get(uint64_t{1} << 32));
  EXPECT_EQ(std::nullopt, table.EntryEnd(table.size()));
  EXPECT_EQ(std::nullopt, table.FirstChild(kNone));
  for (uint64_t offset :
       std::array<uint64_t, 8>{0, 11, 14, 20, 26, 34, 35, (UINT64_C(1) << 32) + 12})
    EXPECT_EQ(std::nullopt, table.Find(offset)) << offset;
}

TEST(DieTable, Dwarf64BigEndian) {
  constexpr Endian kEndian = Endian::kBig;
  constexpr uint64_t kWideCode = UINT64_C(0x100000002);
  std::vector<uint8_t> abbrev_bytes;
  DwarfSectionBuilder abbrev(abbrev_bytes, kEndian);
  Abbrev(abbrev, 1, DwarfTag::kCompileUnit, true);
  Abbrev(abbrev, 2, DwarfTag::kVariable, false,
         std::array{Attribute(DwarfAttr::kSpecification, DwarfForm::kRefAddr),
                    Attribute(DwarfAttr::kType, DwarfForm::kRefSig8),
                    Attribute(DwarfAttr::kLowPc, DwarfForm::kAddr),
                    Attribute(DwarfAttr::kConstValue, DwarfForm::kImplicitConst, -10)});
  Abbrev(abbrev, kWideCode, DwarfTag::kVariable, false,
         std::array{Attribute(DwarfAttr::kLocation, DwarfForm::kBlock2),
                    Attribute(DwarfAttr::kExternal, DwarfForm::kFlagPresent),
                    Attribute(DwarfAttr::kDeclColumn, DwarfForm::kImplicitConst, 7),
                    Attribute(DwarfAttr::kName, DwarfForm::kString),
                    Attribute(DwarfAttr::kConstValue, DwarfForm::kIndirect)});
  abbrev.EndAbbrevTable();

  std::vector<uint8_t> info_bytes(3, 0);
  DwarfSectionBuilder info(info_bytes, kEndian);
  auto unit = info.BeginUnit({.offset_size = 8, .address_size = 4});
  info.Uleb(1);
  info.Uleb(2);
  info.Fixed(0x100000000, 8);
  info.Fixed(0x0102030405060708, 8);
  info.Fixed(0x11223344, 4);
  info.Uleb(kWideCode);
  info.Fixed(2, 2);
  info.Fixed(0x3344, 2);
  info.CString("name");
  info.Uleb(static_cast<uint64_t>(DwarfForm::kImplicitConst));
  info.Sleb(-65);
  info.Uleb(0);
  info.EndUnit(unit);

  auto header = ReadUnitHeader(info_bytes, 3, kEndian, UnitSection::kInfo);
  ASSERT_TRUE(header.ok()) << header.err().msg();
  auto table = DieTable::Parse(info_bytes, header.value(), kEndian, ParseAbbrevs(abbrev_bytes));
  ASSERT_TRUE(table.error().ok()) << table.error().msg();
  ASSERT_EQ(4u, table.size());
  // A DWARF 5 DWARF64 header takes 24 bytes. The fixed-width entry takes 21 bytes
  // including its code. The variable-width entry has a 5-byte code, a 4-byte block,
  // a 5-byte string and a 3-byte indirect constant (DWARF 5 errata).
  EXPECT_EQ(3u, table.header().offset);
  EXPECT_EQ(info_bytes.data(), table.section().data());
  EXPECT_EQ(27u, table.Get(0)->offset);
  EXPECT_EQ(28u, table.Get(1)->offset);
  EXPECT_EQ(49u, table.Get(2)->offset);
  ASSERT_NE(nullptr, table.Get(2)->abbrev);
  EXPECT_EQ(kWideCode, table.Get(2)->abbrev->code);
  EXPECT_EQ(66u, table.Get(3)->offset);
  EXPECT_EQ(67u, table.EntryEnd(3));
}

TEST(DieTable, Dwarf2RefAddrUsesAddressSize) {
  constexpr Endian kEndian = Endian::kLittle;
  std::vector<uint8_t> abbrev_bytes;
  DwarfSectionBuilder abbrev(abbrev_bytes, kEndian);
  Abbrev(abbrev, 1, DwarfTag::kCompileUnit, true);
  Abbrev(abbrev, 2, DwarfTag::kVariable, false,
         std::array{Attribute(DwarfAttr::kType, DwarfForm::kRefAddr)});
  abbrev.EndAbbrevTable();

  std::vector<uint8_t> info_bytes;
  DwarfSectionBuilder info(info_bytes, kEndian);
  auto unit = info.BeginUnit({.version = 2, .address_size = 2});
  info.Uleb(1);
  info.Uleb(2);
  info.Fixed(0x1122, 2);
  info.Uleb(0);
  info.EndUnit(unit);

  auto header = ReadUnitHeader(info_bytes, 0, kEndian, UnitSection::kInfo);
  ASSERT_TRUE(header.ok()) << header.err().msg();
  auto table = DieTable::Parse(info_bytes, header.value(), kEndian, ParseAbbrevs(abbrev_bytes));
  ASSERT_TRUE(table.error().ok()) << table.error().msg();
  ASSERT_EQ(3u, table.size());
  // DWARF 2 uses the address width for DW_FORM_ref_addr. With an 11-byte
  // header, the root is at 11 and the child list terminator is at 15.
  EXPECT_EQ(11u, table.Get(0)->offset);
  EXPECT_EQ(12u, table.Get(1)->offset);
  EXPECT_EQ(15u, table.Get(2)->offset);
  EXPECT_EQ(nullptr, table.Get(2)->abbrev);
}

TEST(DieTable, MalformedTail) {
  constexpr Endian kEndian = Endian::kLittle;
  std::vector<uint8_t> abbrev_bytes;
  DwarfSectionBuilder abbrev(abbrev_bytes, kEndian);
  Abbrev(abbrev, 1, DwarfTag::kCompileUnit, true);
  Abbrev(abbrev, 2, DwarfTag::kVariable, false);
  Abbrev(abbrev, 3, DwarfTag::kVariable, false,
         std::array{Attribute(DwarfAttr::kType, DwarfForm::kRefSig8)});
  Abbrev(abbrev, 4, DwarfTag::kVariable, false,
         std::array{Attribute(DwarfAttr::kName, DwarfForm::kString)});
  Abbrev(abbrev, 5, DwarfTag::kVariable, false,
         std::array{Attribute(DwarfAttr::kLocation, DwarfForm::kBlock1)});
  Abbrev(abbrev, 6, DwarfTag::kVariable, false,
         std::array{Attribute(DwarfAttr::kName, DwarfForm::kIndirect)});
  Abbrev(abbrev, 7, DwarfTag::kVariable, false,
         std::array{Attribute(DwarfAttr::kExternal, DwarfForm::kFlagPresent),
                    AttributeSpec{static_cast<uint64_t>(DwarfAttr::kLocation), 0x100000008, {}}});
  Abbrev(abbrev, 8, DwarfTag::kCompileUnit, false);
  abbrev.EndAbbrevTable();
  auto abbrevs = ParseAbbrevs(abbrev_bytes);
  ASSERT_TRUE(abbrevs);

  const std::vector<uint8_t> tails[] = {
      {},                        // Missing child list terminator.
      {0x80},                    // Truncated abbreviation code.
      {3, 0, 0, 0, 0, 0, 0, 0},  // Truncated fixed-width reference.
      {4, 'x'},                  // Unterminated string.
      {5, 2, 0},                 // Truncated block.
      {6},                       // Missing indirect form code.
      {7, 0},                    // Full-width unknown form that would alias DW_FORM_string.
      {9},                       // Unknown abbreviation code.
  };
  for (const auto& tail : tails) {
    SCOPED_TRACE(::testing::PrintToString(tail));
    std::vector<uint8_t> info_bytes;
    DwarfSectionBuilder info(info_bytes, kEndian);
    auto first = info.BeginUnit({});
    info.Uleb(1);
    info.Uleb(2);
    info.Bytes(tail);
    info.EndUnit(first);
    auto second = info.BeginUnit({});
    info.Uleb(8);
    info.EndUnit(second);

    auto header = ReadUnitHeader(info_bytes, 0, kEndian, UnitSection::kInfo);
    ASSERT_TRUE(header.ok()) << header.err().msg();
    const auto table = DieTable::Parse(info_bytes, header.value(), kEndian, abbrevs);
    // The root at 12 and its child at 13 are complete. The next unit cannot supply missing
    // bytes for the first unit's incomplete child list or for a partial entry at 14.
    EXPECT_TRUE(table.error().has_error());
    ASSERT_EQ(2u, table.size());
    EXPECT_EQ(1u, table.FirstChild(0));
    EXPECT_EQ(0u, table.Get(1)->parent);
    EXPECT_EQ(kNone, table.Get(1)->sibling);
    EXPECT_EQ(14u, table.EntryEnd(1));
    EXPECT_EQ(std::nullopt, table.Find(14));

    auto next_header = ReadUnitHeader(info_bytes, 14 + tail.size(), kEndian, UnitSection::kInfo);
    ASSERT_TRUE(next_header.ok()) << next_header.err().msg();
    const auto next_table = DieTable::Parse(info_bytes, next_header.value(), kEndian, abbrevs);
    ASSERT_TRUE(next_table.error().ok()) << next_table.error().msg();
    ASSERT_EQ(1u, next_table.size());
    EXPECT_EQ(26 + tail.size(), next_table.Get(0)->offset);
  }
}

TEST(DieTable, MissingRootAndInvalidBounds) {
  constexpr Endian kEndian = Endian::kLittle;
  std::vector<uint8_t> info_bytes;
  DwarfSectionBuilder info(info_bytes, kEndian);
  info.EndUnit(info.BeginUnit({}));
  auto header = ReadUnitHeader(info_bytes, 0, kEndian, UnitSection::kInfo);
  ASSERT_TRUE(header.ok()) << header.err().msg();

  const auto missing = DieTable::Parse(info_bytes, header.value(), kEndian, nullptr);
  EXPECT_TRUE(missing.error().has_error());
  EXPECT_EQ(0u, missing.size());

  // Guard against fabricated lengths that would wrap the unit end.
  auto bad_header = header.value();
  bad_header.length = UINT64_MAX;
  const auto invalid = DieTable::Parse(info_bytes, bad_header, kEndian, nullptr);
  EXPECT_TRUE(invalid.error().has_error());
  EXPECT_EQ(0u, invalid.size());
}

TEST(DieTable, DeepNesting) {
  constexpr Endian kEndian = Endian::kLittle;
  constexpr Index kDepth = 70000;
  std::vector<uint8_t> abbrev_bytes;
  DwarfSectionBuilder abbrev(abbrev_bytes, kEndian);
  Abbrev(abbrev, 1, DwarfTag::kCompileUnit, true);
  Abbrev(abbrev, 2, DwarfTag::kLexicalBlock, true);
  abbrev.EndAbbrevTable();

  std::vector<uint8_t> info_bytes;
  DwarfSectionBuilder info(info_bytes, kEndian);
  auto unit = info.BeginUnit({});
  info.Uleb(1);
  for (Index i = 1; i < kDepth; ++i)
    info.Uleb(2);
  for (Index i = 0; i < kDepth; ++i)
    info.Uleb(0);
  info.EndUnit(unit);

  auto header = ReadUnitHeader(info_bytes, 0, kEndian, UnitSection::kInfo);
  ASSERT_TRUE(header.ok()) << header.err().msg();
  auto table = DieTable::Parse(info_bytes, header.value(), kEndian, ParseAbbrevs(abbrev_bytes));
  ASSERT_TRUE(table.error().ok()) << table.error().msg();
  ASSERT_EQ(2 * kDepth, table.size());
  // Each child list contains one child and its null sibling. The innermost
  // list is empty, so its first child is also its null terminator.
  EXPECT_EQ(kNone, table.Get(0)->parent);
  EXPECT_EQ(kNone, table.Get(0)->sibling);
  for (Index i : {1u, kDepth / 2, kDepth - 1}) {
    SCOPED_TRACE(i);
    EXPECT_EQ(i - 1, table.Get(i)->parent);
    EXPECT_EQ(i + 1, table.FirstChild(i));
    EXPECT_EQ(2 * kDepth - i, table.Get(i)->sibling);
    EXPECT_EQ(i - 1, table.Get(2 * kDepth - i)->parent);
  }
  EXPECT_EQ(nullptr, table.Get(kDepth)->abbrev);
  EXPECT_EQ(kDepth - 1, table.Get(kDepth)->parent);
  EXPECT_EQ(0u, table.Get(2 * kDepth - 1)->parent);
}

}  // namespace
}  // namespace zxdb
