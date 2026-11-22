// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_op.h"

#include <gtest/gtest.h>

namespace zxdb {

TEST(DwarfOp, DwarfOpToString) {
  // Spot check a few to make sure the string array is in-sync.
  EXPECT_EQ("<undefined (0x0)>", DwarfOpToString(0x00));
  EXPECT_EQ("DW_OP_addr (0x03)", DwarfOpToString(DwarfOp::kAddr, true));
  EXPECT_EQ("<undefined (0x4)>", DwarfOpToString(0x04));
  EXPECT_EQ("DW_OP_lit0", DwarfOpToString(DwarfOp::kLit0));
  EXPECT_EQ("DW_OP_reg31", DwarfOpToString(DwarfOp::kReg31));
  EXPECT_EQ("DW_OP_breg0", DwarfOpToString(DwarfOp::kBreg0));
  EXPECT_EQ("DW_OP_implicit_value", DwarfOpToString(DwarfOp::kImplicitValue));  // Last DWARF 4.
  EXPECT_EQ("DW_OP_reinterpret", DwarfOpToString(DwarfOp::kReinterpret));       // Last DWARF 5.

  // Vendor opcodes with constants.
  EXPECT_EQ("DW_OP_GNU_push_tls_address", DwarfOpToString(DwarfOp::kGnuPushTlsAddress));
  EXPECT_EQ("DW_OP_GNU_variable_value (0xfd)", DwarfOpToString(DwarfOp::kGnuVariableValue, true));

  // Test some out-of-range ones.
  EXPECT_EQ("<undefined (0xaa)>", DwarfOpToString(0xaa, true));
  EXPECT_EQ("<undefined (0xe1)>", DwarfOpToString(DwarfOp::kLoUser + 1));
  EXPECT_EQ("<undefined (0xff)>", DwarfOpToString(DwarfOp::kHiUser));
}

}  // namespace zxdb
