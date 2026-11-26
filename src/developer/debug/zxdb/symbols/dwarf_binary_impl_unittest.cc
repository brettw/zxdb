// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_binary_impl.h"

#include <elf.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/compile_unit.h"
#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"
#include "src/developer/debug/zxdb/symbols/dwarf_tag.h"
#include "src/developer/debug/zxdb/symbols/dwarf_unit.h"
#include "src/developer/debug/zxdb/symbols/function.h"
#include "src/developer/debug/zxdb/symbols/line_program_test_util.h"
#include "src/developer/debug/zxdb/symbols/module_indexer.h"
#include "src/developer/debug/zxdb/symbols/module_symbols_impl.h"
#include "src/developer/debug/zxdb/symbols/test_symbol_module.h"
#include "src/developer/debug/zxdb/symbols/variable.h"
#include "src/lib/elflib/elflib.h"
#include "src/lib/files/file.h"
#include "src/lib/files/scoped_temp_dir.h"

namespace zxdb {

namespace {

using ElfTableSymbol = DwarfBinary::ElfTableSymbol;

// Returns the symbol with the given name, or fails the test.
const ElfTableSymbol* GetSymbol(const std::map<std::string, ElfTableSymbol>& symbols,
                                const std::string& name) {
  auto found = symbols.find(name);
  if (found == symbols.end()) {
    ADD_FAILURE() << "No symbol " << name;
    return nullptr;
  }
  return &found->second;
}

// Changes the size of the named symbol in the ".dynsym" section of a 64-bit ELF file. Returns false
// if the file doesn't have the symbol.
bool SetDynamicSymbolSize(std::string& elf, std::string_view name, uint64_t size) {
  Elf64_Ehdr ehdr;
  memcpy(&ehdr, elf.data(), sizeof(ehdr));
  auto get_section = [&](size_t index) {
    Elf64_Shdr shdr;
    memcpy(&shdr, elf.data() + ehdr.e_shoff + index * ehdr.e_shentsize, sizeof(shdr));
    return shdr;
  };

  for (size_t i = 0; i < ehdr.e_shnum; i++) {
    Elf64_Shdr symtab = get_section(i);
    if (symtab.sh_type != SHT_DYNSYM)
      continue;

    Elf64_Shdr strtab = get_section(symtab.sh_link);
    for (uint64_t offset = symtab.sh_offset; offset < symtab.sh_offset + symtab.sh_size;
         offset += sizeof(Elf64_Sym)) {
      Elf64_Sym sym;
      memcpy(&sym, elf.data() + offset, sizeof(sym));
      if (elf.c_str() + strtab.sh_offset + sym.st_name != name)
        continue;

      sym.st_size = size;
      memcpy(elf.data() + offset, &sym, sizeof(sym));
      return true;
    }
  }
  return false;
}

TEST(DwarfBinaryImpl, DebugAddr) {
  std::string path = TestSymbolModule::GetTestDataDir() + "addrx_offset/clang_addrx_offset.so";
  TestSymbolModule setup(path, "");
  ASSERT_TRUE(setup.Init().ok());
  const DwarfBinary& binary = *setup.symbols()->binary();

  // The fixture's address table starts with the address of AddrxOffsetFirst(int).
  constexpr uint64_t kAddrBase = 8;
  const auto* function = GetSymbol(binary.GetELFSymbols(), "_Z16AddrxOffsetFirsti");
  ASSERT_TRUE(function);
  EXPECT_EQ(function->value, binary.GetDebugAddrEntry(kAddrBase, 0));

  EXPECT_EQ(std::nullopt, binary.GetDebugAddrEntry(binary.GetSections().addr.size(), 0));
  EXPECT_EQ(std::nullopt, binary.GetDebugAddrEntry(kAddrBase, UINT64_MAX / 8 + 1));
  EXPECT_EQ(std::nullopt, binary.GetDebugAddrEntry(UINT64_MAX - 7, 2));
}

TEST(DwarfBinaryImpl, UnitAddressFallbackUsesRootRnglist) {
  TestSymbolModule setup(TestSymbolModule::GetCheckedInTestFileName(), "");
  ASSERT_TRUE(setup.Init().ok());
  DwarfBinary& binary = *setup.symbols()->binary();
  ASSERT_TRUE(binary.GetSections().aranges.empty());

  // MyFunction(int) is in the third CU, whose root DIE describes its code with DW_FORM_rnglistx.
  auto expected_unit = binary.GetUnitAtIndex(UnitIndex(false, 2));
  ASSERT_TRUE(expected_unit);
  EXPECT_EQ(expected_unit, binary.UnitForRelativeAddress(0x1420));
  EXPECT_EQ(expected_unit, binary.UnitForRelativeAddress(0x14cc));
  EXPECT_FALSE(binary.UnitForRelativeAddress(0x14cd));
}

TEST(DwarfBinaryImpl, UnitAddressArangesMerge) {
  std::string elf;
  ASSERT_TRUE(
      files::ReadFileToString(TestSymbolModule::GetTestDataDir() + "fission/fission", &elf));
  const auto* bytes = reinterpret_cast<const uint8_t*>(elf.data());
  auto object = elflib::ElfLib::Create(bytes, elf.size());
  ASSERT_TRUE(object);
  auto aranges = object->GetSectionData(".debug_aranges");
  ASSERT_TRUE(aranges.ptr);
  ASSERT_EQ(96u, aranges.size);  // Two 48-byte sets.

  // Move the first set's start past the first CU's low PC and extend it into the second CU.
  // Invalidate the second set's header. The second CU's root range starts at 0x1165.
  size_t section_offset = aranges.ptr - bytes;
  std::vector<uint8_t> tuple;
  DwarfSectionBuilder tuple_builder(tuple, Endian::kLittle);
  tuple_builder.Address(0x113a, 8);
  tuple_builder.Address(0x40, 8);
  elf.replace(section_offset + 16, tuple.size(), reinterpret_cast<const char*>(tuple.data()),
              tuple.size());
  elf[section_offset + 48] = 0;

  files::ScopedTempDir temp_dir;
  std::string path;
  ASSERT_TRUE(temp_dir.NewTempFileWithData(elf, &path));
  TestSymbolModule setup(path, "");
  ASSERT_TRUE(setup.Init().ok());
  DwarfBinary& binary = *setup.symbols()->binary();
  auto first = binary.GetUnitAtIndex(UnitIndex(false, 0));
  auto second = binary.GetUnitAtIndex(UnitIndex(false, 1));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_FALSE(binary.UnitForRelativeAddress(0x1139));
  EXPECT_EQ(first, binary.UnitForRelativeAddress(0x113a));
  EXPECT_EQ(first, binary.UnitForRelativeAddress(0x1164));
  EXPECT_EQ(second, binary.UnitForRelativeAddress(0x1165));
  EXPECT_EQ(second, binary.UnitForRelativeAddress(0x1183));
  EXPECT_FALSE(binary.UnitForRelativeAddress(0x1184));
}

TEST(DwarfBinaryImpl, Dwarf4TypeUnits) {
  std::string path = TestSymbolModule::GetTestDataDir() + "type_unit/gcc_dwarf4.so";
  if (!std::filesystem::exists(path))
    GTEST_SKIP() << "The GCC type unit fixture is unavailable.";

  TestSymbolModule setup(path, "");
  ASSERT_TRUE(setup.Init().ok());
  DwarfBinary& binary = *setup.symbols()->binary();

  // The fixture has one compilation unit. Its TypeUnitOuter and TypeUnitInner each get a
  // type unit.
  ASSERT_EQ(3u, binary.GetNormalUnitCount());

  // The first unit in each section starts at offset zero.
  auto info = binary.GetUnitAtIndex(UnitIndex(false, 0));
  auto first_type = binary.GetUnitAtIndex(UnitIndex(false, 1));
  auto second_type = binary.GetUnitAtIndex(UnitIndex(false, 2));
  ASSERT_TRUE(info);
  ASSERT_TRUE(first_type);
  ASSERT_TRUE(second_type);
  EXPECT_EQ(DwarfDieRef::Main(0), info->GetDieRef());
  EXPECT_EQ(DwarfDieRef::Type(0), first_type->GetDieRef());
  EXPECT_EQ(DwarfDieRef::Section::kType, second_type->GetDieRef().section());
}

TEST(DwarfBinaryImpl, Dwarf5TypeUnits) {
  std::string path = TestSymbolModule::GetTestDataDir() + "type_unit/gcc_dwarf5.so";
  if (!std::filesystem::exists(path))
    GTEST_SKIP() << "The GCC type unit fixture is unavailable.";

  TestSymbolModule setup(path, "");
  ASSERT_TRUE(setup.Init().ok());
  DwarfBinary& binary = *setup.symbols()->binary();

  // The same fixture has one compilation unit and two type units, all in .debug_info.
  ASSERT_EQ(3u, binary.GetNormalUnitCount());
  uint32_t compile_count = 0;
  uint32_t type_count = 0;
  for (uint32_t i = 0; i < binary.GetNormalUnitCount(); ++i) {
    auto unit = binary.GetUnitAtIndex(UnitIndex(false, i));
    ASSERT_TRUE(unit);
    if (i == 0)
      EXPECT_EQ(DwarfDieRef::Main(0), unit->GetDieRef());
    EXPECT_EQ(DwarfDieRef::Section::kMain, unit->GetDieRef().section());
    auto tag = static_cast<DwarfTag>(unit->GetUnitDie().getTag());
    compile_count += tag == DwarfTag::kCompileUnit;
    type_count += tag == DwarfTag::kTypeUnit;
  }
  EXPECT_EQ(1u, compile_count);
  EXPECT_EQ(2u, type_count);
}

// The checked-in test binary has a ".symtab" and a ".dynsym" section. The symbols of both are in
// the result.
TEST(DwarfBinaryImpl, ElfSymbols) {
  TestSymbolModule setup(TestSymbolModule::GetCheckedInTestFileName(), "");
  ASSERT_TRUE(setup.Init().ok());
  const std::map<std::string, ElfTableSymbol>& symbols = setup.symbols()->binary()->GetELFSymbols();

  // MyFunction(int) is a defined function in both tables.
  const ElfTableSymbol* function = GetSymbol(symbols, "_Z10MyFunctioni");
  ASSERT_TRUE(function);
  EXPECT_EQ(ElfTableSymbol::Type::kFunction, function->type);
  EXPECT_TRUE(function->is_defined);
  EXPECT_EQ(0x1420u, function->value);
  EXPECT_EQ(173u, function->size);

  // The vtable of VirtualDerived is a local object, so only ".symtab" has it.
  const ElfTableSymbol* object = GetSymbol(symbols, "_ZTV14VirtualDerived");
  ASSERT_TRUE(object);
  EXPECT_EQ(ElfTableSymbol::Type::kObject, object->type);
  EXPECT_TRUE(object->is_defined);
  EXPECT_EQ(0x814u, object->value);
  EXPECT_EQ(20u, object->size);

  // The binary imports memset, so the symbol is an undefined function.
  const ElfTableSymbol* imported = GetSymbol(symbols, "memset");
  ASSERT_TRUE(imported);
  EXPECT_EQ(ElfTableSymbol::Type::kFunction, imported->type);
  EXPECT_FALSE(imported->is_defined);
  EXPECT_EQ(0u, imported->value);

  // A source file symbol has a type that the index doesn't use.
  const ElfTableSymbol* file = GetSymbol(symbols, "zxdb_symbol_test.cc");
  ASSERT_TRUE(file);
  EXPECT_EQ(ElfTableSymbol::Type::kOther, file->type);
}

// The stripped binary has only a ".dynsym" section.
TEST(DwarfBinaryImpl, ElfSymbolsStripped) {
  TestSymbolModule setup(TestSymbolModule::GetStrippedCheckedInTestFileName(), "");
  ASSERT_TRUE(setup.Init().ok());
  const std::map<std::string, ElfTableSymbol>& symbols = setup.symbols()->binary()->GetELFSymbols();

  // The table has 21 entries with different names. The first entry is the null symbol, which has
  // an empty name.
  EXPECT_EQ(21u, symbols.size());
  const ElfTableSymbol* null_symbol = GetSymbol(symbols, "");
  ASSERT_TRUE(null_symbol);
  EXPECT_EQ(ElfTableSymbol::Type::kOther, null_symbol->type);
  EXPECT_FALSE(null_symbol->is_defined);

  const ElfTableSymbol* function = GetSymbol(symbols, "_Z10MyFunctioni");
  ASSERT_TRUE(function);
  EXPECT_EQ(ElfTableSymbol::Type::kFunction, function->type);
  EXPECT_TRUE(function->is_defined);
  EXPECT_EQ(0x1420u, function->value);
  EXPECT_EQ(173u, function->size);

  EXPECT_EQ(symbols.end(), symbols.find("_ZTV14VirtualDerived"));
}

// When both tables have a name, the ".dynsym" record is in the result. The ".symtab" record of
// MyFunction(int) has a size of 173. A copy of the binary gives the ".dynsym" record a different
// size.
TEST(DwarfBinaryImpl, ElfSymbolsDynamicPrecedence) {
  std::string elf;
  ASSERT_TRUE(files::ReadFileToString(TestSymbolModule::GetCheckedInTestFileName(), &elf));
  ASSERT_TRUE(SetDynamicSymbolSize(elf, "_Z10MyFunctioni", 172));

  files::ScopedTempDir temp_dir;
  std::string path;
  ASSERT_TRUE(temp_dir.NewTempFileWithData(elf, &path));

  TestSymbolModule setup(path, "");
  ASSERT_TRUE(setup.Init().ok());
  const ElfTableSymbol* function =
      GetSymbol(setup.symbols()->binary()->GetELFSymbols(), "_Z10MyFunctioni");
  ASSERT_TRUE(function);
  EXPECT_EQ(0x1420u, function->value);
  EXPECT_EQ(172u, function->size);
}

}  // namespace

class UnitContextCacheTest : public testing::Test {
 protected:
  static size_t UnitEntries(const DwarfBinaryImpl& binary, const llvm::DWARFUnit& unit) {
    return binary.unit_contexts_.units_.count(&unit);
  }

