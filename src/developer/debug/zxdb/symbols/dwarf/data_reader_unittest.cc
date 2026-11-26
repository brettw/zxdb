// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"

#include <iterator>

#include <gtest/gtest.h>

namespace zxdb {

namespace {

TEST(DataReader, Fixed) {
  constexpr uint8_t kBytes[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
  constexpr uint64_t kLittle[] = {
      0x01,         0x2301,         0x452301,         0x67452301,
      0x8967452301, 0xab8967452301, 0xcdab8967452301, 0xefcdab8967452301};
  constexpr uint64_t kBig[] = {0x01,         0x0123,         0x012345,         0x01234567,
                               0x0123456789, 0x0123456789ab, 0x0123456789abcd, 0x0123456789abcdef};
  for (size_t size = 1; size <= std::size(kBytes); size++) {
    SCOPED_TRACE(size);
    auto bytes = std::span(kBytes).first(size);
    DataReader little(bytes, Endian::kLittle);
    DataReader big(bytes, Endian::kBig);
    EXPECT_EQ(0u, little.offset());
    EXPECT_EQ(kLittle[size - 1], little.Fixed(size));
    EXPECT_EQ(kBig[size - 1], big.Fixed(size));
    EXPECT_TRUE(little.ok());
    EXPECT_TRUE(big.ok());
    EXPECT_EQ(size, little.offset());
    EXPECT_EQ(size, big.offset());
    EXPECT_EQ(0u, little.remaining());
    EXPECT_EQ(0u, little.Fixed(1));
    EXPECT_FALSE(little.ok());
    little.Seek(0);
    little.Skip(1);
    EXPECT_EQ(0u, little.Fixed(1));
    EXPECT_EQ(0u, little.Uleb());
    EXPECT_EQ(0, little.Sleb());
    EXPECT_EQ(size, little.offset());
  }
  for (size_t invalid : {0u, 9u}) {
    DataReader reader(kBytes, Endian::kLittle);
    EXPECT_EQ(0u, reader.Fixed(invalid));
    EXPECT_FALSE(reader.ok());
    EXPECT_EQ(0u, reader.offset());
  }
}

TEST(DataReader, LebExamples) {
  // DWARF 4 figures 22 and 23 and DWARF 5 tables 7.7 and 7.8 give these encodings.
  constexpr uint8_t kUleb[] = {0x02, 0x7f, 0x80, 0x01, 0x81, 0x01, 0x82, 0x01, 0xb9, 0x64};
  constexpr uint8_t kSleb[] = {0x7e, 0x81, 0x7f, 0x80, 0x7f, 0xff, 0x7e, 0xff, 0x00};
  for (Endian endian : {Endian::kLittle, Endian::kBig}) {
    DataReader uleb(kUleb, endian);
    for (uint64_t value : {2, 127, 128, 129, 130, 12857})
      EXPECT_EQ(value, uleb.Uleb());
    EXPECT_TRUE(uleb.ok());
    EXPECT_EQ(0u, uleb.remaining());

    DataReader sleb(kSleb, endian);
    for (int64_t value : {-2, -127, -128, -129, 127})
      EXPECT_EQ(value, sleb.Sleb());
    EXPECT_TRUE(sleb.ok());
    EXPECT_EQ(0u, sleb.remaining());
  }
}

TEST(DataReader, LebExtremes) {
  constexpr uint8_t kUmax[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01};
  constexpr uint8_t kSmin[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7f};
  constexpr uint8_t kSmax[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
  DataReader umax(kUmax, Endian::kLittle);
  DataReader smin(kSmin, Endian::kLittle);
  DataReader smax(kSmax, Endian::kLittle);
  EXPECT_EQ(UINT64_MAX, umax.Uleb());
  EXPECT_EQ(INT64_MIN, smin.Sleb());
  EXPECT_EQ(INT64_MAX, smax.Sleb());
  for (const DataReader& reader : {umax, smin, smax}) {
    EXPECT_TRUE(reader.ok());
    EXPECT_EQ(10u, reader.offset());
  }
}

TEST(DataReader, LebSignAndZeroExtension) {
  constexpr uint8_t kUnsigned[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                   0xff, 0xff, 0xff, 0x81, 0x80, 0x00};
  constexpr uint8_t kPositive[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                   0xff, 0xff, 0xff, 0x80, 0x80, 0x00};
  constexpr uint8_t kNegative[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                                   0x80, 0x80, 0x80, 0xff, 0xff, 0x7f};
  DataReader unsigned_reader(kUnsigned, Endian::kLittle);
  DataReader positive_reader(kPositive, Endian::kLittle);
  DataReader negative_reader(kNegative, Endian::kLittle);
  EXPECT_EQ(UINT64_MAX, unsigned_reader.Uleb());
  EXPECT_EQ(INT64_MAX, positive_reader.Sleb());
  EXPECT_EQ(INT64_MIN, negative_reader.Sleb());
  for (const DataReader& reader : {unsigned_reader, positive_reader, negative_reader}) {
    EXPECT_TRUE(reader.ok());
    EXPECT_EQ(12u, reader.offset());
  }
}

TEST(DataReader, LebOverflow) {
  constexpr uint8_t kUnsigned[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02};
  constexpr uint8_t kSigned[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
  constexpr uint8_t kUnsignedPadding[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                                          0x80, 0x80, 0x80, 0x81, 0x01};
  constexpr uint8_t kPositivePadding[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                                          0x80, 0x80, 0x80, 0x80, 0x7f};
  constexpr uint8_t kNegativePadding[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                                          0x80, 0x80, 0x80, 0xff, 0x00};
  for (auto bytes :
       {std::span<const uint8_t>(kUnsigned), std::span<const uint8_t>(kUnsignedPadding)}) {
    DataReader read(bytes, Endian::kLittle);
    EXPECT_EQ(0u, read.Uleb());
    EXPECT_FALSE(read.ok());
    EXPECT_EQ(0u, read.offset());
    DataReader skip(bytes, Endian::kLittle);
    skip.SkipUleb();
    EXPECT_FALSE(skip.ok());
    EXPECT_EQ(0u, skip.offset());
  }
  for (auto bytes : {std::span<const uint8_t>(kSigned), std::span<const uint8_t>(kPositivePadding),
                     std::span<const uint8_t>(kNegativePadding)}) {
    DataReader read(bytes, Endian::kLittle);
    EXPECT_EQ(0, read.Sleb());
    EXPECT_FALSE(read.ok());
    EXPECT_EQ(0u, read.offset());
    DataReader skip(bytes, Endian::kLittle);
    skip.SkipSleb();
    EXPECT_FALSE(skip.ok());
    EXPECT_EQ(0u, skip.offset());
  }
}

TEST(DataReader, LebSkipAndTruncation) {
  constexpr uint8_t kBytes[] = {0x81, 0x01, 0xff, 0x7e, 0x80, 0x80, 0x00};
  DataReader reader(kBytes, Endian::kLittle);
  reader.SkipUleb();
  EXPECT_EQ(2u, reader.offset());
  reader.SkipSleb();
  EXPECT_EQ(4u, reader.offset());
  EXPECT_TRUE(reader.ok());

  // The terminating byte lies outside the limit.
  reader.set_limit(6);
  DataReader signed_reader = reader;
  EXPECT_EQ(0u, reader.Uleb());
  EXPECT_EQ(0, signed_reader.Sleb());
  EXPECT_FALSE(reader.ok());
  EXPECT_FALSE(signed_reader.ok());
  EXPECT_EQ(4u, reader.offset());
  EXPECT_EQ(4u, signed_reader.offset());
}

TEST(DataReader, InitialLength) {
  // DWARF 5 section 7.4 specifies the initial length encoding.
  constexpr uint8_t k32[] = {0xef, 0xff, 0xff, 0xff};
  constexpr uint8_t k64[] = {0xff, 0xff, 0xff, 0xff, 0x01, 0x23,
                             0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
  DataReader dwarf32(k32, Endian::kLittle);
  auto length = dwarf32.InitialLength();
  EXPECT_EQ(0xffffffefu, length.length);
  EXPECT_EQ(4u, length.offset_size);
  EXPECT_EQ(4u, dwarf32.offset());
  EXPECT_TRUE(dwarf32.ok());

  DataReader dwarf64(k64, Endian::kBig);
  length = dwarf64.InitialLength();
  EXPECT_EQ(0x0123456789abcdefu, length.length);
  EXPECT_EQ(8u, length.offset_size);
  EXPECT_EQ(12u, dwarf64.offset());
  EXPECT_TRUE(dwarf64.ok());

  constexpr uint8_t kEmpty[] = {0, 0, 0, 0};
  DataReader empty(kEmpty, Endian::kLittle);
  length = empty.InitialLength();
  EXPECT_EQ(0u, length.length);
  EXPECT_EQ(4u, length.offset_size);
  EXPECT_TRUE(empty.ok());

  for (unsigned low = 0xf0; low < 0xff; low++) {
    SCOPED_TRACE(low);
    const uint8_t reserved[] = {static_cast<uint8_t>(low), 0xff, 0xff, 0xff};
    DataReader reader(reserved, Endian::kLittle);
    length = reader.InitialLength();
    EXPECT_EQ(0u, length.length);
    EXPECT_EQ(0u, length.offset_size);
    EXPECT_FALSE(reader.ok());
    EXPECT_EQ(0u, reader.offset());
  }

  DataReader truncated(std::span(k64).first(11), Endian::kBig);
  length = truncated.InitialLength();
  EXPECT_EQ(0u, length.length);
  EXPECT_EQ(0u, length.offset_size);
  EXPECT_FALSE(truncated.ok());
  EXPECT_EQ(0u, truncated.offset());
}

TEST(DataReader, OffsetAndAddressSizes) {
  constexpr uint8_t kBytes[] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
  constexpr uint64_t kExpected[] = {0x12, 0x1234, 0x12345678, 0x123456789abcdef0};
  constexpr size_t kSizes[] = {1, 2, 4, 8};
  for (size_t i = 0; i < std::size(kSizes); i++) {
    DataReader reader(kBytes, Endian::kBig);
    EXPECT_EQ(kExpected[i], reader.Address(kSizes[i]));
    EXPECT_TRUE(reader.ok());
    EXPECT_EQ(kSizes[i], reader.offset());
    reader.Seek(0);
    if (kSizes[i] == 4 || kSizes[i] == 8) {
      EXPECT_EQ(kExpected[i], reader.Offset(kSizes[i]));
      EXPECT_TRUE(reader.ok());
      EXPECT_EQ(kSizes[i], reader.offset());
    }
  }
  for (size_t invalid : {0u, 1u, 2u, 3u, 5u, 6u, 7u, 9u}) {
    DataReader reader(kBytes, Endian::kLittle);
    EXPECT_EQ(0u, reader.Offset(invalid));
    EXPECT_FALSE(reader.ok());
    EXPECT_EQ(0u, reader.offset());
  }
  for (size_t invalid : {0u, 3u, 5u, 6u, 7u, 9u}) {
    DataReader reader(kBytes, Endian::kLittle);
    EXPECT_EQ(0u, reader.Address(invalid));
    EXPECT_FALSE(reader.ok());
    EXPECT_EQ(0u, reader.offset());
  }
}

TEST(DataReader, CString) {
  constexpr uint8_t kBytes[] = {0, 'a', 'b', 0};
  DataReader reader(kBytes, Endian::kLittle);
  EXPECT_TRUE(reader.CString().empty());
  EXPECT_EQ(1u, reader.offset());
  EXPECT_TRUE(reader.ok());
  EXPECT_EQ("ab", reader.CString());
  EXPECT_EQ(4u, reader.offset());
  EXPECT_TRUE(reader.ok());
  EXPECT_TRUE(reader.CString().empty());
  EXPECT_FALSE(reader.ok());

  DataReader missing_nul(kBytes, Endian::kLittle);
  missing_nul.Seek(1);
  missing_nul.set_limit(3);
  EXPECT_TRUE(missing_nul.CString().empty());
  EXPECT_FALSE(missing_nul.ok());
  EXPECT_EQ(1u, missing_nul.offset());
}

TEST(DataReader, Bytes) {
  constexpr uint8_t kBytes[] = {1, 2, 3, 4};
  DataReader reader(kBytes, Endian::kLittle);
  reader.Skip(1);
  auto bytes = reader.Bytes(2);
  EXPECT_EQ(kBytes + 1, bytes.data());
  EXPECT_EQ(2u, bytes.size());
  EXPECT_EQ(3u, reader.offset());
  EXPECT_EQ(1u, reader.remaining());
  EXPECT_TRUE(reader.Bytes(UINT64_MAX).empty());
  EXPECT_FALSE(reader.ok());
  EXPECT_TRUE(reader.Bytes(1).empty());
  EXPECT_EQ(3u, reader.offset());
}

TEST(DataReader, SeekAndLimit) {
  constexpr uint8_t kBytes[] = {1, 2, 3, 4};
  DataReader reader(kBytes, Endian::kLittle);
  reader.set_limit(3);
  reader.set_limit(UINT64_MAX);
  EXPECT_EQ(3u, reader.remaining());
  reader.Seek(3);
  EXPECT_TRUE(reader.ok());
  EXPECT_EQ(0u, reader.remaining());
  reader.Skip(0);
  EXPECT_TRUE(reader.Bytes(0).empty());
  EXPECT_TRUE(reader.ok());

  reader.Seek(1);
  EXPECT_EQ(2u, reader.Fixed(1));
  reader.Seek(4);
  EXPECT_FALSE(reader.ok());
  EXPECT_EQ(2u, reader.offset());
  reader.Seek(0);
  EXPECT_EQ(2u, reader.offset());

  DataReader huge(kBytes, Endian::kLittle);
  huge.Skip(1);
  huge.Skip(UINT64_MAX);
  EXPECT_FALSE(huge.ok());
  EXPECT_EQ(1u, huge.offset());

  DataReader lowered(kBytes, Endian::kLittle);
  lowered.Seek(2);
  lowered.set_limit(1);
  EXPECT_FALSE(lowered.ok());
  EXPECT_EQ(2u, lowered.offset());
  EXPECT_EQ(0u, lowered.remaining());

  DataReader empty({}, Endian::kLittle);
  EXPECT_TRUE(empty.ok());
  EXPECT_EQ(0u, empty.Uleb());
  EXPECT_FALSE(empty.ok());
}

}  // namespace

}  // namespace zxdb
