// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/elflib/elflib.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>

#include <gtest/gtest.h>

#include "src/lib/elflib/elflib_test_support.h"
#include "src/lib/files/scoped_temp_dir.h"

namespace elflib {
namespace {

constexpr uint8_t kPayload[] = {1, 2, 3};

std::unique_ptr<ElfLib> OpenTestElf(const std::string& bytes) {
  return ElfLib::Create(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

template <typename Chdr>
std::vector<uint8_t> ZlibSection(Chdr header) {
  std::vector<uint8_t> bytes(sizeof(header) + compressBound(sizeof(kPayload)));
  std::memcpy(bytes.data(), &header, sizeof(header));
  uLongf size = bytes.size() - sizeof(header);
  EXPECT_EQ(Z_OK, compress(bytes.data() + sizeof(header), &size, kPayload, sizeof(kPayload)));
  bytes.resize(sizeof(header) + size);
  return bytes;
}

TEST(ElfLib, FindAndReadUncompressedSection) {
  // The fixture puts .sample first after the ELF null section.
  auto elf = test::MakeElf({{".sample", kPayload}});
  auto reader = OpenTestElf(elf);
  ASSERT_TRUE(reader);
  EXPECT_EQ(".sample", reader->GetSectionName(1));
  EXPECT_TRUE(reader->GetSectionName(999).empty());
  EXPECT_EQ(1u, reader->FindSection(".sample"));
  EXPECT_FALSE(reader->FindSection(".missing"));

  auto section = reader->ReadSection(1);
  ASSERT_TRUE(section.is_ok());
  EXPECT_EQ(sizeof(kPayload), section.value().size());
  // The fixture stores this section immediately after the ELF header.
  EXPECT_EQ(reinterpret_cast<const uint8_t*>(elf.data()) + sizeof(Elf64_Ehdr),
            section.value().data());
  EXPECT_TRUE(reader->ReadSection(999).is_error());
}

TEST(ElfLib, ReadsZlibFromElf32AndElf64) {
  // ELF defines a distinct compression header for each class. Both encode the same payload.
  auto check = []<typename Ehdr, typename Chdr>() {
    auto compressed = ZlibSection(
        Chdr{.ch_type = ELFCOMPRESS_ZLIB, .ch_size = sizeof(kPayload), .ch_addralign = 1});
    auto elf = test::MakeElf<Ehdr>({{".sample", compressed, SHT_PROGBITS, SHF_COMPRESSED}});
    auto reader = OpenTestElf(elf);
    ASSERT_TRUE(reader);
    auto section = reader->ReadSection(1);
    ASSERT_TRUE(section.is_ok()) << section.error_value();
    EXPECT_TRUE(std::ranges::equal(kPayload, section.value()));
  };
  check.operator()<Elf32_Ehdr, Elf32_Chdr>();
  check.operator()<Elf64_Ehdr, Elf64_Chdr>();
}

TEST(ElfLib, ExtendedSectionNumbering) {
  // ELF uses section zero's sh_size for e_shnum == 0 and sh_link for SHN_XINDEX.
  auto elf = test::MakeElf({{".debug_info", kPayload}}, true);
  auto reader = OpenTestElf(elf);
  ASSERT_TRUE(reader);
  EXPECT_EQ(0u, reader->GetEhdr()->e_shnum);
  EXPECT_EQ(SHN_XINDEX, reader->GetEhdr()->e_shstrndx);
  EXPECT_EQ(3u, reader->GetSectionHeaders().size());
  EXPECT_EQ(1u, reader->FindSection(".debug_info"));
  EXPECT_TRUE(reader->ProbeHasDebugInfo());
  EXPECT_FALSE(reader->ProbeHasProgramBits());
}

TEST(ElfLib, TruncatedExtendedSectionTable) {
  // The extended count still includes the final header after its last byte is removed.
  auto elf = test::MakeElf({{".sample", kPayload}}, true);
  elf.pop_back();
  auto reader = ElfLib::Create(
      [&elf](uint64_t offset, std::vector<uint8_t>* bytes) {
        // A failed extent check must keep later reads from requesting the whole table.
        EXPECT_LE(bytes->size(), sizeof(Elf64_Shdr));
        if (offset > elf.size() || bytes->size() > elf.size() - offset)
          return false;
        std::memcpy(bytes->data(), elf.data() + offset, bytes->size());
        return true;
      },
      0, ElfLib::AddressMode::kFile);
  ASSERT_TRUE(reader);
  EXPECT_TRUE(reader->GetSectionHeaders().empty());
  EXPECT_TRUE(reader->GetSectionHeaders().empty());
}

TEST(ElfLib, ReadSectionCompressionErrors) {
  const uint8_t truncated[] = {ELFCOMPRESS_ZLIB};  // Shorter than an Elf64_Chdr.
  // The stream produces three bytes. ELF requires ch_size to match that size.
  auto wrong_size = ZlibSection(
      Elf64_Chdr{.ch_type = ELFCOMPRESS_ZLIB, .ch_size = sizeof(kPayload) + 1, .ch_addralign = 1});
  auto elf = test::MakeElf({{".truncated", truncated, SHT_PROGBITS, SHF_COMPRESSED},
                            {".wrong_size", wrong_size, SHT_PROGBITS, SHF_COMPRESSED}});
  auto reader = OpenTestElf(elf);
  ASSERT_TRUE(reader);
  auto bad_header = reader->ReadSection(1);
  ASSERT_TRUE(bad_header.is_error());
  EXPECT_STREQ("truncated ELF compression header", bad_header.error_value());
  auto bad_size = reader->ReadSection(2);
  ASSERT_TRUE(bad_size.is_error());
  EXPECT_STREQ("decompressed size differs from the section header size", bad_size.error_value());
}

TEST(ElfLib, MappedLoaderRejectsNonNativeByteOrder) {
  // ELF's EI_DATA identifies big-endian encoding, which the host loader rejects.
  auto elf = test::MakeElf({});
  elf[EI_DATA] = ELFDATA2MSB;
  files::ScopedTempDir temp_dir;
  std::string path;
  ASSERT_TRUE(temp_dir.NewTempFileWithData(elf, &path));
  EXPECT_TRUE(ElfLib::CreateMapped(path).is_error());
}

}  // namespace
}  // namespace elflib
