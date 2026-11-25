// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_EXPR_X87_FLOAT_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_EXPR_X87_FLOAT_H_

#include <stddef.h>
#include <stdint.h>

#include <span>
#include <vector>

#include "src/developer/debug/shared/arch.h"

namespace zxdb {

// The x87 extended-precision format stores a floating-point value in 80 bits: a 64-bit significand
// with an explicit integer bit, a 15-bit exponent, and a sign bit.
constexpr size_t kX87ExtendedSize = 10;

// Returns true when a floating-point value of the given size uses the x87 extended format. A
// 10-byte value always does. On x86-64, a 16-byte value (long double) has the format in its low 10
// bytes. Other architectures use IEEE quadruple precision for 16-byte values.
bool IsX87ExtendedFloat(size_t byte_size, debug::Arch arch);

// Converts an x87 extended value to a double. The data must have at least 10 bytes. A value that
// needs more precision is rounded, and a value outside the double range becomes zero or infinity.
double X87ExtendedToDouble(std::span<const uint8_t> data);

// Converts a double to the x87 extended format. The conversion is exact. Bytes after the first 10
// are zero.
std::vector<uint8_t> DoubleToX87Extended(double value, size_t byte_size);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_EXPR_X87_FLOAT_H_
