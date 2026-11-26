// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/debug_names_reader.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"
#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_sections.h"
#include "src/developer/debug/zxdb/symbols/dwarf_debug_names.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"

namespace zxdb {

namespace {

using Entry = DebugNamesReader::Entry;

constexpr uint64_t kCompileUnit = 1;
constexpr uint64_t kTypeUnit = 2;
constexpr uint64_t kDieOffset = 3;
constexpr uint64_t kParent = 4;
constexpr uint64_t kTypeHash = 5;

struct Attribute {
  uint64_t index;
  DwarfForm form;
};

void WriteAbbrev(DwarfSectionBuilder& out, uint64_t code, DwarfTag tag,
                 std::initializer_list<Attribute> attributes) {
  out.Uleb(code);
  out.Uleb(static_cast<uint64_t>(tag));
  for (const auto& [index, form] : attributes) {
    out.Uleb(index);
    out.Uleb(static_cast<uint64_t>(form));
  }
  out.Uleb(0);
  out.Uleb(0);
}

struct IndexBytes {
  struct Name {
    uint64_t string_offset;
    uint64_t entry_offset;
    uint32_t hash = 0;
  };

  struct Layout {
    size_t cus;
    size_t local_tus;
    size_t strings;
    size_t entry_pool;
  };

  Layout AppendTo(std::vector<uint8_t>& out) const {
    DwarfSectionBuilder builder(out, endian);
    auto length = builder.BeginInitialLength(offset_size);
    builder.Fixed(version, 2);
    builder.Fixed(padding, 2);
    builder.Fixed(cus.size(), 4);
    builder.Fixed(local_tus.size(), 4);
    builder.Fixed(foreign_tus.size(), 4);
    builder.Fixed(buckets.size(), 4);
    builder.Fixed(names.size(), 4);
    builder.Fixed(abbrevs.size(), 4);
    builder.Fixed(augmentation.size(), 4);
    builder.Bytes(AsBytes(augmentation));
    for (size_t i = augmentation.size(); i % 4 != 0; ++i)
      builder.Fixed(0, 1);

    Layout layout{};
    layout.cus = out.size();
    for (auto offset : cus)
      builder.Offset(offset, offset_size);
    layout.local_tus = out.size();
    for (auto offset : local_tus)
      builder.Offset(offset, offset_size);
    for (auto signature : foreign_tus)
      builder.Fixed(signature, 8);
    for (auto row : buckets)
      builder.Fixed(row, 4);
    if (!buckets.empty()) {
      for (const auto& name : names)
        builder.Fixed(name.hash, 4);
    }
    layout.strings = out.size();
    for (const auto& name : names)
      builder.Offset(name.string_offset, offset_size);
    for (const auto& name : names)
      builder.Offset(name.entry_offset, offset_size);
    builder.Bytes(abbrevs);
    layout.entry_pool = out.size();
    builder.Bytes(pool);
    builder.EndInitialLength(length);
    return layout;
  }

