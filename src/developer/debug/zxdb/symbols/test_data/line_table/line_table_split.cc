// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Source for the split-DWARF line table fixture. With -fdebug-types-section, the .dwo file has a type
// unit for LineTableSplitPoint whose DW_AT_stmt_list references the .debug_line.dwo table. See
// line_program_binary_unittest.cc.

struct LineTableSplitPoint {
  int x;
  int y;
};

int LineTableSplitSum(LineTableSplitPoint point) { return point.x + point.y; }
