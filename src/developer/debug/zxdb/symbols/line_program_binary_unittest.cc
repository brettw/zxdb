// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <filesystem>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/dwarf_binary_impl.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die.h"
#include "src/developer/debug/zxdb/symbols/index.h"
#include "src/developer/debug/zxdb/symbols/line_file_table.h"
#include "src/developer/debug/zxdb/symbols/line_program.h"
#include "src/developer/debug/zxdb/symbols/line_program_test_util.h"
#include "src/developer/debug/zxdb/symbols/line_sequence.h"
#include "src/developer/debug/zxdb/symbols/module_symbols_impl.h"
#include "src/developer/debug/zxdb/symbols/source_file_indexer.h"
#include "src/developer/debug/zxdb/symbols/test_symbol_module.h"

// These tests decode the line tables of real binaries. The build makes the line table fixtures with
// each available compiler and linker. See Tests.cmake. The checks don't depend on another parser:
// the rows must be well formed, the file scan and the indexer must agree with the decoded rows, and
// the fixtures must have rows for the lines of their source code.

namespace zxdb {

namespace {

struct CheckStats {
  size_t tables = 0;
  size_t rows = 0;
  size_t stripped_sequences = 0;

  // Files that rows of stripped sequences reference and no live sequence references.
  size_t stripped_only_files = 0;
};

// Collects the files of all rows, including the rows of stripped sequences.
std::vector<uint32_t> GetAllRowFiles(const std::vector<LineRow>& rows, size_t file_count) {
  std::vector<bool> seen(file_count);
  for (const LineRow& row : rows) {
    if (row.file < file_count)
      seen[row.file] = true;
  }
  std::vector<uint32_t> result;
  for (size_t i = 0; i < file_count; i++) {
    if (seen[i])
      result.push_back(static_cast<uint32_t>(i));
  }
  return result;
}

// The rows and the complete file table of a unit's line table.
struct DecodedUnit {
  std::vector<LineRow> rows;
  LineFileTable names;
};

std::optional<DecodedUnit> DecodeUnit(const LineProgramSource& source) {
  ErrOr<LineProgram> program = source.Parse();
  EXPECT_TRUE(program.ok()) << program.err().msg();
  if (program.has_error())
    return std::nullopt;
  DecodedUnit result;
  auto decoded = program.value().DecodeRows();
  EXPECT_EQ(LineProgramStatus::kOk, decoded.status);
  result.rows = std::move(decoded.rows);
  result.names = LineFileTable::FromEntries(program.value().files(decoded.defined_files));
  return result;
}

void CheckUnit(const LineProgramSource& source, uint64_t first_code_address, CheckStats* stats) {
  std::optional<DecodedUnit> decoded = DecodeUnit(source);
  if (!decoded)
    return;
  const std::vector<LineRow>& rows = decoded->rows;
  size_t file_count = decoded->names.size();

  // The program ends each sequence, and addresses don't decrease inside a sequence.
  if (!rows.empty())
    EXPECT_TRUE(rows.back().end_sequence());
  size_t sequence_begin = 0;
  for (size_t i = 0; i < rows.size(); i++) {
    if (i > sequence_begin)
      EXPECT_LE(rows[i - 1].address, rows[i].address) << "row " << i;
    if (rows[i].end_sequence()) {
      if (IsStrippedSequenceStart(rows[sequence_begin].address, first_code_address))
        stats->stripped_sequences++;
      sequence_begin = i + 1;
    }
  }

  // Each file that live rows reference has a name.
  std::vector<uint32_t> live_files = GetLiveRowFiles(rows, file_count, first_code_address);
  for (uint32_t file : live_files)
    EXPECT_TRUE(decoded->names.GetFileName(file)) << "file " << file;

  // The file scan uses a separate parse without the unit, like the indexer.
  ErrOr<LineProgram> scan_program = source.Parse();
  ASSERT_TRUE(scan_program.ok());
  auto scan = scan_program.value().ScanFiles(first_code_address);
  EXPECT_EQ(LineProgramStatus::kOk, scan.status);
  EXPECT_EQ(live_files, scan.file_indices);

  stats->tables++;
  stats->rows += rows.size();
  stats->stripped_only_files += GetAllRowFiles(rows, file_count).size() - live_files.size();
}

// Checks that SourceFileIndexer, which scans the tables and resolves names on worker threads, gives
// the files of the live rows that the decoder gives.
void CheckSourceFileIndex(DwarfBinary& binary) {
  auto jobs = CollectLineTableJobs(binary);
  auto first_code_address = binary.GetFirstCodeAddress();
  SourceFileIndexer::FileIndex expected;
  auto add_units = [&](bool is_dwo, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
      UnitIndex unit_index(is_dwo, i);
      auto source = binary.GetLineProgramSource(unit_index);
      if (!source)
        continue;
      auto decoded = DecodeUnit(*source);
      if (!decoded)
        continue;
      for (uint32_t file :
           GetLiveRowFiles(decoded->rows, decoded->names.size(), first_code_address)) {
        if (const std::string* name = decoded->names.GetFileName(file)) {
          std::vector<UnitIndex>& file_units = expected[*name];
          if (file_units.empty() || file_units.back() != unit_index)
            file_units.push_back(unit_index);
        }
      }
    }
  };
  add_units(false, binary.GetNormalUnitCount());
  add_units(true, binary.GetDWOUnitCount());

