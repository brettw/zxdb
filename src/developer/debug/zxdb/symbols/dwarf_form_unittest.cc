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

TEST(DwarfForm, DwarfFormIsBlock) {
  EXPECT_TRUE(DwarfFormIsBlock(DwarfForm::kBlock));
  EXPECT_TRUE(DwarfFormIsBlock(DwarfForm::kBlock1));
  EXPECT_TRUE(DwarfFormIsBlock(DwarfForm::kBlock2));
  EXPECT_TRUE(DwarfFormIsBlock(DwarfForm::kBlock4));

  EXPECT_FALSE(DwarfFormIsBlock(DwarfForm::kNone));
  EXPECT_FALSE(DwarfFormIsBlock(DwarfForm::kExprloc));
  EXPECT_FALSE(DwarfFormIsBlock(DwarfForm::kData16));
}

TEST(DwarfForm, DwarfFormIsConstant) {
  for (DwarfForm form :
       {DwarfForm::kData1, DwarfForm::kData2, DwarfForm::kData4, DwarfForm::kData8,
        DwarfForm::kData16, DwarfForm::kSdata, DwarfForm::kUdata, DwarfForm::kImplicitConst}) {
    EXPECT_TRUE(DwarfFormIsConstant(form)) << DwarfFormToString(form);
  }

  EXPECT_FALSE(DwarfFormIsConstant(DwarfForm::kNone));
  EXPECT_FALSE(DwarfFormIsConstant(DwarfForm::kFlag));
  EXPECT_FALSE(DwarfFormIsConstant(DwarfForm::kAddr));
  EXPECT_FALSE(DwarfFormIsConstant(DwarfForm::kRef4));
  EXPECT_FALSE(DwarfFormIsConstant(DwarfForm::kSecOffset));
  EXPECT_FALSE(DwarfFormIsConstant(DwarfForm::kBlock1));
}

TEST(DwarfForm, DwarfFormIsAddress) {
  EXPECT_TRUE(DwarfFormIsAddress(DwarfForm::kAddr));
  EXPECT_TRUE(DwarfFormIsAddress(DwarfForm::kAddrx));
  EXPECT_TRUE(DwarfFormIsAddress(DwarfForm::kAddrx1));
  EXPECT_TRUE(DwarfFormIsAddress(DwarfForm::kAddrx4));
  EXPECT_TRUE(DwarfFormIsAddress(DwarfForm::kGnuAddrIndex));
  EXPECT_TRUE(DwarfFormIsAddress(DwarfForm::kLlvmAddrxOffset));

  EXPECT_FALSE(DwarfFormIsAddress(DwarfForm::kNone));
  EXPECT_FALSE(DwarfFormIsAddress(DwarfForm::kData8));
  EXPECT_FALSE(DwarfFormIsAddress(DwarfForm::kUdata));
  EXPECT_FALSE(DwarfFormIsAddress(DwarfForm::kRefAddr));
  EXPECT_FALSE(DwarfFormIsAddress(DwarfForm::kSecOffset));
}

}  // namespace zxdb
