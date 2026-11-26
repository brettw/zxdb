// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/source_file_indexer.h"

#include <thread>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/line_file_table.h"
#include "src/developer/debug/zxdb/symbols/line_program_test_util.h"

namespace zxdb {

namespace {

using Builder = LineTableBuilder;

// Makes a DWARF 5 table with one row for each given file name. All files are in "src".
Builder MakeTable(const std::vector<std::string>& files,
                  Builder::StringForm form = Builder::StringForm::kLineStrp) {
  Builder builder(5);
  builder.set_string_form(form);
  builder.AddDirectory("/build");
  uint64_t src = builder.AddDirectory("src");
  builder.SetAddress(0x1000);
  for (const std::string& file : files) {
    builder.SetFile(builder.AddFile(file, src));
    builder.Copy();
  }
  builder.AdvancePc(4);
  builder.EndSequence();
  return builder;
}

LineTableJob MakeJob(const LineTableTestSections& test_sections, uint32_t unit, uint64_t offset) {
  const DwarfSections sections = test_sections.sections();
  LineTableJob job;
  job.unit_index = UnitIndex(false, unit);
  job.section = AsStringView(sections.line);
  job.offset = offset;
  job.size = 100;
  job.strings = GetLineStringSections(sections);
  return job;
}

std::vector<uint32_t> UnitNumbers(const std::vector<UnitIndex>& units) {
  std::vector<uint32_t> result;
  for (const UnitIndex& unit : units)
    result.push_back(unit.index);
  return result;
}

}  // namespace

TEST(LineFilePathResolver, CachesMergedStrings) {
  // Both tables use the same .debug_line_str strings, like a linked binary.
  LineTableTestSections sections;
  uint64_t first = sections.Add(MakeTable({"a.cc", "b.h"}));
  uint64_t second = sections.Add(MakeTable({"b.h", "a.cc"}));
  sections.Finish();

  LineFilePathResolver resolver;
  ErrOr<LineProgram> program1 = sections.Parse(first);
  ErrOr<LineProgram> program2 = sections.Parse(second);
  ASSERT_TRUE(program1.ok() && program2.ok());

  const std::string* a1 = resolver.Resolve(program1.value().header_files(), 0);
  const std::string* b1 = resolver.Resolve(program1.value().header_files(), 1);
  const std::string* b2 = resolver.Resolve(program2.value().header_files(), 0);
  const std::string* a2 = resolver.Resolve(program2.value().header_files(), 1);
  ASSERT_TRUE(a1 && b1);
  EXPECT_EQ("src/a.cc", *a1);
  EXPECT_EQ("src/b.h", *b1);
  EXPECT_EQ(a1, a2);  // From the cache.
  EXPECT_EQ(b1, b2);
  EXPECT_EQ(2u, resolver.cache_size());

  // Entries outside the table have no name.
  EXPECT_FALSE(resolver.Resolve(program1.value().header_files(), 2));
}

TEST(LineFilePathResolver, MatchesLineFileTable) {
  // The resolver must give the same name as LineFileTable for every entry, including entries that
  // LLVM resolves in unusual ways. Without a unit, DW_FORM_strx1 strings can't be decoded, which
  // makes directories and names that fail to decode.
  struct Case {
    const char* name;
    Builder::StringForm dir_form;
    Builder::StringForm file_form;
  };
  for (const Case& c : {
           Case{"inline", Builder::StringForm::kString, Builder::StringForm::kString},
           Case{"line_strp", Builder::StringForm::kLineStrp, Builder::StringForm::kLineStrp},
           // LLVM uses an empty directory when the directory doesn't decode.
           Case{"bad directory", Builder::StringForm::kStrx1, Builder::StringForm::kLineStrp},
           // A name that doesn't decode means the entry has no file.
           Case{"bad name", Builder::StringForm::kLineStrp, Builder::StringForm::kStrx1},
       }) {
    SCOPED_TRACE(c.name);
    Builder builder(5);
    builder.set_dir_string_form(c.dir_form);
    builder.set_string_form(c.file_form);
    builder.AddDirectory("/build");
    uint64_t src = builder.AddDirectory("src");
    uint64_t abs = builder.AddDirectory("/abs");
    builder.AddFile("a.cc", 0);
    builder.AddFile("b.cc", src);
    builder.AddFile("c.h", abs);
    builder.AddFile("/root/d.h", src);  // Absolute names ignore the directory.
    builder.AddFile("", src);           // An empty name gives the directory.
    builder.AddFile("", 0);             // An empty name in the compilation directory.
    builder.AddFile("e.h", 99);         // An invalid directory index.

    LineTableTestSections sections;
    uint64_t offset = sections.Add(builder);
    sections.Finish();
    ErrOr<LineProgram> program = sections.Parse(offset);
    ASSERT_TRUE(program.ok());
    const LineProgramHeader& header = program.value().header();
    LineFileTable table = LineFileTable::FromEntries(LineFileEntries(header));

    LineFilePathResolver resolver;
    for (uint64_t i = 0; i <= table.size(); i++) {  // One past the end is invalid.
      SCOPED_TRACE(i);
      const std::string* expected = table.GetFileName(i);
      const std::string* resolved = resolver.Resolve(LineFileEntries(header), i);
      ASSERT_EQ(!!expected, !!resolved);
      if (expected)
        EXPECT_EQ(*expected, *resolved);
    }
    if (c.file_form == Builder::StringForm::kStrx1)
      EXPECT_FALSE(table.GetFileName(1));
    if (c.dir_form == Builder::StringForm::kStrx1)
      EXPECT_EQ("b.cc", *table.GetFileName(1));
    else if (c.file_form != Builder::StringForm::kStrx1)
      EXPECT_EQ("src/b.cc", *table.GetFileName(1));
  }
}

TEST(LineFilePathResolver, SameNameDifferentDirectory) {
  // The cache key includes the directory string.
  Builder builder(5);
  builder.AddDirectory("/build");
  uint64_t one = builder.AddDirectory("one");
  uint64_t two = builder.AddDirectory("two");
  builder.AddFile("same.h", one);
  builder.AddFile("same.h", two);
  builder.AddFile("same.h", 0);

  LineTableTestSections sections;
  uint64_t offset = sections.Add(builder);
  sections.Finish();
  ErrOr<LineProgram> program = sections.Parse(offset);
  ASSERT_TRUE(program.ok());

  LineFilePathResolver resolver;
  EXPECT_EQ("one/same.h", *resolver.Resolve(program.value().header_files(), 0));
  EXPECT_EQ("two/same.h", *resolver.Resolve(program.value().header_files(), 1));
  EXPECT_EQ("same.h", *resolver.Resolve(program.value().header_files(), 2));
}

TEST(LineFilePathResolver, InlineStrings) {
  // Inline strings have a different address in each table, so the cache doesn't merge them. The
  // results are still correct.
  for (uint16_t version : {4, 5}) {
    SCOPED_TRACE(version);
    LineTableTestSections sections;
    Builder first(version);
    Builder second(version);
    for (Builder* builder : {&first, &second}) {
      builder->set_string_form(Builder::StringForm::kString);
      if (version >= 5)
        builder->AddDirectory("/build");
      uint64_t dir = builder->AddDirectory("inc");
      builder->AddFile("x.h", dir);
    }
    uint64_t first_offset = sections.Add(first);
    uint64_t second_offset = sections.Add(second);
    sections.Finish();

    uint64_t file = version >= 5 ? 0 : 1;
    LineFilePathResolver resolver;
    ErrOr<LineProgram> program1 = sections.Parse(first_offset);
    ErrOr<LineProgram> program2 = sections.Parse(second_offset);
    EXPECT_EQ("inc/x.h", *resolver.Resolve(program1.value().header_files(), file));
    EXPECT_EQ("inc/x.h", *resolver.Resolve(program2.value().header_files(), file));
    EXPECT_EQ(2u, resolver.cache_size());
  }
}

class SourceFileIndexerTest : public ::testing::TestWithParam<bool> {};

TEST_P(SourceFileIndexerTest, MergesInJobOrder) {
  bool threads = GetParam();

  LineTableTestSections sections;
  std::vector<uint64_t> offsets;
  // Unit 0 is at section offset 0, which is a valid offset.
  offsets.push_back(sections.Add(MakeTable({"common.h", "zero.cc"})));
  // Unit 1 has a corrupt table. This doesn't stop the other units.
  offsets.push_back(sections.AddRaw({0xf0, 0xff, 0xff, 0xff}));
  // Unit 2 has two file entries with the same name. The unit is added once.
  offsets.push_back(sections.Add(MakeTable({"two.cc", "common.h", "common.h"})));
  // Many more units, so the threads share the work.
  for (int i = 3; i < 100; i++)
    offsets.push_back(sections.Add(MakeTable({"common.h", "unit" + std::to_string(i) + ".cc"})));
  sections.Finish();

  // Add the jobs in reverse order. The merge uses job order, not unit order.
  std::vector<LineTableJob> jobs;
  for (size_t i = offsets.size(); i > 0; i--)
    jobs.push_back(MakeJob(sections, static_cast<uint32_t>(i - 1), offsets[i - 1]));

  SourceFileIndexer indexer(std::move(jobs), {.thread_count = threads ? 2 : 0});
  EXPECT_EQ(threads ? 2u : 0u, indexer.worker_count());

  SourceFileIndexer::FileIndex files;
  indexer.FinishInto(&files);

  EXPECT_EQ(100u, files.size());  // common.h, zero.cc, two.cc, and 97 others.
  EXPECT_EQ((std::vector<uint32_t>{0}), UnitNumbers(files["src/zero.cc"]));
  EXPECT_EQ((std::vector<uint32_t>{2}), UnitNumbers(files["src/two.cc"]));

  std::vector<uint32_t> expected_common;
  for (uint32_t i = 99; i >= 3; i--)
    expected_common.push_back(i);
  expected_common.push_back(2);
  expected_common.push_back(0);
  EXPECT_EQ(expected_common, UnitNumbers(files["src/common.h"]));
}

INSTANTIATE_TEST_SUITE_P(SourceFileIndexer, SourceFileIndexerTest, ::testing::Bool());

TEST(SourceFileIndexer, NoJobs) {
  LineTableTestSections sections;
  sections.Finish();

  SourceFileIndexer indexer({});
  EXPECT_EQ(0u, indexer.worker_count());

  SourceFileIndexer::FileIndex files;
  indexer.FinishInto(&files);
  EXPECT_TRUE(files.empty());
}

TEST(SourceFileIndexer, AddsToExistingIndex) {
  LineTableTestSections sections;
  uint64_t offset = sections.Add(MakeTable({"a.cc"}));
  sections.Finish();

  SourceFileIndexer::FileIndex files;
  files["src/a.cc"].push_back(UnitIndex(true, 7));

  SourceFileIndexer indexer({MakeJob(sections, 1, offset)});
  indexer.FinishInto(&files);

  ASSERT_EQ(2u, files["src/a.cc"].size());
  EXPECT_EQ(UnitIndex(true, 7), files["src/a.cc"][0]);
  EXPECT_EQ(UnitIndex(false, 1), files["src/a.cc"][1]);
}

TEST(SourceFileIndexer, DestroyWithoutFinish) {
  // The destructor waits for the workers.
  LineTableTestSections sections;
  std::vector<uint64_t> offsets;
  for (int i = 0; i < 10; i++)
    offsets.push_back(sections.Add(MakeTable({"a.cc"})));
  sections.Finish();

  std::vector<LineTableJob> jobs;
  for (size_t i = 0; i < offsets.size(); i++)
    jobs.push_back(MakeJob(sections, static_cast<uint32_t>(i), offsets[i]));

  SourceFileIndexer indexer(std::move(jobs), {.thread_count = 2});
}

TEST(SourceFileIndexer, DefaultThreadCount) {
  // Small line tables run on the calling thread because starting threads takes longer.
  LineTableTestSections sections;
  std::vector<uint64_t> offsets;
  for (int i = 0; i < 64; i++)
    offsets.push_back(sections.Add(MakeTable({"a.cc"})));
  sections.Finish();

  std::vector<LineTableJob> jobs;
  for (size_t i = 0; i < offsets.size(); i++) {
    jobs.push_back(MakeJob(sections, static_cast<uint32_t>(i), offsets[i]));
    EXPECT_GT(jobs.back().size, 0u);
  }
  SourceFileIndexer small(jobs);
  EXPECT_EQ(0u, small.worker_count());

  // The thread count depends on the bytes of line table data. Pretend the tables are large.
  if (std::thread::hardware_concurrency() >= 2) {
    for (LineTableJob& job : jobs)
      job.size = 1024 * 1024;
    SourceFileIndexer large(jobs);
    EXPECT_GE(large.worker_count(), 2u);
    EXPECT_LE(large.worker_count(), 6u);
  }
}

// DW_FORM_strx names need the unit's string offsets table. The jobs have the string offsets base,
// so the workers resolve the names.
class SourceFileIndexerStrxTest : public ::testing::TestWithParam<bool> {};

TEST_P(SourceFileIndexerStrxTest, ResolvesWithUnit) {
  bool threads = GetParam();

  Builder strx(5);
  strx.set_string_form(Builder::StringForm::kStrx1);
  strx.AddDirectory("/build");
  uint64_t src = strx.AddDirectory("src");
  strx.SetAddress(0x1000);
  strx.SetFile(strx.AddFile("indexed.cc", src));
  strx.Copy();
  strx.AdvancePc(4);
  strx.EndSequence();

  LineTableTestSections sections;
  uint64_t strx_offset = sections.Add(strx);
  uint64_t plain_offset = sections.Add(MakeTable({"plain.cc"}));
  sections.AddCompileUnit(5, strx_offset, {});
  sections.AddCompileUnit(5, plain_offset, {});
  sections.Finish();

  std::vector<LineTableJob> jobs;
  AddLineTableJob(sections.sections(), UnitIndex(false, 0), *sections.unit(0), 0x1000, &jobs);
  AddLineTableJob(sections.sections(), UnitIndex(false, 1), *sections.unit(1), 0x1000, &jobs);
  ASSERT_EQ(2u, jobs.size());
  EXPECT_EQ(0x1000u, jobs[0].first_code_address);

  // Without the unit, the names don't resolve.
  ErrOr<LineProgram> program = sections.Parse(strx_offset);
  ASSERT_TRUE(program.ok());
  EXPECT_FALSE(LineFileTable::FromEntries(program.value().header_files()).GetFileName(0));

  SourceFileIndexer indexer(std::move(jobs), {.thread_count = threads ? 2 : 0});

  // Index::CreateIndex() extracts and frees each unit's DIEs while the workers run. Do the same,
  // so FinishInto() must extract the unit DIE again for the string offsets table.
  for (size_t i = 0; i < 2; i++) {
    ASSERT_TRUE(sections.unit(i)->getUnitDIE(false).isValid());
    sections.unit(i)->clear();
  }

  SourceFileIndexer::FileIndex files;
  indexer.FinishInto(&files);

  EXPECT_EQ((std::vector<uint32_t>{0}), UnitNumbers(files["src/indexed.cc"]));
  EXPECT_EQ((std::vector<uint32_t>{1}), UnitNumbers(files["src/plain.cc"]));
  EXPECT_EQ(2u, files.size());
}

INSTANTIATE_TEST_SUITE_P(SourceFileIndexer, SourceFileIndexerStrxTest, ::testing::Bool());

TEST(SourceFileIndexer, StrippedBelowFirstCode) {
  // The workers use the first code address of the job. A sequence that starts below it is stripped
  // code, so its file isn't indexed.
  Builder builder(5);
  builder.AddDirectory("/build");
  uint64_t dead = builder.AddFile("dead.cc", 0);
  uint64_t live = builder.AddFile("live.cc", 0);
  builder.SetAddress(0x20);
  builder.SetFile(dead);
  builder.Copy();
  builder.AdvancePc(4);
  builder.EndSequence();
  builder.SetAddress(0x1000);
  builder.SetFile(live);
  builder.Copy();
  builder.AdvancePc(4);
  builder.EndSequence();
  LineTableTestSections sections;
  uint64_t offset = sections.Add(builder);
  sections.Finish();

  for (uint64_t first_code_address : {0, 0x1000}) {
    SCOPED_TRACE(first_code_address);
    LineTableJob job = MakeJob(sections, 0, offset);
    job.first_code_address = first_code_address;
    SourceFileIndexer indexer({job}, {.thread_count = 1});
    SourceFileIndexer::FileIndex files;
    indexer.FinishInto(&files);
    EXPECT_EQ(first_code_address == 0, files.contains("dead.cc"));
    EXPECT_TRUE(files.contains("live.cc"));
  }
}

TEST(SourceFileIndexer, UnitWithoutLineTable) {
  // A unit without DW_AT_stmt_list gets no job. The other units are indexed.
  LineTableTestSections sections;
  uint64_t offset = sections.Add(MakeTable({"a.cc"}));
  sections.AddCompileUnit(5, std::nullopt, {});
  sections.AddCompileUnit(5, offset, {});
  sections.Finish();

  std::vector<LineTableJob> jobs;
  AddLineTableJob(sections.sections(), UnitIndex(false, 0), *sections.unit(0), 0x1000, &jobs);
  EXPECT_TRUE(jobs.empty());
  AddLineTableJob(sections.sections(), UnitIndex(false, 1), *sections.unit(1), 0x1000, &jobs);
  ASSERT_EQ(1u, jobs.size());

  SourceFileIndexer indexer(std::move(jobs));
  SourceFileIndexer::FileIndex files;
  indexer.FinishInto(&files);
  EXPECT_EQ((std::vector<uint32_t>{1}), UnitNumbers(files["src/a.cc"]));
}

}  // namespace zxdb