  static size_t AbbrevEntries(const UnitContextCache& cache) { return cache.abbrevs_.size(); }

  static size_t AbbrevEntries(const DwarfBinaryImpl& binary) {
    return AbbrevEntries(binary.unit_contexts_);
  }

  static UnitContextResult GetOrParse(UnitContextCache& cache, const DwarfSections& sections,
                                      llvm::DWARFUnit& unit) {
    return cache.GetOrParse(sections, unit, {});
  }

  static void Clear(UnitContextCache& cache) { cache.Clear(); }

  static void ClearUnitContexts(DwarfBinaryImpl& binary) { Clear(binary.unit_contexts_); }
};

TEST_F(UnitContextCacheTest, ReusesContextAcrossAttributesAndClears) {
  TestSymbolModule setup(TestSymbolModule::kCheckedIn);
  ASSERT_TRUE(setup.Init("", false).ok());
  auto& binary = *setup.symbols()->binary();
  // The first CU starts at offset zero. Its first function uses addrx for its low PC.
  auto unit = binary.GetUnitAtIndex(UnitIndex(false, 0));
  ASSERT_TRUE(unit);
  auto& llvm_unit = *unit->GetLLVMUnit();
  EXPECT_EQ(0u, UnitEntries(binary, llvm_unit));

  constexpr DwarfAttr kAttrs[] = {DwarfAttr::kLowPc};
  DwarfDieDecoder decoder(binary, kAttrs);
  ASSERT_TRUE(decoder.Decode(binary.GetLLVMDieAtOffset(DwarfDieRef::Main(0x2b))));
  EXPECT_EQ(0x1000u, decoder.GetAddress(DwarfAttr::kLowPc));
  ASSERT_EQ(1u, UnitEntries(binary, llvm_unit));

  auto first = binary.GetUnitContext(llvm_unit);
  ASSERT_TRUE(first) << first.err().msg();
  EXPECT_EQ(0x1000u, first.value()->ReadAddress(0));
  EXPECT_EQ(".", unit->GetCompilationDir());
  auto second = binary.GetUnitContext(llvm_unit);
  ASSERT_TRUE(second);
  EXPECT_EQ(first.value(), second.value());

  binary.ClearLLVMCache();
  EXPECT_EQ(0u, UnitEntries(binary, llvm_unit));
  auto rebuilt = binary.GetUnitContext(llvm_unit);
  ASSERT_TRUE(rebuilt);
  // Keep the first snapshot alive so the allocator cannot reuse its address.
  EXPECT_NE(first.value(), rebuilt.value());
  EXPECT_EQ(".", rebuilt.value()->compilation_dir());
  EXPECT_EQ(0x1000u, rebuilt.value()->ReadAddress(0));
}

TEST_F(UnitContextCacheTest, DistinctUnitsShareAbbreviations) {
  LineTableTestSections sections;
  sections.AddCompileUnit(5, 0, {});
  sections.AddCompileUnit(5, 0x20, {});
  sections.Finish(Endian::kLittle);
  auto views = sections.sections();

  UnitContextCache cache;
  auto first = GetOrParse(cache, views, *sections.unit(0));
  auto second = GetOrParse(cache, views, *sections.unit(1));
  ASSERT_TRUE(first) << first.err().msg();
  ASSERT_TRUE(second) << second.err().msg();
  EXPECT_NE(first.value(), second.value());
  EXPECT_EQ(0u, first.value()->stmt_list());
  EXPECT_EQ(0x20u, second.value()->stmt_list());
  EXPECT_EQ(1u, AbbrevEntries(cache));

  // A binary's cache must not adopt units from a stand-alone LLVM context.
  DwarfBinaryImpl binary("", "", "");
  auto foreign = GetUnitContext(*sections.unit(0), &binary);
  auto foreign_again = GetUnitContext(*sections.unit(0), &binary);
  ASSERT_TRUE(foreign);
  ASSERT_TRUE(foreign_again);
  EXPECT_EQ(0u, foreign.value()->stmt_list());
  EXPECT_NE(foreign.value(), foreign_again.value());
  EXPECT_EQ(0u, UnitEntries(binary, *sections.unit(0)));

  Clear(cache);
  EXPECT_EQ(0u, AbbrevEntries(cache));
}

TEST_F(UnitContextCacheTest, LineTableCallersPopulateCache) {
  TestSymbolModule setup(TestSymbolModule::kCheckedIn);
  ASSERT_TRUE(setup.Init("", false).ok());
  auto& binary = *setup.symbols()->binary();
  auto unit = binary.GetUnitAtIndex(UnitIndex(false, 0));
  ASSERT_TRUE(unit);
  auto* llvm_unit = unit->GetLLVMUnit();

  EXPECT_EQ(0u, UnitEntries(binary, *llvm_unit));
  EXPECT_EQ(0u, AbbrevEntries(binary));
  auto files = binary.GetLineFileTable(llvm_unit, 0);
  ASSERT_TRUE(files);
  ASSERT_TRUE(files->GetFileName(0));
  EXPECT_TRUE(files->GetFileName(0)->ends_with("/line_lookup_symbol_test.cc"));
  EXPECT_EQ(1u, UnitEntries(binary, *llvm_unit));
  EXPECT_EQ(1u, AbbrevEntries(binary));
  EXPECT_EQ(files, binary.GetLineFileTable(llvm_unit, 0));
  EXPECT_EQ(1u, UnitEntries(binary, *llvm_unit));
  EXPECT_EQ(1u, AbbrevEntries(binary));

  // Start with empty caches so DecodeLineTable has to fetch the unit context itself.
  binary.ClearLLVMCache();
  EXPECT_EQ(0u, UnitEntries(binary, *llvm_unit));
  auto lines = binary.DecodeLineTable(llvm_unit);
  ASSERT_GT(lines.GetNumSequences(), 0u);
  EXPECT_EQ(1u, UnitEntries(binary, *llvm_unit));
  EXPECT_EQ(1u, AbbrevEntries(binary));
  EXPECT_EQ(lines.GetNumSequences(), binary.DecodeLineTable(llvm_unit).GetNumSequences());
  EXPECT_EQ(1u, UnitEntries(binary, *llvm_unit));
  EXPECT_EQ(1u, AbbrevEntries(binary));
}

TEST_F(UnitContextCacheTest, RangedFunctionDecodePopulatesCache) {
  const std::string path = TestSymbolModule::GetTestDataDir() + "line_table/clang_dwarf5_gc.so";
  TestSymbolModule setup(path, "");
  // Supply a build directory so FileLine does not need a unit context.
  ASSERT_TRUE(setup.Init("/fixture", false).ok());
  auto& binary = *setup.symbols()->binary();
  auto unit = binary.GetUnitAtIndex(UnitIndex(false, 0));
  ASSERT_TRUE(unit);
  auto* llvm_unit = unit->GetLLVMUnit();

  // Find the inlined LineTableFixtureSum instance with a range list. The compiler can change its
  // DIE offset.
  constexpr DwarfAttr kAttrs[] = {DwarfAttr::kRanges, DwarfAttr::kLowPc};
  DwarfDieDecoder decoder(binary, kAttrs);
  DwarfDieRef function_ref;
  for (const auto& entry : llvm_unit->dies()) {
    llvm::DWARFDie die(llvm_unit, &entry);
    if (die.getTag() == llvm::dwarf::DW_TAG_inlined_subroutine && decoder.Decode(die) &&
        decoder.GetValue(DwarfAttr::kRanges) && !decoder.GetValue(DwarfAttr::kLowPc)) {
      function_ref = DwarfDieRef::Main(die.getOffset());
      break;
    }
  }
  ASSERT_TRUE(function_ref);

  // Keep the complete line file table while clearing the unit context. With file attributes
  // served by the line table cache, only range decoding needs the unit context.
  ASSERT_GT(binary.DecodeLineTable(llvm_unit).GetNumSequences(), 0u);
  ClearUnitContexts(binary);
  ASSERT_EQ(0u, UnitEntries(binary, *llvm_unit));
  ASSERT_EQ(0u, AbbrevEntries(binary));
  for (int i = 0; i < 2; ++i) {
    auto symbol = binary.GetSymbolFactory()->CreateSymbol(function_ref);
    const auto* function = symbol->As<Function>();
    ASSERT_TRUE(function);
    EXPECT_TRUE(function->GetAssignedName().starts_with("LineTableFixtureSum"));
    EXPECT_GE(function->code_ranges().size(), 2u);
    EXPECT_EQ(1u, UnitEntries(binary, *llvm_unit));
    EXPECT_EQ(1u, AbbrevEntries(binary));
  }
}

TEST_F(UnitContextCacheTest, LocationListVariableDecodePopulatesCache) {
  TestSymbolModule setup(TestSymbolModule::kCheckedIn);
  ASSERT_TRUE(setup.Init("", false).ok());
  auto& binary = *setup.symbols()->binary();
  auto unit = binary.GetUnitAtIndex(UnitIndex(false, 2));
  ASSERT_TRUE(unit);
  auto& llvm_unit = *unit->GetLLVMUnit();
  ASSERT_EQ(0u, UnitEntries(binary, llvm_unit));
  ASSERT_EQ(0u, AbbrevEntries(binary));

  // The my_class DIE uses DW_FORM_loclistx. Its list contains one entry.
  for (int i = 0; i < 2; ++i) {
    auto symbol = binary.GetSymbolFactory()->CreateSymbol(DwarfDieRef::Main(0x6d6));
    const auto* variable = symbol->As<Variable>();
    ASSERT_TRUE(variable);
    EXPECT_EQ("my_class", variable->GetAssignedName());
    const auto& locations = variable->location().locations();
    ASSERT_EQ(1u, locations.size());
    EXPECT_EQ(AddressRange(0x145b, 0x14cd), locations[0].range);
    EXPECT_EQ(1u, UnitEntries(binary, llvm_unit));
    EXPECT_EQ(1u, AbbrevEntries(binary));
  }
}

TEST_F(UnitContextCacheTest, SplitFunctionDecodePopulatesSkeletonCache) {
  const std::string dir = TestSymbolModule::GetTestDataDir() + "fission/";
  TestSymbolModule setup(dir + "fission", "");
  ASSERT_TRUE(setup.Init("", false).ok());
  auto* module = setup.symbols();
  auto& binary = *module->binary();

  // The indexer loads the DWO binary on its worker thread before handing it to this thread.
  auto indexed = IndexModule(module->GetWeakPtr(), binary, dir, nullptr);
  auto dwo = std::find_if(indexed.dwo_info.begin(), indexed.dwo_info.end(), [](const auto& info) {
    return info && info->skeleton().dwo_name == "main.dwo";
  });
  ASSERT_NE(indexed.dwo_info.end(), dwo);
  auto unit = binary.GetUnitAtIndex(UnitIndex(false, 1));
  ASSERT_TRUE(unit);
  auto& skeleton = *unit->GetLLVMUnit();
  ASSERT_EQ(0u, UnitEntries(binary, skeleton));
  ASSERT_EQ(0u, AbbrevEntries(binary));

  // main uses addrx for its low PC. The address table lives in the skeleton binary.
  for (int i = 0; i < 2; ++i) {
    auto symbol = (*dwo)->symbol_factory()->CreateSymbol(DwarfDieRef::Main(0x47));
    const auto* function = symbol->As<Function>();
    ASSERT_TRUE(function);
    EXPECT_EQ("main", function->GetAssignedName());
    ASSERT_EQ(1u, function->code_ranges().size());
    EXPECT_EQ(AddressRange(0x1165, 0x1184), function->code_ranges()[0]);
    EXPECT_EQ(1u, UnitEntries(binary, skeleton));
    EXPECT_EQ(1u, AbbrevEntries(binary));
  }
}

TEST_F(UnitContextCacheTest, SplitUnitUsesSkeletonCache) {
  const std::string dir = TestSymbolModule::GetTestDataDir() + "fission/";
  TestSymbolModule setup(dir + "fission", "");
  ASSERT_TRUE(setup.Init("", false).ok());
  auto& main_binary = *setup.symbols()->binary();
  auto main_unit = main_binary.GetUnitAtIndex(UnitIndex(false, 1));
  ASSERT_TRUE(main_unit);
  auto& skeleton = *main_unit->GetLLVMUnit();
  EXPECT_EQ(0u, UnitEntries(main_binary, skeleton));

  DwarfBinaryImpl split_binary(dir + "main.dwo", "", "");
  ASSERT_TRUE(split_binary.Load({}, DwarfSymbolFactory::kDWO).ok());
  auto split_unit = split_binary.GetDwoUnit();
  ASSERT_TRUE(split_unit);
  auto& split = *split_unit->GetLLVMUnit();

  auto unlinked = split_binary.GetUnitContext(split);
  ASSERT_TRUE(unlinked);
  EXPECT_EQ(std::nullopt, unlinked.value()->base_address());
  EXPECT_EQ(std::nullopt, unlinked.value()->ReadAddress(0));

  UnitContextSource source{&skeleton, &main_binary};
  auto linked = split_binary.GetUnitContext(split, source);
  ASSERT_TRUE(linked);
  EXPECT_NE(unlinked.value(), linked.value());
  EXPECT_EQ(0x1165u, linked.value()->base_address());
  EXPECT_EQ(0x1165u, linked.value()->ReadAddress(0));
  EXPECT_EQ(".", linked.value()->compilation_dir());
  ASSERT_EQ(1u, UnitEntries(main_binary, skeleton));
  auto skeleton_context = main_binary.GetUnitContext(skeleton);
  ASSERT_TRUE(skeleton_context);
  auto repeated = split_binary.GetUnitContext(split, source);
  ASSERT_TRUE(repeated);
  EXPECT_EQ(linked.value(), repeated.value());

  split_binary.ClearLLVMCache();
  auto rebuilt = split_binary.GetUnitContext(split, source);
  ASSERT_TRUE(rebuilt);
  EXPECT_NE(linked.value(), rebuilt.value());
  EXPECT_EQ(0x1165u, rebuilt.value()->ReadAddress(0));
  auto reused = main_binary.GetUnitContext(skeleton);
  ASSERT_TRUE(reused);
  EXPECT_EQ(skeleton_context.value(), reused.value());
}

TEST_F(UnitContextCacheTest, CachesMalformedUnitError) {
  std::string elf;
  ASSERT_TRUE(files::ReadFileToString(TestSymbolModule::GetCheckedInTestFileName(), &elf));
  const auto* bytes = reinterpret_cast<const uint8_t*>(elf.data());
  auto object = elflib::ElfLib::Create(bytes, elf.size());
  ASSERT_TRUE(object);
  auto info = object->GetSectionData(".debug_info");
  ASSERT_TRUE(info.ptr);
  ASSERT_GT(info.size, 0xcu);
  // Replace the first CU's root abbreviation code with zero. Its header stays valid.
  elf[(info.ptr - bytes) + 0xc] = 0;

  files::ScopedTempDir temp_dir;
  std::string path;
  ASSERT_TRUE(temp_dir.NewTempFileWithData(elf, &path));
  TestSymbolModule setup(path, "");
  ASSERT_TRUE(setup.Init("", false).ok());
  auto& binary = *setup.symbols()->binary();
  auto unit = binary.GetUnitAtIndex(UnitIndex(false, 0));
  ASSERT_TRUE(unit);
  auto& llvm_unit = *unit->GetLLVMUnit();

  auto initial = binary.GetUnitContext(llvm_unit);
  ASSERT_TRUE(initial.has_error());
  EXPECT_EQ("DWARF unit DIE for unit at offset 0x0: invalid root abbreviation code",
            initial.err().msg());
  ASSERT_EQ(1u, UnitEntries(binary, llvm_unit));
  auto repeated = binary.GetUnitContext(llvm_unit);
  ASSERT_TRUE(repeated.has_error());
  EXPECT_EQ(initial.err(), repeated.err());
  EXPECT_EQ("", unit->GetCompilationDir());

  auto next_unit = binary.GetUnitAtIndex(UnitIndex(false, 1));
  ASSERT_TRUE(next_unit);
  auto next = binary.GetUnitContext(*next_unit->GetLLVMUnit());
  ASSERT_TRUE(next);
  EXPECT_EQ(0xb4u, next.value()->stmt_list());
}

}  // namespace zxdb
