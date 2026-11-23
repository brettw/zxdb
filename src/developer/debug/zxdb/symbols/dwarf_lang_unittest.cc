// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_lang.h"

#include <gtest/gtest.h>

namespace zxdb {

TEST(DwarfLang, DwarfLangDefaultLowerBound) {
  // Spot check DWARF 5 Table 7.17.
  EXPECT_EQ(0, DwarfLangDefaultLowerBound(DwarfLang::kC));
  EXPECT_EQ(0, DwarfLangDefaultLowerBound(DwarfLang::kCpp14));
  EXPECT_EQ(0, DwarfLangDefaultLowerBound(DwarfLang::kRust));
  EXPECT_EQ(1, DwarfLangDefaultLowerBound(DwarfLang::kFortran90));
  EXPECT_EQ(1, DwarfLangDefaultLowerBound(DwarfLang::kAda95));
  EXPECT_EQ(1, DwarfLangDefaultLowerBound(DwarfLang::kJulia));
  EXPECT_EQ(1, DwarfLangDefaultLowerBound(DwarfLang::kModula3));

  // Languages that the table doesn't list.
  EXPECT_EQ(0, DwarfLangDefaultLowerBound(DwarfLang::kNone));
  EXPECT_EQ(0, DwarfLangDefaultLowerBound(static_cast<DwarfLang>(0x8000)));
}

}  // namespace zxdb
