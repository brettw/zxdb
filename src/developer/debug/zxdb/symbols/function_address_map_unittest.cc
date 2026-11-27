// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string_view>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/dwarf_binary_impl.h"
#include "src/developer/debug/zxdb/symbols/dwarf_unit.h"
#include "src/developer/debug/zxdb/symbols/function.h"
#include "src/developer/debug/zxdb/symbols/lazy_symbol.h"
#include "src/developer/debug/zxdb/symbols/module_symbols_impl.h"
#include "src/developer/debug/zxdb/symbols/test_symbol_module.h"

namespace zxdb {

TEST(FunctionAddressMap, InlinesAndCrossingRanges) {
  TestSymbolModule setup(
      TestSymbolModule::GetTestDataDir() + "function_address/function_address.so", "");
  ASSERT_TRUE(setup.Init("", false).ok());
  auto unit = setup.symbols()->binary()->GetUnitAtIndex(UnitIndex(false, 0));
  ASSERT_TRUE(unit);
  EXPECT_EQ(0u, unit->GetOffset());

  // These addresses come from the low and high PCs and the range list in the assembly fixture.
  const struct {
    uint64_t address;
    std::string_view name;
  } kCases[] = {
      {0, {}},
      {0x0fff, {}},
      {0x1000, "outer"},
      {0x100f, "outer"},
      {0x1010, "innermost"},
      {0x1014, "innermost"},
      {0x1015, "inlined"},
      {0x1017, "crossing"},
      {0x1020, "crossing"},
      {0x1028, "crossing"},
      {0x1029, "inlined"},
      {0x1030, "outer"},
      {0x104f, "outer"},
      {0x1050, {}},
      {0x1060, {}},
      {0x1080, "disjoint"},
      {0x1083, "disjoint"},
      {0x1084, {}},
  };
  for (const auto& [address, name] : kCases) {
    SCOPED_TRACE(address);
    LazySymbol result = unit->FunctionForRelativeAddress(address);
    if (name.empty()) {
      EXPECT_FALSE(result.is_valid());
    } else {
      ASSERT_TRUE(result.is_valid());
      EXPECT_EQ(name, result.Get()->GetAssignedName());
    }
  }

  // The address map survives DIE table eviction. Resolution can rebuild the table from an offset.
  unit->ClearDieCache();
  EXPECT_EQ("innermost", unit->FunctionForRelativeAddress(0x1010).Get()->GetAssignedName());
}

TEST(FunctionAddressMap, SeparateDwoUsesSkeletonAddresses) {
  const std::string dir = TestSymbolModule::GetTestDataDir() + "fission/";
  TestSymbolModule setup(dir + "fission", "");
  ASSERT_TRUE(setup.Init(dir, true).ok());
  const SymbolContext context(0x10000000);

  // main.dwo gives main's low PC as an index into the skeleton's address table. The fixture's
  // address table and main's high PC offset give the half-open range [0x1165, 0x1184).
  constexpr uint64_t kBegin = 0x1165;
  constexpr uint64_t kEnd = 0x1184;
  auto found = setup.symbols()->GetDwarfUnitForAddress(context, context.RelativeToAbsolute(kBegin));
  ASSERT_TRUE(found);
  ASSERT_TRUE(found.details_unit->GetUnitDie(DwarfDie::Load::kRootOnly).unit_is_dwo());
  EXPECT_FALSE(found.details_unit->FunctionForRelativeAddress(kBegin - 1).is_valid());
  EXPECT_EQ("main",
            found.details_unit->FunctionForRelativeAddress(kBegin).Get()->GetAssignedName());
  EXPECT_EQ("main",
            found.details_unit->FunctionForRelativeAddress(kEnd - 1).Get()->GetAssignedName());
  EXPECT_FALSE(found.details_unit->FunctionForRelativeAddress(kEnd).is_valid());
}

TEST(FunctionAddressMap, CompilerGeneratedNestedInlines) {
  TestSymbolModule setup(
      TestSymbolModule::GetTestDataDir() + "inline_params/clang_inline_params.so", "");
  ASSERT_TRUE(setup.Init("", false).ok());
  const auto& symbols = setup.symbols()->binary()->GetELFSymbols();
  auto unit = setup.symbols()->binary()->GetUnitAtIndex(UnitIndex(false, 0));
  ASSERT_TRUE(unit);

  // The fixture places an ELF label in the caller, in its inline callee, and one level deeper.
  const struct {
    const char* marker;
    const char* name;
    DwarfTag tag;
  } kCases[] = {
      {"NestedInlineCallerMarker", "NestedInlineCaller", DwarfTag::kSubprogram},
      {"NestedInlineMiddleMarker", "NestedInlineMiddle", DwarfTag::kInlinedSubroutine},
      {"NestedInlineInnerMarker", "NestedInlineInner", DwarfTag::kInlinedSubroutine},
  };
  for (const auto& [marker, name, tag] : kCases) {
    SCOPED_TRACE(marker);
    auto symbol = symbols.find(marker);
    ASSERT_NE(symbols.end(), symbol);
    LazySymbol result = unit->FunctionForRelativeAddress(symbol->second.value);
    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(name, result.Get()->GetAssignedName());
    EXPECT_EQ(tag, result.Get()->tag());
  }
}

}  // namespace zxdb
