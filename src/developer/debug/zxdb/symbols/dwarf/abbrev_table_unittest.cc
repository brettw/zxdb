// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/abbrev_table.h"

#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"

namespace zxdb {
namespace {

// Encodings from DWARF 5 sections 7.5.3 through 7.5.5.
constexpr uint64_t kCompileUnit = 0x11;
constexpr uint64_t kConstant = 0x27;
constexpr uint64_t kSubprogram = 0x2e;
constexpr uint64_t kName = 0x03;
constexpr uint64_t kConstValue = 0x1c;
constexpr uint64_t kDeclLine = 0x3b;
constexpr uint64_t kIndirect = 0x16;
constexpr uint64_t kImplicitConst = 0x21;

TEST(AbbrevTable, AttributesAtUnitOffset) {
  std::vector<uint8_t> section;
  DwarfSectionBuilder builder(section);
  builder.AbbrevDeclaration(1, kCompileUnit, true);
  builder.EndAbbrevTable();
  const uint64_t offset = section.size();

  const AbbrevTable::AttributeSpec specs[] = {
      {kName, kIndirect, {}},
      {kConstValue, kImplicitConst, -129},
      {kDeclLine, kImplicitConst, 0},
  };
  builder.AbbrevDeclaration(129, kConstant, false, specs);
  builder.EndAbbrevTable();
  const std::vector<uint8_t> expected = {
      1,    0x11, 1,    0,    0, 0,  // First table.
      0x81, 0x01, 0x27, 0,           // Code 129, DW_TAG_constant, DW_CHILDREN_no.
      0x03, 0x16,                    // DW_AT_name, DW_FORM_indirect.
      0x1c, 0x21, 0xff, 0x7e,        // DW_AT_const_value, DW_FORM_implicit_const, -129.
      0x3b, 0x21, 0,                 // DW_AT_decl_line, DW_FORM_implicit_const, 0.
      0,    0,    0,                 // Attribute list and table terminators.
  };
  EXPECT_EQ(expected, section);

  // The first table stops at its terminator.
  const auto first = AbbrevTable::Parse(section, 0);
  ASSERT_TRUE(first.ok()) << first.err_or_empty().msg();
  const auto* compile_unit = first.value().Find(1);
  ASSERT_NE(nullptr, compile_unit);
  EXPECT_EQ(kCompileUnit, compile_unit->tag);
  EXPECT_TRUE(compile_unit->has_children);
  EXPECT_TRUE(compile_unit->attributes.empty());
  EXPECT_EQ(nullptr, first.value().Find(129));

  // Byte order and offset size affect the unit header. They have no effect on the table.
  std::vector<uint8_t> info;
  DwarfSectionBuilder info_builder(info, false);
  info_builder.EndUnit(info_builder.BeginUnit({.offset_size = 8, .abbrev_offset = offset}));
  const auto header = ReadUnitHeader(info, 0, false, UnitSection::kInfo);
  ASSERT_TRUE(header.ok()) << header.err_or_empty().msg();
  const auto second = AbbrevTable::Parse(section, header.value().abbrev_offset);
  ASSERT_TRUE(second.ok()) << second.err_or_empty().msg();
  EXPECT_EQ(nullptr, second.value().Find(0));
  EXPECT_EQ(nullptr, second.value().Find(1));
  const auto* constant = second.value().Find(129);
  ASSERT_NE(nullptr, constant);
  EXPECT_EQ(129u, constant->code);
  EXPECT_EQ(kConstant, constant->tag);
  EXPECT_FALSE(constant->has_children);
  ASSERT_EQ(3u, constant->attributes.size());
  for (size_t i = 0; i < constant->attributes.size(); ++i) {
    EXPECT_EQ(specs[i].attribute, constant->attributes[i].attribute);
    EXPECT_EQ(specs[i].form, constant->attributes[i].form);
    EXPECT_EQ(specs[i].implicit_const, constant->attributes[i].implicit_const);
  }
  EXPECT_EQ(std::nullopt, constant->FixedAttributeSize(header.value().form_params()));
}

TEST(AbbrevTable, FixedAttributeSize) {
  const AbbrevTable::AttributeSpec specs[] = {
      {0x11, 0x01, {}},  // DW_AT_low_pc, DW_FORM_addr.
      {0x47, 0x10, {}},  // DW_AT_specification, DW_FORM_ref_addr.
      {0x03, 0x0e, {}},  // DW_AT_name, DW_FORM_strp.
  };
  const AbbrevTable::AttributeSpec dwarf5_specs[] = {
      {0x6e, 0x27, {}},  // DW_AT_linkage_name, DW_FORM_strx3.
      {0x3f, 0x19, {}},  // DW_AT_external, DW_FORM_flag_present.
      {0x3b, 0x21, 1},   // DW_AT_decl_line, DW_FORM_implicit_const.
  };
  std::vector<uint8_t> bytes;
  DwarfSectionBuilder builder(bytes);
  builder.AbbrevDeclaration(1, kSubprogram, false, specs);
  builder.AbbrevDeclaration(2, kSubprogram, false, dwarf5_specs);
  builder.AbbrevDeclaration(3, kConstant, false);
  builder.EndAbbrevTable();

  const auto table = AbbrevTable::Parse(bytes, 0);
  ASSERT_TRUE(table.ok()) << table.err_or_empty().msg();
  const auto* decl = table.value().Find(1);
  ASSERT_NE(nullptr, decl);
  // DWARF 2 uses the address size for DW_FORM_ref_addr. DWARF 3 and later use the offset size.
  EXPECT_EQ(20u, decl->FixedAttributeSize({.version = 2, .offset_size = 4, .address_size = 8}));
  EXPECT_EQ(16u, decl->FixedAttributeSize({.version = 3, .offset_size = 4, .address_size = 8}));
  const FormParams dwarf64{.version = 5, .offset_size = 8, .address_size = 4};
  EXPECT_EQ(20u, decl->FixedAttributeSize(dwarf64));
  EXPECT_EQ(std::nullopt, decl->FixedAttributeSize({}));
  ASSERT_NE(nullptr, table.value().Find(2));
  EXPECT_EQ(3u, table.value().Find(2)->FixedAttributeSize(dwarf64));
  ASSERT_NE(nullptr, table.value().Find(3));
  EXPECT_EQ(0u, table.value().Find(3)->FixedAttributeSize(dwarf64));
}

TEST(AbbrevTable, ConsecutiveCodes) {
  std::vector<uint8_t> bytes;
  DwarfSectionBuilder builder(bytes);
  builder.AbbrevDeclaration(UINT64_MAX - 1, kCompileUnit, true);
  builder.AbbrevDeclaration(UINT64_MAX, kSubprogram, false);
  builder.EndAbbrevTable();

  const auto table = AbbrevTable::Parse(bytes, 0);
  ASSERT_TRUE(table.ok()) << table.err_or_empty().msg();
  EXPECT_EQ(nullptr, table.value().Find(0));
  EXPECT_EQ(nullptr, table.value().Find(UINT64_MAX - 2));
  const auto* first = table.value().Find(UINT64_MAX - 1);
  ASSERT_NE(nullptr, first);
  EXPECT_EQ(kCompileUnit, first->tag);
  const auto* last = table.value().Find(UINT64_MAX);
  ASSERT_NE(nullptr, last);
  EXPECT_EQ(kSubprogram, last->tag);
}

TEST(AbbrevTable, SparseCodesAndDuplicates) {
  constexpr uint64_t kWideCode = uint64_t{1} << 32;
  std::vector<uint8_t> bytes;
  DwarfSectionBuilder builder(bytes);
  builder.AbbrevDeclaration(3, kSubprogram, true);
  builder.AbbrevDeclaration(kWideCode, kConstant, false);
  builder.AbbrevDeclaration(1, kCompileUnit, true);
  builder.AbbrevDeclaration(3, kConstant, false);
  builder.EndAbbrevTable();

  const auto table = AbbrevTable::Parse(bytes, 0);
  ASSERT_TRUE(table.ok()) << table.err_or_empty().msg();
  EXPECT_EQ(nullptr, table.value().Find(0));
  EXPECT_EQ(nullptr, table.value().Find(2));
  EXPECT_EQ(nullptr, table.value().Find(UINT64_MAX));
  ASSERT_NE(nullptr, table.value().Find(1));
  const auto* wide = table.value().Find(kWideCode);
  ASSERT_NE(nullptr, wide);
  EXPECT_EQ(kWideCode, wide->code);
  EXPECT_EQ(kConstant, wide->tag);
  const auto* duplicate = table.value().Find(3);
  ASSERT_NE(nullptr, duplicate);
  EXPECT_EQ(kSubprogram, duplicate->tag);
  EXPECT_TRUE(duplicate->has_children);
}

TEST(AbbrevTable, UnknownValues) {
  constexpr uint8_t kBytes[] = {
      1,    0x80, 0x81, 0x01, 2,              // Tag 0x4080 (DW_TAG_lo_user), children byte 2.
      0x80, 0x40, 0x80, 0x40,                 // Attribute 0x2000 (DW_AT_lo_user), form 0x2000.
      0,    0,    2,    0x80, 0x80, 0x04, 0,  // Tag 0x10000.
      0x80, 0x80, 0x04, 0xa1, 0x80, 0x04,     // Attribute 0x10000, form 0x10021.
      0,    0,    0,
  };
  const auto table = AbbrevTable::Parse(kBytes, 0);
  ASSERT_TRUE(table.ok()) << table.err_or_empty().msg();
  const auto* vendor = table.value().Find(1);
  ASSERT_NE(nullptr, vendor);
  EXPECT_EQ(0x4080u, vendor->tag);
  EXPECT_FALSE(vendor->has_children);
  ASSERT_EQ(1u, vendor->attributes.size());
  EXPECT_EQ(0x2000u, vendor->attributes[0].attribute);
  EXPECT_EQ(0x2000u, vendor->attributes[0].form);
  EXPECT_EQ(std::nullopt, vendor->FixedAttributeSize({.version = 5}));

  // Wide encodings retain their full values. In particular, 0x10021 has no SLEB128 operand.
  const auto* wide = table.value().Find(2);
  ASSERT_NE(nullptr, wide);
  EXPECT_EQ(0x10000u, wide->tag);
  ASSERT_EQ(1u, wide->attributes.size());
  EXPECT_EQ(0x10000u, wide->attributes[0].attribute);
  EXPECT_EQ(0x10021u, wide->attributes[0].form);
  EXPECT_EQ(std::nullopt, wide->attributes[0].implicit_const);
  EXPECT_EQ(std::nullopt, wide->FixedAttributeSize({.version = 5}));
}

TEST(AbbrevTable, EmptyTableAndInvalidOffsets) {
  constexpr uint8_t kBytes[] = {0, 0x80};  // Empty table followed by an incomplete LEB128.
  const auto table = AbbrevTable::Parse(kBytes, 0);
  ASSERT_TRUE(table.ok()) << table.err_or_empty().msg();
  EXPECT_EQ(nullptr, table.value().Find(0));
  EXPECT_EQ(nullptr, table.value().Find(1));
  EXPECT_TRUE(AbbrevTable::Parse({}, 0).has_error());
  for (uint64_t offset : {uint64_t{2}, uint64_t{3}, UINT64_MAX}) {
    SCOPED_TRACE(offset);
    auto result = AbbrevTable::Parse(kBytes, offset);
    ASSERT_TRUE(result.has_error());
    EXPECT_NE(std::string::npos, result.err().msg().find("outside the section"));
  }
}

TEST(AbbrevTable, MissingTableTerminator) {
  constexpr uint8_t kBytes[] = {1, 0x11, 1, 0, 0};
  const auto result = AbbrevTable::Parse(kBytes, 0);
  ASSERT_TRUE(result.has_error());
  EXPECT_NE(std::string::npos, result.err().msg().find("missing table terminator"));
}

TEST(AbbrevTable, TruncatedDeclaration) {
  const AbbrevTable::AttributeSpec specs[] = {{0x2000, kImplicitConst, -129}};
  std::vector<uint8_t> bytes;
  DwarfSectionBuilder builder(bytes);
  builder.AbbrevDeclaration(128, 0x4080, false, specs);
  builder.EndAbbrevTable();
  ASSERT_TRUE(AbbrevTable::Parse(bytes, 0).ok());

  // Cut the table at each byte boundary before the table terminator.
  for (size_t length = 1; length < bytes.size() - 1; ++length) {
    SCOPED_TRACE(length);
    EXPECT_TRUE(AbbrevTable::Parse(std::span(bytes).first(length), 0).has_error());
  }
}

TEST(AbbrevTable, MalformedDeclaration) {
  const struct {
    std::vector<uint8_t> bytes;
    const char* error;
  } cases[] = {
      {{1, 0, 0, 0, 0, 0}, "zero tag"},
      {{1, 0x11, 0, 0, 0x21, 0, 0, 0}, "both be nonzero"},
      {{1, 0x11, 0, 0x03, 0, 0, 0, 0}, "both be nonzero"},
      {{1, 0x11, 0, 0, 0, 1, 0, 0, 0, 0, 0}, "zero tag"},  // Malformed duplicate.
      {{0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02, 0},
       "invalid abbreviation code"},  // Unsigned overflow.
      {{1, 0x27, 0, 0x1c, 0x21, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01, 0, 0,
        0},
       "implicit constant"},  // Signed overflow, even with valid terminators.
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.error);
    auto result = AbbrevTable::Parse(test.bytes, 0);
    ASSERT_TRUE(result.has_error());
    EXPECT_NE(std::string::npos, result.err().msg().find(test.error));
  }
}

TEST(AbbrevTable, SharedReadsAfterInputDestruction) {
  const auto table = [] {
    const std::vector<uint8_t> bytes = {1, 0x11, 1, 0x03, 0x08, 0, 0, 0};
    return AbbrevTable::Parse(bytes, 0);
  }();
  ASSERT_TRUE(table.ok()) << table.err_or_empty().msg();
  const AbbrevTable::Declaration* first = nullptr;
  const AbbrevTable::Declaration* second = nullptr;
  std::thread reader1([&] { first = table.value().Find(1); });
  std::thread reader2([&] { second = table.value().Find(1); });
  reader1.join();
  reader2.join();
  ASSERT_NE(nullptr, first);
  EXPECT_EQ(first, second);
  ASSERT_EQ(1u, first->attributes.size());
  EXPECT_EQ(kName, first->attributes[0].attribute);
  EXPECT_EQ(0x08u, first->attributes[0].form);  // DW_FORM_string.
}

}  // namespace
}  // namespace zxdb
