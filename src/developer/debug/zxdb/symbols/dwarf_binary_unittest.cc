// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_binary.h"

#include <elf.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/compile_unit.h"
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
#include "src/lib/elflib/elflib_test_support.h"
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

using elflib::test::MakeElf;

TEST(DwarfBinary, RejectsRepeatedDwarfSections) {
  files::ScopedTempDir temp_dir;
  for (const auto& [name, duplicate] : {std::pair{".debug_types", ".debug_types"},
                                        {".debug_info.dwo", ".debug_info.dwo"},
                                        {".debug_line", ".zdebug_line"}}) {
    SCOPED_TRACE(name);
    const uint8_t byte[] = {0};
    auto elf = MakeElf({{name, byte}, {duplicate, byte}});
    std::string path;
    ASSERT_TRUE(temp_dir.NewTempFileWithData(elf, &path));
    DwarfBinary binary(path, "", "");
    Err err = binary.Load({}, DwarfSymbolFactory::kMainBinary);
    ASSERT_TRUE(err.has_error());
    EXPECT_EQ(
        "The object \"" + path + "\" has more than one " + name +
            " section. This usually means a relocatable (.o) file. Link it first. "
            "zxdb reads linked executables, shared libraries, and their separate debug files.",
        err.msg());
  }
}

TEST(DwarfBinary, RejectsDebugRelocations) {
  // The relocation section header's sh_info gives the target section index. The section
  // name and relocation records are irrelevant to this check.
  const uint8_t byte[] = {0};
  const uint8_t relocation[sizeof(Elf64_Rela)] = {};
  files::ScopedTempDir temp_dir;
  constexpr uint32_t kCrelSectionType = 0x40000014;
  for (uint32_t type : std::array<uint32_t, 3>{SHT_REL, SHT_RELA, kCrelSectionType}) {
    SCOPED_TRACE(type);
    const auto elf = MakeElf({{".debug_info", byte}, {".relocations", relocation, type, 0, 0, 1}});
    std::string path;
    ASSERT_TRUE(temp_dir.NewTempFileWithData(elf, &path));
    DwarfBinary binary(path, "", "");
    Err err = binary.Load({}, DwarfSymbolFactory::kMainBinary);
    ASSERT_TRUE(err.has_error());
    EXPECT_EQ("The object \"" + path + "\" has relocations for .debug_info. Link it first.",
              err.msg());
  }
}

TEST(DwarfBinary, FirstCodeAddress) {
  // ELF marks executable sections with SHF_EXECINSTR. A separate debug file can preserve
  // this flag on a SHT_NOBITS section.
  const uint8_t code[] = {0};
  auto elf = MakeElf({{".text", code, SHT_PROGBITS, SHF_EXECINSTR, 0x1000},
                      {".init", {}, SHT_NOBITS, SHF_EXECINSTR, 0x800},
                      {".rodata", code, SHT_PROGBITS, SHF_ALLOC, 0x400}});
  files::ScopedTempDir temp_dir;
  std::string path;
  ASSERT_TRUE(temp_dir.NewTempFileWithData(elf, &path));
  DwarfBinary binary(path, "", "");
  Err err = binary.Load({}, DwarfSymbolFactory::kMainBinary);
  ASSERT_TRUE(err.ok()) << err.msg();
  EXPECT_EQ(0x800u, binary.GetFirstCodeAddress());
}

TEST(DwarfBinary, SkipsSectionWithBadCompressionHeader) {
  // A single byte is shorter than an Elf64_Chdr. The unrelated abbreviation section is readable.
  const uint8_t truncated[] = {ELFCOMPRESS_ZLIB};
  const uint8_t abbrev[] = {0};
  const auto elf = MakeElf(
      {{".debug_info", truncated, SHT_PROGBITS, SHF_COMPRESSED}, {".debug_abbrev", abbrev}});
  files::ScopedTempDir temp_dir;
  std::string path;
  ASSERT_TRUE(temp_dir.NewTempFileWithData(elf, &path));

  DwarfBinary binary(path, "", "");
  Err err = binary.Load({}, DwarfSymbolFactory::kMainBinary);
  ASSERT_TRUE(err.ok()) << err.msg();
  EXPECT_TRUE(binary.GetSections().info.empty());
  EXPECT_EQ(1u, binary.GetSections().abbrev.size());
}

TEST(DwarfBinary, ZstdCompressedSections) {
  // readelf -x .debug_info shows the ELF zstd compression header. The units match the four
  // fixture sources. readelf --debug-dump=info lists line_lookup_symbol_test.cc first.
  DwarfBinary binary(TestSymbolModule::GetTestFileName(), "", "");
  Err err = binary.Load({}, DwarfSymbolFactory::kMainBinary);
  ASSERT_TRUE(err.ok()) << err.msg();
  EXPECT_EQ(4u, binary.GetNormalUnitCount());
  auto cu = binary.GetUnitAtIndex(UnitIndex(false, 0));
  ASSERT_TRUE(cu);
  EXPECT_EQ(0u, cu->GetDieRef().offset());
  const char* name = cu->GetUnitDie().short_name();
  ASSERT_TRUE(name);
  EXPECT_TRUE(std::string_view(name).ends_with("/line_lookup_symbol_test.cc"));
}

