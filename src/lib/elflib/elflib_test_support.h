// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_ELFLIB_ELFLIB_TEST_SUPPORT_H_
#define SRC_LIB_ELFLIB_ELFLIB_TEST_SUPPORT_H_

#include <elf.h>

#include <cstring>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace elflib::test {

struct Section {
  std::string_view name;
  std::span<const uint8_t> bytes;
  uint32_t type = SHT_PROGBITS;
  uint64_t flags = 0;
  uint64_t address = 0;
  uint32_t info = 0;
};

// Writes a little-endian ELF file with section headers and no program headers.
template <typename Ehdr = Elf64_Ehdr>
std::string MakeElf(std::initializer_list<Section> data, bool extended_sections = false) {
  constexpr bool is_64_bit = std::is_same_v<Ehdr, Elf64_Ehdr>;
  using Shdr = std::conditional_t<is_64_bit, Elf64_Shdr, Elf32_Shdr>;
  std::string names("\0.shstrtab\0", 11);
  std::vector<Shdr> sections(1);  // ELF section zero is null.
  std::string elf(sizeof(Ehdr), '\0');

  for (const auto& section : data) {
    auto& header = sections.emplace_back();
    header.sh_name = names.size();
    header.sh_type = section.type;
    header.sh_flags = section.flags;
    header.sh_addr = section.address;
    header.sh_offset = elf.size();
    header.sh_size = section.bytes.size();
    header.sh_info = section.info;
    names.append(section.name).push_back('\0');
    if (!section.bytes.empty())
      elf.append(reinterpret_cast<const char*>(section.bytes.data()), section.bytes.size());
  }
  size_t names_index = sections.size();
  auto& name_header = sections.emplace_back();
  name_header.sh_name = 1;
  name_header.sh_type = SHT_STRTAB;
  name_header.sh_offset = elf.size();
  name_header.sh_size = names.size();
  elf.append(names);
  elf.resize((elf.size() + alignof(Shdr) - 1) & ~(alignof(Shdr) - 1));

  Ehdr header = {
      .e_ident = {ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3, is_64_bit ? ELFCLASS64 : ELFCLASS32,
                  ELFDATA2LSB, EV_CURRENT},
      .e_type = ET_REL,
      .e_machine = is_64_bit ? EM_X86_64 : EM_386,
      .e_version = EV_CURRENT,
      .e_ehsize = sizeof(Ehdr),
      .e_shentsize = sizeof(Shdr),
  };
  header.e_shoff = elf.size();
  if (extended_sections) {
    header.e_shstrndx = SHN_XINDEX;
    sections[0].sh_size = sections.size();
    sections[0].sh_link = names_index;
  } else {
    header.e_shnum = sections.size();
    header.e_shstrndx = names_index;
  }
  std::memcpy(elf.data(), &header, sizeof(header));
  elf.append(reinterpret_cast<const char*>(sections.data()), sections.size() * sizeof(Shdr));
  return elf;
}

}  // namespace elflib::test

#endif  // SRC_LIB_ELFLIB_ELFLIB_TEST_SUPPORT_H_
