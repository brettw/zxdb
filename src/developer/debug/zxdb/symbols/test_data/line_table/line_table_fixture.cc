// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Source for the line table test binaries. The build compiles it with several compilers, DWARF
// versions, and optimization levels. See line_program_binary_unittest.cc.

#include "src/developer/debug/zxdb/symbols/test_data/line_table/line_table_fixture.h"

namespace {

// Loops at -O2 give discriminators, is_stmt changes, and line 0 rows.
__attribute__((noinline)) int Loop(int count) {
  int result = 0;
  for (int i = 0; i < count; i++) {
    if (i % 3 == 0)
      result += i;
    else if (i % 3 == 1)
      result ^= i;
    else
      result -= 1;
  }
  return result;
}

}  // namespace

#line 500 "line_table_fixture_virtual.inc"
__attribute__((noinline)) int LineTableFixtureVirtual(int x) {
  return x * 3 + 1;
}
#line 37 "line_table_fixture.cc"

int LineTableFixtureEntry(int x) {
  int values[4] = {x, -x, x + 1, x - 2};
  int sum = LineTableFixtureSum(values, 4);
  double dvalues[2] = {1.5, static_cast<double>(x)};
  sum += static_cast<int>(LineTableFixtureSum(dvalues, 2));
  return sum + Loop(x) + LineTableFixtureVirtual(x);
}
