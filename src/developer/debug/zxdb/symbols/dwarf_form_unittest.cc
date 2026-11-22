// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_form.h"

#include <gtest/gtest.h>

namespace zxdb {

TEST(DwarfForm, DwarfFormToString) {
  // Spot check a few to make sure the string array is in-sync.
  EXPECT_EQ("<none>", DwarfFormToString(DwarfForm::kNone));
  EXPECT_EQ("DW_FORM_addr (0x01)", DwarfFormToString(DwarfForm::kAddr, true));
  EXPECT_EQ("<undefined (0x2)>", DwarfFormToString(static_cast<DwarfForm>(0x02)));
  EXPECT_EQ("DW_FORM_ref_sig8", DwarfFormToString(DwarfForm::kRefSig8));  // Last DWARF 4.
  EXPECT_EQ("DW_FORM_addrx4", DwarfFormToString(DwarfForm::kAddrx4));     // Last DWARF 5.

  // Vendor forms with constants.
  EXPECT_EQ("DW_FORM_GNU_addr_index", DwarfFormToString(DwarfForm::kGnuAddrIndex));
  EXPECT_EQ("DW_FORM_LLVM_addrx_offset (0x2001)",
            DwarfFormToString(DwarfForm::kLlvmAddrxOffset, true));

  // Test some out-of-range ones.
  EXPECT_EQ("<undefined (0x2d)>", DwarfFormToString(static_cast<DwarfForm>(0x2d), true));
  EXPECT_EQ("<undefined (0x1f00)>", DwarfFormToString(static_cast<DwarfForm>(0x1f00)));
}

}  // namespace zxdb
