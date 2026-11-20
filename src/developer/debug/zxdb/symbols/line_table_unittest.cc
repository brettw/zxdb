// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/line_table.h"

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/line_details.h"
#include "src/developer/debug/zxdb/symbols/line_table_test_support.h"
#include "src/developer/debug/zxdb/symbols/symbol_context.h"


namespace zxdb {

TEST(LineTable, GetRowForAddress) {
  // This load address makes the addresses in the table start at 0x1000.
  SymbolContext context(0x1000);

  std::vector<std::string> files;
  files.push_back("file.cc");  // Name for file #1.

  LineTable empty_table = MakeLineTable(files, {});
  auto result = empty_table.GetRowForAddress(context, 0x1000);
  EXPECT_TRUE(result.empty());  // Nothing is in the table.

  // This contains only odd addresses so even ones should be covered by the previous row.
  std::vector<LineRow> rows;
  rows.push_back(MakeStatementRow(0x1, 1, 2));
  rows.push_back(MakeStatementRow(0x3, 1, 1));  // Dupe for File 1, line 1.
  rows.push_back(MakeStatementRow(0x5, 1, 100));
  rows.push_back(MakeStatementRow(0x5, 1, 100));  // Address dupe.
  // No end sequence marker.

  LineTable table = MakeLineTable(files, rows);

  // Without an end sequence marker, the rows won't be found.
  result = table.GetRowForAddress(context, 0x1001);
  EXPECT_TRUE(result.empty());  // Nothing is in the table.

  // Make one with an end sequence marker, it should become valid.
  rows.push_back(MakeEndSequenceRow(0x7, 1, 100));  // End marker, not a real addr.
  table = MakeLineTable(files, rows);

  result = table.GetRowForAddress(context, 0x1000);
  EXPECT_TRUE(result.empty());  // Should be not found since it's before the table.

  // All queries in this test should result in the "symbolizable" and "statement" indices being
  // equivalent.
  result = table.GetRowForAddress(context, 0x1001);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(5u, result.sequence.size());
  EXPECT_EQ(0u, result.symbolizable_index);
  EXPECT_EQ(result.symbolizable_index, result.statement_index);

  // Should be covered by the previous entry since there isn't a specific one for this address.
  result = table.GetRowForAddress(context, 0x1002);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(5u, result.sequence.size());
  EXPECT_EQ(0u, result.symbolizable_index);
  EXPECT_EQ(result.symbolizable_index, result.statement_index);

  // Should find the first of the two dupes.
  result = table.GetRowForAddress(context, 0x1005);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(5u, result.sequence.size());
  EXPECT_EQ(2u, result.symbolizable_index);
  EXPECT_EQ(result.symbolizable_index, result.statement_index);

  // End of table.
  result = table.GetRowForAddress(context, 0x1007);
  ASSERT_TRUE(result.empty());

  // Add some rows corresponding to stripped code (sequence starting @ 0). Note that this overlaps
  // the previous valid sequence and this one should be ignored.
  rows.push_back(MakeStatementRow(0x0, 1, 100));
  rows.push_back(MakeStatementRow(0x0, 1, 101));
  rows.push_back(MakeEndSequenceRow(0x0, 1, 101));

  // Add some more valid rows. These ones are added to the beginning so the initial table is
  // out-of-order.
  std::vector<LineRow> new_rows;
  new_rows.push_back(MakeStatementRow(0x101, 1, 2));
  new_rows.push_back(MakeStatementRow(0x103, 1, 1));  // Dupe for File 1, line 1.
  new_rows.push_back(MakeStatementRow(0x105, 1, 100));
  new_rows.push_back(MakeEndSequenceRow(0x107, 1, 100));

  rows.insert(rows.begin(), new_rows.begin(), new_rows.end());
  table = MakeLineTable(files, rows);

  // Address 0 should still be not found (the sequence starting @ 0 should be considered stripped).
  result = table.GetRowForAddress(context, 0x1000);
  EXPECT_TRUE(result.empty());  // Should be not found since it's before the table.

  // Valid query from above.
  result = table.GetRowForAddress(context, 0x1001);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(0u, result.symbolizable_index);
  EXPECT_EQ(0x1u, result.sequence[result.symbolizable_index].address);
  EXPECT_EQ(result.symbolizable_index, result.statement_index);

  // Valid query for sequence added later.
  result = table.GetRowForAddress(context, 0x1104);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(1u, result.symbolizable_index);  // Index within newer sequence.
  EXPECT_EQ(
      0x103u,
      result.sequence[result.symbolizable_index].address);  // Line before queried addr.
  EXPECT_EQ(result.symbolizable_index, result.statement_index);

  // Query in between the two valid sequences
  result = table.GetRowForAddress(context, 0x1080);
  EXPECT_TRUE(result.empty());  // Should be not found.
}

// Tests handling for addresses matching "line 0" entries (indicating compiler-generated).
TEST(LineTable, GetRowForAddress_Line0) {
  // This load address makes the addresses in the table start at 0x1000.
  SymbolContext context(0x1000);

  std::vector<std::string> files;
  files.push_back("file.cc");  // Name for file #1.

  std::vector<LineRow> rows;
  rows.push_back(MakeStatementRow(0x1, 1, 2));
  rows.push_back(MakeStatementRow(0x3, 1, 0));  // <- Compiler-generated.
  rows.push_back(MakeStatementRow(0x5, 1, 3));
  rows.push_back(MakeStatementRow(0x7, 1, 0));  // <- Compiler-generated
  // The settings for the "end sequence" marker aren't really used. They're normally inherited from
  // the previous line because of the way the data is encoded in DWARF using a state machine. We
  // inherit the same file/line from the previous entry in this same way.
  rows.push_back(MakeEndSequenceRow(0x9, 1, 0));

  LineTable table = MakeLineTable(files, rows);

  // Exact match query for the "line 0" address should return it.
  auto result = table.GetRowForAddress(context, 0x1003, LineTable::kExactMatch);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(0x3u, result.get_statement().address);
  EXPECT_EQ(0u, result.get_statement().line);
  EXPECT_EQ(0x1u, result.get_symbolizable().address);
  EXPECT_EQ(2u, result.get_symbolizable().line);

  // Exact match query for the address immediately following 0x7 should also be covered by that
  // entry.
  result = table.GetRowForAddress(context, 0x1008, LineTable::kExactMatch);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(0x7u, result.get_statement().address);
  EXPECT_EQ(0u, result.get_statement().line);
  EXPECT_EQ(0x5u, result.get_symbolizable().address);
  EXPECT_EQ(3u, result.get_symbolizable().line);

  // Querying the first "line 0" entry with skipping should yield the next address.
  result = table.GetRowForAddress(context, 0x1003, LineTable::kSkipCompilerGenerated);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(0x5u, result.get_statement().address);
  EXPECT_EQ(3u, result.get_statement().line);
  EXPECT_EQ(0x1u, result.get_symbolizable().address);
  EXPECT_EQ(2u, result.get_symbolizable().line);

  // Query the address immediately following 0x7. Since this is the last real entry in the table
  // (the end sequence marker doesn't count), the line should not be advanced and the "line 0"
  // entry should be returned.
  result = table.GetRowForAddress(context, 0x1008, LineTable::kSkipCompilerGenerated);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(0x7u, result.get_statement().address);
  EXPECT_EQ(0u, result.get_statement().line);
  EXPECT_EQ(0x5u, result.get_symbolizable().address);
  EXPECT_EQ(3u, result.get_symbolizable().line);
}

TEST(LineTable, GetRowForAddressLine0Bounds) {
  // This load address makes the addresses in the table start at 0x1000.
  SymbolContext context(0x1000);

  std::vector<std::string> files;
  files.push_back("file.cc");  // Name for file #1.

  std::vector<LineRow> rows;
  rows.push_back(MakeStatementRow(0x1, 1, 0));  // <- Compiler-generated.
  rows.push_back(MakeStatementRow(0x3, 1, 2));
  rows.push_back(MakeStatementRow(0x5, 1, 0));  // <- Compiler-generated
  rows.push_back(MakeStatementRow(0x7, 1, 0));  // <- Compiler-generated
  // The settings for the "end sequence" marker aren't really used. They're normally inherited from
  // the previous line because of the way the data is encoded in DWARF using a state machine. We
  // inherit the same file/line from the previous entry in this same way.
  rows.push_back(MakeEndSequenceRow(0x9, 1, 0));

  LineTable table = MakeLineTable(files, rows);

  // Exact match query for the "line 0" address should return it. The symbolizable address can't
  // back up to find a better candidate so it just returns what we have. It's unclear if the
  // symbolizable address should also slide forward in this case to match the statement address.
  auto result = table.GetRowForAddress(context, 0x1001, LineTable::kExactMatch);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(0x1u, result.get_statement().address);
  EXPECT_EQ(0u, result.get_statement().line);
  EXPECT_EQ(0x1u, result.get_symbolizable().address);
  EXPECT_EQ(0u, result.get_symbolizable().line);

  // Exact match query for the address immediately following 0x7 should also be covered by that
  // entry, since we have nowhere to slide down to.
  result = table.GetRowForAddress(context, 0x1008, LineTable::kExactMatch);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(0x7u, result.get_statement().address);
  EXPECT_EQ(0u, result.get_statement().line);
  EXPECT_EQ(0x3u, result.get_symbolizable().address);
  EXPECT_EQ(2u, result.get_symbolizable().line);

  // Querying the first "line 0" entry with skipping should yield the next address for the statement
  // and the first address for the symbolizable location since we have nowhere to slide up to.
  result = table.GetRowForAddress(context, 0x1001, LineTable::kSkipCompilerGenerated);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(0x3u, result.get_statement().address);
  EXPECT_EQ(2u, result.get_statement().line);
  EXPECT_EQ(0x1u, result.get_symbolizable().address);
  EXPECT_EQ(0u, result.get_symbolizable().line);

  // Query the address immediately following 0x7. Since this is the last real entry in the table
  // (the end sequence marker doesn't count), the line should not be advanced and the "line 0"
  // entry should be returned. The symbolizable address will slide up to find a non-zero line.
  result = table.GetRowForAddress(context, 0x1008, LineTable::kSkipCompilerGenerated);
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(0x7u, result.get_statement().address);
  EXPECT_EQ(0u, result.get_statement().line);
  EXPECT_EQ(0x3u, result.get_symbolizable().address);
  EXPECT_EQ(2u, result.get_symbolizable().line);
}

// Tests that when an address matches two lines, one an EndSequence and one a good match, that the
// second one is returned. This construct can appear on function boundaries when there is no padding
// between them. The EndSequence marker is non-inclusive, it just marks the end of the previous
// function.
TEST(LineTable, GetRowForAddress_EndSequence) {
  SymbolContext context(0x1000);

  std::vector<std::string> files;
  files.push_back("file.cc");  // Name for file #1.

  std::vector<LineRow> rows;
  rows.push_back(MakeStatementRow(0x4, 1, 2));
  rows.push_back(MakeEndSequenceRow(0x8, 1, 2));
  rows.push_back(MakeStatementRow(0x8, 1, 3));  // <- Duplicate address.
  rows.push_back(MakeStatementRow(0xa, 1, 4));
  rows.push_back(MakeEndSequenceRow(0xa, 1, 4));

  LineTable table = MakeLineTable(files, rows);
  auto found = table.GetRowForAddress(context, 0x1008, LineTable::kSkipCompilerGenerated);
  ASSERT_FALSE(found.empty());
  EXPECT_EQ(0x8u, found.get_statement().address);
  EXPECT_EQ(3u, found.get_statement().line);  // This is the non-end-sequence row at this address.
}

TEST(LineTable, Sequences) {
  std::vector<LineRow> rows = {
      // A sequence after the next one in address order.
      MakeStatementRow(0x200, 1, 20),
      MakeEndSequenceRow(0x210, 1, 20),
      // Stripped sequences, with start address 0 and the all-ones tombstone.
      MakeStatementRow(0x0, 1, 1),
      MakeEndSequenceRow(0x8, 1, 1),
      MakeStatementRow(UINT64_MAX, 1, 1),
      MakeEndSequenceRow(UINT64_MAX, 1, 1),
      // Addresses that go backwards are corrupt.
      MakeStatementRow(0x500, 1, 1),
      MakeEndSequenceRow(0x400, 1, 1),
      MakeStatementRow(0x100, 1, 10),
      MakeStatementRow(0x104, 1, 11),
      MakeEndSequenceRow(0x110, 1, 11),
      // Rows after the last end_sequence aren't a complete sequence.
      MakeStatementRow(0x300, 1, 30),
  };
  LineTable table = MakeLineTable({"file.cc"}, rows);

  ASSERT_EQ(2u, table.GetNumSequences());
  EXPECT_EQ(AddressRange(0x100, 0x110), table.sequences()[0].addresses);
  EXPECT_EQ(AddressRange(0x200, 0x210), table.sequences()[1].addresses);

  // The rows are copied into address order.
  ASSERT_EQ(5u, table.rows().size());
  EXPECT_EQ(0x100u, table.rows()[0].address);
  EXPECT_EQ(0x200u, table.rows()[3].address);

  // GetSequenceAt() doesn't include the end_sequence row.
  ASSERT_EQ(2u, table.GetSequenceAt(0).size());
  EXPECT_EQ(0x104u, table.GetSequenceAt(0)[1].address);

  SymbolContext context = SymbolContext::ForRelativeAddresses();
  EXPECT_EQ(3u, table.GetRowSequenceForAddress(context, 0x105).size());
  EXPECT_TRUE(table.GetRowSequenceForAddress(context, 0x4).empty());
  EXPECT_TRUE(table.GetRowSequenceForAddress(context, 0x450).empty());
  EXPECT_TRUE(table.GetRowSequenceForAddress(context, 0x300).empty());
}

TEST(LineTable, SequencesInOrderKeepRows) {
  std::vector<LineRow> rows = {
      MakeStatementRow(0x100, 1, 10),
      MakeEndSequenceRow(0x110, 1, 10),
      MakeStatementRow(0x200, 1, 20),
      MakeEndSequenceRow(0x210, 1, 20),
  };
  LineTable table = MakeLineTable({"file.cc"}, rows);
  EXPECT_EQ(rows, table.rows());
}

TEST(LineTable, EmptySequenceAtSameAddress) {
  // An empty sequence with the same start address as a nonempty one doesn't hide it.
  LineTable table = MakeLineTable({"file.cc"}, {
                                                   MakeStatementRow(0x100, 1, 10),
                                                   MakeEndSequenceRow(0x100, 1, 10),
                                                   MakeStatementRow(0x100, 1, 20),
                                                   MakeEndSequenceRow(0x120, 1, 20),
                                               });
  ASSERT_EQ(2u, table.GetNumSequences());

  SymbolContext context = SymbolContext::ForRelativeAddresses();
  for (uint64_t address : {0x100, 0x110}) {
    LineTable::FoundRow found = table.GetRowForAddress(context, address);
    ASSERT_FALSE(found.empty());
    EXPECT_EQ(20u, found.get_statement().line);
  }
}

TEST(LineTable, EmptySequenceInsideLiveSequence) {
  // An empty sequence that starts inside a live sequence doesn't hide the live sequence. LLVM finds
  // the live sequence for this table too.
  SymbolContext context = SymbolContext::ForRelativeAddresses();
  LineTable table = MakeLineTable({"file.cc"}, {
                                                   MakeStatementRow(0x1000, 1, 10),
                                                   MakeStatementRow(0x1010, 1, 11),
                                                   MakeEndSequenceRow(0x1030, 1, 11),
                                                   MakeStatementRow(0x1010, 1, 20),
                                                   MakeEndSequenceRow(0x1010, 1, 20),
                                               });
  LineTable::FoundRow found = table.GetRowForAddress(context, 0x1018);
  ASSERT_FALSE(found.empty());
  EXPECT_EQ(11u, found.get_statement().line);

  LineDetails details = table.GetLineDetails(context, 0x1018, false, "/build");
  EXPECT_EQ(FileLine("file.cc", "/build", 11), details.file_line());
  EXPECT_EQ(AddressRange(0x1010, 0x1030), details.GetExtent());
}

TEST(LineTable, OverlappingSequences) {
  // Corrupt or unusual tables can have overlapping sequences. The lookup returns the containing
  // sequence with the latest start.
  SymbolContext context = SymbolContext::ForRelativeAddresses();
  LineTable table = MakeLineTable({"file.cc"}, {
                                                   MakeStatementRow(0x1000, 1, 1),
                                                   MakeEndSequenceRow(0x1050, 1, 1),
                                                   MakeStatementRow(0x1020, 1, 2),
                                                   MakeEndSequenceRow(0x1030, 1, 2),
                                                   MakeStatementRow(0x1040, 1, 3),
                                                   MakeEndSequenceRow(0x1048, 1, 3),
                                                   MakeStatementRow(0x2000, 1, 4),
                                                   MakeEndSequenceRow(0x2010, 1, 4),
                                               });
  struct Case {
    uint64_t address;
    uint32_t line;  // 0 means not found.
  };
  for (Case c : {Case{0x0fff, 0}, Case{0x1000, 1}, Case{0x1010, 1}, Case{0x1020, 2},
                 Case{0x1030, 1}, Case{0x1044, 3}, Case{0x1048, 1}, Case{0x1050, 0},
                 Case{0x2008, 4}, Case{0x2010, 0}}) {
    SCOPED_TRACE(c.address);
    LineTable::FoundRow found = table.GetRowForAddress(context, c.address);
    if (c.line == 0) {
      EXPECT_TRUE(found.empty());
    } else {
      ASSERT_FALSE(found.empty());
      EXPECT_EQ(c.line, found.get_statement().line);
    }
  }
}

TEST(LineTable, FileNames) {
  LineTable table = MakeLineTable({"a.cc", "b.h"}, {});
  EXPECT_EQ(3u, table.files().size());
  EXPECT_FALSE(table.GetFileNameForRow(MakeStatementRow(0, 0, 1)));
  EXPECT_EQ("a.cc", *table.GetFileNameForRow(MakeStatementRow(0, 1, 1)));
  EXPECT_EQ("b.h", *table.GetFileNameForRow(MakeStatementRow(0, 2, 1)));
  EXPECT_FALSE(table.GetFileNameForRow(MakeStatementRow(0, 3, 1)));

  // A default table and a table without files have an empty file table.
  EXPECT_EQ(0u, LineTable().files().size());
  EXPECT_EQ(0u, LineTable(nullptr, {}).files().size());
}

TEST(LineTable, GetLineDetails) {
  SymbolContext context(0x10000);
  LineTable table = MakeLineTable(
      {"file.cc", "other.cc"}, {
                                   MakeStatementRow(0x100, 1, 5),
                                   MakeStatementRow(0x104, 1, 5),  // Same line.
                                   MakeStatementRow(0x108, 1, 0),  // Compiler-generated.
                                   MakeStatementRow(0x10c, 1, 5),
                                   // Two rows at one address. The last one describes the code.
                                   MakeStatementRow(0x110, 2, 7),
                                   MakeStatementRow(0x110, 1, 8),
                                   MakeEndSequenceRow(0x118, 1, 8),
                                   // The next sequence has the same line but isn't joined.
                                   MakeStatementRow(0x118, 1, 8),
                                   MakeEndSequenceRow(0x120, 1, 8),
                               });

  LineDetails details = table.GetLineDetails(context, 0x10106, false, "/build");
  EXPECT_EQ(FileLine("file.cc", "/build", 5), details.file_line());
  ASSERT_EQ(2u, details.entries().size());
  EXPECT_EQ(AddressRange(0x10100, 0x10104), details.entries()[0].range);
  EXPECT_EQ(AddressRange(0x10104, 0x10108), details.entries()[1].range);

  // Greedy mode continues across the line 0 row.
  details = table.GetLineDetails(context, 0x10106, true, "/build");
  ASSERT_EQ(4u, details.entries().size());
  EXPECT_EQ(AddressRange(0x10100, 0x10110), details.GetExtent());

  // A line 0 row has no file.
  details = table.GetLineDetails(context, 0x10108, false, "/build");
  EXPECT_EQ(FileLine(), details.file_line());
  EXPECT_EQ(AddressRange(0x10108, 0x1010c), details.GetExtent());

  details = table.GetLineDetails(context, 0x10110, false, "/build");
  EXPECT_EQ(FileLine("file.cc", "/build", 8), details.file_line());
  EXPECT_EQ(AddressRange(0x10110, 0x10118), details.GetExtent());

  details = table.GetLineDetails(context, 0x1011a, false, "/build");
  EXPECT_EQ(AddressRange(0x10118, 0x10120), details.GetExtent());

  EXPECT_FALSE(table.GetLineDetails(context, 0x100fc, false, "/build").is_valid());
  EXPECT_FALSE(table.GetLineDetails(context, 0x10120, false, "/build").is_valid());
}

TEST(LineTable, GetLineDetailsMissingFile) {
  SymbolContext context = SymbolContext::ForRelativeAddresses();
  LineTable table = MakeLineTable({"file.cc"}, {
                                                   MakeStatementRow(0x100, 9, 5),
                                                   MakeEndSequenceRow(0x104, 9, 5),
                                               });
  LineDetails details = table.GetLineDetails(context, 0x100, false, "/build");
  EXPECT_EQ(FileLine("", "/build", 5), details.file_line());
}

}  // namespace zxdb
