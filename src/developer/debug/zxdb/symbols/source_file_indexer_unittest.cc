// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/source_file_indexer.h"

#include <gtest/gtest.h>

#include <thread>

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

LineTableJob MakeJob(const LineTableTestSections& sections, uint32_t unit, uint64_t offset) {
  LineTableJob job;
  job.unit_index = UnitIndex(false, unit);
  job.section = &sections.line_section();
  job.offset = offset;
  job.address_size = 8;
  job.size = 100;
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

  const std::string* a1 = resolver.Resolve(program1.value().prologue(), 0);
  const std::string* b1 = resolver.Resolve(program1.value().prologue(), 1);
  const std::string* b2 = resolver.Resolve(program2.value().prologue(), 0);
  const std::string* a2 = resolver.Resolve(program2.value().prologue(), 1);
  ASSERT_TRUE(a1 && b1);
  EXPECT_EQ("src/a.cc", *a1);
  EXPECT_EQ("src/b.h", *b1);
  EXPECT_EQ(a1, a2);  // From the cache.
  EXPECT_EQ(b1, b2);
  EXPECT_EQ(2u, resolver.cache_size());

  // Entries outside the table have no name.
  EXPECT_FALSE(resolver.Resolve(program1.value().prologue(), 2));
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
    const llvm::DWARFDebugLine::Prologue& prologue = program.value().prologue();
    LineFileTable table = LineFileTable::FromPrologue(prologue);

    LineFilePathResolver resolver;
    for (uint64_t i = 0; i <= table.size(); i++) {  // One past the end is invalid.
      SCOPED_TRACE(i);
      const std::string* expected = table.GetFileName(i);
      const std::string* resolved = resolver.Resolve(prologue, i);
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
  EXPECT_EQ("one/same.h", *resolver.Resolve(program.value().prologue(), 0));
  EXPECT_EQ("two/same.h", *resolver.Resolve(program.value().prologue(), 1));
  EXPECT_EQ("same.h", *resolver.Resolve(program.value().prologue(), 2));
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
    EXPECT_EQ("inc/x.h", *resolver.Resolve(program1.value().prologue(), file));
    EXPECT_EQ("inc/x.h", *resolver.Resolve(program2.value().prologue(), file));
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

  SourceFileIndexer indexer(sections.context(), std::move(jobs),
                            {.thread_count = threads ? 2 : 0});
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

  SourceFileIndexer indexer(sections.context(), {});
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

  SourceFileIndexer indexer(sections.context(), {MakeJob(sections, 1, offset)});
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

  SourceFileIndexer indexer(sections.context(), std::move(jobs), {.thread_count = 2});
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
  SourceFileIndexer small(sections.context(), jobs);
  EXPECT_EQ(0u, small.worker_count());

  // The thread count depends on the bytes of line table data. Pretend the tables are large.
  if (std::thread::hardware_concurrency() >= 2) {
    for (LineTableJob& job : jobs)
      job.size = 1024 * 1024;
    SourceFileIndexer large(sections.context(), jobs);
    EXPECT_GE(large.worker_count(), 2u);
    EXPECT_LE(large.worker_count(), 6u);
  }
}

// DW_FORM_strx names need the unit's string offsets table, which workers can't use. FinishInto()
// resolves these tables with the unit on the calling thread.
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
  AddLineTableJob(UnitIndex(false, 0), *sections.unit(0), &jobs);
  AddLineTableJob(UnitIndex(false, 1), *sections.unit(1), &jobs);
  ASSERT_EQ(2u, jobs.size());

  ErrOr<LineProgram> program = sections.Parse(strx_offset);
  ASSERT_TRUE(program.ok());
  EXPECT_TRUE(LineNamesNeedUnit(program.value().prologue()));

  SourceFileIndexer indexer(sections.context(), std::move(jobs),
                            {.thread_count = threads ? 2 : 0});

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

}  // namespace zxdb
