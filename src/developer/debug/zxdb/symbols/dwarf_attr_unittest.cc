// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_attr.h"

#include <gtest/gtest.h>

namespace zxdb {

TEST(DwarfAttr, DwarfAttrToString) {
  // Spot check a few to make sure the string array is in-sync.
  EXPECT_EQ("<none>", DwarfAttrToString(DwarfAttr::kNone));
  EXPECT_EQ("DW_AT_sibling (0x01)", DwarfAttrToString(DwarfAttr::kSibling, true));
  EXPECT_EQ("<undefined (0x4)>", DwarfAttrToString(static_cast<DwarfAttr>(0x04)));
  EXPECT_EQ("DW_AT_type", DwarfAttrToString(DwarfAttr::kType));
  EXPECT_EQ("DW_AT_linkage_name", DwarfAttrToString(DwarfAttr::kLinkageName));  // Last DWARF 4.
  EXPECT_EQ("<undefined (0x75)>", DwarfAttrToString(static_cast<DwarfAttr>(0x75)));  // Reserved.
  EXPECT_EQ("DW_AT_loclists_base", DwarfAttrToString(DwarfAttr::kLoclistsBase));  // Last DWARF 5.

  // Vendor attributes with constants.
  EXPECT_EQ("DW_AT_GNU_dwo_name", DwarfAttrToString(DwarfAttr::kGnuDwoName));
  EXPECT_EQ("DW_AT_GNU_dwo_name (0x2130)", DwarfAttrToString(DwarfAttr::kGnuDwoName, true));

  // Test some out-of-range ones.
  EXPECT_EQ("<undefined (0x8d)>", DwarfAttrToString(static_cast<DwarfAttr>(0x8d), true));
  EXPECT_EQ("<undefined (0x2000)>", DwarfAttrToString(DwarfAttr::kLoUser, false));
  EXPECT_EQ("<undefined (0x3fff)>", DwarfAttrToString(DwarfAttr::kHiUser, false));
}

}  // namespace zxdb
