// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_TEST_DATA_LINE_TABLE_LINE_TABLE_FIXTURE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_TEST_DATA_LINE_TABLE_LINE_TABLE_FIXTURE_H_

// Inline code from a header gives the line tables rows for more than one file.
template <typename T>
inline T LineTableFixtureSum(const T* values, int count) {
  T sum = 0;
  for (int i = 0; i < count; i++) {
    if (values[i] > 0)
      sum += values[i];
    else
      sum -= values[i];
  }
  return sum;
}

// The build hides symbols by default. This is the exported entry point that keeps the code.
__attribute__((visibility("default"))) int LineTableFixtureEntry(int x);

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_TEST_DATA_LINE_TABLE_LINE_TABLE_FIXTURE_H_
