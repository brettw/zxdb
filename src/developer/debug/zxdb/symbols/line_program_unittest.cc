// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/line_program.h"

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/line_file_table.h"
#include "src/developer/debug/zxdb/symbols/line_program_test_util.h"
#include "src/developer/debug/zxdb/symbols/line_table.h"

// These tests write line tables in memory and decode them with the zxdb decoder. Most tests also
// parse the same bytes with LLVM and require the same rows. This is the reference behavior that the
// decoder must keep. See line_tables.md for how this coverage maps to LLVM's own tests.

namespace zxdb {

namespace {

using Builder = LineTableBuilder;

constexpr uint8_t kStmt = LineRow::kIsStmt;
constexpr uint8_t kEnd = LineRow::kEndSequence;

struct Decoded {
  LineProgramStatus status = LineProgramStatus::kOk;
  std::vector<LineRow> rows;
  std::vector<uint32_t> files;
  size_t file_count = 0;
};

// Decodes the table at the offset with LineProgram, which falls back to LLVM when needed. Checks
// that the file scan agrees with the decoded rows. When compare_llvm is set, also checks that the
// rows equal LLVM's rows.
Decoded DecodeAndCheck(const LineTableTestSections& sections, uint64_t offset,
                       bool compare_llvm = true) {
  Decoded result;
  ErrOr<LineProgram> program = sections.Parse(offset);
  EXPECT_TRUE(program.ok()) << program.err().msg();
  if (!program.ok())
    return result;

  result.status = program.value().DecodeRows(&result.rows);
  result.file_count = program.value().GetFileCount();

  // The scan must give the same files as the rows, from a separate parse.
  ErrOr<LineProgram> scan_program = sections.Parse(offset);
  if (scan_program.has_error())
    return result;
  LineProgramStatus scan_status = scan_program.value().ScanFiles(&result.files);
  EXPECT_EQ(result.status, scan_status);
  EXPECT_EQ(GetLiveRowFiles(result.rows, result.file_count), result.files);

  if (compare_llvm) {
    LineTableTestSections::LLVMResult llvm = sections.ParseWithLLVM(offset);
    EXPECT_EQ(llvm.rows, result.rows);
  }
  return result;
}

Decoded BuildAndCheck(const Builder& builder, bool compare_llvm = true) {
  LineTableTestSections sections;
  uint64_t offset = sections.Add(builder);
  sections.Finish();
  return DecodeAndCheck(sections, offset, compare_llvm);
}

// Builder for DWARF 5 with one file, which is file 1 for both DWARF 4 and 5 numbering.
Builder MakeBuilder(uint16_t version = 5, Builder::Format format = Builder::Format::kDwarf32) {
  Builder builder(version, format);
  if (version >= 5) {
    builder.AddDirectory("/build");
    builder.AddFile("main.cc", 0);  // File 0.
  }
  builder.AddFile("main.cc", 0);  // File 1.
  return builder;
}

}  // namespace

// Each DWARF version and format with a program that uses the common opcodes.
class LineProgramVersionTest
    : public ::testing::TestWithParam<std::tuple<uint16_t, Builder::Format>> {};

TEST_P(LineProgramVersionTest, Basic) {
  auto [version, format] = GetParam();
  Builder builder(version, format);
  uint64_t dir = builder.AddDirectory(version >= 5 ? "/build" : "src");
  uint64_t file_a = builder.AddFile("a.cc", dir);
  uint64_t file_b = builder.AddFile("b.h", dir);

  builder.SetAddress(0x1000);
  builder.SetFile(file_a);
  builder.Copy();
  builder.Special(4, 2);
  builder.SetFile(file_b);
  builder.AdvanceLine(10);
  builder.Copy();
  builder.AdvancePc(0x10);
  builder.EndSequence();

  Decoded decoded = BuildAndCheck(builder);
  EXPECT_EQ(LineProgramStatus::kOk, decoded.status);
  std::vector<LineRow> expected = {
      LineRow(0x1000, file_a, 1, 0, kStmt),
      LineRow(0x1004, file_a, 3, 0, kStmt),
      LineRow(0x1004, file_b, 13, 0, kStmt),
      LineRow(0x1014, file_b, 13, 0, kStmt | kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);
  EXPECT_EQ((std::vector<uint32_t>{static_cast<uint32_t>(file_a), static_cast<uint32_t>(file_b)}),
            decoded.files);
}

INSTANTIATE_TEST_SUITE_P(
    LineProgram, LineProgramVersionTest,
    ::testing::Combine(::testing::Values(2, 3, 4, 5),
                       ::testing::Values(Builder::Format::kDwarf32, Builder::Format::kDwarf64)));

TEST(LineProgram, StandardOpcodes) {
  Builder builder = MakeBuilder();
  builder.SetAddress(0x2000);
  builder.SetColumn(7);
  builder.SetPrologueEnd();
  builder.SetBasicBlock();
  builder.SetEpilogueBegin();
  builder.SetIsa(3);
  builder.Copy();  // Row 0 has the one-row flags.
  builder.NegateStmt();
  builder.AdvancePc(0x1234);  // A multi-byte ULEB128.
  builder.Copy();             // Row 1 has no one-row flags and no is_stmt.
  builder.NegateStmt();
  builder.ConstAddPc();  // (255 - 13) / 14 = 17.
  builder.FixedAdvancePc(0x100);
  builder.AdvanceLine(-1);  // Line 0.
  builder.Copy();
  builder.AdvanceLine(1000000);
  builder.SetColumn(0x12345);  // Truncated to 16 bits like LLVM.
  builder.Copy();
  builder.EndSequence();

  Decoded decoded = BuildAndCheck(builder);
  EXPECT_EQ(LineProgramStatus::kOk, decoded.status);
  std::vector<LineRow> expected = {
      LineRow(0x2000, 1, 1, 7,
              kStmt | LineRow::kPrologueEnd | LineRow::kBasicBlock | LineRow::kEpilogueBegin),
      LineRow(0x3234, 1, 1, 7, 0),
      LineRow(0x3345, 1, 0, 7, kStmt),
      LineRow(0x3345, 1, 1000000, 0x2345, kStmt),
      LineRow(0x3345, 1, 1000000, 0x2345, kStmt | kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);
}

TEST(LineProgram, RegistersResetAfterEndSequence) {
  Builder builder = MakeBuilder();
  builder.set_default_is_stmt(false);
  builder.SetAddress(0x1000);
  builder.SetFile(0);
  builder.SetColumn(4);
  builder.AdvanceLine(9);
  builder.NegateStmt();
  builder.Copy();
  builder.EndSequence();
  // The second sequence starts with the initial registers and no set_address.
  builder.AdvancePc(0x50);
  builder.Copy();
  builder.AdvancePc(1);
  builder.EndSequence();

  Decoded decoded = BuildAndCheck(builder);
  std::vector<LineRow> expected = {
      LineRow(0x1000, 0, 10, 4, kStmt),
      LineRow(0x1000, 0, 10, 4, kStmt | kEnd),
      LineRow(0x50, 1, 1, 0, 0),
      LineRow(0x51, 1, 1, 0, kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);
}

// Runs every special opcode with different header parameters. LLVM is the reference.
class LineProgramSpecialTest
    : public ::testing::TestWithParam<std::tuple<int8_t, uint8_t, uint8_t, uint8_t>> {};

TEST_P(LineProgramSpecialTest, AllOpcodes) {
  auto [line_base, line_range, opcode_base, min_inst_length] = GetParam();
  Builder builder = MakeBuilder(4);
  builder.set_line_base(line_base);
  builder.set_line_range(line_range);
  builder.set_opcode_base(opcode_base);
  builder.set_min_inst_length(min_inst_length);

  // Use only extended opcodes and special opcodes, because a low opcode_base removes standard
  // opcodes.
  builder.SetAddress(0x10000);
  int first_special = std::max<int>(opcode_base, 1);
  for (int opcode = first_special; opcode <= 255; opcode++)
    builder.SpecialOpcode(static_cast<uint8_t>(opcode));
  builder.EndSequence();

  // LLVM decodes special opcode 8 as DW_LNS_const_add_pc when opcode_base is 8 or less. The zxdb
  // decoder follows the DWARF standard there. See SpecialOpcodeEightWithLowOpcodeBase.
  Decoded decoded = BuildAndCheck(builder, opcode_base > 8);
  EXPECT_EQ(LineProgramStatus::kOk, decoded.status);
  ASSERT_EQ(static_cast<size_t>(255 - first_special + 1 + 1), decoded.rows.size());

  // Check the last special opcode against the DWARF formula.
  uint64_t address = 0x10000;
  uint32_t line = 1;
  for (int opcode = first_special; opcode <= 255; opcode++) {
    int adjusted = opcode - opcode_base;
    if (line_range) {
      address += (adjusted / line_range) * min_inst_length;
      line += line_base + adjusted % line_range;
    }
  }
  EXPECT_EQ(address, decoded.rows[decoded.rows.size() - 2].address);
  EXPECT_EQ(line, decoded.rows[decoded.rows.size() - 2].line);
}

TEST(LineProgram, ConstAddPc) {
  for (uint8_t opcode_base : {10, 13, 20}) {
    SCOPED_TRACE(static_cast<int>(opcode_base));
    Builder builder = MakeBuilder(4);
    builder.set_opcode_base(opcode_base);
    builder.set_min_inst_length(4);
    builder.SetAddress(0x1000);
    builder.ConstAddPc();
    builder.Copy();
    builder.FixedAdvancePc(3);  // Not scaled by min_inst_length.
    builder.EndSequence();

    Decoded decoded = BuildAndCheck(builder);
    ASSERT_EQ(2u, decoded.rows.size());
    uint64_t const_add_pc = ((255 - opcode_base) / 14) * 4;
    EXPECT_EQ(0x1000 + const_add_pc, decoded.rows[0].address);
    EXPECT_EQ(0x1000 + const_add_pc + 3, decoded.rows[1].address);
  }
}

TEST(LineProgram, SpecialOpcodeEightWithLowOpcodeBase) {
  // With opcode_base 1, opcode 8 is a special opcode with adjusted value 7. LLVM uses the address
  // advance of DW_LNS_const_add_pc for it, which isn't what the DWARF standard says.
  Builder builder = MakeBuilder(4);
  builder.set_opcode_base(1);
  builder.set_line_base(0);
  builder.set_line_range(4);
  builder.SetAddress(0x1000);
  builder.SpecialOpcode(8);
  builder.EndSequence();

  Decoded decoded = BuildAndCheck(builder, false);
  ASSERT_EQ(2u, decoded.rows.size());
  EXPECT_EQ(0x1001u, decoded.rows[0].address);  // 7 / 4.
  EXPECT_EQ(4u, decoded.rows[0].line);          // 1 + 7 % 4.

  LineTableTestSections sections;
  uint64_t offset = sections.Add(builder);
  sections.Finish();
  EXPECT_NE(decoded.rows, sections.ParseWithLLVM(offset).rows);
}

INSTANTIATE_TEST_SUITE_P(
    LineProgram, LineProgramSpecialTest,
    ::testing::Values(std::make_tuple(-5, 14, 13, 1),  // Clang.
                      std::make_tuple(-10, 242, 13, 1),  // GCC.
                      std::make_tuple(1, 1, 10, 4),      // DWARF 2 opcode base, 4-byte instrs.
                      std::make_tuple(-3, 12, 1, 1),     // Every nonzero opcode is special.
                      std::make_tuple(0, 255, 20, 2),    // Unknown standard opcodes 13 to 19.
                      std::make_tuple(-5, 14, 13, 0),    // No address advance.
                      std::make_tuple(-5, 0, 13, 1)));   // line_range 0 doesn't advance.

TEST(LineProgram, OpcodeBaseBelowStandard) {
  // With opcode_base 10, opcodes 10 to 12 are special opcodes, not prologue_end, epilogue_begin,
  // and set_isa.
  Builder builder = MakeBuilder(2);
  builder.set_opcode_base(10);
  builder.SetAddress(0x1000);
  builder.SpecialOpcode(10);  // Line -5.
  builder.SpecialOpcode(12);  // Line -3.
  builder.EndSequence();

  Decoded decoded = BuildAndCheck(builder);
  std::vector<LineRow> expected = {
      LineRow(0x1000, 1, static_cast<uint32_t>(-4), 0, kStmt),
      LineRow(0x1000, 1, static_cast<uint32_t>(-7), 0, kStmt),
      LineRow(0x1000, 1, static_cast<uint32_t>(-7), 0, kStmt | kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);
}

TEST(LineProgram, UnknownStandardOpcodes) {
  // Opcodes 13 and 14 are unknown standard opcodes. The header gives their operand counts.
  Builder builder = MakeBuilder();
  builder.set_opcode_base(16);
  builder.set_standard_opcode_length(13, 2);
  builder.set_standard_opcode_length(15, 1);
  builder.SetAddress(0x1000);
  builder.Raw({13});
  builder.Uleb(0x81);  // Two-byte operand.
  builder.Uleb(2);
  builder.Raw({14});  // No operands.
  builder.Raw({15});
  builder.Uleb(1);
  builder.Copy();
  builder.SpecialOpcode(16);  // The first special opcode.
  builder.EndSequence();

  Decoded decoded = BuildAndCheck(builder);
  EXPECT_EQ(LineProgramStatus::kOk, decoded.status);
  ASSERT_EQ(3u, decoded.rows.size());
  EXPECT_EQ(LineRow(0x1000, 1, 1, 0, kStmt), decoded.rows[0]);
}

TEST(LineProgram, ExtendedOpcodes) {
  Builder builder = MakeBuilder();
  builder.SetAddress(0x1000);
  builder.SetDiscriminator(5);
  builder.Extended(0, {});                // Zero length: no sub-opcode.
  builder.Extended(4, {0x80, 1, 2, 3});   // Unknown vendor opcode.
  builder.Copy();
  // An end_sequence with a longer length. The extra byte is skipped, so the negate_stmt opcode in
  // it has no effect.
  builder.Extended(2, {1, 6});
  // A set_discriminator with a length too short for its operand. The operand byte (6) runs as
  // DW_LNS_negate_stmt.
  builder.Extended(1, {4});
  builder.Raw({6});
  builder.Copy();
  builder.EndSequence();

  Decoded decoded = BuildAndCheck(builder);
  std::vector<LineRow> expected = {
      LineRow(0x1000, 1, 1, 0, kStmt),
      LineRow(0x1000, 1, 1, 0, kStmt | kEnd),
      LineRow(0, 1, 1, 0, 0),
      LineRow(0, 1, 1, 0, kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);
}

TEST(LineProgram, SetAddressSizes) {
  Builder builder = MakeBuilder();
  builder.SetAddress(0x12, 1);
  builder.Copy();
  builder.SetAddress(0x1234, 2);
  builder.Copy();
  builder.SetAddress(0x12345678, 4);  // Doesn't match the header's address size of 8.
  builder.Copy();
  builder.SetAddress(0x123456789abcdef0, 8);
  builder.Copy();
  // Unsupported sizes are ignored.
  builder.Extended(4, {2, 0xaa, 0xbb, 0xcc});
  builder.Copy();
  std::vector<uint8_t> huge(0x109, 0);
  huge[0] = 2;
  builder.Extended(huge.size(), huge);
  builder.Copy();
  builder.EndSequence();

  Decoded decoded = BuildAndCheck(builder);
  ASSERT_EQ(7u, decoded.rows.size());
  EXPECT_EQ(0x12u, decoded.rows[0].address);
  EXPECT_EQ(0x1234u, decoded.rows[1].address);
  EXPECT_EQ(0x12345678u, decoded.rows[2].address);
  EXPECT_EQ(0x123456789abcdef0u, decoded.rows[3].address);
  EXPECT_EQ(0x123456789abcdef0u, decoded.rows[4].address);
  EXPECT_EQ(0x123456789abcdef0u, decoded.rows[5].address);
}

TEST(LineProgram, Tombstones) {
  Builder builder = MakeBuilder();
  // Rows aren't emitted while the address is the tombstone for the operand size.
  builder.SetAddress(UINT64_MAX);
  builder.Copy();
  builder.AdvancePc(4);
  builder.Copy();
  builder.EndSequence();
  // The suppression continues after end_sequence until the next set_address.
  builder.Copy();
  builder.EndSequence();
  builder.SetAddress(0xffffffff, 4);
  builder.Copy();
  builder.EndSequence();
  builder.SetAddress(0xffff, 2);
  builder.Copy();
  builder.EndSequence();
  // An 8-byte 0xffffffff isn't a tombstone.
  builder.SetAddress(0xffffffff, 8);
  builder.Copy();
  builder.EndSequence();

  Decoded decoded = BuildAndCheck(builder);
  std::vector<LineRow> expected = {
      LineRow(0xffffffff, 1, 1, 0, kStmt),
      LineRow(0xffffffff, 1, 1, 0, kStmt | kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);
}

TEST(LineProgram, StrippedSequences) {
  // Linkers that don't write tombstones set the address of removed code to 0. These rows are
  // decoded, but LineTable drops them and the file scan ignores them.
  Builder builder = MakeBuilder(4);
  uint64_t live_file = builder.AddFile("live.cc", 0);
  uint64_t dead_file = builder.AddFile("dead.cc", 0);
  uint64_t both_file = builder.AddFile("both.cc", 0);

  builder.SetAddress(0);
  builder.SetFile(dead_file);
  builder.Copy();
  builder.SetFile(both_file);
  builder.AdvancePc(4);
  builder.Copy();
  builder.AdvancePc(4);
  builder.EndSequence();

  builder.SetAddress(0x1000);
  builder.SetFile(live_file);
  builder.Copy();
  builder.SetFile(both_file);
  builder.AdvancePc(4);
  builder.Copy();
  builder.AdvancePc(4);
  builder.EndSequence();

  Decoded decoded = BuildAndCheck(builder);
  EXPECT_EQ(6u, decoded.rows.size());
  EXPECT_EQ((std::vector<uint32_t>{static_cast<uint32_t>(live_file),
                                   static_cast<uint32_t>(both_file)}),
            decoded.files);

  LineTable table(nullptr, decoded.rows);
  ASSERT_EQ(1u, table.GetNumSequences());
  EXPECT_EQ(0x1000u, table.sequences()[0].addresses.begin());
}

TEST(LineProgram, ScanFiles) {
  Builder builder = MakeBuilder(4);
  uint64_t no_row_file = builder.AddFile("no_row.cc", 0);
  uint64_t end_row_file = builder.AddFile("end_row.cc", 0);
  uint64_t unterminated_file = builder.AddFile("unterminated.cc", 0);

  builder.SetAddress(0x1000);
  builder.Copy();  // File 1.
  // A file that is set but never has a row isn't referenced.
  builder.SetFile(no_row_file);
  builder.SetFile(end_row_file);
  builder.AdvancePc(4);
  // A file that only the end_sequence row has is referenced, like LLVM's rows show.
  builder.EndSequence();

  // A file index outside the table is ignored.
  builder.SetAddress(0x2000);
  builder.SetFile(1000);
  builder.Copy();
  builder.AdvancePc(4);
  builder.EndSequence();

  // A sequence without an end_sequence row isn't complete.
  builder.SetAddress(0x3000);
  builder.SetFile(unterminated_file);
  builder.Copy();

  Decoded decoded = BuildAndCheck(builder);
  EXPECT_EQ((std::vector<uint32_t>{1, static_cast<uint32_t>(end_row_file)}), decoded.files);

  LineTable table(nullptr, decoded.rows);
  EXPECT_EQ(2u, table.GetNumSequences());
}

TEST(LineProgram, LargeFileIndex) {
  // LLVM truncates file register values to 16 bits. zxdb keeps 32 bits, so this differs from LLVM.
  Builder builder = MakeBuilder(4);
  builder.SetAddress(0x1000);
  builder.SetFile(0x12345);
  builder.Copy();
  builder.EndSequence();

  Decoded decoded = BuildAndCheck(builder, false);
  ASSERT_EQ(2u, decoded.rows.size());
  EXPECT_EQ(0x12345u, decoded.rows[0].file);
}

TEST(LineProgram, TruncatedOpcodes) {
  struct Case {
    const char* name;
    std::vector<uint8_t> bytes;
  };
  // Each program has a complete sequence followed by a truncated opcode.
  Case cases[] = {
      {"advance_pc", {2, 0x80}},
      {"advance_line", {3, 0xff}},
      {"set_file", {4}},
      {"set_column", {5, 0x80, 0x80}},
      {"fixed_advance_pc", {9, 1}},
      {"set_isa", {12}},
      {"extended length", {0, 0x80}},
      {"extended operands", {0, 9, 2, 1, 2}},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    Builder builder = MakeBuilder();
    builder.SetAddress(0x1000);
    builder.Copy();
    builder.AdvancePc(2);
    builder.EndSequence();
    builder.Raw(c.bytes);

    // LLVM reports an error for these and its rows aren't comparable in every case.
    Decoded decoded = BuildAndCheck(builder, false);
    EXPECT_EQ(LineProgramStatus::kCorrupt, decoded.status);
    ASSERT_EQ(2u, decoded.rows.size());
    EXPECT_EQ((std::vector<uint32_t>{1}), decoded.files);
  }
}

TEST(LineProgram, UnitLengthLongerThanSection) {
  // LLVM uses the data up to the end of the section.
  Builder builder = MakeBuilder();
  builder.SetAddress(0x1000);
  builder.Copy();
  builder.AdvancePc(1);
  builder.EndSequence();
  builder.set_unit_length_adjust(10);

  Decoded decoded = BuildAndCheck(builder);
  EXPECT_EQ(LineProgramStatus::kOk, decoded.status);
  EXPECT_EQ(2u, decoded.rows.size());
}

TEST(LineProgram, UnitLengthShorterThanProgram) {
  // The decoder stops at the end of the unit. The rest is the next table, which isn't valid here.
  Builder builder = MakeBuilder();
  builder.SetAddress(0x1000);
  builder.Copy();
  builder.AdvancePc(1);
  builder.EndSequence();
  builder.Copy();
  builder.Copy();
  builder.set_unit_length_adjust(-2);

  Decoded decoded = BuildAndCheck(builder);
  EXPECT_EQ(2u, decoded.rows.size());
}

TEST(LineProgram, HeaderLengthMismatch) {
  // The program starts at the offset that header_length gives, even if the tables end before it.
  // Here the extra header byte is a DW_LNS_copy opcode that isn't run.
  Builder builder = MakeBuilder(4);
  builder.set_header_length_adjust(1);
  builder.Copy();
  builder.SetAddress(0x1000);
  builder.Copy();
  builder.EndSequence();

  Decoded decoded = BuildAndCheck(builder);
  EXPECT_EQ(2u, decoded.rows.size());

  // A header_length that ends inside the file table. The last header byte (the terminator of the
  // file table) runs as an opcode. It is 0, so it starts an extended opcode.
  Builder short_header = MakeBuilder(4);
  short_header.set_header_length_adjust(-1);
  short_header.Raw({1, 1});  // Length and DW_LNE_end_sequence for the extended opcode.
  short_header.SetAddress(0x1000);
  short_header.Copy();
  short_header.EndSequence();
  decoded = BuildAndCheck(short_header);
  EXPECT_EQ(3u, decoded.rows.size());
}

TEST(LineProgram, UnsupportedHeaderAddressSize) {
  // The decoder uses the operand size of each DW_LNE_set_address, not the DWARF 5 header field.
  // When LLVM accepts the header, the rows must match.
  for (uint8_t size : {0, 3, 4, 8}) {
    SCOPED_TRACE(static_cast<int>(size));
    Builder builder = MakeBuilder();
    builder.set_address_size(size);
    builder.SetAddress(0x123456789a);
    builder.Copy();
    builder.EndSequence();

    LineTableTestSections sections;
    uint64_t offset = sections.Add(builder);
    sections.Finish();
    if (sections.Parse(offset).ok())
      DecodeAndCheck(sections, offset);
  }
}

namespace {

bool ParseFails(const Builder& builder) {
  LineTableTestSections sections;
  uint64_t offset = sections.Add(builder);
  sections.Finish();
  return sections.Parse(offset).has_error();
}

}  // namespace

TEST(LineProgram, ParseErrors) {
  for (uint16_t version : {0, 1, 6}) {
    SCOPED_TRACE(version);
    EXPECT_TRUE(ParseFails(MakeBuilder(version)));
  }

  Builder reserved = MakeBuilder();
  reserved.set_raw_unit_length(0xfffffff0);
  EXPECT_TRUE(ParseFails(reserved));

  Builder long_header = MakeBuilder();
  long_header.set_header_length_adjust(100);
  EXPECT_TRUE(ParseFails(long_header));

  // The data ends inside the file table.
  Builder truncated = MakeBuilder(4);
  truncated.set_truncate_bytes(3);
  EXPECT_TRUE(ParseFails(truncated));

  LineTableTestSections sections;
  sections.Add(MakeBuilder());
  sections.Finish();
  EXPECT_TRUE(sections.Parse(100000).has_error());
}

TEST(LineProgram, MultipleTables) {
  LineTableTestSections sections;
  std::vector<uint64_t> offsets;
  for (uint64_t i = 0; i < 3; i++) {
    Builder builder = MakeBuilder(i == 1 ? 4 : 5);
    builder.SetAddress(0x1000 * (i + 1));
    builder.Copy();
    builder.EndSequence();
    offsets.push_back(sections.Add(builder));
  }
  sections.Finish();

  EXPECT_EQ(0u, offsets[0]);
  for (uint64_t i = 0; i < 3; i++) {
    Decoded decoded = DecodeAndCheck(sections, offsets[i]);
    ASSERT_EQ(2u, decoded.rows.size());
    EXPECT_EQ(0x1000 * (i + 1), decoded.rows[0].address);
  }
}

TEST(LineProgram, EmptyProgram) {
  Decoded decoded = BuildAndCheck(MakeBuilder());
  EXPECT_EQ(LineProgramStatus::kOk, decoded.status);
  EXPECT_TRUE(decoded.rows.empty());
  EXPECT_TRUE(decoded.files.empty());
}

TEST(LineProgram, MaxOpsPerInstFallsBackToLLVM) {
  Builder builder = MakeBuilder(4);
  builder.set_max_ops_per_inst(4);
  builder.SetAddress(0x1000);
  builder.Copy();
  builder.SpecialOpcode(13 + 14 * 5);  // Five operations.
  builder.AdvancePc(3);
  builder.Copy();
  builder.EndSequence();

  LineTableTestSections sections;
  uint64_t offset = sections.Add(builder);
  sections.Finish();

  ErrOr<LineProgram> program = sections.Parse(offset);
  ASSERT_TRUE(program.ok());
  std::vector<LineRow> rows;
  EXPECT_EQ(LineProgramStatus::kUnsupported,
            DecodeLineProgramRows(program.value().header(), &rows));

  Decoded decoded = DecodeAndCheck(sections, offset);
  EXPECT_EQ(LineProgramStatus::kOk, decoded.status);
  EXPECT_EQ(4u, decoded.rows.size());
}

TEST(LineProgram, MaxOpsPerInstZero) {
  // LLVM treats 0 as 1.
  Builder builder = MakeBuilder(4);
  builder.set_max_ops_per_inst(0);
  builder.SetAddress(0x1000);
  builder.Special(3, 1);
  builder.EndSequence();

  Decoded decoded = BuildAndCheck(builder);
  ASSERT_EQ(2u, decoded.rows.size());
  EXPECT_EQ(0x1003u, decoded.rows[0].address);
}

TEST(LineProgram, DefineFileFallsBackToLLVM) {
  Builder builder(4);
  builder.AddDirectory("inc");
  builder.AddFile("a.cc", 0);
  builder.SetAddress(0x1000);
  builder.DefineFile("defined.h", 1);  // File 2.
  builder.SetFile(2);
  builder.Copy();
  builder.EndSequence();

  LineTableTestSections sections;
  uint64_t offset = sections.Add(builder);
  sections.Finish();

  ErrOr<LineProgram> program = sections.Parse(offset);
  ASSERT_TRUE(program.ok());
  EXPECT_EQ(2u, program.value().GetFileCount());

  std::vector<uint32_t> files;
  EXPECT_EQ(LineProgramStatus::kOk, program.value().ScanFiles(&files));
  EXPECT_EQ((std::vector<uint32_t>{2}), files);
  // The fallback replaced the prologue, so the defined file has a name.
  EXPECT_EQ(3u, program.value().GetFileCount());
  LineFileTable table = LineFileTable::FromPrologue(program.value().prologue());
  ASSERT_TRUE(table.GetFileName(2));
  EXPECT_EQ("inc/defined.h", *table.GetFileName(2));

  Decoded decoded = DecodeAndCheck(sections, offset);
  ASSERT_EQ(2u, decoded.rows.size());
}

TEST(LineProgram, BigEndianIsUnsupported) {
  LineProgramHeader header;
  header.little_endian = false;
  header.line_range = 14;
  header.opcode_base = 13;
  std::vector<uint8_t> program = {1};
  header.program = program;

  std::vector<LineRow> rows;
  EXPECT_EQ(LineProgramStatus::kUnsupported, DecodeLineProgramRows(header, &rows));
  std::vector<uint32_t> files;
  EXPECT_EQ(LineProgramStatus::kUnsupported, ScanLineProgramFiles(header, 2, &files));
}

// File name tables for DWARF 2 to 4. Names are inline strings and file 0 doesn't exist.
TEST(LineProgram, FileNamesDwarf4) {
  for (uint16_t version : {2, 3, 4}) {
    SCOPED_TRACE(version);
    Builder builder(version);
    uint64_t src = builder.AddDirectory("src");
    uint64_t abs = builder.AddDirectory("/abs/include");
    builder.AddDirectory("../up/./dir");
    builder.AddFile("in_comp_dir.cc", 0);
    builder.AddFile("a.cc", src);
    builder.AddFile("b.h", abs);
    builder.AddFile("/abs/file.h", src);  // Absolute names ignore the directory.
    builder.AddFile("c.h", 3);
    builder.AddFile("bad_dir.h", 99);  // An invalid directory index is ignored.

    LineTableTestSections sections;
    uint64_t offset = sections.Add(builder);
    sections.Finish();
    ErrOr<LineProgram> program = sections.Parse(offset);
    ASSERT_TRUE(program.ok());

    LineFileTable table = LineFileTable::FromPrologue(program.value().prologue());
    ASSERT_EQ(7u, table.size());
    EXPECT_FALSE(table.GetFileName(0));
    EXPECT_EQ("in_comp_dir.cc", *table.GetFileName(1));
    EXPECT_EQ("src/a.cc", *table.GetFileName(2));
    EXPECT_EQ("/abs/include/b.h", *table.GetFileName(3));
    EXPECT_EQ("/abs/file.h", *table.GetFileName(4));
    EXPECT_EQ("../up/dir/c.h", *table.GetFileName(5));
    EXPECT_EQ("bad_dir.h", *table.GetFileName(6));
    EXPECT_FALSE(table.GetFileName(7));
  }
}

// File name tables for DWARF 5 with each string form, directory index form, and format.
class LineProgramFileNameTest
    : public ::testing::TestWithParam<std::tuple<Builder::StringForm, Builder::DirIndexForm, bool,
                                                 Builder::Format>> {};

TEST_P(LineProgramFileNameTest, Dwarf5) {
  auto [string_form, dir_index_form, md5, format] = GetParam();
  Builder builder(5, format);
  builder.set_string_form(string_form);
  builder.set_dir_index_form(dir_index_form);
  builder.set_include_md5(md5);
  builder.AddDirectory("/build/out");  // The compilation directory.
  uint64_t src = builder.AddDirectory("../../src");
  uint64_t abs = builder.AddDirectory("/usr/include");
  builder.AddFile("main.cc", 0);  // The primary source file is file 0.
  builder.AddFile("lib.cc", src);
  builder.AddFile("stdio.h", abs);
  builder.AddFile("gen/./x.h", 0);
  builder.SetAddress(0x1000);
  builder.SetFile(0);
  builder.Copy();
  builder.SetFile(3);
  builder.Copy();
  builder.EndSequence();

  LineTableTestSections sections;
  uint64_t offset = sections.Add(builder);
  sections.Finish();
  ErrOr<LineProgram> program = sections.Parse(offset);
  ASSERT_TRUE(program.ok()) << program.err().msg();

  // Relative names don't include directory 0.
  LineFileTable table = LineFileTable::FromPrologue(program.value().prologue());
  ASSERT_EQ(4u, table.size());
  EXPECT_EQ("main.cc", *table.GetFileName(0));
  EXPECT_EQ("../../src/lib.cc", *table.GetFileName(1));
  EXPECT_EQ("/usr/include/stdio.h", *table.GetFileName(2));
  EXPECT_EQ("gen/x.h", *table.GetFileName(3));
  EXPECT_FALSE(table.GetFileName(4));

  Decoded decoded = DecodeAndCheck(sections, offset);
  EXPECT_EQ((std::vector<uint32_t>{0, 3}), decoded.files);
}

INSTANTIATE_TEST_SUITE_P(
    LineProgram, LineProgramFileNameTest,
    ::testing::Combine(::testing::Values(Builder::StringForm::kString,
                                         Builder::StringForm::kLineStrp,
                                         Builder::StringForm::kStrp),
                       ::testing::Values(Builder::DirIndexForm::kUdata,
                                         Builder::DirIndexForm::kData1,
                                         Builder::DirIndexForm::kData2),
                       ::testing::Bool(),
                       ::testing::Values(Builder::Format::kDwarf32, Builder::Format::kDwarf64)));

TEST(LineProgram, ManyFilesDwarf5) {
  // More than 128 files, so the counts and indices need multi-byte ULEB128 values.
  Builder builder(5);
  builder.AddDirectory("/build");
  for (int i = 0; i < 300; i++)
    builder.AddFile("file" + std::to_string(i) + ".cc", 0);
  builder.SetAddress(0x1000);
  builder.SetFile(299);
  builder.Copy();
  builder.EndSequence();

  LineTableTestSections sections;
  uint64_t offset = sections.Add(builder);
  sections.Finish();
  Decoded decoded = DecodeAndCheck(sections, offset);
  EXPECT_EQ(300u, decoded.file_count);
  EXPECT_EQ((std::vector<uint32_t>{299}), decoded.files);

  ErrOr<LineProgram> program = sections.Parse(offset);
  LineFileTable table = LineFileTable::FromPrologue(program.value().prologue());
  EXPECT_EQ("file299.cc", *table.GetFileName(299));
}

}  // namespace zxdb
