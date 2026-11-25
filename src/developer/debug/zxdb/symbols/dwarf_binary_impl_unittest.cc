// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_binary_impl.h"

#include <elf.h>

#include <cstring>
#include <map>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/module_symbols_impl.h"
#include "src/developer/debug/zxdb/symbols/test_symbol_module.h"
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

}  // namespace zxdb