  for (size_t threads : {0, 2}) {
    SCOPED_TRACE(threads);
    SourceFileIndexer indexer(jobs, {.thread_count = threads});
    SourceFileIndexer::FileIndex files;
    indexer.FinishInto(&files);
    EXPECT_EQ(expected, files);
  }
}

CheckStats CheckBinary(const std::string& path) {
  CheckStats stats;
  SCOPED_TRACE(path);
  DwarfBinaryImpl binary(path, "", "");
  auto err = binary.Load(
      {}, path.ends_with(".dwo") ? DwarfSymbolFactory::kDWO : DwarfSymbolFactory::kMainBinary);
  if (err.has_error()) {
    ADD_FAILURE() << err.msg();
    return stats;
  }
  for (bool is_dwo : {false, true}) {
    uint32_t count = is_dwo ? binary.GetDWOUnitCount() : binary.GetNormalUnitCount();
    for (uint32_t i = 0; i < count; ++i) {
      if (auto source = binary.GetLineProgramSource(UnitIndex(is_dwo, i)))
        CheckUnit(*source, binary.GetFirstCodeAddress(), &stats);
    }
  }
  CheckSourceFileIndex(binary);
  return stats;
}

std::string FixturePath(const char* name) {
  return TestSymbolModule::GetTestDataDir() + "line_table/" + name + ".so";
}

}  // namespace

TEST(LineProgramBinary, DecodesTestBinaries) {
  std::string data_dir = TestSymbolModule::GetTestDataDir();
  std::vector<std::string> binaries = {
      TestSymbolModule::GetTestFileName(), TestSymbolModule::GetCheckedInTestFileName(),
      data_dir + "fission/fission",        data_dir + "fission/main.dwo",
      data_dir + "fission/other.dwo",
  };
  for (const auto& entry : std::filesystem::directory_iterator(data_dir + "line_table"))
    binaries.push_back(entry.path().string());
  std::sort(binaries.begin(), binaries.end());

  size_t tables = 0;
  for (const std::string& path : binaries)
    tables += CheckBinary(path).tables;
  EXPECT_GT(tables, 10u);
}

TEST(LineProgramBinary, DwoTypeUnitLineTable) {
  // The type unit in the .dwo file references its .debug_line.dwo table. DecodesTestBinaries also
  // covers this file. Check here that the table is really decoded.
  std::string path = TestSymbolModule::GetTestDataDir() + "line_table/clang_split.dwo";
  CheckStats stats = CheckBinary(path);
  EXPECT_GT(stats.tables, 0u);

  DwarfBinaryImpl binary(path, "", "");
  ASSERT_TRUE(binary.Load({}, DwarfSymbolFactory::kDWO).ok());
  bool found_type_unit = false;
  for (uint32_t i = 0; i < binary.GetDWOUnitCount(); ++i) {
    UnitIndex index(true, i);
    auto root = binary.GetUnitAtIndex(index)->GetUnitDie(DwarfDie::Load::kRootOnly);
    ASSERT_TRUE(root);
    if (!root.unit_header().is_type_unit())
      continue;
    auto source = binary.GetLineProgramSource(index);
    ASSERT_TRUE(source);
    auto program = source->Parse();
    ASSERT_TRUE(program.ok()) << program.err().msg();
    LineFileTable files = LineFileTable::FromEntries(program.value().header_files());
    ASSERT_GT(files.size(), 0u);
    ASSERT_TRUE(files.GetFileName(0));
    EXPECT_NE(std::string::npos, files.GetFileName(0)->find("line_table_split.cc"));
    found_type_unit = true;
  }
  EXPECT_TRUE(found_type_unit);
}

TEST(LineProgramBinary, RequiredFixtures) {
  // The compiler that builds zxdb makes these fixtures, so they always exist.
  for (const char* name : {"clang_dwarf4", "clang_dwarf5_gc", "clang_dwarf64"}) {
    SCOPED_TRACE(name);
    EXPECT_TRUE(std::filesystem::exists(FixturePath(name)));
  }
}

