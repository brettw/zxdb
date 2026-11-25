// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"

#include <iterator>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"

namespace zxdb {

namespace {

TEST(DwarfSectionBuilder, Values) {
  for (bool little_endian : {false, true}) {
    SCOPED_TRACE(little_endian);
    std::vector<uint8_t> bytes;
    DwarfSectionBuilder writer(bytes, little_endian);
    constexpr uint64_t kFixed[] = {
        0x12,         0x1234,         0x123456,         0x12345678,
        0x123456789a, 0x123456789abc, 0x123456789abcde, 0x123456789abcdef0};
    for (size_t i = 0; i < std::size(kFixed); i++)
      writer.Fixed(kFixed[i], i + 1);
    writer.Uleb(12857);
    writer.Uleb(UINT64_MAX);
    writer.Sleb(-129);
    writer.Sleb(INT64_MIN);
    writer.Sleb(INT64_MAX);
    writer.Offset(0, 4);
    writer.Offset(0x876543210fedcbab, 8);
    writer.Address(0x54, 1);
    writer.Address(0x5432, 2);
    writer.Address(0x543210fe, 4);
    writer.Address(0x543210fedcba9876, 8);
    writer.CString("kernel");
    writer.CString("");
    constexpr uint8_t kRaw[] = {0x01, 0xfe};
    writer.Bytes(kRaw);

    DataReader reader(bytes, little_endian);
    for (size_t i = 0; i < std::size(kFixed); i++)
      EXPECT_EQ(kFixed[i], reader.Fixed(i + 1));
    EXPECT_EQ(12857u, reader.Uleb());
    EXPECT_EQ(UINT64_MAX, reader.Uleb());
    EXPECT_EQ(-129, reader.Sleb());
    EXPECT_EQ(INT64_MIN, reader.Sleb());
    EXPECT_EQ(INT64_MAX, reader.Sleb());
    EXPECT_EQ(0u, reader.Offset(4));
    EXPECT_EQ(0x876543210fedcbabu, reader.Offset(8));
    EXPECT_EQ(0x54u, reader.Address(1));
    EXPECT_EQ(0x5432u, reader.Address(2));
    EXPECT_EQ(0x543210feu, reader.Address(4));
    EXPECT_EQ(0x543210fedcba9876u, reader.Address(8));
    EXPECT_EQ("kernel", reader.CString());
    EXPECT_TRUE(reader.CString().empty());
    EXPECT_EQ(0x01u, reader.Fixed(1));
    EXPECT_EQ(0xfeu, reader.Fixed(1));
    EXPECT_EQ(0u, reader.remaining());
    EXPECT_TRUE(reader.ok());
  }
}

TEST(DwarfSectionBuilder, InitialLength) {
  for (bool little_endian : {false, true}) {
    SCOPED_TRACE(little_endian);
    std::vector<uint8_t> bytes;
    DwarfSectionBuilder writer(bytes, little_endian);
    writer.Fixed(0x42, 1);
    auto dwarf32 = writer.BeginInitialLength(4);
    writer.Fixed(0x1234, 2);
    writer.EndInitialLength(dwarf32);
    auto dwarf64 = writer.BeginInitialLength(8);
    writer.Fixed(0xabcdef, 3);
    writer.EndInitialLength(dwarf64);

    const std::vector<uint8_t> expected =
        little_endian ? std::vector<uint8_t>{0x42, 2, 0, 0, 0, 0x34, 0x12, 0xff, 0xff, 0xff, 0xff,
                                             3,    0, 0, 0, 0, 0,    0,    0,    0xef, 0xcd, 0xab}
                      : std::vector<uint8_t>{0x42, 0, 0, 0, 2, 0x12, 0x34, 0xff, 0xff, 0xff, 0xff,
                                             0,    0, 0, 0, 0, 0,    0,    3,    0xab, 0xcd, 0xef};
    EXPECT_EQ(expected, bytes);

    DataReader reader(bytes, little_endian);
    EXPECT_EQ(0x42u, reader.Fixed(1));
    auto length32 = reader.InitialLength();
    EXPECT_EQ(2u, length32.length);
    EXPECT_EQ(4u, length32.offset_size);
    EXPECT_EQ(0x1234u, reader.Fixed(2));
    auto length64 = reader.InitialLength();
    EXPECT_EQ(3u, length64.length);
    EXPECT_EQ(8u, length64.offset_size);
    EXPECT_EQ(0xabcdefu, reader.Fixed(3));
    EXPECT_EQ(0u, reader.remaining());
    EXPECT_TRUE(reader.ok());
  }
}

TEST(DwarfSectionBuilder, StringMerging) {
  DwarfStringSectionBuilder strings;
  EXPECT_EQ(0u, strings.AddString("alpha"));
  EXPECT_EQ(6u, strings.AddString(""));
  EXPECT_EQ(0u, strings.AddString("alpha"));
  EXPECT_EQ(7u, strings.AddString("beta"));
  EXPECT_EQ(6u, strings.AddString(""));
  EXPECT_EQ(7u, strings.AddString("beta"));
  EXPECT_EQ(12u, strings.bytes().size());

  DataReader reader(strings.bytes(), true);
  EXPECT_EQ("alpha", reader.CString());
  EXPECT_TRUE(reader.CString().empty());
  EXPECT_EQ("beta", reader.CString());
  EXPECT_TRUE(reader.ok());
  EXPECT_EQ(0u, reader.remaining());
}

}  // namespace

}  // namespace zxdb
