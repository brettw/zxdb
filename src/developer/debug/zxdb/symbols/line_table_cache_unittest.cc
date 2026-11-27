// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/line_table_cache.h"

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/dwarf_binary_impl.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"
#include "src/developer/debug/zxdb/symbols/line_program_test_util.h"

namespace zxdb {

namespace {

using Builder = LineTableBuilder;

// A DWARF 4 line table with one file in its header. The program adds a second file with
// DW_LNE_define_file and has a row for it.
uint64_t AddDefineFileTable(LineTableTestSections& sections) {
  Builder builder(4);
  builder.AddDirectory("inc");
  builder.AddFile("a.cc", 0);  // File 1.
  builder.SetAddress(0x1000);
  builder.Copy();
  builder.DefineFile("defined.h", 1);  // File 2.
  builder.SetFile(2);
  builder.AdvancePc(4);
  builder.Copy();
  builder.AdvancePc(4);
  builder.EndSequence();
  return sections.Add(builder);
}

// Decodes DW_AT_decl_file of a child of the unit DIE.
std::optional<std::string> DecodeDeclFile(DwarfBinary& binary, size_t child_index) {
  DwarfDie die = binary.GetUnitAtIndex(UnitIndex(false, 0))->GetUnitDie().first_child();
  for (size_t i = 0; i < child_index; i++)
    die = die.sibling();

  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kDeclFile};
  DwarfDieDecoder decoder(binary, kAttrs);
  EXPECT_TRUE(decoder.Decode(die));
  return decoder.GetFile(DwarfAttr::kDeclFile);
}

}  // namespace

TEST(LineTableCache, HeaderOnlyUntilNeeded) {
  LineTableTestSections sections;
  uint64_t offset = AddDefineFileTable(sections);
  sections.AddCompileUnit(4, offset, {});
  sections.Finish();
  DwarfBinaryImpl binary(sections.sections());
  const UnitIndex unit(false, 0);

  // A header entry uses the header's table, which doesn't have the defined file.
  std::shared_ptr<const LineFileTable> header_files = binary.GetLineFileTable(unit, 1);
  ASSERT_TRUE(header_files);
  EXPECT_EQ(2u, header_files->size());
  EXPECT_EQ("a.cc", *header_files->GetFileName(1));

  // An entry past the header decodes the program once and finds the defined file.
  std::shared_ptr<const LineFileTable> files = binary.GetLineFileTable(unit, 2);
  ASSERT_TRUE(files);
  EXPECT_EQ(3u, files->size());
  ASSERT_TRUE(files->GetFileName(2));
  EXPECT_EQ("inc/defined.h", *files->GetFileName(2));

  // The complete table is cached, also for indices that no table has.
  EXPECT_EQ(files, binary.GetLineFileTable(unit, 1));
  EXPECT_EQ(files, binary.GetLineFileTable(unit, 100));

  // A decoded line table shares the file table.
  LineTable table = binary.DecodeLineTable(unit);
  EXPECT_EQ(files.get(), &table.files());
  ASSERT_EQ(1u, table.GetNumSequences());
  EXPECT_EQ("inc/defined.h", *table.GetFileNameForRow(table.rows_with_end_markers()[1]));
}

TEST(LineTableCache, DecodeFirst) {
  // Decoding the table first makes the file table complete. A later lookup uses it.
  LineTableTestSections sections;
  uint64_t offset = AddDefineFileTable(sections);
  sections.AddCompileUnit(4, offset, {});
  sections.Finish();
  DwarfBinaryImpl binary(sections.sections());
  const UnitIndex unit(false, 0);

  LineTable table = binary.DecodeLineTable(unit);
  EXPECT_EQ(3u, table.files().size());
  EXPECT_EQ(&table.files(), binary.GetLineFileTable(unit, 1).get());
}

TEST(LineTableCache, SharesHeaderTableWithoutDefineFile) {
  // Without DW_LNE_define_file, decoding keeps the file table from the header.
  Builder builder(5);
  builder.AddDirectory("/build");
  builder.AddFile("main.cc", 0);
  builder.SetAddress(0x1000);
  builder.SetFile(0);
  builder.Copy();
  builder.EndSequence();

  LineTableTestSections sections;
  uint64_t offset = sections.Add(builder);
  sections.AddCompileUnit(5, offset, {});
  sections.Finish();
  DwarfBinaryImpl binary(sections.sections());
  const UnitIndex unit(false, 0);

  std::shared_ptr<const LineFileTable> files = binary.GetLineFileTable(unit, 0);
  ASSERT_TRUE(files);
  EXPECT_EQ(files.get(), &binary.DecodeLineTable(unit).files());

  // After Clear(), the table is resolved again.
  binary.ClearDwarfCaches();
  EXPECT_NE(files, binary.GetLineFileTable(unit, 0));
}

TEST(LineTableCache, BadLineTable) {
  // The unit's line table offset is outside the section.
  LineTableTestSections sections;
  sections.Add(Builder(5));
  sections.AddCompileUnit(5, 100000, {});
  sections.Finish();
  DwarfBinaryImpl binary(sections.sections());
  const UnitIndex unit(false, 0);

  EXPECT_FALSE(binary.GetLineFileTable(unit, 0));
  EXPECT_FALSE(binary.GetLineFileTable(unit, 5));
  EXPECT_EQ(0u, binary.DecodeLineTable(unit).GetNumSequences());
}

TEST(LineTableCache, UnitWithoutLineTable) {
  LineTableTestSections sections;
  sections.Add(Builder(5));
  sections.AddCompileUnit(5, std::nullopt, {1});
  sections.Finish();
  DwarfBinaryImpl binary(sections.sections());
  const UnitIndex unit(false, 0);

  EXPECT_FALSE(binary.GetLineFileTable(unit, 1));
  EXPECT_EQ(0u, binary.DecodeLineTable(unit).GetNumSequences());

  // DW_AT_decl_file has no file table to use.
  EXPECT_EQ(std::nullopt, DecodeDeclFile(binary, 0));
}

TEST(DwarfDieDecoder, GetFileDefinedByProgram) {
  // DW_AT_decl_file can reference a file that DW_LNE_define_file adds.
  LineTableTestSections sections;
  uint64_t offset = AddDefineFileTable(sections);
  sections.AddCompileUnit(4, offset, {1, 2, 9});
  sections.Finish();
  DwarfBinaryImpl binary(sections.sections());
  EXPECT_EQ("a.cc", DecodeDeclFile(binary, 0));
  EXPECT_EQ("inc/defined.h", DecodeDeclFile(binary, 1));
  EXPECT_EQ(std::nullopt, DecodeDeclFile(binary, 2));
}

TEST(DwarfDieDecoder, GetFileStrx) {
  // File names with DW_FORM_strx1 resolve through the unit's string offsets table.
  Builder builder(5);
  builder.set_string_form(Builder::StringForm::kStrx1);
  builder.AddDirectory("/build");
  uint64_t src = builder.AddDirectory("src");
  builder.AddFile("indexed.cc", src);

  LineTableTestSections sections;
  uint64_t offset = sections.Add(builder);
  sections.AddCompileUnit(5, offset, {0});
  sections.Finish();
  DwarfBinaryImpl binary(sections.sections());
  EXPECT_EQ("src/indexed.cc", DecodeDeclFile(binary, 0));
}

}  // namespace zxdb