TEST(LineProgramBinary, StrippedCode) {
  // The --gc-sections builds remove LineTableFixtureDead(). The BFD linker sets its sequence to
  // start at address 0, so its rows exist but its file has no live rows. The LLD fixture uses the
  // all-ones tombstone, so the decoder doesn't emit its rows at all.
  struct Fixture {
    const char* name;
    bool zero_address;
  };
  for (Fixture fixture : {Fixture{"clang_dwarf5_gc", true}, Fixture{"gcc_dwarf4_gc", true},
                          Fixture{"clang_dwarf4_lld_tombstone", false}}) {
    SCOPED_TRACE(fixture.name);
    std::string path = FixturePath(fixture.name);
    if (!std::filesystem::exists(path))
      continue;  // GCC and LLD are optional.
    CheckStats stats = CheckBinary(path);
    if (fixture.zero_address) {
      EXPECT_GT(stats.stripped_sequences, 0u);
      EXPECT_GT(stats.stripped_only_files, 0u);
    }

    // The index has only the files that have live code.
    TestSymbolModule setup(path, "");
    ASSERT_TRUE(setup.Init().ok());
    const Index& index = setup.symbols()->GetIndex();
    EXPECT_FALSE(index.FindFileMatches("line_table_fixture.cc").empty());
    EXPECT_FALSE(index.FindFileMatches("line_table_fixture.h").empty());
    EXPECT_FALSE(index.FindFileMatches("line_table_fixture_virtual.inc").empty());
    EXPECT_TRUE(index.FindFileMatches("line_table_fixture_dead.cc").empty());
  }
}

TEST(LineProgramBinary, FixtureSourceLines) {
  // Each fixture has rows in live sequences for the code of each function, in the right file. The
  // ranges are the lines of the functions in the fixture sources, so they hold for each compiler
  // and optimization level. No live row has a line past the end of its file. The #line directives
  // in line_table_fixture.cc change the line numbers after them.
  struct Range {
    const char* file;  // The end of the file name.
    uint32_t first;
    uint32_t last;
  };
  const Range kFunctions[] = {
      {"line_table_fixture.cc", 13, 24},             // Loop().
      {"line_table_fixture.cc", 38, 44},             // LineTableFixtureEntry(), after #line 37.
      {"line_table_fixture.h", 10, 19},              // LineTableFixtureSum().
      {"line_table_fixture_virtual.inc", 500, 502},  // LineTableFixtureVirtual(), from #line.
  };
  const Range kFiles[] = {
      {"line_table_fixture.cc", 0, 44},
      {"line_table_fixture.h", 0, 24},
      {"line_table_fixture_virtual.inc", 0, 502},
  };
  // Matches a whole path component, so "a.h" doesn't match "aa.h".
  auto ends_with = [](const std::string& name, std::string_view suffix) {
    return name.ends_with(suffix) &&
           (name.size() == suffix.size() || name[name.size() - suffix.size() - 1] == '/');
  };

  size_t fixtures = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator(TestSymbolModule::GetTestDataDir() + "line_table")) {
    if (entry.path().extension() != ".so")
      continue;
    SCOPED_TRACE(entry.path().string());
    fixtures++;
    DwarfBinaryImpl binary(entry.path().string(), "", "");
    auto err = binary.Load({}, DwarfSymbolFactory::kMainBinary);
    ASSERT_TRUE(err.ok()) << err.msg();
    uint64_t first_code_address = binary.GetFirstCodeAddress();
    EXPECT_GT(first_code_address, 0u);

    std::vector<bool> found(std::size(kFunctions));
    for (uint32_t i = 0; i < binary.GetNormalUnitCount(); ++i) {
      auto source = binary.GetLineProgramSource(UnitIndex(false, i));
      if (!source)
        continue;
      auto decoded = DecodeUnit(*source);
      if (!decoded)
        continue;
      size_t sequence_begin = 0;
      for (size_t i = 0; i < decoded->rows.size(); i++) {
        const LineRow& row = decoded->rows[i];
        if (row.end_sequence()) {
          sequence_begin = i + 1;
          continue;
        }
        if (IsStrippedSequenceStart(decoded->rows[sequence_begin].address, first_code_address))
          continue;
        const std::string* name = decoded->names.GetFileName(row.file);
        ASSERT_TRUE(name) << "row " << i;
        for (const Range& file : kFiles) {
          if (ends_with(*name, file.file))
            EXPECT_LE(row.line, file.last) << *name;
        }
        for (size_t f = 0; f < std::size(kFunctions); f++) {
          if (ends_with(*name, kFunctions[f].file) && row.line >= kFunctions[f].first &&
              row.line <= kFunctions[f].last)
            found[f] = true;
        }
      }
    }
    for (size_t f = 0; f < std::size(kFunctions); f++) {
      EXPECT_TRUE(found[f]) << kFunctions[f].file << " lines " << kFunctions[f].first << " to "
                            << kFunctions[f].last;
    }
  }
  EXPECT_GE(fixtures, 3u);  // See RequiredFixtures.
}

#ifdef ZXDB_INDEX_BENCHMARK_BINARY
// The benchmark binary is large, so this runs only when the build sets it.
TEST(LineProgramBinary, BenchmarkBinary) {
  CheckStats stats = CheckBinary(ZXDB_INDEX_BENCHMARK_BINARY);
  EXPECT_GT(stats.tables, 0u);
}
#endif

}  // namespace zxdb
