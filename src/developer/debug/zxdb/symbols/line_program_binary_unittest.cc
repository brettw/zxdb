// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include <filesystem>

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/Object/ObjectFile.h"
#include "src/developer/debug/zxdb/symbols/index.h"
#include "src/developer/debug/zxdb/symbols/line_file_table.h"
#include "src/developer/debug/zxdb/symbols/line_program.h"
#include "src/developer/debug/zxdb/symbols/line_program_test_util.h"
#include "src/developer/debug/zxdb/symbols/module_symbols_impl.h"
#include "src/developer/debug/zxdb/symbols/source_file_indexer.h"
#include "src/developer/debug/zxdb/symbols/test_symbol_module.h"

// These tests decode the line tables of real binaries and compare the result with LLVM. The build
// makes the line table fixtures with each available compiler and linker. See Tests.cmake.

namespace zxdb {

namespace {

void IgnoreError(llvm::Error err) { llvm::consumeError(std::move(err)); }

struct CompareStats {
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

void CompareUnit(llvm::DWARFContext& context, llvm::DWARFUnit& unit, CompareStats* stats) {
  if (!LineProgram::GetUnitLineTableOffset(unit))
    return;
  const llvm::DWARFDebugLine::LineTable* llvm_table = context.getLineTableForUnit(&unit);
  ASSERT_TRUE(llvm_table);
  std::vector<LineRow> llvm_rows = LineRowsFromLLVM(*llvm_table);

  ErrOr<LineProgram> program = LineProgram::ParseForUnit(unit);
  ASSERT_TRUE(program.ok()) << program.err().msg();
  std::vector<LineRow> rows;
  EXPECT_EQ(LineProgramStatus::kOk, program.value().DecodeRows(&rows));
  EXPECT_EQ(llvm_rows, rows);

  // The file scan uses a separate parse without the unit, like the indexer.
  ErrOr<LineProgram> scan_program = LineProgram::Parse(context, unit.getLineSection(),
                                                       *LineProgram::GetUnitLineTableOffset(unit),
                                                       unit.getAddressByteSize(), nullptr);
  ASSERT_TRUE(scan_program.ok());
  std::vector<uint32_t> files;
  EXPECT_EQ(LineProgramStatus::kOk, scan_program.value().ScanFiles(&files));
  size_t file_count = GetLineFileCount(llvm_table->Prologue);
  std::vector<uint32_t> live_files = GetLiveRowFiles(llvm_rows, file_count);
  EXPECT_EQ(live_files, files);

  // The file names match LLVM's prologue.
  LineFileTable ours = LineFileTable::FromPrologue(program.value().prologue());
  LineFileTable theirs = LineFileTable::FromPrologue(llvm_table->Prologue);
  ASSERT_EQ(theirs.size(), ours.size());
  for (size_t i = 0; i < ours.size(); i++) {
    const std::string* a = ours.GetFileName(i);
    const std::string* b = theirs.GetFileName(i);
    ASSERT_EQ(!!a, !!b);
    if (a)
      EXPECT_EQ(*b, *a);
  }

  stats->tables++;
  stats->rows += rows.size();
  size_t sequence_begin = 0;
  for (size_t i = 0; i < rows.size(); i++) {
    if (rows[i].end_sequence()) {
      if (IsStrippedSequenceStart(rows[sequence_begin].address))
        stats->stripped_sequences++;
      sequence_begin = i + 1;
    }
  }
  stats->stripped_only_files += GetAllRowFiles(llvm_rows, file_count).size() - live_files.size();
}

// Checks that SourceFileIndexer, which resolves names on worker threads, gives the files that LLVM
// gives for the live rows of each unit.
void CompareSourceFileIndex(llvm::DWARFContext& context) {
  std::vector<LineTableJob> jobs;
  SourceFileIndexer::FileIndex expected;
  auto add_units = [&](bool is_dwo, auto units) {
    uint32_t index = 0;
    for (const auto& unit : units) {
      UnitIndex unit_index(is_dwo, index++);
      AddLineTableJob(unit_index, *unit, &jobs);
      const llvm::DWARFDebugLine::LineTable* llvm_table = context.getLineTableForUnit(unit.get());
      if (!llvm_table)
        continue;
      LineFileTable names = LineFileTable::FromPrologue(llvm_table->Prologue);
      for (uint32_t file :
           GetLiveRowFiles(LineRowsFromLLVM(*llvm_table), GetLineFileCount(llvm_table->Prologue))) {
        if (const std::string* name = names.GetFileName(file)) {
          std::vector<UnitIndex>& file_units = expected[*name];
          if (file_units.empty() || file_units.back() != unit_index)
            file_units.push_back(unit_index);
        }
      }
    }
  };
  add_units(false, context.normal_units());
  add_units(true, context.dwo_units());

  for (size_t threads : {0, 2}) {
    SCOPED_TRACE(threads);
    SourceFileIndexer indexer(context, jobs, {.thread_count = threads});
    SourceFileIndexer::FileIndex files;
    indexer.FinishInto(&files);
    EXPECT_EQ(expected, files);
  }
}

CompareStats CompareBinary(const std::string& path) {
  CompareStats stats;
  SCOPED_TRACE(path);
  auto binary = llvm::object::ObjectFile::createObjectFile(path);
  if (!binary) {
    ADD_FAILURE() << "Can't load " << path;
    llvm::consumeError(binary.takeError());
    return stats;
  }
  std::unique_ptr<llvm::DWARFContext> context = llvm::DWARFContext::create(
      *binary->getBinary(), llvm::DWARFContext::ProcessDebugRelocations::Process, nullptr, "",
      IgnoreError, IgnoreError);

  for (const auto& unit : context->normal_units())
    CompareUnit(*context, *unit, &stats);
  for (const auto& unit : context->dwo_units())
    CompareUnit(*context, *unit, &stats);
  CompareSourceFileIndex(*context);
  return stats;
}

std::string FixturePath(const char* name) {
  return TestSymbolModule::GetTestDataDir() + "line_table/" + name + ".so";
}

}  // namespace

TEST(LineProgramBinary, MatchesLLVM) {
  std::string data_dir = TestSymbolModule::GetTestDataDir();
  std::vector<std::string> binaries = {
      TestSymbolModule::GetTestFileName(),
      TestSymbolModule::GetCheckedInTestFileName(),
      data_dir + "fission/fission",
      data_dir + "fission/main.dwo",
      data_dir + "fission/other.dwo",
  };
  // Builds other than CMake don't make the fixtures. See RequiredFixtures.
  if (std::filesystem::is_directory(data_dir + "line_table")) {
    for (const auto& entry : std::filesystem::directory_iterator(data_dir + "line_table"))
      binaries.push_back(entry.path().string());
  }
  std::sort(binaries.begin(), binaries.end());

  size_t tables = 0;
  for (const std::string& path : binaries)
    tables += CompareBinary(path).tables;
  EXPECT_GT(tables, 5u);
}

#ifdef ZXDB_LINE_TABLE_FIXTURES
TEST(LineProgramBinary, RequiredFixtures) {
  // The compiler that builds zxdb makes these fixtures, so they always exist in the CMake build.
  for (const char* name : {"clang_dwarf4", "clang_dwarf5_gc", "clang_dwarf64"}) {
    SCOPED_TRACE(name);
    EXPECT_TRUE(std::filesystem::exists(FixturePath(name)));
  }
}
#endif

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
    CompareStats stats = CompareBinary(path);
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

#ifdef ZXDB_INDEX_BENCHMARK_BINARY
// The benchmark binary is large, so this runs only when the build sets it.
TEST(LineProgramBinary, BenchmarkBinaryMatchesLLVM) {
  CompareStats stats = CompareBinary(ZXDB_INDEX_BENCHMARK_BINARY);
  EXPECT_GT(stats.tables, 0u);
}
#endif

}  // namespace zxdb
