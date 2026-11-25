// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/expr/x87_float.h"

#include <string.h>

#include <cmath>
#include <limits>

#include "lib/syslog/cpp/macros.h"

namespace zxdb {

namespace {

constexpr uint16_t kSignBit = 0x8000;
constexpr uint16_t kMaxExponent = 0x7fff;
constexpr int kExponentBias = 16383;
constexpr uint64_t kIntegerBit = 0x8000'0000'0000'0000;

}  // namespace

bool IsX87ExtendedFloat(size_t byte_size, debug::Arch arch) {
  return byte_size == kX87ExtendedSize || (byte_size == 16 && arch == debug::Arch::kX64);
}

double X87ExtendedToDouble(std::span<const uint8_t> data) {
  FX_DCHECK(data.size() >= kX87ExtendedSize);
  uint64_t significand = 0;
  uint16_t sign_exponent = 0;
  memcpy(&significand, data.data(), sizeof(significand));
  memcpy(&sign_exponent, data.data() + sizeof(significand), sizeof(sign_exponent));

  bool negative = sign_exponent & kSignBit;
  int exponent = sign_exponent & kMaxExponent;
  double result = 0.0;
  if (exponent == kMaxExponent) {
    // The fraction bits below the integer bit tell infinity from NaN.
    result = (significand << 1) == 0 ? std::numeric_limits<double>::infinity()
                                     : std::numeric_limits<double>::quiet_NaN();
  } else {
    // The value is significand * 2^(exponent - bias - 63). Denormals use the minimum exponent.
    int unbiased = (exponent == 0 ? 1 : exponent) - kExponentBias - 63;
    result = std::ldexp(static_cast<double>(significand), unbiased);
  }
  return negative ? -result : result;
}

std::vector<uint8_t> DoubleToX87Extended(double value, size_t byte_size) {
  FX_DCHECK(byte_size >= kX87ExtendedSize);
  uint16_t sign_exponent = std::signbit(value) ? kSignBit : 0;
  uint64_t significand = 0;
  if (std::isnan(value)) {
    sign_exponent |= kMaxExponent;
    significand = kIntegerBit | (kIntegerBit >> 1);  // Quiet NaN.
  } else if (std::isinf(value)) {
    sign_exponent |= kMaxExponent;
    significand = kIntegerBit;
  } else if (value != 0.0) {
    // frexp() gives |value| = fraction * 2^exponent with fraction in [0.5, 1). The significand is
    // the fraction scaled to 64 bits, so its integer bit is set and the exponent is one less.
    int exponent = 0;
    double fraction = std::frexp(std::fabs(value), &exponent);
    significand = static_cast<uint64_t>(std::ldexp(fraction, 64));
    sign_exponent |= static_cast<uint16_t>(exponent - 1 + kExponentBias);
  }

  std::vector<uint8_t> result(byte_size);
  memcpy(result.data(), &significand, sizeof(significand));
  memcpy(result.data() + sizeof(significand), &sign_exponent, sizeof(sign_exponent));
  return result;
}

}  // namespace zxdb
