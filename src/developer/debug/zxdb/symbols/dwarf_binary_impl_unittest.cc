// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_binary_impl.h"

#include <elf.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "llvm/ADT/StringMap.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Support/MemoryBuffer.h"
#include "src/developer/debug/zxdb/symbols/compile_unit.h"
#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"
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

TEST(DwarfBinaryImpl, RejectsRepeatedDwarfSections) {
  files::ScopedTempDir temp_dir;
  for (std::string_view name : {".debug_types", ".debug_info.dwo", ".debug_line"}) {
    SCOPED_TRACE(name);
    constexpr std::string_view kStringTablePrefix("\0.shstrtab\0", 11);
    std::string names(kStringTablePrefix);
    names.append(name);
    names.push_back('\0');

    // The file has a null section, a section name table, and two sections with the same name.
    constexpr uint64_t kNamesOffset = sizeof(Elf64_Ehdr) + 4 * sizeof(Elf64_Shdr);
    const Elf64_Ehdr header = {
        .e_ident = {ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3, ELFCLASS64, ELFDATA2LSB, EV_CURRENT},
        .e_type = ET_REL,
        .e_machine = EM_X86_64,
        .e_version = EV_CURRENT,
        .e_shoff = sizeof(Elf64_Ehdr),
        .e_ehsize = sizeof(Elf64_Ehdr),
        .e_shentsize = sizeof(Elf64_Shdr),
        .e_shnum = 4,
        .e_shstrndx = 1,
    };
    Elf64_Shdr sections[] = {
        {},
        {.sh_name = 1, .sh_type = SHT_STRTAB, .sh_offset = kNamesOffset, .sh_size = names.size()},
        {.sh_name = kStringTablePrefix.size(),
         .sh_type = SHT_PROGBITS,
         .sh_offset = kNamesOffset + names.size(),
         .sh_size = 1},
        {.sh_name = kStringTablePrefix.size(),
         .sh_type = SHT_PROGBITS,
         .sh_offset = kNamesOffset + names.size() + 1,
         .sh_size = 1},
    };
    std::string elf(reinterpret_cast<const char*>(&header), sizeof(header));
    elf.append(reinterpret_cast<const char*>(sections), sizeof(sections));
    elf.append(names).append(2, '\0');

    std::string path;
    ASSERT_TRUE(temp_dir.NewTempFileWithData(elf, &path));
    DwarfBinaryImpl binary(path, "", "");
    Err err = binary.Load(
        {}, name.ends_with(".dwo") ? DwarfSymbolFactory::kDWO : DwarfSymbolFactory::kMainBinary);
    ASSERT_TRUE(err.has_error());
    EXPECT_NE(std::string::npos,
              err.msg().find("has more than one " + std::string(name) + " section"))
        << err.msg();
    EXPECT_NE(std::string::npos, err.msg().find("Link it first")) << err.msg();
  }
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

  LazySymbol function = expected_unit->FunctionForRelativeAddress(0x1420);
  ASSERT_TRUE(function.is_valid());
  EXPECT_EQ("MyFunction", function.Get()->GetAssignedName());
  EXPECT_FALSE(expected_unit->FunctionForRelativeAddress(0x14cd).is_valid());
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

TEST(DwarfBinaryImpl, DieLookupInSelectedSections) {
  const std::string dir = TestSymbolModule::GetTestDataDir() + "fission/";
  for (bool is_dwo : {false, true}) {
    SCOPED_TRACE(is_dwo);
    DwarfBinaryImpl binary(dir + (is_dwo ? "main.dwo" : "fission"), "", "");
    ASSERT_TRUE(
        binary.Load({}, is_dwo ? DwarfSymbolFactory::kDWO : DwarfSymbolFactory::kMainBinary).ok());
    auto unit = binary.GetUnitAtIndex(UnitIndex(is_dwo, 0));
    ASSERT_TRUE(unit);
    DwarfDie root = unit->GetUnitDie();
    ASSERT_TRUE(root);

    EXPECT_EQ(root, binary.GetDieAtOffset(root.ref()));
    EXPECT_EQ(root, binary.GetDieAtOffset(root.ref(), is_dwo));
    EXPECT_FALSE(binary.GetDieAtOffset(root.ref(), !is_dwo));
    EXPECT_FALSE(binary.GetDieAtOffset({}, is_dwo));

    // A reference to the header identifies the unit but has no DIE at that offset.
    EXPECT_EQ(root, binary.GetUnitDieForRef(unit->GetDieRef(), is_dwo));
    EXPECT_FALSE(binary.GetDieAtOffset(unit->GetDieRef(), is_dwo));
    EXPECT_FALSE(binary.GetUnitDieForRef(unit->GetDieRef(), !is_dwo));
    EXPECT_FALSE(binary.GetUnitDieForRef({}, is_dwo));
  }
}

TEST(DwarfBinaryImpl, SplitTypeSignatureWithOverlappingOffsets) {
  constexpr uint64_t kSignature = 0;
  constexpr uint64_t kTypeOffset = 25;  // The type unit header and its root abbreviation code.
  constexpr AbbrevTable::AttributeSpec kName[] = {
      {static_cast<uint64_t>(DwarfAttr::kName), static_cast<uint64_t>(DwarfForm::kString)}};
  constexpr AbbrevTable::AttributeSpec kType[] = {
      {static_cast<uint64_t>(DwarfAttr::kType), static_cast<uint64_t>(DwarfForm::kRefSig8)}};
  std::vector<uint8_t> abbrev;
  DwarfSectionBuilder abbrev_builder(abbrev, Endian::kLittle);
  abbrev_builder.AbbrevDeclaration(1, static_cast<uint64_t>(DwarfTag::kTypeUnit), true);
  abbrev_builder.AbbrevDeclaration(2, static_cast<uint64_t>(DwarfTag::kStructureType), false,
                                   kName);
  abbrev_builder.AbbrevDeclaration(3, static_cast<uint64_t>(DwarfTag::kCompileUnit), true);
  abbrev_builder.AbbrevDeclaration(4, static_cast<uint64_t>(DwarfTag::kVariable), false, kType);
  abbrev_builder.EndAbbrevTable();

  auto write_type_unit = [](DwarfSectionBuilder& builder, uint8_t unit_type, uint64_t signature,
                            std::string_view name) {
    auto unit = builder.BeginUnit(
        {.unit_type = unit_type, .type_signature = signature, .type_offset = kTypeOffset});
    builder.Uleb(1);
    builder.Uleb(2);
    builder.CString(name);
    builder.Uleb(0);
    builder.EndUnit(unit);
  };
  std::vector<uint8_t> info, info_dwo;
  DwarfSectionBuilder normal(info, Endian::kLittle);
  DwarfSectionBuilder split(info_dwo, Endian::kLittle);
  write_type_unit(normal, kUnitTypeType, 1, "NormalType");
  write_type_unit(split, kUnitTypeSplitType, kSignature, "SplitType");
  auto split_compile = split.BeginUnit({.unit_type = kUnitTypeSplitCompile, .dwo_id = 1});
  split.Uleb(3);
  split.Uleb(4);
  split.Fixed(kSignature, 8);
  split.Uleb(0);
  split.EndUnit(split_compile);

  llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> buffers;
  buffers["debug_info"] = llvm::MemoryBuffer::getMemBufferCopy(AsStringView(info));
  buffers["debug_info.dwo"] = llvm::MemoryBuffer::getMemBufferCopy(AsStringView(info_dwo));
  buffers["debug_abbrev"] = llvm::MemoryBuffer::getMemBufferCopy(AsStringView(abbrev));
  buffers["debug_abbrev.dwo"] = llvm::MemoryBuffer::getMemBufferCopy(AsStringView(abbrev));
  DwarfBinaryImpl binary(llvm::DWARFContext::create(buffers, 8, true));

  auto unit = binary.GetUnitAtIndex(UnitIndex(true, 1));
  ASSERT_TRUE(unit);
  constexpr DwarfAttr kAttrs[] = {DwarfAttr::kType, DwarfAttr::kName};
  DwarfDieDecoder decoder(binary, kAttrs);
  ASSERT_TRUE(decoder.Decode(unit->GetUnitDie().first_child()));
  DwarfDie type = decoder.GetReference(DwarfAttr::kType);
  ASSERT_TRUE(type);
  EXPECT_EQ(kTypeOffset, type.offset());
  ASSERT_TRUE(decoder.Decode(type));
  EXPECT_STREQ("SplitType", decoder.GetCString(DwarfAttr::kName).value_or(nullptr));
}

TEST(DwarfBinaryImpl, RawPackageDieUsesAbbreviationContribution) {
  constexpr uint64_t kDwoId = 1;
  constexpr AbbrevTable::AttributeSpec kName[] = {
      {static_cast<uint64_t>(DwarfAttr::kName), static_cast<uint64_t>(DwarfForm::kString)}};
  std::vector<uint8_t> abbrev;
  DwarfSectionBuilder abbrev_builder(abbrev, Endian::kLittle);
  abbrev_builder.EndAbbrevTable();  // Empty table at offset 0.
  const size_t abbrev_offset = abbrev.size();
  abbrev_builder.AbbrevDeclaration(1, static_cast<uint64_t>(DwarfTag::kCompileUnit), false, kName);
  abbrev_builder.EndAbbrevTable();

  std::vector<uint8_t> info;
  DwarfSectionBuilder info_builder(info, Endian::kLittle);
  auto unit = info_builder.BeginUnit({.unit_type = kUnitTypeSplitCompile, .dwo_id = kDwoId});
  const auto ref = DwarfDieRef::Main(info.size());
  info_builder.Uleb(1);
  info_builder.CString("package.cc");
  info_builder.EndUnit(unit);

  // DWARF 5 CU index with one unit, two hash slots, and info and abbreviation contributions.
  std::vector<uint8_t> index;
  DwarfSectionBuilder index_builder(index, Endian::kLittle);
  for (uint32_t word : {5u, 2u, 1u, 2u})
    index_builder.Fixed(word, 4);
  index_builder.Fixed(0, 8);
  index_builder.Fixed(kDwoId, 8);  // Hash slot 1 refers to row 1.
  for (uint32_t word : {0u, 1u, 1u, 3u})
    index_builder.Fixed(word, 4);  // Row indexes, then DW_SECT_INFO and DW_SECT_ABBREV.
  index_builder.Fixed(0, 4);
  index_builder.Fixed(abbrev_offset, 4);
  index_builder.Fixed(info.size(), 4);
  index_builder.Fixed(abbrev.size() - abbrev_offset, 4);

  llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> buffers;
  buffers["debug_info.dwo"] = llvm::MemoryBuffer::getMemBufferCopy(AsStringView(info));
  buffers["debug_abbrev.dwo"] = llvm::MemoryBuffer::getMemBufferCopy(AsStringView(abbrev));
  buffers["debug_cu_index"] = llvm::MemoryBuffer::getMemBufferCopy(AsStringView(index));
  DwarfBinaryImpl binary(llvm::DWARFContext::create(buffers, 8, true));

  DwarfDie die;
  std::jthread reader([&] { die = binary.GetDieAtOffset(ref, true); });
  auto* llvm_unit = binary.GetLLVMUnit(UnitIndex(true, 0));
  ASSERT_TRUE(llvm_unit);
  auto context = binary.GetUnitContext(*llvm_unit);
  ASSERT_TRUE(context) << context.err().msg();
  reader.join();

  ASSERT_TRUE(die);
  EXPECT_EQ(DwarfTag::kCompileUnit, die.tag());
  EXPECT_STREQ("package.cc", die.short_name());
  binary.ClearDwarfCaches();
  EXPECT_EQ(die, binary.GetDieAtOffset(ref, true));
}

TEST(DwarfBinaryImpl, Dwarf4TypeUnits) {
  std::string path = TestSymbolModule::GetTestDataDir() + "type_unit/gcc_dwarf4.so";
  if (!std::filesystem::exists(path))
    GTEST_SKIP() << "The GCC type unit fixture is unavailable.";

  TestSymbolModule setup(path, "");
  ASSERT_TRUE(setup.Init().ok());
  DwarfBinary& binary = *setup.symbols()->binary();

  // readelf -S build/test_data/zxdb/type_unit/gcc_dwarf4.so shows the .debug_types section.
  // readelf --debug-dump=info build/test_data/zxdb/type_unit/gcc_dwarf4.so shows the unit offsets
  // and the signatures and Type Offsets for TypeUnitOuter and TypeUnitInner, defined in
  // test_data/type_unit/type_unit_fixture.cc. The section-relative DIE offset is the unit offset
  // plus the Type Offset.
  ASSERT_EQ(3u, binary.GetNormalUnitCount());

  auto info = binary.GetUnitAtIndex(UnitIndex(false, 0));
  auto first_type = binary.GetUnitAtIndex(UnitIndex(false, 1));
  auto second_type = binary.GetUnitAtIndex(UnitIndex(false, 2));
  ASSERT_TRUE(info);
  ASSERT_TRUE(first_type);
  ASSERT_TRUE(second_type);
  EXPECT_EQ(0u, info->GetOffset());
  EXPECT_EQ(0u, first_type->GetOffset());
  EXPECT_EQ(0x5du, second_type->GetOffset());
  EXPECT_EQ(DwarfDieRef::Main(0), info->GetDieRef());
  EXPECT_EQ(DwarfDieRef::Type(0), first_type->GetDieRef());
  EXPECT_EQ(DwarfDieRef::Type(0x5d), second_type->GetDieRef());

  DwarfDie root = first_type->GetUnitDie();
  ASSERT_TRUE(root);
  EXPECT_EQ(DwarfDieRef::Type(root.offset()), root.ref());
  EXPECT_EQ(root, binary.GetDieAtOffset(root.ref()));

  // TypeUnitOuter is at 0x0 + 0x25, and TypeUnitInner is at 0x5d + 0x25.
  EXPECT_EQ(DwarfDieRef::Type(0x25), binary.GetDieRefForSignature(0xde3dca152f8e7c98));
  EXPECT_EQ(DwarfDieRef::Type(0x82), binary.GetDieRefForSignature(0xc9382c36fccd19d7));
  EXPECT_FALSE(binary.GetDieRefForSignature(0).is_valid());  // Absent from the fixture.
}

TEST(DwarfBinaryImpl, Dwarf5TypeUnits) {
  std::string path = TestSymbolModule::GetTestDataDir() + "type_unit/gcc_dwarf5.so";
  if (!std::filesystem::exists(path))
    GTEST_SKIP() << "The GCC type unit fixture is unavailable.";

  TestSymbolModule setup(path, "");
  ASSERT_TRUE(setup.Init().ok());
  DwarfBinary& binary = *setup.symbols()->binary();

  // readelf --debug-dump=info build/test_data/zxdb/type_unit/gcc_dwarf5.so shows all three units
  // in .debug_info. It gives the unit offsets and the signatures and Type Offsets for
  // TypeUnitOuter and TypeUnitInner, defined in test_data/type_unit/type_unit_fixture.cc.
  // The section-relative DIE offset is the unit offset plus the Type Offset.
  constexpr uint64_t kUnitOffsets[] = {0x0, 0x5e, 0xa5};
  ASSERT_EQ(3u, binary.GetNormalUnitCount());
  uint32_t compile_count = 0;
  uint32_t type_count = 0;
  for (uint32_t i = 0; i < binary.GetNormalUnitCount(); ++i) {
    auto unit = binary.GetUnitAtIndex(UnitIndex(false, i));
    ASSERT_TRUE(unit);
    EXPECT_EQ(kUnitOffsets[i], unit->GetOffset());
    EXPECT_EQ(DwarfDieRef::Main(kUnitOffsets[i]), unit->GetDieRef());
    auto tag = unit->GetUnitDie().tag();
    compile_count += tag == DwarfTag::kCompileUnit;
    type_count += tag == DwarfTag::kTypeUnit;
  }
  EXPECT_EQ(1u, compile_count);
  EXPECT_EQ(2u, type_count);

  // TypeUnitOuter is at 0x0 + 0x26, and TypeUnitInner is at 0x5e + 0x26.
  EXPECT_EQ(DwarfDieRef::Main(0x26), binary.GetDieRefForSignature(0xde3dca152f8e7c98));
  EXPECT_EQ(DwarfDieRef::Main(0x84), binary.GetDieRefForSignature(0xc9382c36fccd19d7));
  EXPECT_FALSE(binary.GetDieRefForSignature(0).is_valid());  // Absent from the fixture.
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
  auto& llvm_unit = *binary.GetLLVMUnit(UnitIndex(false, 0));
  EXPECT_EQ(0u, UnitEntries(binary, llvm_unit));

  constexpr DwarfAttr kAttrs[] = {DwarfAttr::kLowPc};
  DwarfDieDecoder decoder(binary, kAttrs);
  ASSERT_TRUE(decoder.Decode(binary.GetDieAtOffset(DwarfDieRef::Main(0x2b))));
  EXPECT_EQ(0x1000u, decoder.GetAddress(DwarfAttr::kLowPc));
  ASSERT_EQ(1u, UnitEntries(binary, llvm_unit));

  auto first = binary.GetUnitContext(llvm_unit);
  ASSERT_TRUE(first) << first.err().msg();
  EXPECT_EQ(0x1000u, first.value()->ReadAddress(0));
  EXPECT_EQ(".", unit->GetCompilationDir());
  auto second = binary.GetUnitContext(llvm_unit);
  ASSERT_TRUE(second);
  EXPECT_EQ(first.value(), second.value());

  binary.ClearDwarfCaches();
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
  auto* llvm_unit = binary.GetLLVMUnit(UnitIndex(false, 0));

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
  binary.ClearDwarfCaches();
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
  auto* llvm_unit = binary.GetLLVMUnit(UnitIndex(false, 0));

  // Find the inlined LineTableFixtureSum instance with a range list. The compiler can change its
  // DIE offset.
  constexpr DwarfAttr kAttrs[] = {DwarfAttr::kRanges, DwarfAttr::kLowPc};
  DwarfDieDecoder decoder(binary, kAttrs);
  DwarfDieRef function_ref;
  for (const DwarfDie& die : unit->GetDies()) {
    if (die.tag() == DwarfTag::kInlinedSubroutine && decoder.Decode(die) &&
        decoder.GetValue(DwarfAttr::kRanges) && !decoder.GetValue(DwarfAttr::kLowPc)) {
      function_ref = die.ref();
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
  auto& llvm_unit = *binary.GetLLVMUnit(UnitIndex(false, 2));
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
  auto& skeleton = *binary.GetLLVMUnit(UnitIndex(false, 1));
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
  auto& skeleton = *main_binary.GetLLVMUnit(UnitIndex(false, 1));
  EXPECT_EQ(0u, UnitEntries(main_binary, skeleton));

  DwarfBinaryImpl split_binary(dir + "main.dwo", "", "");
  ASSERT_TRUE(split_binary.Load({}, DwarfSymbolFactory::kDWO).ok());
  auto split_unit = split_binary.GetDwoUnit();
  ASSERT_TRUE(split_unit);
  auto& split = *split_binary.GetLLVMUnit(UnitIndex(true, 0));

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

  split_binary.ClearDwarfCaches();
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
  auto& llvm_unit = *binary.GetLLVMContext()->getUnitAtIndex(0);

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
  auto next = binary.GetUnitContext(*binary.GetLLVMUnit(UnitIndex(false, 1)));
  ASSERT_TRUE(next);
  EXPECT_EQ(0xb4u, next.value()->stmt_list());
}

}  // namespace zxdb
