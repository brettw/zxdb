// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/expr/x87_float.h"

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

namespace zxdb {

namespace {

// Returns the 16-byte x87 slot for a significand and a sign/exponent word.
std::vector<uint8_t> MakeX87(uint64_t significand, uint16_t sign_exponent) {
  std::vector<uint8_t> result(16);
  memcpy(result.data(), &significand, sizeof(significand));
  memcpy(result.data() + 8, &sign_exponent, sizeof(sign_exponent));
  return result;
}

}  // namespace

TEST(X87Float, IsX87ExtendedFloat) {
  EXPECT_TRUE(IsX87ExtendedFloat(10, debug::Arch::kArm64));
  EXPECT_TRUE(IsX87ExtendedFloat(16, debug::Arch::kX64));
  EXPECT_FALSE(IsX87ExtendedFloat(16, debug::Arch::kArm64));
  EXPECT_FALSE(IsX87ExtendedFloat(16, debug::Arch::kUnknown));
  EXPECT_FALSE(IsX87ExtendedFloat(8, debug::Arch::kX64));
}

TEST(X87Float, ToDouble) {
  EXPECT_EQ(7.5, X87ExtendedToDouble(MakeX87(0xf000'0000'0000'0000, 0x4001)));
  EXPECT_EQ(1.0, X87ExtendedToDouble(MakeX87(0x8000'0000'0000'0000, 0x3fff)));
  EXPECT_EQ(-2.0, X87ExtendedToDouble(MakeX87(0x8000'0000'0000'0000, 0xc000)));
  EXPECT_EQ(0.0, X87ExtendedToDouble(MakeX87(0, 0)));
  EXPECT_TRUE(std::signbit(X87ExtendedToDouble(MakeX87(0, 0x8000))));
  EXPECT_EQ(-std::numeric_limits<double>::infinity(),
            X87ExtendedToDouble(MakeX87(0x8000'0000'0000'0000, 0xffff)));
  EXPECT_TRUE(std::isnan(X87ExtendedToDouble(MakeX87(0xc000'0000'0000'0000, 0x7fff))));

  // Values outside the double range become infinity or zero.
  EXPECT_EQ(std::numeric_limits<double>::infinity(),
            X87ExtendedToDouble(MakeX87(0x8000'0000'0000'0000, 0x7ffe)));
  EXPECT_EQ(0.0, X87ExtendedToDouble(MakeX87(0x8000'0000'0000'0000, 0x0001)));

  // A 10-byte value has no padding.
  std::vector<uint8_t> ten = MakeX87(0xf000'0000'0000'0000, 0x4001);
  ten.resize(10);
  EXPECT_EQ(7.5, X87ExtendedToDouble(ten));
}

TEST(X87Float, FromDouble) {
  EXPECT_EQ(MakeX87(0xf000'0000'0000'0000, 0x4001), DoubleToX87Extended(7.5, 16));
  EXPECT_EQ(MakeX87(0xa000'0000'0000'0000, 0xc002), DoubleToX87Extended(-10.0, 16));
  EXPECT_EQ(MakeX87(0, 0), DoubleToX87Extended(0.0, 16));
  EXPECT_EQ(MakeX87(0x8000'0000'0000'0000, 0x7fff),
            DoubleToX87Extended(std::numeric_limits<double>::infinity(), 16));
  EXPECT_EQ(10u, DoubleToX87Extended(1.0, 10).size());

  // Doubles convert exactly, including denormals.
  for (double value : {0.1, -1e300, 1e-310, std::numeric_limits<double>::max(),
                       std::numeric_limits<double>::denorm_min()}) {
    EXPECT_EQ(value, X87ExtendedToDouble(DoubleToX87Extended(value, 16))) << value;
  }
  EXPECT_TRUE(std::isnan(
      X87ExtendedToDouble(DoubleToX87Extended(std::numeric_limits<double>::quiet_NaN(), 16))));
}

}  // namespace zxdb
