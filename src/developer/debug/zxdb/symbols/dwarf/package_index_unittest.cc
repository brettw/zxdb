// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/package_index.h"

#include <vector>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"

namespace zxdb {
namespace {

using Kind = PackageIndex::Kind;
using Section = PackageIndex::Section;

TEST(PackageIndex, GnuVersion2TypeIndex) {
  // readelf --debug-dump=cu_index on GCC 14 split DWARF 4 for
  // "struct Widget { int member; }; Widget w; int main() { return w.member; }"
  // gives this TU row. Write it in big endian to exercise the 32-bit version field.
  constexpr uint64_t kSignature = 0x5bab94418b06a57e;
  std::vector<uint8_t> bytes;
  DwarfSectionBuilder builder(bytes, Endian::kBig);
  for (uint32_t word : {2, 4, 1, 2})
    builder.Fixed(word, 4);
  builder.Fixed(kSignature, 8);
  builder.Fixed(0, 8);
  for (uint32_t word : {1, 0, 2, 3, 4, 6, 0, 0, 0, 0, 66, 126, 43, 24})
    builder.Fixed(word, 4);

  auto index = PackageIndex::Parse(bytes, Endian::kBig, Kind::kTu);
  ASSERT_TRUE(index) << index.err().msg();
  auto row = index.value().FindByHash(kSignature);
  ASSERT_EQ(1u, row);
  EXPECT_EQ(row, index.value().FindByOffset(0));
  auto types = index.value().GetContribution(*row, Section::kTypes);
  ASSERT_TRUE(types);
  EXPECT_EQ(0u, types->offset);
  EXPECT_EQ(66u, types->size);
  EXPECT_FALSE(index.value().GetContribution(*row, Section::kInfo));
}

std::vector<uint8_t> MakeDwarf5Index() {
  // DWARF 5 section 7.3.5.3. With four slots, both signatures start at slot 0.
  // The second signature probes with stride 3 and locates row 2 in slot 3.
  std::vector<uint8_t> bytes;
  DwarfSectionBuilder builder(bytes, Endian::kBig);
  builder.Fixed(5, 2);
  builder.Fixed(0, 2);
  for (uint32_t word : {3, 2, 4})
    builder.Fixed(word, 4);
  for (uint64_t signature : {0ULL, 0ULL, 0ULL, 0x200000004ULL})
    builder.Fixed(signature, 8);
  for (uint32_t word : {1, 0, 0, 2, 1, 3, 8, 0, 0, 0, 20, 30, 40, 20, 30, 40, 50, 60, 70})
    builder.Fixed(word, 4);
  return bytes;
}

TEST(PackageIndex, Dwarf5SignaturesAndContributions) {
  auto bytes = MakeDwarf5Index();
  auto index = PackageIndex::Parse(bytes, Endian::kBig, Kind::kTu);
  ASSERT_TRUE(index) << index.err().msg();

  EXPECT_EQ(1u, index.value().FindByHash(0));
  EXPECT_EQ(2u, index.value().FindByHash(0x200000004));
  EXPECT_FALSE(index.value().FindByHash(0x200000008));
  EXPECT_EQ(2u, index.value().FindByOffset(20));
  EXPECT_FALSE(index.value().FindByOffset(19));
  auto ranges = index.value().GetContribution(2, Section::kRnglists);
  ASSERT_TRUE(ranges);
  EXPECT_EQ(40u, ranges->offset);
  EXPECT_EQ(70u, ranges->size);
  EXPECT_FALSE(index.value().GetContribution(2, Section::kLine));
}

TEST(PackageIndex, InvalidTables) {
  auto bytes = MakeDwarf5Index();
  // In the layout from DWARF 5 section 7.3.5.3, the final byte belongs to the size matrix.
  EXPECT_TRUE(PackageIndex::Parse(std::span(bytes).first(bytes.size() - 1), Endian::kBig, Kind::kCu)
                  .has_error());
  bytes[16 + 4 * 8 + 3] = 3;  // Slot 0 points past the two declared rows.
  EXPECT_TRUE(PackageIndex::Parse(bytes, Endian::kBig, Kind::kCu).has_error());
}

}  // namespace
}  // namespace zxdb