  Endian endian = Endian::kLittle;
  uint8_t offset_size = 4;
  uint16_t version = 5;
  uint16_t padding = 0;
  std::string_view augmentation;
  std::vector<uint64_t> cus = {0};
  std::vector<uint64_t> local_tus;
  std::vector<uint64_t> foreign_tus;
  std::vector<uint32_t> buckets;
  std::vector<Name> names;
  std::vector<uint8_t> abbrevs;
  std::vector<uint8_t> pool;
};

// A name list with one subprogram. The index has no hash table until the test adds buckets.
IndexBytes SingleName(uint64_t string_offset, uint64_t cu_offset, uint8_t die_unit_offset) {
  return {.cus = {cu_offset},
          .names = {{string_offset, 0}},
          .abbrevs = {1, 0x2e, kDieOffset, 0x11, 0, 0, 0},  // DW_FORM_ref1.
          .pool = {1, die_unit_offset, 0}};
}

std::vector<Entry> ByName(const DebugNamesReader& reader, std::string_view name) {
  std::vector<Entry> result;
  reader.LookupName(name, [&](std::string_view spelling, const Entry& entry) {
    EXPECT_EQ(name, spelling);
    result.push_back(entry);
  });
  return result;
}

void SetInteger(std::vector<uint8_t>& bytes, size_t offset, uint64_t value, size_t width,
                Endian endian) {
  std::vector<uint8_t> field;
  AppendInteger(field, value, width, endian);
  std::ranges::copy(field, bytes.begin() + offset);
}

TEST(DebugNamesReader, MultipleIndexesAndUnhashedLists) {
  // The first list has two entries for one name. A second index has the same name.
  IndexBytes first = SingleName(0, 0, 0);
  first.pool = {1, 0, 1, 0x18, 0};
  IndexBytes second = SingleName(0, 0x100000000, 0x2a);
  second.offset_size = 8;
  second.version = 6;             // The layout is usable even if the version is unknown.
  second.augmentation = "VENDx";  // The consumer rounds this size up to four bytes.
  second.padding = 0xabcd;

  std::vector<uint8_t> section;
  first.AppendTo(section);
  second.AppendTo(section);
  const auto reader =
      DebugNamesReader::Parse({.str = AsBytes(std::string_view("name\0", 5)), .names = section});
  ASSERT_FALSE(reader.error().has_error()) << reader.error().msg();
  EXPECT_EQ(2u, reader.index_count());
  auto entries = ByName(reader, "name");
  ASSERT_EQ(3u, entries.size());
  EXPECT_EQ(0u, entries[0].cu_offset);
  EXPECT_EQ(0u, entries[0].die_unit_offset);
  EXPECT_FALSE(entries[0].has_parent_information);
  EXPECT_EQ(0u, entries[1].cu_offset);
  EXPECT_EQ(0x18u, entries[1].die_unit_offset);
  EXPECT_EQ(0x100000000u, entries[2].cu_offset);
  EXPECT_EQ(0x2au, entries[2].die_unit_offset);

  const std::vector<DebugNamesEntry> expected = {
      {DwarfDieRef::Main(0), DwarfTag::kSubprogram},
      {DwarfDieRef::Main(0x18), DwarfTag::kSubprogram},
      {DwarfDieRef::Main(0x10000002a), DwarfTag::kSubprogram}};
  EXPECT_EQ(expected, FindDebugNamesEntries(reader, "name"));
  EXPECT_EQ(expected, FindDebugNamesPrefixEntries(reader, "na"));
  EXPECT_TRUE(ByName(reader, "Name").empty());
  reader.LookupHash(2090536006, [](std::string_view, const Entry&) {
    ADD_FAILURE() << "A hash-only query visited an unhashed index.";
  });
}

TEST(DebugNamesReader, HashCollisions) {
  DwarfStringSectionBuilder strings;
  IndexBytes index = SingleName(0, 0, 0);
  // DWARF 5 simple folding gives "Aa" and "AA" the hash of "aa". The different
  // spelling "b@" has the same DJB hash because 97 * 33 + 97 == 98 * 33 + 64.
  index.buckets = {0, 1, 5};
  index.names = {{strings.AddString("a"), 0, 177670},
                 {strings.AddString("Aa"), 3, 5863207},
                 {strings.AddString("b@"), 6, 5863207},
                 {strings.AddString("AA"), 9, 5863207},
                 {strings.AddString("zz"), 12, 5864057}};
  index.pool = {1, 10, 0, 1, 20, 0, 1, 30, 0, 1, 40, 0, 1, 50, 0};

  std::vector<uint8_t> section;
  index.AppendTo(section);
  const auto reader = DebugNamesReader::Parse({.str = strings.bytes(), .names = section});
  ASSERT_FALSE(reader.error().has_error()) << reader.error().msg();
  for (const auto& [name, die] :
       {std::pair{"a", 10}, {"Aa", 20}, {"b@", 30}, {"AA", 40}, {"zz", 50}}) {
    SCOPED_TRACE(name);
    auto entries = ByName(reader, name);
    ASSERT_EQ(1u, entries.size());
    EXPECT_EQ(die, entries[0].die_unit_offset);
  }
  EXPECT_TRUE(ByName(reader, "aa").empty());
  EXPECT_TRUE(ByName(reader, "c").empty());
  EXPECT_TRUE(ByName(reader, std::string_view("Aa\0", 3)).empty());

  std::vector<std::string_view> matches;
  reader.LookupHash(5863207, [&](std::string_view name, const Entry&) { matches.push_back(name); });
  EXPECT_EQ((std::vector<std::string_view>{"Aa", "b@", "AA"}), matches);
  reader.LookupHash(0, [](std::string_view, const Entry&) { ADD_FAILURE() << "Empty bucket."; });
}

TEST(DebugNamesReader, UnicodeHash) {
  DwarfStringSectionBuilder strings;
  IndexBytes index = SingleName(0, 0, 0);
  index.buckets = {1};
  // Σ and final sigma fold to σ (CF 83). Both Turkish I variants fold to ASCII i.
  // U+10400 folds to U+10428 (F0 90 90 A8). Hash the bytes of the folded UTF-8.
  index.names = {{strings.AddString("Σ"), 0, 5866871},
                 {strings.AddString("ς"), 0, 5866871},
                 {strings.AddString("İ"), 0, 177678},
                 {strings.AddString("ı"), 0, 177678},
                 {strings.AddString("\U00010400"), 0, 2095260221}};
  std::vector<uint8_t> section;
  index.AppendTo(section);
  const auto reader = DebugNamesReader::Parse({.str = strings.bytes(), .names = section});
  ASSERT_FALSE(reader.error().has_error()) << reader.error().msg();
  for (const auto& name : {"Σ", "ς", "İ", "ı", "\U00010400"}) {
    SCOPED_TRACE(name);
    EXPECT_EQ(1u, ByName(reader, name).size());
  }
  EXPECT_TRUE(ByName(reader, "σ").empty());
  EXPECT_TRUE(ByName(reader, "i").empty());
}

TEST(DebugNamesReader, TypeUnitsAndForms) {
  for (Endian endian : {Endian::kLittle, Endian::kBig}) {
    for (uint8_t width : {4, 8}) {
      SCOPED_TRACE(width);
      SCOPED_TRACE(static_cast<int>(endian));

      IndexBytes index = {.endian = endian, .offset_size = width};
      index.cus = {0, 0x500};
      index.local_tus = {0, 0x600};
      index.foreign_tus = {0, 0x123456789abcdef0};
      index.buckets = {1};
      index.names = {{0, 0, 177693}};  // DJB("x").
      DwarfSectionBuilder abbrevs(index.abbrevs, endian);
      WriteAbbrev(abbrevs, 0x100000001, DwarfTag::kClassType,
                  {{kCompileUnit, DwarfForm::kData1},
                   {kTypeUnit, DwarfForm::kUdata},
                   {kDieOffset, DwarfForm::kRefAddr},
                   {kTypeHash, DwarfForm::kData8},
                   {0x2000, DwarfForm::kBlock1}});
      abbrevs.Uleb(0);
      abbrevs.Fixed(0xff, 1);  // Padding after the abbreviation terminator.
      DwarfSectionBuilder entries(index.pool, endian);
      for (uint64_t tu = 0; tu <= 4; ++tu) {
        entries.Uleb(0x100000001);
        entries.Fixed(tu % 2, 1);
        entries.Uleb(tu);
        entries.Offset(width == 4 ? 0x10203040 : 0x1020304050, width);
        entries.Fixed(0, 8);
        entries.Fixed(2, 1);
        entries.Fixed(0xabcd, 2);
      }
      entries.Uleb(0);

      std::vector<uint8_t> section;
      index.AppendTo(section);
      const auto reader = DebugNamesReader::Parse(
          {.str = AsBytes(std::string_view("x\0", 2)), .names = section, .endian = endian});
      ASSERT_FALSE(reader.error().has_error()) << reader.error().msg();
      auto results = ByName(reader, "x");
      ASSERT_EQ(5u, results.size());
      EXPECT_EQ(0u, results[0].local_tu_offset);
      EXPECT_EQ(0x600u, results[1].local_tu_offset);
      EXPECT_EQ(0u, results[2].foreign_tu_signature);
      EXPECT_EQ(0x123456789abcdef0u, results[3].foreign_tu_signature);
      EXPECT_EQ(std::nullopt, results[4].foreign_tu_signature);
      EXPECT_EQ(0u, results[0].related_cu_offset);
      EXPECT_EQ(0x500u, results[1].related_cu_offset);
      for (const auto& result : results) {
        EXPECT_EQ(DwarfTag::kClassType, result.tag);
        EXPECT_EQ(std::nullopt, result.cu_offset);
        EXPECT_EQ(width == 4 ? 0x10203040u : 0x1020304050u, result.die_unit_offset);
        EXPECT_EQ(0u, result.type_hash);
      }

      // The public symbol result can represent only DIEs in this object.
      auto symbols = FindDebugNamesEntries(reader, "x");
      ASSERT_EQ(2u, symbols.size());
      EXPECT_EQ(DwarfDieRef::Main(*results[0].die_unit_offset), symbols[0].die_ref);
      EXPECT_EQ(DwarfDieRef::Main(0x600 + *results[1].die_unit_offset), symbols[1].die_ref);
      EXPECT_EQ(symbols, FindDebugNamesPrefixEntries(reader, "x"));
    }
  }
}

TEST(DebugNamesReader, ParentEntries) {
  DwarfStringSectionBuilder strings;
  IndexBytes index;
  DwarfSectionBuilder abbrevs(index.abbrevs, index.endian);
  WriteAbbrev(abbrevs, 1, DwarfTag::kSubprogram, {{kDieOffset, DwarfForm::kRef1}});
  WriteAbbrev(abbrevs, 2, DwarfTag::kVariable,
              {{kDieOffset, DwarfForm::kRef1}, {kParent, DwarfForm::kRef4}});
  WriteAbbrev(abbrevs, 3, DwarfTag::kVariable,
              {{kDieOffset, DwarfForm::kRef1}, {kParent, DwarfForm::kFlagPresent}});
  abbrevs.Uleb(0);

  DwarfSectionBuilder entries(index.pool, index.endian);
  index.names.push_back({strings.AddString("function"), 0});
  entries.Uleb(1);
  entries.Fixed(0x12, 1);
  entries.Uleb(0);
  for (const auto& [name, parent] : {std::pair{"local", 0u}, {"bad", 0xffffffffu}, {"end", 2u}}) {
    index.names.push_back({strings.AddString(name), index.pool.size()});
    entries.Uleb(2);
    entries.Fixed(0x14, 1);
    entries.Fixed(parent, 4);
    entries.Uleb(0);
  }
  index.names.push_back({strings.AddString("global"), index.pool.size()});
  entries.Uleb(3);
  entries.Fixed(0x16, 1);
  entries.Uleb(0);

  std::vector<uint8_t> section;
  index.AppendTo(section);
  const auto reader = DebugNamesReader::Parse({.str = strings.bytes(), .names = section});
  ASSERT_FALSE(reader.error().has_error()) << reader.error().msg();

  auto local = ByName(reader, "local");
  ASSERT_EQ(1u, local.size());
  EXPECT_TRUE(local[0].has_parent_information);
  EXPECT_EQ(0u, local[0].parent_offset);
  auto parent = reader.ReadParent(local[0]);
  ASSERT_TRUE(parent.ok()) << parent.err_or_empty().msg();
  ASSERT_TRUE(parent.value());
  EXPECT_EQ(DwarfTag::kSubprogram, parent.value()->tag);
  EXPECT_TRUE(FindDebugNamesEntries(reader, "local").empty());

  auto global = ByName(reader, "global");
  ASSERT_EQ(1u, global.size());
  EXPECT_TRUE(global[0].has_parent_information);
  EXPECT_FALSE(global[0].parent_offset);
  parent = reader.ReadParent(global[0]);
  ASSERT_TRUE(parent.ok()) << parent.err_or_empty().msg();
  EXPECT_FALSE(parent.value());
  EXPECT_EQ(1u, FindDebugNamesEntries(reader, "global").size());

  for (auto name : {"bad", "end"}) {
    auto bad = ByName(reader, name);
    ASSERT_EQ(1u, bad.size());
    EXPECT_TRUE(reader.ReadParent(bad[0]).has_error());
    // The symbol layer can still resolve the child through the DIE tree.
    EXPECT_EQ(1u, FindDebugNamesEntries(reader, name).size());
  }
}

TEST(DebugNamesReader, ImplicitConstantAndBadUnitIndexes) {
  IndexBytes index;
  index.cus = {0x300, 0x500};
  index.names = {{0, 0}};
  DwarfSectionBuilder abbrevs(index.abbrevs, index.endian);
  // The implicit constant takes an operand from the abbreviation.
  abbrevs.Uleb(1);
  abbrevs.Uleb(static_cast<uint64_t>(DwarfTag::kClassType));
  abbrevs.Uleb(kCompileUnit);
  abbrevs.Uleb(static_cast<uint64_t>(DwarfForm::kImplicitConst));
  abbrevs.Sleb(1);
  abbrevs.Uleb(kDieOffset);
  abbrevs.Uleb(static_cast<uint64_t>(DwarfForm::kIndirect));
  abbrevs.Uleb(0);
  abbrevs.Uleb(0);
  WriteAbbrev(abbrevs, 2, DwarfTag::kClassType,
              {{kCompileUnit, DwarfForm::kUdata}, {kDieOffset, DwarfForm::kRef1}});
  WriteAbbrev(abbrevs, 3, DwarfTag::kClassType, {{kDieOffset, DwarfForm::kRef1}});
  abbrevs.Uleb(0);

  DwarfSectionBuilder pool(index.pool, index.endian);
  pool.Uleb(1);
  pool.Uleb(static_cast<uint64_t>(DwarfForm::kRefUdata));
  pool.Uleb(0x123);
  pool.Uleb(2);
  pool.Uleb(UINT64_MAX);
  pool.Fixed(0x15, 1);
  pool.Uleb(3);
  pool.Fixed(0x16, 1);
  pool.Uleb(2);
  pool.Uleb(0);
  pool.Fixed(0x17, 1);
  pool.Uleb(0);

  std::vector<uint8_t> section;
  index.AppendTo(section);
  const auto reader =
      DebugNamesReader::Parse({.str = AsBytes(std::string_view("x\0", 2)), .names = section});
  ASSERT_FALSE(reader.error().has_error()) << reader.error().msg();
  auto entries = ByName(reader, "x");
  ASSERT_EQ(4u, entries.size());
  EXPECT_EQ(0x500u, entries[0].cu_offset);
  EXPECT_EQ(0x123u, entries[0].die_unit_offset);
  EXPECT_EQ(std::nullopt, entries[1].cu_offset);
  EXPECT_EQ(std::nullopt, entries[2].cu_offset);
  EXPECT_EQ(0x300u, entries[3].cu_offset);
}

TEST(DebugNamesReader, TruncationAtEveryByte) {
  for (uint8_t width : {4, 8}) {
    IndexBytes index = SingleName(0, 0, 0x18);
    index.offset_size = width;
    index.augmentation = "VEND";
    index.local_tus = {0};
    index.foreign_tus = {0};
    index.buckets = {1};
    index.names[0].hash = 177693;
    index.abbrevs[3] = 0x13;  // DW_FORM_ref4.
    index.pool = {1, 0x18, 0, 0, 0, 0};

    std::vector<uint8_t> complete;
    auto layout = index.AppendTo(complete);
    size_t length_field_size = width == 4 ? 4 : 12;
    for (size_t size = 1; size <= complete.size(); ++size) {
      SCOPED_TRACE(width);
      SCOPED_TRACE(size);
      std::vector<uint8_t> section(complete.begin(), complete.begin() + size);
      if (size >= length_field_size) {
        // Keep the declared length in bounds so each truncation reaches the field it cuts.
        SetInteger(section, length_field_size - width, size - length_field_size, width,
                   index.endian);
      }
      const auto reader =
          DebugNamesReader::Parse({.str = AsBytes(std::string_view("x\0", 2)), .names = section});
      EXPECT_EQ(size < layout.entry_pool, reader.error().has_error()) << reader.error().msg();
      auto entries = ByName(reader, "x");
      // A complete entry survives a missing list terminator.
      ASSERT_EQ(size >= layout.entry_pool + 5 ? 1u : 0u, entries.size());
      if (!entries.empty())
        EXPECT_EQ(0x18u, entries[0].die_unit_offset);
    }
  }
}

TEST(DebugNamesReader, InvalidCountsAndLengths) {
  IndexBytes index = SingleName(0, 0, 0x18);
  index.offset_size = 8;
  index.buckets = {1};
  std::vector<uint8_t> complete;
  index.AppendTo(complete);

  // All seven header count or size fields must fit within the declared contribution.
  constexpr size_t kCounts = 12 + 4;
  for (size_t pos = kCounts; pos < kCounts + 7 * 4; pos += 4) {
    SCOPED_TRACE(pos);
    auto section = complete;
    SetInteger(section, pos, UINT32_MAX, 4, index.endian);
    const auto result = DebugNamesReader::Parse({.names = section});
    EXPECT_TRUE(result.error().has_error());
    EXPECT_EQ(0u, result.index_count());
  }
  SetInteger(complete, 4, UINT64_MAX, 8, index.endian);
  EXPECT_TRUE(DebugNamesReader::Parse({.names = complete}).error().has_error());
  SetInteger(complete, 0, 0xfffffff0, 4, index.endian);
  EXPECT_TRUE(DebugNamesReader::Parse({.names = complete}).error().has_error());
  EXPECT_FALSE(DebugNamesReader::Parse({}).error().has_error());
}

TEST(DebugNamesReader, InvalidNamesAndOffsets) {
  const auto strings = AsBytes(std::string_view("x\0y", 3));
  IndexBytes index = SingleName(0, UINT64_MAX, 1);
  index.offset_size = 8;
  index.names = {{0, 0}, {2, 0}, {3, 0}, {UINT64_MAX, 0}};
  std::vector<uint8_t> section;
  index.AppendTo(section);
  const auto reader = DebugNamesReader::Parse({.str = strings, .names = section});
  ASSERT_FALSE(reader.error().has_error()) << reader.error().msg();
  EXPECT_EQ(1u, ByName(reader, "x").size());
  EXPECT_TRUE(FindDebugNamesEntries(reader, "x").empty());  // CU plus DIE offset would wrap.
  EXPECT_TRUE(ByName(reader, "y").empty());                 // Missing NUL.
  EXPECT_TRUE(ByName(reader, "").empty());
  reader.VisitNamesWithPrefix("y", [](std::string_view) { ADD_FAILURE() << "Missing NUL."; });

  index.cus = {0};
  index.names = {{0, UINT64_MAX, 177693}};
  index.buckets = {1};
  section.clear();
  index.AppendTo(section);
  auto bad_entry = DebugNamesReader::Parse({.str = strings, .names = section});
  ASSERT_FALSE(bad_entry.error().has_error()) << bad_entry.error().msg();
  EXPECT_TRUE(ByName(bad_entry, "x").empty());

  index.names[0].entry_offset = 0;
  index.buckets = {UINT32_MAX};
  section.clear();
  index.AppendTo(section);
  auto bad_bucket = DebugNamesReader::Parse({.str = strings, .names = section});
  ASSERT_FALSE(bad_bucket.error().has_error()) << bad_bucket.error().msg();
  EXPECT_TRUE(ByName(bad_bucket, "x").empty());
}

TEST(DebugNamesReader, ErrorsStayLocal) {
  const auto strings = AsBytes(std::string_view("x\0y\0", 4));
  IndexBytes first = SingleName(0, 0, 0x18);
  // The first list has a valid entry followed by an unknown abbreviation. The second
  // list starts at 4. The third index has a bad declaration and stops the header scan.
  first.pool = {1, 0x18, 0x7f, 0, 1, 0x20, 0};
  first.names.push_back({2, 4});
  IndexBytes second = SingleName(0, 0x100, 0x12);
  IndexBytes third = SingleName(0, 0x200, 0x14);
  third.abbrevs.pop_back();
  std::vector<uint8_t> section;
  first.AppendTo(section);
  second.AppendTo(section);
  third.AppendTo(section);
  second.AppendTo(section);

  auto reader = DebugNamesReader::Parse({.str = strings, .names = section});
  EXPECT_EQ(2u, reader.index_count());
  EXPECT_TRUE(reader.error().has_error());
  auto entries = ByName(reader, "x");
  ASSERT_EQ(2u, entries.size());
  EXPECT_EQ(0x18u, entries[0].die_unit_offset);
  EXPECT_EQ(0x12u, entries[1].die_unit_offset);
  EXPECT_EQ(1u, ByName(reader, "y").size());

  // Abbreviations and entry lists cannot finish in the next contribution.
  first = SingleName(0, 0, 0x18);
  first.pool = {1};
  section.clear();
  first.AppendTo(section);
  second.AppendTo(section);
  reader = DebugNamesReader::Parse({.str = strings, .names = section});
  ASSERT_EQ(2u, reader.index_count());
  entries = ByName(reader, "x");
  ASSERT_EQ(1u, entries.size());
  EXPECT_EQ(0x100u, entries[0].cu_offset);
}

TEST(DebugNamesReader, InvalidAbbreviations) {
  for (const std::vector<uint8_t>& abbreviations : {
           std::vector<uint8_t>{},
           {0x80},
           {1, 0x80},
           {1, 0x2e, 0, 0x11, 0, 0},
           {1, 0x2e, kDieOffset, 0, 0},
           {1, 0x2e, 0, 0, 1, 0x2e, 0, 0, 0},
           {1, 0x2e, kCompileUnit, 0x21, 0x80},
       }) {
    IndexBytes index = SingleName(0, 0, 0x18);
    index.abbrevs = abbreviations;
    std::vector<uint8_t> section;
    index.AppendTo(section);
    EXPECT_TRUE(DebugNamesReader::Parse({.names = section}).error().has_error());
  }
}

TEST(DebugNamesReader, OffsetRelocationsAndConcurrentReads) {
  const auto strings = AsBytes(std::string_view("unused\0name\0", 12));
  IndexBytes index = SingleName(0, 0, 0x14);
  index.local_tus = {0};
  std::vector<uint8_t> section;
  auto layout = index.AppendTo(section);
  std::vector<uint64_t> visited_fields;
  const auto reader = DebugNamesReader::Parse(
      {.str = strings, .names = section}, [&](uint64_t offset, uint8_t width) {
        EXPECT_EQ(4u, width);
        visited_fields.push_back(offset);
        return offset == layout.cus ? 0x140u : offset == layout.strings ? 7u : 0u;
      });
  ASSERT_FALSE(reader.error().has_error()) << reader.error().msg();
  const std::vector<uint64_t> expected_fields = {layout.cus, layout.local_tus, layout.strings};
  EXPECT_EQ(expected_fields, visited_fields);
  auto check = [&] {
    EXPECT_TRUE(ByName(reader, "unused").empty());
    auto entries = ByName(reader, "name");
    ASSERT_EQ(1u, entries.size());
    EXPECT_EQ(0x140u, entries[0].cu_offset);
    EXPECT_EQ(0x14u, entries[0].die_unit_offset);
  };
  std::thread worker(check);
  check();
  worker.join();
  EXPECT_EQ(expected_fields, visited_fields);
}

}  // namespace

}  // namespace zxdb