TEST(DwarfBinary, SeparateDebugFile) {
  const std::string dir = TestSymbolModule::GetTestDataDir() + "elf_sections/";
  const std::string stripped_path = dir + "symbols.so";
  DwarfBinary stripped(stripped_path, "", "");
  Err err = stripped.Load({}, DwarfSymbolFactory::kMainBinary);
  ASSERT_TRUE(err.ok()) << err.msg();
  EXPECT_TRUE(stripped.GetSections().info.empty());

  // readelf -x .debug_info shows zlib compression in symbols.debug. readelf -Ws on the
  // stripped library gives DoLineLookupTest's address. Its DIE comes from the first CU.
  TestSymbolModule setup(dir + "symbols.debug", stripped_path);
  err = setup.Init("", false);
  ASSERT_TRUE(err.ok()) << err.msg();
  DwarfBinary& binary = *setup.symbols()->binary();
  EXPECT_GT(stripped.GetMappedLength(), 0u);
  EXPECT_EQ(stripped.GetMappedLength(), binary.GetMappedLength());
  EXPECT_EQ(stripped.GetFirstCodeAddress(), binary.GetFirstCodeAddress());
  auto cu = binary.GetUnitAtIndex(UnitIndex(false, 0));
  ASSERT_TRUE(cu);
  const auto* symbol = GetSymbol(binary.GetELFSymbols(), "_Z16DoLineLookupTesti");
  ASSERT_TRUE(symbol);
  auto function = cu->FunctionForRelativeAddress(symbol->value);
  ASSERT_TRUE(function.is_valid());
  EXPECT_EQ("DoLineLookupTest", function.Get()->GetAssignedName());
}

TEST(DwarfBinary, SplitDwarfSections) {
  // readelf --debug-dump=info shows a split type unit at offset zero and a split compile unit.
  auto path = TestSymbolModule::GetTestDataDir() + "line_table/clang_split.dwo";
  DwarfBinary binary(path, "", "");
  Err err = binary.Load({}, DwarfSymbolFactory::kDWO);
  ASSERT_TRUE(err.ok()) << err.msg();
  EXPECT_EQ(0u, binary.GetNormalUnitCount());
  ASSERT_EQ(2u, binary.GetDWOUnitCount());
  auto type = binary.GetUnitAtIndex(UnitIndex(true, 0));
  ASSERT_TRUE(type);
  EXPECT_EQ(0u, type->GetDieRef().offset());
  EXPECT_EQ(DwarfTag::kTypeUnit, type->GetUnitDie().tag());
  EXPECT_FALSE(binary.GetSections().line_dwo.empty());

  auto only_compile_unit = binary.FindSplitCompileUnit(std::nullopt);
  ASSERT_TRUE(only_compile_unit);
  EXPECT_EQ(UnitIndex(true, 1), only_compile_unit.value());
}

TEST(DwarfBinary, MatchesGnuSplitCompileUnitId) {
  std::vector<uint8_t> abbrev, info;
  DwarfSectionBuilder table(abbrev, Endian::kLittle);
  const AbbrevTable::AttributeSpec attrs[] = {
      {static_cast<uint64_t>(DwarfAttr::kGnuDwoId), static_cast<uint64_t>(DwarfForm::kData8)}};
  table.AbbrevDeclaration(1, static_cast<uint64_t>(DwarfTag::kCompileUnit), false, attrs);
  table.EndAbbrevTable();

  // Only the second root has ID zero. The other two roots have the same GNU DWO ID.
  DwarfSectionBuilder units(info, Endian::kLittle);
  for (uint64_t id : {1, 0, 1}) {
    auto unit = units.BeginUnit({.version = 4});
    units.Uleb(1);
    units.Fixed(id, 8);
    units.EndUnit(unit);
  }
  DwarfBinary binary(DwarfSections{.info_dwo = info, .abbrev_dwo = abbrev});
  auto match = binary.FindSplitCompileUnit(0);
  ASSERT_TRUE(match);
  EXPECT_EQ(UnitIndex(true, 1), match.value());
  EXPECT_TRUE(binary.FindSplitCompileUnit(1).has_error());
  EXPECT_TRUE(binary.FindSplitCompileUnit(std::nullopt).has_error());
}

