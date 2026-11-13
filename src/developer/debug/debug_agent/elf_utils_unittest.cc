// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/elf_utils.h"

#include <gtest/gtest.h>
#include <link.h>
#undef PT_LOAD  // The fixtures use elflib::PT_LOAD.
#include "src/developer/debug/debug_agent/mock_process_handle.h"

#if defined(__Fuchsia__)
#include "src/developer/debug/debug_agent/zircon_process_handle.h"
#endif

#include "src/developer/debug/ipc/records.h"

namespace debug_agent {

namespace {

#if defined(__Fuchsia__)

void ValidateModules(const std::vector<debug_ipc::Module>& modules) {
  // It should contain at least libc, libfdio, vdso and the main executable.
  EXPECT_GT(modules.size(), 4u);

  bool has_libc = false;
  for (const auto& module : modules) {
    if (module.name == "libc.so") {
      has_libc = true;
      EXPECT_FALSE(module.build_id.empty());
    }
  }
  EXPECT_TRUE(has_libc);
}

TEST(ElfUtils, GetElfModulesForProcess) {
  zx::process handle;
  zx::process::self()->duplicate(ZX_RIGHT_SAME_RIGHTS, &handle);
  ZirconProcessHandle self(std::move(handle));

  uintptr_t dl_debug_addr;
  ASSERT_EQ(ZX_OK, zx::process::self()->get_property(ZX_PROP_PROCESS_DEBUG_ADDR, &dl_debug_addr,
                                                     sizeof(dl_debug_addr)));

  ValidateModules(GetElfModulesForProcess(self, dl_debug_addr));
}

TEST(ElfUtils, GetElfModulesForProcessNoDebugAddr) {
  zx::process handle;
  zx::process::self()->duplicate(ZX_RIGHT_SAME_RIGHTS, &handle);
  ZirconProcessHandle self(std::move(handle));

  ValidateModules(GetElfModulesForProcess(self, 0));
}

#endif

TEST(ElfUtils, MergeMmapedModules) {
  const char kBinaryName[] = "/home/me/a.out";
  const char kLibCName[] = "/lib/libc.so.6";
  const char kLibFooName[] = "/home/me/libfoo.so";

  constexpr uint64_t kBase1 = 0x1000000;
  constexpr uint64_t kBase2 = 0x2000000;
  constexpr uint64_t kBase3 = 0x3000000;

  std::vector<debug_ipc::Module> modules;
  modules.push_back(debug_ipc::Module{.name = kBinaryName, .base = kBase1, .build_id = "1234"});
  modules.push_back(debug_ipc::Module{.name = kLibCName, .base = kBase2, .build_id = "2345"});

  std::vector<debug_ipc::AddressRegion> maps;
  // A duplicate region for the existing map of a.out.
  maps.push_back(
      debug_ipc::AddressRegion{.name = kBinaryName, .base = kBase1, .size = 0x1000, .read = true});
  maps.push_back(debug_ipc::AddressRegion{
      .name = kBinaryName, .base = kBase1 + 0x1000, .size = 0x1000, .read = true});

  std::vector<elflib::Elf64_Phdr> a_segs{
      {.p_type = elflib::PT_LOAD, .p_vaddr = 0, .p_memsz = 0x1000},
      {.p_type = elflib::PT_LOAD, .p_vaddr = 0x1000, .p_memsz = 0x1000}};

  // Non-readable and writable sections shouldn't be probed for ELF headers.
  maps.push_back(debug_ipc::AddressRegion{.name = "/unreadable", .base = kBase2, .size = 0x1000});
  maps.push_back(debug_ipc::AddressRegion{
      .name = "/writable", .base = kBase2 + 0x1000, .size = 0x1000, .read = true, .write = true});

  // This region should be merged in.
  maps.push_back(
      debug_ipc::AddressRegion{.name = kLibFooName, .base = kBase3, .size = 0x1000, .read = true});
  maps.push_back(debug_ipc::AddressRegion{
      .name = kLibFooName, .base = kBase3 + 0x1000, .size = 0x1000, .read = true});

  std::vector<elflib::Elf64_Phdr> foo_segs{
      {.p_type = elflib::PT_LOAD, .p_vaddr = 0, .p_memsz = 0x1000},
      {.p_type = elflib::PT_LOAD, .p_vaddr = 0x1000, .p_memsz = 0x1000}};

  auto get_elf_info = [&](uint64_t base) -> std::optional<internal::ElfSegInfo> {
    switch (base) {
      case kBase1: {
        return internal::ElfSegInfo{
            .segment_headers = a_segs, .so_name = kBinaryName, .build_id = "1234"};
      }
      case kBase3: {
        return internal::ElfSegInfo{
            .segment_headers = foo_segs, .so_name = kLibFooName, .build_id = "abcd"};
      }
      default:
        ADD_FAILURE() << "Got address 0x" << std::hex << base;
        return {};
    }
  };

  internal::MergeMmapedModules(modules, maps, get_elf_info);

  ASSERT_EQ(3u, modules.size());
  EXPECT_EQ(modules[0].name, kBinaryName);
  EXPECT_EQ(modules[0].base, kBase1);
  EXPECT_EQ(modules[1].name, kLibCName);
  EXPECT_EQ(modules[1].base, kBase2);
  EXPECT_EQ(modules[2].name, kLibFooName);
  EXPECT_EQ(modules[2].base, kBase3);
}

TEST(ElfUtils, DifferentLoadBiases) {
  std::vector<debug_ipc::Module> modules;
  std::vector<debug_ipc::AddressRegion> maps = {
      {.base = 0x400000, .size = 0x2000, .read = true},
      {.base = 0x800000, .size = 0x1000, .read = true},
      {.base = 0x801000, .size = 0x1000, .read = true},
  };
  std::vector<uint64_t> probes;
  internal::MergeMmapedModules(modules, maps, [&](uint64_t base) {
    probes.push_back(base);
    uint64_t vaddr = base == 0x400000 ? 0x400000 : 0;
    return std::optional<internal::ElfSegInfo>({
        .segment_headers = {{.p_type = elflib::PT_LOAD, .p_vaddr = vaddr, .p_memsz = 0x2000}}});
  });
  EXPECT_EQ((std::vector<uint64_t>{0x400000, 0x800000}), probes);
  ASSERT_EQ(2u, modules.size());
  EXPECT_EQ(0u, modules[0].base);
  EXPECT_EQ(0x800000u, modules[1].base);
}

// This is the same as above, but inserts a module between two segment headers from another module.
TEST(ElfUtils, MergeMmapedModulesWithHole) {
  const char kBinaryName[] = "/home/me/a.out";
  const char kLibCName[] = "/lib/libc.so.6";
  const char kLibFooName[] = "/home/me/libfoo.so";
  const char kLibFillsHoleName[] = "blob-1234";

  constexpr uint64_t kBase1 = 0x1000000;
  constexpr uint64_t kBase2 = 0x2000000;
  constexpr uint64_t kBase3 = 0x3000000;
  // Be very careful to not place the base address of the module that fills the hole precisely at
  // the previous base + size of the first segment.
  constexpr uint64_t kBase4 = 0x3004000;

  std::vector<debug_ipc::Module> modules;
  modules.push_back(debug_ipc::Module{.name = kBinaryName, .base = kBase1, .build_id = "1234"});
  modules.push_back(debug_ipc::Module{.name = kLibCName, .base = kBase2, .build_id = "2345"});
  modules.push_back(debug_ipc::Module{.name = kLibFooName, .base = kBase3, .build_id = "3456"});

  std::vector<debug_ipc::AddressRegion> maps;
  // A duplicate region for the existing map of a.out.
  maps.push_back(
      debug_ipc::AddressRegion{.name = kBinaryName, .base = kBase1, .size = 0x1000, .read = true});
  maps.push_back(debug_ipc::AddressRegion{
      .name = kBinaryName, .base = kBase1 + 0x1000, .size = 0x1000, .read = true});

  std::vector<elflib::Elf64_Phdr> a_segs{
      {.p_type = elflib::PT_LOAD, .p_vaddr = 0, .p_memsz = 0x1000},
      {.p_type = elflib::PT_LOAD, .p_vaddr = 0x1000, .p_memsz = 0x1000}};

  // Non-readable and writable sections shouldn't be probed for ELF headers.
  maps.push_back(debug_ipc::AddressRegion{.name = "/unreadable", .base = kBase2, .size = 0x1000});
  maps.push_back(debug_ipc::AddressRegion{
      .name = "/writable", .base = kBase2 + 0x1000, .size = 0x1000, .read = true, .write = true});

  // This region should be merged in.
  maps.push_back(
      debug_ipc::AddressRegion{.name = kLibFooName, .base = kBase3, .size = 0x1000, .read = true});

  // There will only be one mapping that represents the entire module that fills the hole.
  // Importantly make sure that it is in fact mapped between the two segments of the module at
  // |kBase3|.
  maps.push_back(debug_ipc::AddressRegion{
      .name = kLibFillsHoleName, .base = kBase4, .size = 0x1000, .read = true});

  std::vector<elflib::Elf64_Phdr> hole_segs{
      {.p_type = elflib::PT_LOAD, .p_vaddr = 0, .p_memsz = 0x1000}};

  // This mapping represents the second header that has a large offset from the base address of this
  // module. In between this header and the one above will be the entirety of another module that we
  // must be sure to find.
  maps.push_back(debug_ipc::AddressRegion{
      .name = kLibFooName, .base = kBase3 + 0x10000, .size = 0x1000, .read = true});

  std::vector<elflib::Elf64_Phdr> foo_segs{
      {.p_type = elflib::PT_LOAD, .p_vaddr = 0, .p_memsz = 0x1000},
      {.p_type = elflib::PT_LOAD, .p_vaddr = 0x10000, .p_memsz = 0x1000}};

  auto get_elf_info = [&](uint64_t base) -> std::optional<internal::ElfSegInfo> {
    switch (base) {
      case kBase1: {
        return internal::ElfSegInfo{
            .segment_headers = a_segs, .so_name = kBinaryName, .build_id = "1234"};
      }
      case kBase3 + 0x10000:
        // In practice the headers that are mapped after the "hole" do not contain the ELF headers
        // necessary to do any parsing so they won't be recognized as individual modules separate
        // from the first instance (which should always contain the ELF header).
        return std::nullopt;
      case kBase3: {
        return internal::ElfSegInfo{
            .segment_headers = foo_segs, .so_name = kLibFooName, .build_id = "abcd"};
      }
      case kBase4: {
        return internal::ElfSegInfo{
            .segment_headers = hole_segs, .so_name = kLibFillsHoleName, .build_id = "3456"};
      }
      default:
        ADD_FAILURE() << "Got address 0x" << std::hex << base;
        return {};
    }
  };

  internal::MergeMmapedModules(modules, maps, get_elf_info);

  ASSERT_EQ(4u, modules.size());
  EXPECT_EQ(modules[0].name, kBinaryName);
  EXPECT_EQ(modules[0].base, kBase1);
  EXPECT_EQ(modules[1].name, kLibCName);
  EXPECT_EQ(modules[1].base, kBase2);
  EXPECT_EQ(modules[2].name, kLibFooName);
  EXPECT_EQ(modules[2].base, kBase3);
  EXPECT_EQ(modules[3].name, kLibFillsHoleName);
  EXPECT_EQ(modules[3].base, kBase4);
}

}  // namespace


TEST(ElfUtils, RejectShortAndChangingLoaderReads) {
  MockProcessHandle process(1234);
  int callbacks = 0;
  auto visit = [&](uint64_t, uint64_t) { ++callbacks; return true; };
  process.mock_memory().AddMemory(0x1000, {0});
  EXPECT_TRUE(WalkElfModules(process, 0x1000, visit).has_error());
  r_debug header{};
  header.r_state = r_debug::RT_ADD;
  auto bytes = reinterpret_cast<const uint8_t*>(&header);
  process.mock_memory().AddMemory(0x1000, {bytes, bytes + sizeof(header)});
  EXPECT_TRUE(WalkElfModules(process, 0x1000, visit).has_error());
  header.r_state = r_debug::RT_CONSISTENT;
  header.r_map = reinterpret_cast<link_map*>(0x2000);
  process.mock_memory().AddMemory(0x1000, {bytes, bytes + sizeof(header)});
  process.mock_memory().AddMemory(0x2000, {0});
  EXPECT_TRUE(WalkElfModules(process, 0x1000, visit).has_error());
  link_map entry{};
  entry.l_next = reinterpret_cast<link_map*>(0x2000);
  auto map_bytes = reinterpret_cast<const uint8_t*>(&entry);
  process.mock_memory().AddMemory(0x2000, {map_bytes, map_bytes + sizeof(entry)});
  EXPECT_TRUE(WalkElfModules(process, 0x1000, visit).has_error());
  EXPECT_EQ(0, callbacks);
  entry.l_next = nullptr;
  process.mock_memory().AddMemory(0x2000, {map_bytes, map_bytes + sizeof(entry)});
  EXPECT_TRUE(WalkElfModules(process, 0x1000, visit).ok());
  EXPECT_EQ(1, callbacks);
}

}  // namespace debug_agent
