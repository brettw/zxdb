// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/line_program.h"

#include <string.h>

#include <gtest/gtest.h>

#include "llvm/BinaryFormat/Dwarf.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"
#include "src/developer/debug/zxdb/symbols/line_file_table.h"
#include "src/developer/debug/zxdb/symbols/line_program_llvm.h"
#include "src/developer/debug/zxdb/symbols/line_program_test_util.h"
#include "src/developer/debug/zxdb/symbols/line_table.h"

// These tests write line tables in memory, decode them, and check the rows and file names against
// expected values. The expected values come from the DWARF standard and the rules in
// line_tables.md. DecodeAndCheck() also checks that the file scan agrees with the decoded rows.

namespace zxdb {

namespace {

using Builder = LineTableBuilder;

constexpr uint8_t kStmt = LineRow::kIsStmt;
constexpr uint8_t kEnd = LineRow::kEndSequence;

// The first code address of the binary for the file scan. A sequence that starts at address 0 is
// below it, so it is stripped code.
constexpr uint64_t kFirstCodeAddress = 1;

struct Decoded {
  LineProgramStatus status = LineProgramStatus::kOk;
  std::vector<LineRow> rows;
  std::vector<uint32_t> files;
  size_t file_count = 0;
  LineFileTable names;  // The complete file table, with the files that the program defines.
};

// Checks the names of a file table. A null name means that the file has no name.
void ExpectFileNames(const LineFileTable& table, const std::vector<const char*>& expected) {
  ASSERT_EQ(expected.size(), table.size());
  for (size_t i = 0; i < expected.size(); i++) {
    SCOPED_TRACE(i);
    const std::string* name = table.GetFileName(i);
    if (expected[i]) {
      ASSERT_TRUE(name);
      EXPECT_EQ(expected[i], *name);
    } else {
      EXPECT_FALSE(name);
    }
  }
}

// Decodes the table at the offset with LineProgram. Checks that the file scan agrees with the
// decoded rows.
Decoded DecodeAndCheck(const LineTableTestSections& sections, uint64_t offset) {
  Decoded result;
  ErrOr<LineProgram> program = sections.Parse(offset);
  EXPECT_TRUE(program.ok()) << program.err().msg();
  if (!program.ok())
    return result;

  LineProgram::DecodeResult decoded = program.value().DecodeRows();
  result.status = decoded.status;
  result.rows = std::move(decoded.rows);
  LineFileEntries entries = program.value().files(decoded.defined_files);
  result.file_count = entries.size();
  result.names = LineFileTable::FromEntries(entries);

  // The scan must give the same files as the rows, from a separate parse.
  ErrOr<LineProgram> scan_program = sections.Parse(offset);
  if (scan_program.has_error())
    return result;
  LineProgram::FileScanResult scan = scan_program.value().ScanFiles(kFirstCodeAddress);
  result.files = std::move(scan.file_indices);
  EXPECT_EQ(result.status, scan.status);
  EXPECT_EQ(GetLiveRowFiles(result.rows, result.file_count, kFirstCodeAddress), result.files);
  return result;
}

Decoded BuildAndCheck(const Builder& builder) {
  LineTableTestSections sections;
  uint64_t offset = sections.Add(builder);
  sections.Finish();
  return DecodeAndCheck(sections, offset);
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

INSTANTIATE_TEST_SUITE_P(LineProgram, LineProgramVersionTest,
                         ::testing::Combine(::testing::Values(2, 3, 4, 5),
                                            ::testing::Values(Builder::Format::kDwarf32,
                                                              Builder::Format::kDwarf64)));

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
  builder.SetColumn(0x12345);  // Truncated to 16 bits.
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

// Runs every special opcode with different header parameters. Each row must follow the DWARF
// formula.
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

  Decoded decoded = BuildAndCheck(builder);
  EXPECT_EQ(LineProgramStatus::kOk, decoded.status);

  // A line_range of 0 has no valid special opcodes, so they don't change the address or line.
  std::vector<LineRow> expected;
  uint64_t address = 0x10000;
  uint32_t line = 1;
  for (int opcode = first_special; opcode <= 255; opcode++) {
    int adjusted = opcode - opcode_base;
    if (line_range) {
      address += (adjusted / line_range) * min_inst_length;
      line += line_base + adjusted % line_range;
    }
    expected.emplace_back(address, 1, line, 0, kStmt);
  }
  expected.emplace_back(address, 1, line, 0, kStmt | kEnd);
  EXPECT_EQ(expected, decoded.rows);
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
    builder.AdvancePc(2);       // Scaled by min_inst_length.
    builder.FixedAdvancePc(3);  // Not scaled by min_inst_length.
    builder.EndSequence();

    Decoded decoded = BuildAndCheck(builder);
    ASSERT_EQ(2u, decoded.rows.size());
    uint64_t const_add_pc = ((255 - opcode_base) / 14) * 4;
    EXPECT_EQ(0x1000 + const_add_pc, decoded.rows[0].address);
    EXPECT_EQ(0x1000 + const_add_pc + 2 * 4 + 3, decoded.rows[1].address);
  }
}

TEST(LineProgram, SpecialOpcodeEightWithLowOpcodeBase) {
  // With opcode_base 1, opcode 8 is a special opcode with adjusted value 7. It isn't
  // DW_LNS_const_add_pc.
  Builder builder = MakeBuilder(4);
  builder.set_opcode_base(1);
  builder.set_line_base(0);
  builder.set_line_range(4);
  builder.SetAddress(0x1000);
  builder.SpecialOpcode(8);
  builder.EndSequence();

  Decoded decoded = BuildAndCheck(builder);
  std::vector<LineRow> expected = {
      LineRow(0x1001, 1, 4, 0, kStmt),  // Address 0x1000 + 7 / 4, line 1 + 7 % 4.
      LineRow(0x1001, 1, 4, 0, kStmt | kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);
}

INSTANTIATE_TEST_SUITE_P(
    LineProgram, LineProgramSpecialTest,
    ::testing::Values(std::make_tuple(-5, 14, 13, 1),    // Clang.
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
  std::vector<LineRow> expected = {
      LineRow(0x1000, 1, 1, 0, kStmt),
      LineRow(0x1000, 1, static_cast<uint32_t>(-4), 0, kStmt),  // Adjusted 0: line_base, -5.
      LineRow(0x1000, 1, static_cast<uint32_t>(-4), 0, kStmt | kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);
}

TEST(LineProgram, ExtendedOpcodes) {
  Builder builder = MakeBuilder();
  builder.SetAddress(0x1000);
  builder.SetDiscriminator(5);
  builder.Extended(0, {});               // Zero length: no sub-opcode.
  builder.Extended(4, {0x80, 1, 2, 3});  // Unknown vendor opcode.
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
  std::vector<LineRow> expected = {
      LineRow(0x12, 1, 1, 0, kStmt),
      LineRow(0x1234, 1, 1, 0, kStmt),
      LineRow(0x12345678, 1, 1, 0, kStmt),
      LineRow(0x123456789abcdef0, 1, 1, 0, kStmt),
      LineRow(0x123456789abcdef0, 1, 1, 0, kStmt),  // A 3-byte operand.
      LineRow(0x123456789abcdef0, 1, 1, 0, kStmt),  // A 0x108-byte operand.
      LineRow(0x123456789abcdef0, 1, 1, 0, kStmt | kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);
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
  uint32_t live = static_cast<uint32_t>(live_file);
  uint32_t dead = static_cast<uint32_t>(dead_file);
  uint32_t both = static_cast<uint32_t>(both_file);
  std::vector<LineRow> expected = {
      LineRow(0, dead, 1, 0, kStmt),        LineRow(4, both, 1, 0, kStmt),
      LineRow(8, both, 1, 0, kStmt | kEnd), LineRow(0x1000, live, 1, 0, kStmt),
      LineRow(0x1004, both, 1, 0, kStmt),   LineRow(0x1008, both, 1, 0, kStmt | kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);
  EXPECT_EQ(
      (std::vector<uint32_t>{static_cast<uint32_t>(live_file), static_cast<uint32_t>(both_file)}),
      decoded.files);

  LineTable table(nullptr, decoded.rows, kFirstCodeAddress);
  ASSERT_EQ(1u, table.GetNumSequences());
  EXPECT_EQ(0x1000u, table.GetSequenceAt(0).addresses().begin());
}

TEST(LineProgram, StrippedSequencesBelowFirstCode) {
  // Some linkers give removed code the address 0 plus its offset in the removed section. Every
  // sequence that starts below the first code address of the binary is stripped, like in LLDB. A
  // sequence at the first code address is live.
  constexpr uint64_t kFirstCode = 0x1000;
  Builder builder = MakeBuilder(4);
  uint64_t live_file = builder.AddFile("live.cc", 0);
  uint64_t dead_file = builder.AddFile("dead.cc", 0);

  builder.SetAddress(0x20);
  builder.SetFile(dead_file);
  builder.Copy();
  builder.AdvancePc(4);
  builder.EndSequence();

  builder.SetAddress(kFirstCode);
  builder.SetFile(live_file);
  builder.Copy();
  builder.AdvancePc(4);
  builder.EndSequence();

  LineTableTestSections sections;
  uint64_t offset = sections.Add(builder);
  sections.Finish();
  ErrOr<LineProgram> program = sections.Parse(offset);
  ASSERT_TRUE(program.ok());
  auto scan = program.value().ScanFiles(kFirstCode);
  EXPECT_EQ(LineProgramStatus::kOk, scan.status);
  auto files = scan.file_indices;
  EXPECT_EQ((std::vector<uint32_t>{static_cast<uint32_t>(live_file)}), files);

  auto decoded = program.value().DecodeRows();
  EXPECT_EQ(LineProgramStatus::kOk, decoded.status);
  const auto& rows = decoded.rows;
  EXPECT_EQ(files,
            GetLiveRowFiles(rows, program.value().files(decoded.defined_files).size(), kFirstCode));
  LineTable table(nullptr, rows, kFirstCode);
  ASSERT_EQ(1u, table.GetNumSequences());
  EXPECT_EQ(kFirstCode, table.GetSequenceAt(0).addresses().begin());

  // Without code sections, the first code address is 0 and every sequence is live.
  scan = program.value().ScanFiles(0);
  EXPECT_EQ(LineProgramStatus::kOk, scan.status);
  files = scan.file_indices;
  EXPECT_EQ(
      (std::vector<uint32_t>{static_cast<uint32_t>(live_file), static_cast<uint32_t>(dead_file)}),
      files);
  EXPECT_EQ(2u, LineTable(nullptr, rows, 0).GetNumSequences());
}

TEST(LineProgram, RejectsDecreasingSequences) {
  // Check both a reversed end and an internal decrease whose end is after the start. A later
  // valid sequence must survive in the file scan and in the lookup table.
  for (uint64_t end_address : {0x1ff0, 0x2010}) {
    SCOPED_TRACE(end_address);
    Builder builder = MakeBuilder(4);
    uint32_t bad_file = builder.AddFile("bad.h", 0);
    uint32_t good_file = builder.AddFile("good.cc", 0);
    builder.SetAddress(0x2000);
    builder.SetFile(bad_file);
    builder.Copy();
    builder.SetAddress(0x2008);
    builder.Copy();
    if (end_address > 0x2000) {
      builder.SetAddress(0x2004);
      builder.Copy();
    }
    builder.SetAddress(end_address);
    builder.EndSequence();
    builder.SetAddress(0x3000);
    builder.SetFile(good_file);
    builder.Copy();
    builder.AdvancePc(4);
    builder.EndSequence();

    Decoded decoded = BuildAndCheck(builder);
    EXPECT_EQ(LineProgramStatus::kOk, decoded.status);
    EXPECT_EQ((std::vector<uint32_t>{good_file}), decoded.files);
    LineTable table(nullptr, decoded.rows, kFirstCodeAddress);
    ASSERT_EQ(1u, table.GetNumSequences());
    EXPECT_TRUE(table.GetSequenceForRelativeAddress(0x2000).empty());
    EXPECT_EQ(AddressRange(0x3000, 0x3004), table.GetSequenceAt(0).addresses());
  }
}

TEST(LineProgram, PartialResultsKeepDefinedFiles) {
  Builder builder(4);
  builder.AddFile("main.cc", 0);
  builder.DefineFile("defined.h", 0);
  builder.SetAddress(0x1000);
  builder.SetFile(2);
  builder.Copy();
  builder.AdvancePc(4);
  builder.EndSequence();
  builder.Raw({2, 0x80});  // Truncated DW_LNS_advance_pc operand.

  LineTableTestSections sections;
  uint64_t offset = sections.Add(builder);
  sections.Finish();
  auto parsed = sections.Parse(offset);
  ASSERT_TRUE(parsed.ok());
  const LineProgram& program = parsed.value();
  auto decoded = program.DecodeRows();
  auto scan = program.ScanFiles(kFirstCodeAddress);
  EXPECT_EQ(LineProgramStatus::kCorrupt, decoded.status);
  EXPECT_EQ(LineProgramStatus::kCorrupt, scan.status);
  ASSERT_EQ(2u, decoded.rows.size());
  EXPECT_TRUE(decoded.rows.back().end_sequence());
  EXPECT_EQ((std::vector<uint32_t>{2}), scan.file_indices);
  EXPECT_EQ(2u, program.header_files().size());
  for (auto entries : {program.files(decoded.defined_files), program.files(scan.defined_files)}) {
    LineFileTable names = LineFileTable::FromEntries(entries);
    ASSERT_EQ(3u, names.size());
    ASSERT_TRUE(names.GetFileName(2));
    EXPECT_EQ("defined.h", *names.GetFileName(2));
  }
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
  // A file that only the end_sequence row has is referenced.
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

  LineTable table(nullptr, decoded.rows, kFirstCodeAddress);
  EXPECT_EQ(2u, table.GetNumSequences());
}

TEST(LineProgram, LargeFileIndex) {
  // The file register keeps 32 bits.
  Builder builder = MakeBuilder(4);
  builder.SetAddress(0x1000);
  builder.SetFile(0x12345);
  builder.Copy();
  builder.EndSequence();

  Decoded decoded = BuildAndCheck(builder);
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

    Decoded decoded = BuildAndCheck(builder);
    EXPECT_EQ(LineProgramStatus::kCorrupt, decoded.status);
    ASSERT_EQ(2u, decoded.rows.size());
    EXPECT_EQ((std::vector<uint32_t>{1}), decoded.files);
  }
}

// LEB128 values that don't fit in 64 bits end the program, like truncated values. Values that fit,
// including ones with more bytes than needed, decode.
TEST(LineProgram, LebOverflow) {
  std::vector<uint8_t> too_big(9, 0x80);
  too_big.push_back(0x02);  // 2^64 in both ULEB128 and SLEB128.

  struct Case {
    const char* name;
    uint8_t opcode;
    uint8_t last_byte = 0x02;  // 2^64 after nine 0x80 bytes.
  };
  for (const Case& c :
       {Case{"advance_pc", 2}, Case{"advance_line INT64_MAX + 1", 3, 0x01},
        Case{"advance_line 2^64", 3}, Case{"set_file", 4}, Case{"set_column", 5},
        Case{"set_isa", 12}, Case{"unknown standard", 13}, Case{"extended length", 0}}) {
    SCOPED_TRACE(c.name);
    Builder builder = MakeBuilder();
    builder.set_opcode_base(14);
    builder.set_standard_opcode_length(13, 1);
    builder.SetAddress(0x1000);
    builder.Copy();
    builder.AdvancePc(2);
    builder.EndSequence();
    builder.Raw({c.opcode});
    std::vector<uint8_t> operand = too_big;
    operand.back() = c.last_byte;
    builder.Raw(operand);
    // A sequence that the decoder doesn't reach.
    builder.SetAddress(0x2000);
    builder.Copy();
    builder.EndSequence();

    Decoded decoded = BuildAndCheck(builder);
    EXPECT_EQ(LineProgramStatus::kCorrupt, decoded.status);
    EXPECT_EQ(2u, decoded.rows.size());
  }

  {
    // The largest values, and values with extra bytes that only extend them.
    Builder builder = MakeBuilder();
    builder.SetAddress(0x1000);
    builder.Raw({2, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01});  // 2^64 - 1.
    builder.Raw({2, 0x81, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00});  // 1.
    builder.Copy();
    builder.Raw({3, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7f});  // INT64_MIN.
    builder.Raw({3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f});  // -1.
    builder.Raw({5, 0x82, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00});  // 2.
    builder.Copy();
    builder.EndSequence();

    Decoded decoded = BuildAndCheck(builder);
    EXPECT_EQ(LineProgramStatus::kOk, decoded.status);
    // The address advances wrap to 0x1000. The line is 1 + INT64_MIN - 1 in 32 bits.
    std::vector<LineRow> expected = {
        LineRow(0x1000, 1, 1, 0, kStmt),
        LineRow(0x1000, 1, 0, 2, kStmt),
        LineRow(0x1000, 1, 0, 2, kStmt | kEnd),
    };
    EXPECT_EQ(expected, decoded.rows);
  }

  {
    // A negative value with a zero bit, extended past 64 bits. The bytes past bit 63 add nothing,
    // so they don't set the zero bit.
    Builder builder = MakeBuilder();
    builder.SetAddress(0x1000);
    builder.Raw({3, 0xe3, 0x00});  // Line 100.
    builder.Raw({3, 0xbf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f});  // -65.
    builder.Copy();
    builder.EndSequence();

    Decoded decoded = BuildAndCheck(builder);
    EXPECT_EQ(LineProgramStatus::kOk, decoded.status);
    ASSERT_EQ(2u, decoded.rows.size());
    EXPECT_EQ(35u, decoded.rows[0].line);
  }

  {
    // A DWARF 4 file entry with a modification time above 64 bits ends the file table.
    std::vector<uint8_t> tables = {0};  // No directories.
    tables.insert(tables.end(), {'a', '.', 'c', 'c', 0, 0, 0, 0});
    tables.insert(tables.end(), {'b', '.', 'c', 'c', 0, 0});
    tables.insert(tables.end(), too_big.begin(), too_big.end());
    tables.insert(tables.end(), {0, 0});
    Builder builder(4);
    builder.set_raw_tables(tables);
    builder.SetAddress(0x1000);
    builder.Copy();
    builder.EndSequence();

    Decoded decoded = BuildAndCheck(builder);
    EXPECT_EQ(2u, decoded.file_count);  // File 0 and "a.cc".
    ExpectFileNames(decoded.names, {nullptr, "a.cc"});
  }
}

TEST(LineProgram, UnitLengthLongerThanSection) {
  // The decoder uses the data up to the end of the section.
  Builder builder = MakeBuilder();
  builder.SetAddress(0x1000);
  builder.Copy();
  builder.AdvancePc(1);
  builder.EndSequence();
  builder.set_unit_length_adjust(10);

  Decoded decoded = BuildAndCheck(builder);
  EXPECT_EQ(LineProgramStatus::kOk, decoded.status);
  std::vector<LineRow> expected = {
      LineRow(0x1000, 1, 1, 0, kStmt),
      LineRow(0x1001, 1, 1, 0, kStmt | kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);
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
  std::vector<LineRow> expected = {
      LineRow(0x1000, 1, 1, 0, kStmt),
      LineRow(0x1001, 1, 1, 0, kStmt | kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);
}

// The tables end where header_length says, so an entry that crosses that end is dropped. The
// program starts at the end of the header.
TEST(LineProgram, TableEntryPastHeaderLength) {
  for (uint16_t version : {4, 5}) {
    SCOPED_TRACE(version);
    Builder builder = MakeBuilder(version);
    builder.AddFile("last.cc", 0);
    // Cut the last field of the last entry. For DWARF 4, also cut the end of the table.
    builder.set_header_length_adjust(version >= 5 ? -1 : -2);
    LineTableTestSections sections;
    uint64_t offset = sections.Add(builder);
    sections.Finish();

    ErrOr<LineProgram> program = sections.Parse(offset);
    ASSERT_TRUE(program.ok());
    LineFileTable names = LineFileTable::FromEntries(program.value().header_files());
    if (version >= 5)
      ExpectFileNames(names, {"main.cc", "main.cc"});
    else
      ExpectFileNames(names, {nullptr, "main.cc"});
  }

  {
    // A DWARF 5 directory whose second field crosses the end. The parser drops the whole entry,
    // including its path. The file table after it is lost.
    using namespace llvm::dwarf;
    std::vector<uint8_t> tables = {
        2, DW_LNCT_path, FormByte(DwarfForm::kString), 0x81, 0x40, FormByte(DwarfForm::kData4), 1};
    tables.insert(tables.end(), {'/', 'b', 0, 1, 2, 3, 4});
    tables.insert(tables.end(), {1, DW_LNCT_path, FormByte(DwarfForm::kString), 0});  // No files.
    Builder builder(5);
    builder.set_raw_tables(tables);
    builder.set_header_length_adjust(-6);  // The end is inside the DW_FORM_data4 value.
    LineTableTestSections sections;
    uint64_t offset = sections.Add(builder);
    sections.Finish();

    ErrOr<LineProgram> program = sections.Parse(offset);
    ASSERT_TRUE(program.ok());
    EXPECT_TRUE(program.value().header().include_dirs.empty());
    EXPECT_TRUE(program.value().header().files.empty());
  }
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
  std::vector<LineRow> expected = {
      LineRow(0x1000, 1, 1, 0, kStmt),
      LineRow(0x1000, 1, 1, 0, kStmt | kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);

  // A header_length that ends inside the file table. The last header byte (the terminator of the
  // file table) runs as an opcode. It is 0, so it starts an extended opcode.
  Builder short_header = MakeBuilder(4);
  short_header.set_header_length_adjust(-1);
  short_header.Raw({1, 1});  // Length and DW_LNE_end_sequence for the extended opcode.
  short_header.SetAddress(0x1000);
  short_header.Copy();
  short_header.EndSequence();
  decoded = BuildAndCheck(short_header);
  expected = {
      LineRow(0, 1, 1, 0, kStmt | kEnd),  // The end_sequence of the extended opcode.
      LineRow(0x1000, 1, 1, 0, kStmt),
      LineRow(0x1000, 1, 1, 0, kStmt | kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);
  ExpectFileNames(decoded.names, {nullptr, "main.cc"});
}

TEST(LineProgram, UnsupportedHeaderAddressSize) {
  // The decoder uses the operand size of each DW_LNE_set_address, not the DWARF 5 header field.
  for (uint8_t size : {0, 3, 4, 8}) {
    SCOPED_TRACE(static_cast<int>(size));
    Builder builder = MakeBuilder();
    builder.set_address_size(size);
    builder.SetAddress(0x123456789a);
    builder.Copy();
    builder.EndSequence();

    Decoded decoded = BuildAndCheck(builder);
    std::vector<LineRow> expected = {
        LineRow(0x123456789a, 1, 1, 0, kStmt),
        LineRow(0x123456789a, 1, 1, 0, kStmt | kEnd),
    };
    EXPECT_EQ(expected, decoded.rows);
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

// DW_FORM_strx indices must be inside the string offsets table. An index whose entry offset
// overflows must not wrap around to a valid entry.
TEST(LineProgram, IndexedStringBounds) {
  using namespace llvm::dwarf;
  const std::string str("first.cc\0other.cc\0", 18);

  for (uint8_t entry_size : {4, 8}) {
    for (uint64_t base : {0, 8}) {
      SCOPED_TRACE(::testing::Message()
                   << "entry size " << static_cast<int>(entry_size) << " base " << base);
      // The table has one entry, for "first.cc" at offset 0.
      std::string str_offsets(base + entry_size, '\0');
      LineStringSections strings;
      strings.str = str;
      strings.str_offsets = str_offsets;
      strings.str_offsets_base = base;
      strings.str_offset_size = entry_size;

      // base + index * entry_size is base again after it wraps.
      const uint64_t wrapping_index = (~uint64_t{0} / entry_size) + 1;
      std::vector<uint8_t> tables = {1, DW_LNCT_path, FormByte(DwarfForm::kString), 1};
      tables.insert(tables.end(), {'/', 'b', 0});
      tables.insert(tables.end(), {2, DW_LNCT_path, FormByte(DwarfForm::kStrx),
                                   DW_LNCT_directory_index, FormByte(DwarfForm::kUdata), 3});
      for (uint64_t index : {uint64_t{0}, uint64_t{1}, wrapping_index}) {
        AppendUleb(tables, index);
        tables.push_back(0);  // Directory index.
      }

      Builder builder(5);
      builder.set_raw_tables(tables);
      LineTableTestSections sections;
      uint64_t offset = sections.Add(builder);
      sections.Finish();
      llvm::StringRef line = sections.line_section().Data;
      ErrOr<LineProgram> program =
          LineProgram::Parse(std::string_view(line.data(), line.size()), offset, true, strings);
      ASSERT_TRUE(program.ok()) << program.err().msg();
      ASSERT_EQ(3u, program.value().header().files.size());
      EXPECT_EQ("first.cc", program.value().header().files[0].name);
      EXPECT_EQ(std::nullopt, program.value().header().files[1].name);  // Past the table.
      EXPECT_EQ(std::nullopt, program.value().header().files[2].name);  // Wraps.
    }
  }
}

// Nonzero string offsets exercise byte order in both the indexed forms and their offset entries.
// Real units give the string offsets base and entry width, so the test also checks those.
TEST(LineProgram, IndexedStringByteOrder) {
  using namespace llvm::dwarf;
  for (bool little_endian : {false, true}) {
    for (bool dwarf64 : {false, true}) {
      for (DwarfForm form : {DwarfForm::kStrx, DwarfForm::kStrx1, DwarfForm::kStrx2,
                             DwarfForm::kStrx3, DwarfForm::kStrx4, DwarfForm::kGnuStrIndex}) {
        SCOPED_TRACE(::testing::Message() << "little endian " << little_endian << " DWARF64 "
                                          << dwarf64 << " form " << DwarfFormToString(form));
        const size_t offset_size = dwarf64 ? 8 : 4;
        const uint64_t str_offsets_base = dwarf64 ? 16 : 8;
        auto put = [little_endian](std::vector<uint8_t>& out, uint64_t value, size_t size) {
          AppendInteger(out, value, size, little_endian);
        };
        auto contribution = [&](const std::vector<uint8_t>& body) {
          std::vector<uint8_t> out;
          if (dwarf64)
            put(out, 0xffffffff, 4);
          put(out, body.size(), offset_size);
          out.insert(out.end(), body.begin(), body.end());
          return out;
        };

        std::vector<uint8_t> offsets;
        put(offsets, 5, 2);  // Version.
        put(offsets, 0, 2);  // Padding.
        put(offsets, 4, offset_size);
        put(offsets, 13, offset_size);

        std::vector<uint8_t> tables = {
            1, DW_LNCT_path, FormByte(DwarfForm::kString), 1, '/', 'b', 0, 1, DW_LNCT_path};
        AppendUleb(tables, form);
        AppendUleb(tables, 2);
        for (uint64_t index : {1, 0}) {
          if (form == DwarfForm::kStrx || form == DwarfForm::kGnuStrIndex)
            AppendUleb(tables, index);
          else
            put(tables, index, static_cast<int>(form) - static_cast<int>(DwarfForm::kStrx1) + 1);
        }
        Builder builder(5, dwarf64 ? Builder::Format::kDwarf64 : Builder::Format::kDwarf32);
        builder.set_big_endian(!little_endian);
        builder.set_raw_tables(tables);
        LineTableTestSections line;
        line.Add(builder);
        line.Finish(8, little_endian);

        std::vector<uint8_t> abbrev = {1,
                                       DW_TAG_compile_unit,
                                       0,
                                       DW_AT_stmt_list,
                                       FormByte(DwarfForm::kSecOffset),
                                       DW_AT_str_offsets_base,
                                       FormByte(DwarfForm::kSecOffset),
                                       0,
                                       0,
                                       0};
        std::vector<uint8_t> info;
        put(info, 5, 2);
        info.push_back(DW_UT_compile);
        info.push_back(8);          // Address size.
        put(info, 0, offset_size);  // Abbreviation offset.
        AppendUleb(info, 1);
        put(info, 0, offset_size);  // Line table offset.
        put(info, str_offsets_base, offset_size);

        llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> buffers;
        auto add = [&](const char* name, const std::vector<uint8_t>& bytes) {
          buffers[name] = llvm::MemoryBuffer::getMemBufferCopy(
              llvm::StringRef(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        };
        add("debug_info", contribution(info));
        add("debug_abbrev", abbrev);
        add("debug_str_offsets", contribution(offsets));
        buffers["debug_str"] =
            llvm::MemoryBuffer::getMemBufferCopy(llvm::StringRef("pad\0first.cc\0second.cc\0", 23));
        buffers["debug_line"] = llvm::MemoryBuffer::getMemBufferCopy(line.line_section().Data);
        auto context = llvm::DWARFContext::create(buffers, 8, little_endian);
        llvm::DWARFUnit* unit = context->getUnitAtIndex(0);
        ASSERT_TRUE(unit);
        LineStringSections strings = GetLineStringSections(*unit);
        EXPECT_EQ(str_offsets_base, strings.str_offsets_base);
        EXPECT_EQ(offset_size, strings.str_offset_size);

        ErrOr<LineProgram> program = ParseLineProgramForUnit(*unit);
        ASSERT_TRUE(program.ok()) << program.err().msg();
        ASSERT_EQ(2u, program.value().header().files.size());
        EXPECT_EQ("second.cc", program.value().header().files[0].name);
        EXPECT_EQ("first.cc", program.value().header().files[1].name);
      }
    }
  }
}

// Unit and header lengths near 2^64 must not overflow.
TEST(LineProgram, LineTableExtent) {
  for (bool little_endian : {true, false}) {
    SCOPED_TRACE(little_endian);
    auto put = [little_endian](std::vector<uint8_t>& out, uint64_t value, size_t size) {
      AppendInteger(out, value, size, little_endian);
    };
    auto extent = [little_endian](const std::vector<uint8_t>& data, uint64_t offset = 0) {
      return ReadLineTableExtent(
          std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), offset,
          little_endian);
    };

    // A DWARF64 length that wraps when it is added to the offset means the rest of the section.
    std::vector<uint8_t> data;
    put(data, 0xffffffff, 4);
    put(data, ~uint64_t{0}, 8);
    data.resize(32);
    ErrOr<LineTableExtent> result = extent(data);
    ASSERT_TRUE(result.ok()) << result.err().msg();
    EXPECT_EQ(12u, result.value().begin);
    EXPECT_EQ(32u, result.value().end);
    EXPECT_EQ(8u, result.value().offset_size);

    // A length inside the section.
    data.clear();
    put(data, 6, 4);
    data.resize(32);
    result = extent(data);
    ASSERT_TRUE(result.ok()) << result.err().msg();
    EXPECT_EQ(4u, result.value().begin);
    EXPECT_EQ(10u, result.value().end);
    EXPECT_EQ(4u, result.value().offset_size);

    // Offsets at the end of the section and a truncated DWARF64 length.
    EXPECT_TRUE(extent(data, 32).has_error());
    data.resize(8);
    data[0] = data[1] = data[2] = data[3] = 0xff;
    EXPECT_TRUE(extent(data).has_error());

    // A DWARF64 header length that wraps when it is added to its offset.
    data.clear();
    put(data, 0xffffffff, 4);
    put(data, 40, 8);
    put(data, 4, 2);
    put(data, ~uint64_t{0} - 4, 8);
    data.resize(52);
    EXPECT_TRUE(LineProgram::Parse(
                    std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), 0,
                    little_endian, LineStringSections())
                    .has_error());
  }
}

// Cuts the header at every byte. Only the complete header parses. A header that is longer than the
// data is an error.
TEST(LineProgram, TruncatedHeaders) {
  for (uint16_t version : {2, 3, 4, 5}) {
    for (Builder::Format format : {Builder::Format::kDwarf32, Builder::Format::kDwarf64}) {
      SCOPED_TRACE(::testing::Message() << "version " << version << " DWARF64 "
                                        << (format == Builder::Format::kDwarf64));
      Builder builder(version, format);
      if (version >= 5) {
        builder.set_include_md5(true);
        builder.AddDirectory("/build");
      }
      uint64_t dir = builder.AddDirectory("src");
      builder.AddFile("a.cc", dir);
      builder.AddFile("b.h", 0);
      builder.AddFile("c.h", dir);

      // Parse prefixes of one section. The parser reads only the given bytes, so a prefix is a
      // truncated section.
      LineTableTestSections full;
      full.Add(builder);
      full.Finish();
      const llvm::StringRef section = full.line_section().Data;
      LineStringSections strings = GetLineStringSections(full.context());

      for (size_t length = 1; length < section.size(); length++) {
        SCOPED_TRACE(length);
        std::string_view prefix(section.data(), length);
        EXPECT_TRUE(LineProgram::Parse(prefix, 0, true, strings).has_error());
      }

      ErrOr<LineProgram> program =
          LineProgram::Parse(std::string_view(section.data(), section.size()), 0, true, strings);
      ASSERT_TRUE(program.ok()) << program.err().msg();
      LineFileTable names = LineFileTable::FromEntries(program.value().header_files());
      if (version >= 5)
        ExpectFileNames(names, {"src/a.cc", "b.h", "src/c.h"});
      else
        ExpectFileNames(names, {nullptr, "src/a.cc", "b.h", "src/c.h"});
    }
  }
}

// DWARF 5 tables with unusual or invalid entry formats. An error in a table keeps the entries
// before it and doesn't fail the header.
TEST(LineProgram, Dwarf5TableFormats) {
  using namespace llvm::dwarf;
  auto cstr = [](std::vector<uint8_t>& out, const char* str) {
    out.insert(out.end(), str, str + strlen(str) + 1);
  };
  // A directory table with "/build" and "d", using inline strings.
  auto dirs = [&](std::vector<uint8_t>& out) {
    out.push_back(1);
    AppendUleb(out, DW_LNCT_path);
    AppendUleb(out, DwarfForm::kString);
    AppendUleb(out, 2);
    cstr(out, "/build");
    cstr(out, "d");
  };

  struct Case {
    const char* name;
    std::vector<uint8_t> tables;
    std::vector<const char*> expected;  // Names of files 0, 1, ...; null for no name.
  };
  std::vector<Case> cases;

  {
    Case& c = cases.emplace_back(Case{"no path in the directory format", {}, {}});
    c.tables.push_back(1);
    AppendUleb(c.tables, DW_LNCT_directory_index);
    AppendUleb(c.tables, DwarfForm::kUdata);
  }
  {
    Case& c = cases.emplace_back(Case{"no path in the file format", {}, {}});
    dirs(c.tables);
    c.tables.push_back(1);
    AppendUleb(c.tables, DW_LNCT_directory_index);
    AppendUleb(c.tables, DwarfForm::kUdata);
    AppendUleb(c.tables, 1);
    AppendUleb(c.tables, 1);
  }
  {
    // An unknown form ends the table.
    Case& c = cases.emplace_back(Case{"unknown form in the second entry", {}, {"d/a.cc"}});
    dirs(c.tables);
    c.tables.push_back(2);
    AppendUleb(c.tables, DW_LNCT_path);
    AppendUleb(c.tables, DwarfForm::kIndirect);
    AppendUleb(c.tables, DW_LNCT_directory_index);
    AppendUleb(c.tables, DwarfForm::kIndirect);
    AppendUleb(c.tables, 2);
    // DW_FORM_indirect gives the form of each value, including a second indirection.
    AppendUleb(c.tables, DwarfForm::kIndirect);
    AppendUleb(c.tables, DwarfForm::kString);
    cstr(c.tables, "a.cc");
    AppendUleb(c.tables, DwarfForm::kUdata);
    AppendUleb(c.tables, 1);
    AppendUleb(c.tables, 0x7e);  // Unknown form.
  }
  {
    // A form above the range of int is unknown. Its low 32 bits are DW_FORM_udata.
    Case& c = cases.emplace_back(Case{"form above the range of int", {}, {"d/a.cc"}});
    dirs(c.tables);
    c.tables.push_back(2);
    AppendUleb(c.tables, DW_LNCT_path);
    AppendUleb(c.tables, DwarfForm::kString);
    AppendUleb(c.tables, DW_LNCT_directory_index);
    AppendUleb(c.tables, DwarfForm::kIndirect);
    AppendUleb(c.tables, 2);
    cstr(c.tables, "a.cc");
    AppendUleb(c.tables, DwarfForm::kUdata);
    AppendUleb(c.tables, 1);
    cstr(c.tables, "b.cc");
    AppendUleb(c.tables, (uint64_t{1} << 32) | static_cast<uint64_t>(DwarfForm::kUdata));
    AppendUleb(c.tables, 1);
  }
  {
    Case& c = cases.emplace_back(Case{"MD5 that isn't 16 bytes", {}, {"d/a.cc"}});
    dirs(c.tables);
    c.tables.push_back(3);
    AppendUleb(c.tables, DW_LNCT_path);
    AppendUleb(c.tables, DwarfForm::kString);
    AppendUleb(c.tables, DW_LNCT_directory_index);
    AppendUleb(c.tables, DwarfForm::kData1);
    AppendUleb(c.tables, DW_LNCT_MD5);
    AppendUleb(c.tables, DwarfForm::kBlock1);
    AppendUleb(c.tables, 2);
    cstr(c.tables, "a.cc");
    c.tables.push_back(1);
    c.tables.push_back(16);
    c.tables.insert(c.tables.end(), 16, 0xab);
    cstr(c.tables, "b.cc");
    c.tables.push_back(1);
    c.tables.push_back(8);
    c.tables.insert(c.tables.end(), 8, 0xab);
  }
  {
    Case& c =
        cases.emplace_back(Case{"other content types and forms", {}, {"d/a.cc", nullptr, "b.cc"}});
    dirs(c.tables);
    c.tables.push_back(6);
    AppendUleb(c.tables, DW_LNCT_timestamp);
    AppendUleb(c.tables, DwarfForm::kData4);
    AppendUleb(c.tables, DW_LNCT_path);
    AppendUleb(c.tables, DwarfForm::kString);
    AppendUleb(c.tables, DW_LNCT_size);
    AppendUleb(c.tables, DwarfForm::kUdata);
    AppendUleb(c.tables, DW_LNCT_LLVM_source);
    AppendUleb(c.tables, DwarfForm::kString);
    AppendUleb(c.tables, 0x3000);  // An unknown vendor content type.
    AppendUleb(c.tables, DwarfForm::kBlock);
    AppendUleb(c.tables, DW_LNCT_directory_index);
    AppendUleb(c.tables, DwarfForm::kData8);
    AppendUleb(c.tables, 3);
    auto entry = [&](const char* name, uint64_t dir) {
      c.tables.insert(c.tables.end(), 4, 0);  // Timestamp.
      cstr(c.tables, name);
      c.tables.push_back(0x7f);  // Size 127.
      cstr(c.tables, "int main() {}");
      AppendUleb(c.tables, 2);
      c.tables.insert(c.tables.end(), {0xaa, 0xbb});
      for (int i = 0; i < 8; i++)
        c.tables.push_back(static_cast<uint8_t>(dir >> (8 * i)));
    };
    entry("a.cc", 1);
    entry("", 0);       // An empty name in the compilation directory.
    entry("b.cc", 99);  // An invalid directory index.
  }
  {
    Case& c =
        cases.emplace_back(Case{"names that can't be decoded", {}, {nullptr, nullptr, "d/c.cc"}});
    dirs(c.tables);
    c.tables.push_back(3);
    AppendUleb(c.tables, DW_LNCT_path);
    AppendUleb(c.tables, DwarfForm::kIndirect);
    AppendUleb(c.tables, DW_LNCT_directory_index);
    AppendUleb(c.tables, DwarfForm::kImplicitConst);  // Reads nothing and gives 0.
    AppendUleb(c.tables, 0x2001 + 1);                 // Ignored content type.
    AppendUleb(c.tables, DwarfForm::kUdata);
    AppendUleb(c.tables, 3);
    AppendUleb(c.tables, DwarfForm::kStrpSup);  // A supplementary file string.
    c.tables.insert(c.tables.end(), {0, 0, 0, 0});
    AppendUleb(c.tables, 0);
    AppendUleb(c.tables, DwarfForm::kLineStrp);  // An offset past the end of .debug_line_str.
    c.tables.insert(c.tables.end(), {0xff, 0xff, 0, 0});
    AppendUleb(c.tables, 0);
    AppendUleb(c.tables, DwarfForm::kString);
    cstr(c.tables, "d/c.cc");
    AppendUleb(c.tables, 0);
  }

  // A file "a.cc" whose directory index has the form. Directory 1 is "d".
  auto dir_index_case = [&](const char* name, DwarfForm form, std::vector<uint8_t> value,
                            const char* expected) {
    Case& c = cases.emplace_back(Case{name, {}, {expected}});
    dirs(c.tables);
    c.tables.push_back(2);
    AppendUleb(c.tables, DW_LNCT_path);
    AppendUleb(c.tables, DwarfForm::kString);
    AppendUleb(c.tables, DW_LNCT_directory_index);
    AppendUleb(c.tables, form);
    AppendUleb(c.tables, 1);
    cstr(c.tables, "a.cc");
    c.tables.insert(c.tables.end(), value.begin(), value.end());
  };
  // The flag forms give their value, and DW_FORM_flag_present gives 1. DW_FORM_data16 gives its
  // size, 16, which isn't a valid directory.
  dir_index_case("directory index DW_FORM_flag", DwarfForm::kFlag, {1}, "d/a.cc");
  dir_index_case("directory index DW_FORM_flag_present", DwarfForm::kFlagPresent, {}, "d/a.cc");
  dir_index_case("directory index DW_FORM_data16", DwarfForm::kData16, std::vector<uint8_t>(16, 1),
                 "a.cc");
  // These forms aren't unsigned constants, so they give 0.
  dir_index_case("directory index DW_FORM_ref1", DwarfForm::kRef1, {1}, "a.cc");
  dir_index_case("directory index DW_FORM_sdata", DwarfForm::kSdata, {1}, "a.cc");

  {
    // A LEB128 value that doesn't fit in 64 bits ends the table.
    Case& c = cases.emplace_back(Case{"DW_FORM_strx index above 64 bits", {}, {nullptr}});
    dirs(c.tables);
    c.tables.push_back(1);
    AppendUleb(c.tables, DW_LNCT_path);
    AppendUleb(c.tables, DwarfForm::kStrx);
    AppendUleb(c.tables, 2);
    AppendUleb(c.tables, 0);  // No string offsets table here, so no name.
    c.tables.insert(c.tables.end(), 9, 0x80);
    c.tables.push_back(0x02);  // 2^64.
  }
  {
    Case& c = cases.emplace_back(Case{"DW_FORM_sdata INT64_MAX + 1", {}, {"a.cc"}});
    dirs(c.tables);
    c.tables.push_back(2);
    AppendUleb(c.tables, DW_LNCT_path);
    AppendUleb(c.tables, DwarfForm::kString);
    AppendUleb(c.tables, 0x3000);  // An ignored vendor field.
    AppendUleb(c.tables, DwarfForm::kSdata);
    AppendUleb(c.tables, 2);
    cstr(c.tables, "a.cc");
    c.tables.push_back(1);
    cstr(c.tables, "b.cc");
    c.tables.insert(c.tables.end(), 9, 0x80);
    c.tables.push_back(0x01);  // 2^63 doesn't fit in int64_t.
  }

  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    Builder builder(5);
    builder.set_raw_tables(c.tables);
    builder.SetAddress(0x1000);
    builder.Copy();
    builder.EndSequence();

    Decoded decoded = BuildAndCheck(builder);
    ExpectFileNames(decoded.names, c.expected);
  }
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

TEST(LineProgram, MaxOpsPerInstAboveOneFails) {
  // zxdb doesn't support line tables with more than one operation per instruction (VLIW).
  for (uint8_t max_ops : {2, 4, 255}) {
    SCOPED_TRACE(static_cast<int>(max_ops));
    Builder builder = MakeBuilder(4);
    builder.set_max_ops_per_inst(max_ops);
    builder.SetAddress(0x1000);
    builder.Copy();
    builder.EndSequence();

    LineTableTestSections sections;
    uint64_t offset = sections.Add(builder);
    sections.Finish();
    EXPECT_TRUE(sections.Parse(offset).has_error());
  }
}

TEST(LineProgram, MaxOpsPerInstZero) {
  // 0 is treated as 1.
  Builder builder = MakeBuilder(4);
  builder.set_max_ops_per_inst(0);
  builder.SetAddress(0x1000);
  builder.Special(3, 1);
  builder.EndSequence();

  Decoded decoded = BuildAndCheck(builder);
  std::vector<LineRow> expected = {
      LineRow(0x1003, 1, 2, 0, kStmt),
      LineRow(0x1003, 1, 2, 0, kStmt | kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);
}

TEST(LineProgram, DefineFile) {
  // DW_LNE_define_file adds a file after the header's files. The decoder reports it.
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
  const LineProgram& parsed = program.value();
  EXPECT_EQ(2u, parsed.header_files().size());

  auto decoded_rows = parsed.DecodeRows();
  EXPECT_EQ(LineProgramStatus::kOk, decoded_rows.status);
  ASSERT_EQ(1u, decoded_rows.defined_files.size());
  EXPECT_EQ("defined.h", decoded_rows.defined_files[0].name);
  EXPECT_EQ(1u, decoded_rows.defined_files[0].dir_index);

  auto scan = parsed.ScanFiles(kFirstCodeAddress);
  EXPECT_EQ(LineProgramStatus::kOk, scan.status);
  EXPECT_EQ((std::vector<uint32_t>{2}), scan.file_indices);
  EXPECT_EQ(3u, parsed.files(scan.defined_files).size());
  LineFileTable table = LineFileTable::FromEntries(parsed.files(scan.defined_files));
  ASSERT_TRUE(table.GetFileName(2));
  EXPECT_EQ("inc/defined.h", *table.GetFileName(2));

  // Decoding and scanning leave the header unchanged, and each result owns its definitions.
  auto second_decode = parsed.DecodeRows();
  EXPECT_EQ(LineProgramStatus::kOk, second_decode.status);
  EXPECT_EQ(decoded_rows.rows, second_decode.rows);
  EXPECT_EQ(2u, parsed.header_files().size());
  EXPECT_EQ(3u, parsed.files(second_decode.defined_files).size());
  EXPECT_EQ("defined.h", decoded_rows.defined_files[0].name);

  Decoded decoded = DecodeAndCheck(sections, offset);
  ASSERT_EQ(2u, decoded.rows.size());
  ExpectFileNames(decoded.names, {nullptr, "a.cc", "inc/defined.h"});
}

TEST(LineProgram, DefineFileEdgeCases) {
  // A file defined in a table without header files, and an extended opcode length that is shorter
  // than the operands. The decoder reads the operands, which can go to the end of the unit, and
  // then continues at the opcode length.
  Builder builder(4);
  builder.SetAddress(0x1000);
  builder.DefineFile("first.h", 0);  // File 1.
  builder.Extended(2, {3, 'x'});     // Length 2, but the name continues.
  builder.Raw({0, 0, 0, 0});
  builder.SetFile(1);
  builder.Copy();
  builder.EndSequence();

  // The second define_file reads the name "x", then 0 for the directory, time, and size. Decoding
  // continues after the 'x', where two zero-length extended opcodes do nothing.
  Decoded decoded = BuildAndCheck(builder);
  ExpectFileNames(decoded.names, {nullptr, "first.h", "x"});
  std::vector<LineRow> expected = {
      LineRow(0x1000, 1, 1, 0, kStmt),
      LineRow(0x1000, 1, 1, 0, kStmt | kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);
}

// DWARF 5 entry formats whose forms read no data, like DW_FORM_implicit_const. Every entry is the
// same and has no name. The parser makes every entry, so the files that DW_LNE_define_file adds
// keep their indices. It fails when the count is larger than the header size, so a 40-byte table
// can't make 2^40 entries.
TEST(LineProgram, EmptyEntries) {
  using namespace llvm::dwarf;
  const std::vector<uint8_t> dirs = {1, DW_LNCT_path, FormByte(DwarfForm::kString), 1, '/', 'b', 0};
  auto empty_format = [](std::vector<uint8_t>& out, uint64_t count) {
    out.insert(out.end(), {1, DW_LNCT_path, FormByte(DwarfForm::kImplicitConst)});
    AppendUleb(out, count);
  };

  for (uint64_t count : {0, 1, 2}) {
    SCOPED_TRACE(count);
    // Empty files retain their slots before a file defined by the program.
    std::vector<uint8_t> tables = dirs;
    empty_format(tables, count);
    Builder builder(5);
    builder.set_raw_tables(tables);
    builder.DefineFile("defined.cc", 0);
    builder.SetAddress(0x1000);
    builder.SetFile(count);
    builder.Copy();
    builder.AdvancePc(4);
    builder.EndSequence();

    LineTableTestSections sections;
    uint64_t offset = sections.Add(builder);
    sections.Finish();
    Decoded decoded = DecodeAndCheck(sections, offset);
    EXPECT_EQ((std::vector<uint32_t>{static_cast<uint32_t>(count)}), decoded.files);
    EXPECT_EQ(count + 1, decoded.file_count);
    std::vector<const char*> names(count, nullptr);
    names.push_back("defined.cc");
    ExpectFileNames(decoded.names, names);
  }
  for (uint64_t count : {0, 1, 3}) {
    SCOPED_TRACE(count);
    // The file table after the empty directories is still read.
    std::vector<uint8_t> tables;
    empty_format(tables, count);
    tables.insert(tables.end(),
                  {2, DW_LNCT_path, FormByte(DwarfForm::kString), DW_LNCT_directory_index,
                   FormByte(DwarfForm::kUdata), 1, 'a', '.', 'c', 'c', 0, 2});
    Builder builder(5);
    builder.set_raw_tables(tables);
    LineTableTestSections sections;
    uint64_t offset = sections.Add(builder);
    sections.Finish();
    ErrOr<LineProgram> program = sections.Parse(offset);
    ASSERT_TRUE(program.ok());
    EXPECT_EQ(count, program.value().header().include_dirs.size());
    Decoded decoded = DecodeAndCheck(sections, offset);
    ExpectFileNames(decoded.names, {"a.cc"});  // Directory 2 doesn't exist or has no name.
  }
  {
    // A zero-byte entry can have a nonzero directory index. Bulk filling must copy that value.
    std::vector<uint8_t> tables = dirs;
    tables.insert(tables.end(), {2, DW_LNCT_path, FormByte(DwarfForm::kImplicitConst),
                                 DW_LNCT_directory_index, FormByte(DwarfForm::kFlagPresent), 3});
    Builder builder(5);
    builder.set_raw_tables(tables);
    LineTableTestSections sections;
    uint64_t offset = sections.Add(builder);
    sections.Finish();
    Decoded decoded = DecodeAndCheck(sections, offset);
    ExpectFileNames(decoded.names, {nullptr, nullptr, nullptr});
    ErrOr<LineProgram> program = sections.Parse(offset);
    ASSERT_TRUE(program.ok());
    ASSERT_EQ(3u, program.value().header().files.size());
    for (const LineFileEntry& file : program.value().header().files) {
      EXPECT_EQ(std::nullopt, file.name);
      EXPECT_EQ(1u, file.dir_index);
    }
  }

  // Counts larger than the header fail the parse.
  for (bool files : {false, true}) {
    SCOPED_TRACE(files);
    std::vector<uint8_t> tables;
    if (files)
      tables = dirs;
    empty_format(tables, uint64_t{1} << 40);
    Builder builder(5);
    builder.set_raw_tables(tables);
    EXPECT_TRUE(ParseFails(builder));
  }
}

// The file scan counts the same files as the rows of a full decode, which the completed file table
// resolves. DecodeAndCheck() compares the scan with the rows.
TEST(LineProgram, DefineFileScan) {
  struct Case {
    const char* name;
    bool later_rows;  // Rows for file 2 after it is defined.
    bool stripped;
    std::vector<uint32_t> files;
  };
  for (const Case& c : {Case{"only a reference before the definition", false, false, {2}},
                        Case{"references before and after the definition", true, false, {2}},
                        Case{"a stripped sequence", true, true, {}}}) {
    SCOPED_TRACE(c.name);
    Builder builder(4);
    builder.AddFile("a.cc", 0);  // File 1.
    builder.SetAddress(c.stripped ? 0 : 0x1000);
    builder.SetFile(2);
    builder.Copy();
    builder.DefineFile("defined.h", 0);  // File 2.
    if (c.later_rows) {
      builder.AdvancePc(4);
      builder.Copy();
    }
    builder.AdvancePc(4);
    builder.EndSequence();
    EXPECT_EQ(c.files, BuildAndCheck(builder).files);
  }

  {
    // The definition is in a later sequence.
    Builder builder(4);
    builder.AddFile("a.cc", 0);  // File 1.
    builder.SetAddress(0x1000);
    builder.SetFile(2);
    builder.Copy();
    builder.AdvancePc(4);
    builder.EndSequence();
    builder.SetAddress(0x2000);
    builder.DefineFile("defined.h", 0);  // File 2.
    builder.Copy();
    builder.AdvancePc(4);
    builder.EndSequence();
    EXPECT_EQ((std::vector<uint32_t>{1, 2}), BuildAndCheck(builder).files);
  }

  // Two scans, and a decode before a scan, give the same files. File 3 isn't in the table.
  Builder builder(4);
  builder.AddFile("a.cc", 0);  // File 1.
  builder.SetAddress(0x1000);
  builder.DefineFile("defined.h", 0);  // File 2.
  builder.SetFile(2);
  builder.Copy();
  builder.SetFile(3);
  builder.Copy();
  builder.AdvancePc(4);
  builder.EndSequence();

  LineTableTestSections sections;
  uint64_t offset = sections.Add(builder);
  sections.Finish();
  const std::vector<uint32_t> expected = {2};

  ErrOr<LineProgram> program = sections.Parse(offset);
  ASSERT_TRUE(program.ok());
  for (int i = 0; i < 2; i++) {
    auto scan = program.value().ScanFiles(kFirstCodeAddress);
    EXPECT_EQ(LineProgramStatus::kOk, scan.status);
    EXPECT_EQ(expected, scan.file_indices);
    EXPECT_EQ(3u, program.value().files(scan.defined_files).size());
    EXPECT_EQ(2u, program.value().header_files().size());
  }

  auto rows = program.value().DecodeRows();
  EXPECT_EQ(LineProgramStatus::kOk, rows.status);
  auto scan = program.value().ScanFiles(kFirstCodeAddress);
  EXPECT_EQ(LineProgramStatus::kOk, scan.status);
  EXPECT_EQ(expected, scan.file_indices);
  EXPECT_EQ(3u, program.value().files(scan.defined_files).size());
  EXPECT_EQ(3u, program.value().files(rows.defined_files).size());
}

// The same programs in both byte orders.
class LineProgramEndianTest : public ::testing::TestWithParam<std::tuple<uint16_t, bool>> {};

TEST_P(LineProgramEndianTest, Decode) {
  auto [version, big_endian] = GetParam();
  Builder builder(version);
  builder.set_big_endian(big_endian);
  builder.set_dir_index_form(Builder::DirIndexForm::kData2);
  uint64_t dir = builder.AddDirectory(version >= 5 ? "/build" : "src");
  if (version >= 5)
    dir = builder.AddDirectory("src");
  uint64_t file = builder.AddFile("a.cc", dir);
  builder.SetAddress(0x123456789a);
  builder.SetFile(file);
  builder.Copy();
  builder.FixedAdvancePc(0x1234);
  builder.Copy();
  builder.SetAddress(0x89abcdef, 4);
  builder.Copy();
  builder.SetAddress(0xffffffff, 4);  // A 4-byte tombstone.
  builder.Copy();
  builder.SetAddress(0x89abcdf0, 4);
  builder.Copy();
  builder.EndSequence();

  LineTableTestSections sections;
  uint64_t offset = sections.Add(builder);
  sections.Finish(8, !big_endian);

  Decoded decoded = DecodeAndCheck(sections, offset);
  std::vector<LineRow> expected = {
      LineRow(0x123456789a, file, 1, 0, kStmt),
      LineRow(0x123456789a + 0x1234, file, 1, 0, kStmt),
      LineRow(0x89abcdef, file, 1, 0, kStmt),
      LineRow(0x89abcdf0, file, 1, 0, kStmt),
      LineRow(0x89abcdf0, file, 1, 0, kStmt | kEnd),
  };
  EXPECT_EQ(expected, decoded.rows);

  ErrOr<LineProgram> program = sections.Parse(offset);
  ASSERT_TRUE(program.ok());
  LineFileTable table = LineFileTable::FromEntries(program.value().header_files());
  EXPECT_EQ("src/a.cc", *table.GetFileName(file));
}

INSTANTIATE_TEST_SUITE_P(LineProgram, LineProgramEndianTest,
                         ::testing::Combine(::testing::Values(2, 4, 5), ::testing::Bool()));

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

    LineFileTable table = LineFileTable::FromEntries(program.value().header_files());
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
    : public ::testing::TestWithParam<
          std::tuple<Builder::StringForm, Builder::DirIndexForm, bool, Builder::Format>> {};

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
  LineFileTable table = LineFileTable::FromEntries(program.value().header_files());
  ASSERT_EQ(4u, table.size());
  EXPECT_EQ("main.cc", *table.GetFileName(0));
  EXPECT_EQ("../../src/lib.cc", *table.GetFileName(1));
  EXPECT_EQ("/usr/include/stdio.h", *table.GetFileName(2));
  EXPECT_EQ("gen/x.h", *table.GetFileName(3));
  EXPECT_FALSE(table.GetFileName(4));

  Decoded decoded = DecodeAndCheck(sections, offset);
  EXPECT_EQ((std::vector<uint32_t>{0, 3}), decoded.files);
}

INSTANTIATE_TEST_SUITE_P(LineProgram, LineProgramFileNameTest,
                         ::testing::Combine(::testing::Values(Builder::StringForm::kString,
                                                              Builder::StringForm::kLineStrp,
                                                              Builder::StringForm::kStrp),
                                            ::testing::Values(Builder::DirIndexForm::kUdata,
                                                              Builder::DirIndexForm::kData1,
                                                              Builder::DirIndexForm::kData2),
                                            ::testing::Bool(),
                                            ::testing::Values(Builder::Format::kDwarf32,
                                                              Builder::Format::kDwarf64)));

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
  LineFileTable table = LineFileTable::FromEntries(program.value().header_files());
  EXPECT_EQ("file299.cc", *table.GetFileName(299));
}

}  // namespace zxdb