TEST(DwarfBinary, DebugAddr) {
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

TEST(DwarfBinary, UnitAddressFallbackUsesRootRnglist) {
  TestSymbolModule setup(TestSymbolModule::GetCheckedInTestFileName(), "");
  ASSERT_TRUE(setup.Init().ok());
  DwarfBinary& binary = *setup.symbols()->binary();
  ASSERT_TRUE(binary.GetSections().aranges.empty());

  // MyFunction(int) is in the third CU, whose root DIE describes its code with DW_FORM_rnglistx.
  auto expected_unit = binary.GetUnitAtIndex(UnitIndex(false, 2));
  ASSERT_TRUE(expected_unit);
  for (uint64_t address : {0x1420, 0x14cc}) {
    auto unit = binary.UnitForRelativeAddress(address);
    ASSERT_TRUE(unit);
    EXPECT_EQ(expected_unit->GetUnitIndex(), unit->GetUnitIndex());
  }
  EXPECT_FALSE(binary.UnitForRelativeAddress(0x14cd));

  LazySymbol function = expected_unit->FunctionForRelativeAddress(0x1420);
  ASSERT_TRUE(function.is_valid());
  EXPECT_EQ("MyFunction", function.Get()->GetAssignedName());
  EXPECT_FALSE(expected_unit->FunctionForRelativeAddress(0x14cd).is_valid());
}

TEST(DwarfBinary, ClearDwarfCachesEvictsLineTablesAndFunctionMaps) {
  TestSymbolModule setup(TestSymbolModule::kCheckedIn);
  ASSERT_TRUE(setup.Init("", false).ok());
  auto& binary = *setup.symbols()->binary();
  const UnitIndex index(false, 2);
  auto unit = binary.GetUnitAtIndex(index);
  ASSERT_TRUE(unit);

  // MyFunction(int) starts at 0x1420 in the third CU.
  constexpr uint64_t kAddress = 0x1420;
  auto function = unit->FunctionForRelativeAddress(kAddress);
  ASSERT_TRUE(function);
  EXPECT_EQ("MyFunction", function.Get()->GetAssignedName());
  auto compile_unit = function.Get()->GetCompileUnit();
  ASSERT_TRUE(compile_unit);
  const auto* symbol_unit = compile_unit->dwarf_unit();
  ASSERT_TRUE(symbol_unit);
  EXPECT_EQ(index, symbol_unit->GetUnitIndex());

  size_t sequence_count = unit->GetLineTable().GetNumSequences();
  ASSERT_GT(sequence_count, 0u);
  EXPECT_EQ(&unit->GetLineTable(), &symbol_unit->GetLineTable());
  auto files = binary.GetLineFileTable(unit->GetUnitDie(DwarfDie::Load::kRootOnly), 1);
  ASSERT_EQ(&unit->GetLineTable().files(), files.get());
  std::weak_ptr<const LineFileTable> cached_files = files;
  files.reset();

  // The CompileUnit can use the same function map without reloading the full DIE table.
  unit->ClearDieCache();
  EXPECT_EQ(function.die_ref(), symbol_unit->FunctionForRelativeAddress(kAddress).die_ref());
  EXPECT_FALSE(unit->GetUnitDie(DwarfDie::Load::kRootOnly).first_child());

  binary.ClearDwarfCaches();
  EXPECT_TRUE(cached_files.expired());
  EXPECT_FALSE(unit->GetUnitDie(DwarfDie::Load::kRootOnly).first_child());

  // Both lookups rebuild their entries for the surviving CompileUnit.
  EXPECT_EQ(sequence_count, symbol_unit->GetLineTable().GetNumSequences());
  EXPECT_EQ(&unit->GetLineTable(), &symbol_unit->GetLineTable());
  EXPECT_EQ(function.die_ref(), symbol_unit->FunctionForRelativeAddress(kAddress).die_ref());
  EXPECT_TRUE(unit->GetUnitDie(DwarfDie::Load::kRootOnly).first_child());
}

TEST(DwarfBinary, CompileUnitOutlivesBinary) {
  fxl::RefPtr<CompileUnit> compile_unit;
  {
    TestSymbolModule setup(TestSymbolModule::kCheckedIn);
    ASSERT_TRUE(setup.Init("", false).ok());
    auto* binary = setup.symbols()->binary();
    auto unit = binary->GetUnitAtIndex(UnitIndex(false, 0));
    ASSERT_TRUE(unit);
    compile_unit = binary->GetSymbolFactory()->GetCompileUnitForDie(unit->GetDieRef());
    ASSERT_TRUE(compile_unit);
    const auto* symbol_unit = compile_unit->dwarf_unit();
    ASSERT_TRUE(symbol_unit);
    EXPECT_EQ(binary, symbol_unit->GetBinary());
    EXPECT_GT(symbol_unit->GetLineTable().GetNumSequences(), 0u);
  }

  const auto* unit = compile_unit->dwarf_unit();
  EXPECT_FALSE(unit->GetBinary());
  EXPECT_FALSE(unit->GetUnitDie());
  EXPECT_FALSE(unit->GetOffset());
  EXPECT_FALSE(unit->GetDieRef());
  EXPECT_EQ(0u, unit->GetLineTable().GetNumSequences());
}

TEST(DwarfBinary, UnitAddressArangesMerge) {
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
  EXPECT_FALSE(binary.UnitForRelativeAddress(0x1139));
  for (const auto& [address, index] :
       {std::pair{0x113a, 0}, {0x1164, 0}, {0x1165, 1}, {0x1183, 1}}) {
    auto unit = binary.UnitForRelativeAddress(address);
    ASSERT_TRUE(unit);
    EXPECT_EQ(UnitIndex(false, index), unit->GetUnitIndex());
  }
  EXPECT_FALSE(binary.UnitForRelativeAddress(0x1184));
}

TEST(DwarfBinary, DieLookupInSelectedSections) {
  const std::string dir = TestSymbolModule::GetTestDataDir() + "fission/";
  for (bool is_dwo : {false, true}) {
    SCOPED_TRACE(is_dwo);
    DwarfBinary binary(dir + (is_dwo ? "main.dwo" : "fission"), "", "");
    ASSERT_TRUE(
        binary.Load({}, is_dwo ? DwarfSymbolFactory::kDWO : DwarfSymbolFactory::kMainBinary).ok());
    auto unit = binary.GetUnitAtIndex(UnitIndex(is_dwo, 0));
    ASSERT_TRUE(unit);
    DwarfDie root = unit->GetUnitDie();
    ASSERT_TRUE(root);

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

TEST(DwarfBinary, SplitTypeSignatureWithOverlappingOffsets) {
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

  DwarfBinary binary(
      DwarfSections{.info = info, .abbrev = abbrev, .info_dwo = info_dwo, .abbrev_dwo = abbrev});

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

  // The signature exists only in the split set, even though the offset exists in both sets.
  EXPECT_EQ(type, binary.GetDieForSignature(kSignature, false));
  DwarfDie normal_type = binary.GetDieAtOffset(type.ref(), false);
  ASSERT_TRUE(normal_type);
  ASSERT_TRUE(decoder.Decode(normal_type));
  EXPECT_STREQ("NormalType", decoder.GetCString(DwarfAttr::kName).value_or(nullptr));
  EXPECT_EQ(type, binary.GetDieAtOffset(type.ref(), true));

  // The split unit's null entry falls inside the normal type's longer name.
  DwarfDie split_null = type.die_at_index(2);
  ASSERT_TRUE(split_null);
  ASSERT_TRUE(binary.GetUnitDieForRef(split_null.ref(), false));
  EXPECT_FALSE(binary.GetDieAtOffset(split_null.ref(), false));
  EXPECT_EQ(split_null, binary.GetDieAtOffset(split_null.ref(), true));
}

namespace {

class DwarfPackageFixture {
 public:
  static constexpr std::array<uint64_t, 2> kIds{1, 2};
  static constexpr std::array<const char*, 2> kNames{"first.cc", "second.cc"};

  explicit DwarfPackageFixture(uint16_t version, bool bad_first_line_offset = false)
      : version_(version) {
    DwarfSectionBuilder abbrevs(abbrev_, Endian::kLittle);
    DwarfSectionBuilder units(info_, Endian::kLittle);
    std::array<std::array<uint64_t, 3>, 2> offsets, sizes;
    for (size_t i = 0; i < kIds.size(); ++i) {
      LineTableBuilder line(version_);
      line.set_string_form(LineTableBuilder::StringForm::kString);
      if (version_ == 5)
        line.AddDirectory("/build");
      line.AddFile(kNames[i], line.AddDirectory("/src"));
      uint64_t line_offset = lines_.Add(line);
      uint64_t line_size = lines_.line_size() - line_offset;
      offsets[i] = {info_.size(), abbrev_.size(), line_offset};

      // DWARF 5 section 7.3.5.3 makes both offsets relative to their contributions.
      uint64_t relative_abbrev = version_ == 5 && i == 1 ? 1 : 0;
      if (relative_abbrev)
        abbrevs.EndAbbrevTable();
      std::vector<Spec> attrs{Attr(DwarfAttr::kName, DwarfForm::kString),
                              Attr(DwarfAttr::kStmtList, DwarfForm::kSecOffset)};
      if (version_ < 5)
        attrs.push_back(Attr(DwarfAttr::kGnuDwoId, DwarfForm::kData8));
      abbrevs.AbbrevDeclaration(i + 1, static_cast<uint64_t>(DwarfTag::kCompileUnit), false, attrs);
      abbrevs.EndAbbrevTable();

      auto unit = units.BeginUnit({.version = version_,
                                   .unit_type = kUnitTypeSplitCompile,
                                   .abbrev_offset = relative_abbrev,
                                   .dwo_id = kIds[i]});
      refs_[i] = DwarfDieRef::Main(info_.size());
      units.Uleb(i + 1);
      units.CString(kNames[i]);
      units.Fixed(bad_first_line_offset && i == 0 ? line_size : 0, 4);
      if (version_ < 5)
        units.Fixed(kIds[i], 8);
      units.EndUnit(unit);
      sizes[i] = {info_.size() - offsets[i][0], abbrev_.size() - offsets[i][1], line_size};
    }
    lines_.Finish();

    DwarfSectionBuilder index(cu_index_, Endian::kLittle);
    for (uint32_t word : {version_ == 5 ? 5u : 2u, 3u, 2u, 4u})
      index.Fixed(word, 4);
    for (uint64_t hash : {uint64_t{0}, kIds[0], kIds[1], uint64_t{0}})
      index.Fixed(hash, 8);
    // The row numbers reverse unit order. The column IDs are INFO, ABBREV, and LINE.
    for (uint32_t word : {0, 2, 1, 0, 1, 3, 4})
      index.Fixed(word, 4);
    for (const auto& table : {offsets, sizes}) {
      for (size_t i : {1, 0}) {
        for (uint64_t word : table[i])
          index.Fixed(word, 4);
      }
    }
  }

  void AddSkeleton(uint64_t id, uint64_t address) {
    if (skeleton_abbrev_.empty()) {
      DwarfSectionBuilder abbrev(skeleton_abbrev_, Endian::kLittle);
      std::vector<Spec> attrs{Attr(version_ == 5 ? DwarfAttr::kAddrBase : DwarfAttr::kGnuAddrBase,
                                   DwarfForm::kSecOffset),
                              Attr(DwarfAttr::kLowPc, DwarfForm::kAddr)};
      if (version_ < 5)
        attrs.push_back(Attr(DwarfAttr::kGnuDwoId, DwarfForm::kData8));
      auto tag = version_ == 5 ? DwarfTag::kSkeletonUnit : DwarfTag::kCompileUnit;
      abbrev.AbbrevDeclaration(1, static_cast<uint64_t>(tag), false, attrs);
      abbrev.EndAbbrevTable();
    }

    DwarfSectionBuilder skeleton(skeleton_info_, Endian::kLittle);
    auto unit =
        skeleton.BeginUnit({.version = version_, .unit_type = kUnitTypeSkeleton, .dwo_id = id});
    skeleton.Uleb(1);
    skeleton.Fixed(addresses_.size(), 4);
    skeleton.Fixed(address, 8);
    if (version_ < 5)
      skeleton.Fixed(id, 8);
    skeleton.EndUnit(unit);
    DwarfSectionBuilder(addresses_, Endian::kLittle).Fixed(address, 8);
  }

  DwarfSections sections() const {
    return {.info = skeleton_info_,
            .abbrev = skeleton_abbrev_,
            .addr = addresses_,
            .info_dwo = info_,
            .abbrev_dwo = abbrev_,
            .line_dwo = lines_.sections().line,
            .cu_index = cu_index_};
  }

  DwarfDieRef root_ref(size_t i) const { return refs_[i]; }

 private:
  using Spec = AbbrevTable::AttributeSpec;

  static Spec Attr(DwarfAttr name, DwarfForm form) {
    return {static_cast<uint64_t>(name), static_cast<uint64_t>(form)};
  }

  uint16_t version_;
  std::vector<uint8_t> abbrev_, info_, cu_index_;
  std::vector<uint8_t> skeleton_abbrev_, skeleton_info_, addresses_;
  LineTableTestSections lines_;
  std::array<DwarfDieRef, 2> refs_;
};

class DwarfPackageTest : public testing::TestWithParam<uint16_t> {};

TEST_P(DwarfPackageTest, ReadsPackageAbbreviationsOnWorkerThread) {
  DwarfPackageFixture fixture(GetParam());
  DwarfBinary binary(fixture.sections());
  std::array<DwarfDie, 2> roots;
  std::jthread reader([&] {
    for (size_t i = 0; i < roots.size(); ++i)
      roots[i] = binary.GetDieAtOffset(fixture.root_ref(i), true);
  });
  // Unit context lookups on the owning thread share the abbreviation cache with raw DIE lookups.
  for (size_t i = 0; i < fixture.kIds.size(); ++i) {
    auto context = binary.GetUnitContext(UnitIndex(true, i));
    EXPECT_TRUE(context) << context.err().msg();
  }
  reader.join();

  // Each fixture root uses a distinct abbreviation code and encodes its file name.
  for (size_t i = 0; i < roots.size(); ++i) {
    ASSERT_TRUE(roots[i]);
    EXPECT_STREQ(fixture.kNames[i], roots[i].short_name());
  }
}

TEST_P(DwarfPackageTest, ResolvesLineTablesByContribution) {
  DwarfPackageFixture fixture(GetParam());
  DwarfBinary binary(fixture.sections());
  ASSERT_EQ(2u, binary.GetDWOUnitCount());

  // Both fixture units reference relative offset zero and each encodes its own file.
  auto file_index = GetParam() == 5 ? 0 : 1;
  for (size_t i = 0; i < fixture.kNames.size(); ++i) {
    auto unit = binary.GetUnitAtIndex(UnitIndex(true, i));
    ASSERT_TRUE(unit);
    auto files = binary.GetLineFileTable(unit->GetUnitDie(DwarfDie::Load::kRootOnly), file_index);
    ASSERT_TRUE(files);
    const auto* file = files->GetFileName(file_index);
    ASSERT_TRUE(file);
    EXPECT_EQ(std::string("/src/") + fixture.kNames[i], *file);
  }
}

TEST_P(DwarfPackageTest, RejectsLineOffsetAtContributionEnd) {
  // DWARF 5 section 7.3.5.3 bases the offset on the unit's contribution. The first
  // unit's invalid offset reaches the second table in the same section.
  DwarfPackageFixture fixture(GetParam(), true);
  DwarfBinary binary(fixture.sections());
  EXPECT_FALSE(binary.GetLineProgramSource(UnitIndex(true, 0)));
}

TEST_P(DwarfPackageTest, MatchesSkeletonsByDwoId) {
  DwarfPackageFixture fixture(GetParam());
  // The fixture writes the matching skeletons' addresses in reverse unit order.
  constexpr uint64_t kAddresses[] = {0x1000, 0x2000};
  for (size_t i : {1, 0})
    fixture.AddSkeleton(fixture.kIds[i], kAddresses[i]);
  DwarfBinary binary(fixture.sections());
  for (size_t i = 0; i < fixture.kIds.size(); ++i) {
    auto context = binary.GetUnitContext(UnitIndex(true, i));
    ASSERT_TRUE(context) << context.err().msg();
    EXPECT_EQ(kAddresses[i], context.value()->base_address());
    EXPECT_EQ(kAddresses[i], context.value()->ReadAddress(0));
  }
}

TEST_P(DwarfPackageTest, LeavesUnmatchedUnitUnlinked) {
  DwarfPackageFixture fixture(GetParam());
  // The fixture's only skeleton ID matches the second split unit.
  fixture.AddSkeleton(fixture.kIds[1], 0x2000);
  DwarfBinary binary(fixture.sections());
  auto context = binary.GetUnitContext(UnitIndex(true, 0));
  ASSERT_TRUE(context) << context.err().msg();
  EXPECT_FALSE(context.value()->ReadAddress(0));
}

TEST_P(DwarfPackageTest, LeavesDuplicateSkeletonIdsUnlinked) {
  DwarfPackageFixture fixture(GetParam());
  // Both skeletons claim the first split unit, so neither supplies its address table.
  fixture.AddSkeleton(fixture.kIds[0], 0x1000);
  fixture.AddSkeleton(fixture.kIds[0], 0x2000);
  DwarfBinary binary(fixture.sections());
  auto context = binary.GetUnitContext(UnitIndex(true, 0));
  ASSERT_TRUE(context) << context.err().msg();
  EXPECT_FALSE(context.value()->ReadAddress(0));
}

INSTANTIATE_TEST_SUITE_P(Dwarf4And5, DwarfPackageTest, testing::Values(4, 5));

}  // namespace

TEST(DwarfBinary, Dwarf4TypeUnits) {
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
  EXPECT_EQ(root, binary.GetDieAtOffset(root.ref(), false));

  // TypeUnitOuter is at 0x0 + 0x25, and TypeUnitInner is at 0x5d + 0x25.
  EXPECT_EQ(DwarfDieRef::Type(0x25), binary.GetDieForSignature(0xde3dca152f8e7c98, false).ref());
  EXPECT_EQ(DwarfDieRef::Type(0x82), binary.GetDieForSignature(0xc9382c36fccd19d7, false).ref());
  EXPECT_FALSE(binary.GetDieForSignature(0, false));  // Absent from the fixture.
}

TEST(DwarfBinary, Dwarf5TypeUnits) {
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
  EXPECT_EQ(DwarfDieRef::Main(0x26), binary.GetDieForSignature(0xde3dca152f8e7c98, false).ref());
  EXPECT_EQ(DwarfDieRef::Main(0x84), binary.GetDieForSignature(0xc9382c36fccd19d7, false).ref());
  EXPECT_FALSE(binary.GetDieForSignature(0, false));  // Absent from the fixture.
}

// The checked-in test binary has a ".symtab" and a ".dynsym" section. The symbols of both are in
// the result.
TEST(DwarfBinary, ElfSymbols) {
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
TEST(DwarfBinary, ElfSymbolsStripped) {
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
TEST(DwarfBinary, ElfSymbolsDynamicPrecedence) {
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
  static size_t UnitEntries(const DwarfBinary& binary, UnitIndex index) {
    auto entry = binary.GetUnitHeader(index);
    if (!entry)
      return 0;
    return index.is_dwo ? binary.unit_contexts_.split_units_.count(&entry->header)
                        : binary.unit_contexts_.normal_units_.count(&entry->header);
  }

  static size_t AbbrevEntries(const DwarfBinary& binary) {
    return binary.unit_contexts_.abbrevs_.size();
  }

  static void ClearUnitContexts(DwarfBinary& binary) { binary.unit_contexts_.Clear(); }
};

TEST_F(UnitContextCacheTest, ReusesContextAcrossAttributesAndClears) {
  TestSymbolModule setup(TestSymbolModule::kCheckedIn);
  ASSERT_TRUE(setup.Init("", false).ok());
  auto& binary = *setup.symbols()->binary();
  // The first CU starts at offset zero. Its first function uses addrx for its low PC.
  auto unit = binary.GetUnitAtIndex(UnitIndex(false, 0));
  ASSERT_TRUE(unit);
  auto index = unit->GetUnitIndex();
  EXPECT_EQ(0u, UnitEntries(binary, index));

  constexpr DwarfAttr kAttrs[] = {DwarfAttr::kLowPc};
  DwarfDieDecoder decoder(binary, kAttrs);
  ASSERT_TRUE(decoder.Decode(binary.GetDieAtOffset(DwarfDieRef::Main(0x2b), false)));
  EXPECT_EQ(0x1000u, decoder.GetAddress(DwarfAttr::kLowPc));
  ASSERT_EQ(1u, UnitEntries(binary, index));

  auto first = binary.GetUnitContext(index);
  ASSERT_TRUE(first) << first.err().msg();
  EXPECT_EQ(0x1000u, first.value()->ReadAddress(0));
  EXPECT_EQ(".", unit->GetCompilationDir());
  auto second = binary.GetUnitContext(index);
  ASSERT_TRUE(second);
  EXPECT_EQ(first.value(), second.value());

  binary.ClearDwarfCaches();
  EXPECT_EQ(0u, UnitEntries(binary, index));
  auto rebuilt = binary.GetUnitContext(index);
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
  sections.Finish();
  DwarfBinary binary(sections.sections());
  const UnitIndex first_index(false, 0);
  auto first = binary.GetUnitContext(first_index);
  auto second = binary.GetUnitContext(UnitIndex(false, 1));
  ASSERT_TRUE(first) << first.err().msg();
  ASSERT_TRUE(second) << second.err().msg();
  EXPECT_NE(first.value(), second.value());
  EXPECT_EQ(0u, first.value()->stmt_list());
  EXPECT_EQ(0x20u, second.value()->stmt_list());
  EXPECT_EQ(1u, AbbrevEntries(binary));

  // Binaries with identical section bytes still own independent caches.
  DwarfBinary other(sections.sections());
  auto foreign_die = binary.GetUnitAtIndex(first_index)->GetUnitDie();
  EXPECT_TRUE(other.GetUnitContext(foreign_die).has_error());
  EXPECT_EQ(0u, UnitEntries(other, first_index));
  auto foreign = other.GetUnitContext(first_index);
  ASSERT_TRUE(foreign);
  EXPECT_EQ(0u, foreign.value()->stmt_list());
  EXPECT_NE(first.value(), foreign.value());
  binary.ClearDwarfCaches();
  EXPECT_EQ(0u, AbbrevEntries(binary));
}

TEST_F(UnitContextCacheTest, LineTableCallersPopulateCache) {
  TestSymbolModule setup(TestSymbolModule::kCheckedIn);
  ASSERT_TRUE(setup.Init("", false).ok());
  auto& binary = *setup.symbols()->binary();
  auto unit = binary.GetUnitAtIndex(UnitIndex(false, 0));
  ASSERT_TRUE(unit);
  auto index = unit->GetUnitIndex();

  EXPECT_EQ(0u, UnitEntries(binary, index));
  EXPECT_EQ(0u, AbbrevEntries(binary));
  auto root = unit->GetUnitDie(DwarfDie::Load::kRootOnly);
  ASSERT_TRUE(root);
  EXPECT_EQ(0u, UnitEntries(binary, index));
  auto files = binary.GetLineFileTable(root, 0);
  ASSERT_TRUE(files);
  ASSERT_TRUE(files->GetFileName(0));
  EXPECT_TRUE(files->GetFileName(0)->ends_with("/line_lookup_symbol_test.cc"));
  EXPECT_EQ(1u, UnitEntries(binary, index));
  EXPECT_EQ(1u, AbbrevEntries(binary));
  EXPECT_EQ(files, binary.GetLineFileTable(root, 0));
  EXPECT_EQ(1u, UnitEntries(binary, index));
  EXPECT_EQ(1u, AbbrevEntries(binary));

  // Start with empty caches so GetLineTable has to fetch the unit context itself.
  binary.ClearDwarfCaches();
  EXPECT_EQ(0u, UnitEntries(binary, index));
  const auto& lines = unit->GetLineTable();
  ASSERT_GT(lines.GetNumSequences(), 0u);
  EXPECT_EQ(1u, UnitEntries(binary, index));
  EXPECT_EQ(1u, AbbrevEntries(binary));
  EXPECT_EQ(lines.GetNumSequences(), unit->GetLineTable().GetNumSequences());
  EXPECT_EQ(1u, UnitEntries(binary, index));
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
  auto index = unit->GetUnitIndex();

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

  // Keep the complete line file table while testing a cold unit context and abbreviation cache.
  ASSERT_GT(unit->GetLineTable().GetNumSequences(), 0u);
  ClearUnitContexts(binary);
  ASSERT_EQ(0u, UnitEntries(binary, index));
  ASSERT_EQ(0u, AbbrevEntries(binary));
  for (int i = 0; i < 2; ++i) {
    auto symbol = binary.GetSymbolFactory()->CreateSymbol(function_ref);
    const auto* function = symbol->As<Function>();
    ASSERT_TRUE(function);
    EXPECT_TRUE(function->GetAssignedName().starts_with("LineTableFixtureSum"));
    EXPECT_GE(function->code_ranges().size(), 2u);
    EXPECT_EQ(1u, UnitEntries(binary, index));
    EXPECT_EQ(1u, AbbrevEntries(binary));
  }
}

TEST_F(UnitContextCacheTest, LocationListVariableDecodePopulatesCache) {
  TestSymbolModule setup(TestSymbolModule::kCheckedIn);
  ASSERT_TRUE(setup.Init("", false).ok());
  auto& binary = *setup.symbols()->binary();
  auto unit = binary.GetUnitAtIndex(UnitIndex(false, 2));
  ASSERT_TRUE(unit);
  auto index = unit->GetUnitIndex();
  ASSERT_EQ(0u, UnitEntries(binary, index));
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
    EXPECT_EQ(1u, UnitEntries(binary, index));
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
  auto skeleton = unit->GetUnitIndex();
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
  auto skeleton = main_unit->GetUnitIndex();
  EXPECT_EQ(0u, UnitEntries(main_binary, skeleton));

  DwarfBinary split_binary(dir + "main.dwo", "", "");
  ASSERT_TRUE(split_binary.Load({}, DwarfSymbolFactory::kDWO).ok());
  auto split_unit = split_binary.GetUnitAtIndex(UnitIndex(true, 0));
  ASSERT_TRUE(split_unit);
  auto split = split_unit->GetUnitIndex();

  auto unlinked = split_binary.GetUnitContext(split);
  ASSERT_TRUE(unlinked);
  EXPECT_EQ(std::nullopt, unlinked.value()->base_address());
  EXPECT_EQ(std::nullopt, unlinked.value()->ReadAddress(0));

  UnitContextSource source{&main_binary, skeleton};
  auto linked = split_binary.GetUnitContext(split, source);
  ASSERT_TRUE(linked);
  EXPECT_NE(unlinked.value(), linked.value());
  EXPECT_EQ(2u, UnitEntries(split_binary, split));
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
  std::vector<uint8_t> abbrev, info;
  DwarfSectionBuilder table(abbrev, Endian::kLittle);
  const AbbrevTable::AttributeSpec attrs[] = {
      {static_cast<uint64_t>(DwarfAttr::kStmtList), static_cast<uint64_t>(DwarfForm::kSecOffset)}};
  table.AbbrevDeclaration(1, static_cast<uint64_t>(DwarfTag::kCompileUnit), false, attrs);
  table.EndAbbrevTable();
  DwarfSectionBuilder writer(info, Endian::kLittle);
  auto bad = writer.BeginUnit({});
  writer.Uleb(0);  // Invalid root abbreviation, with a valid unit header.
  writer.EndUnit(bad);
  auto good = writer.BeginUnit({});
  writer.Uleb(1);
  writer.Offset(0xb4, 4);
  writer.EndUnit(good);
  DwarfBinary binary(DwarfSections{.info = info, .abbrev = abbrev});
  auto unit = binary.GetUnitAtIndex(UnitIndex(false, 0));
  ASSERT_TRUE(unit);
  auto index = unit->GetUnitIndex();

  auto initial = binary.GetUnitContext(index);
  ASSERT_TRUE(initial.has_error());
  EXPECT_EQ("DWARF unit DIE for unit at offset 0x0: invalid root abbreviation code",
            initial.err().msg());
  ASSERT_EQ(1u, UnitEntries(binary, index));
  auto repeated = binary.GetUnitContext(index);
  ASSERT_TRUE(repeated.has_error());
  EXPECT_EQ(initial.err(), repeated.err());
  EXPECT_EQ("", unit->GetCompilationDir());

  auto next_unit = binary.GetUnitAtIndex(UnitIndex(false, 1));
  ASSERT_TRUE(next_unit);
  auto next = binary.GetUnitContext(next_unit->GetUnitIndex());
  ASSERT_TRUE(next);
  EXPECT_EQ(0xb4u, next.value()->stmt_list());
}

}  // namespace zxdb
