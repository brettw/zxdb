// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/linux_utils.h"

#include <gtest/gtest.h>
#include <lib/fit/defer.h>

#include <sys/mman.h>
#include <unistd.h>
#include "src/developer/debug/debug_agent/linux_process_handle.h"
#include "src/developer/debug/debug_agent/linux_task.h"
#include "src/developer/debug/shared/message_loop_linux.h"


namespace debug_agent {
namespace linux {

TEST(LinuxUtils, ParseMaps) {
  // Empty input.
  std::vector<MapEntry> result = ParseMaps("");
  EXPECT_TRUE(result.empty());

  result = ParseMaps("\n");
  EXPECT_TRUE(result.empty());

  const char kTestInput[] =
      R"(562288f50000-562288f67000 r--p 00000000 fe:01 11534420                   /usr/bin/zsh
562288f67000-562288ffa000 r-xp 00017000 fe:01 11534420                   /usr/bin/zsh
562288ffa000-56228901c000 r--p 000aa000 fe:01 11534420                   /usr/bin/zsh
562289024000-562289038000 rw-p 00000000 00:00 0 
56228966d000-5622897d2000 rw-p 00000000 00:00 0                          [heap]
7fdff47df000-7fdff4a5f000 r--s 00000000 fe:01 14559256                   /usr/share/zsh/functions/Completion/Unix.zwc
7fdff4a6a000-7fdff4a6d000 r--p 00000000 fe:01 12720175                   /usr/lib/x86_64-linux-gnu/zsh/5.8/zsh/computil
.so
)";
  result = ParseMaps(kTestInput);
  ASSERT_EQ(7u, result.size());

  // Check all of the details of one of the lines.
  EXPECT_EQ(0x562288f67000ull, result[1].range.begin());
  EXPECT_EQ(0x562288ffa000ull, result[1].range.end());
  EXPECT_TRUE(result[1].read);
  EXPECT_FALSE(result[1].write);
  EXPECT_TRUE(result[1].execute);
  EXPECT_FALSE(result[1].shared);
  EXPECT_EQ(0x17000ull, result[1].offset);
  EXPECT_EQ("fe:01", result[1].device);
  EXPECT_EQ("/usr/bin/zsh", result[1].path);
}

TEST(LinuxUtils, ParseStatus) {
  // Empty input.
  std::map<std::string, std::string> result = ParseStatus("");
  EXPECT_TRUE(result.empty());

  result = ParseStatus("\n");
  EXPECT_TRUE(result.empty());

  const char kTestInput[] = R"(Name:   zsh
Umask:  0022
State:  S (sleeping)
Tgid:   1738
Ngid:   0
Pid:    1738
PPid:   1736
TracerPid:      0
Uid:    1000    1000    1000    1000
Gid:    1000    1000    1000    1000
FDSize: 64
Groups: 1000 1001 665357 20 24 25 27 29 44 46 100 109 
NStgid: 1738
NSpid:  1738
NSpgid: 1738
NSsid:  1738
VmPeak:     8912 kB
VmSize:     8896 kB
VmLck:         0 kB
VmPin:         0 kB
VmHWM:      4772 kB
VmRSS:      4772 kB
RssAnon:             720 kB
RssFile:            4052 kB
RssShmem:              0 kB
VmData:      660 kB
VmStk:       132 kB
VmExe:       600 kB
VmLib:      2600 kB
VmPTE:        52 kB
VmSwap:        0 kB
HugetlbPages:          0 kB
CoreDumping:    0
THP_enabled:    1
Threads:        1
SigQ:   2/120935
SigPnd: 0000000000000000
ShdPnd: 0000000000000000
SigBlk: 0000000000000002
SigIgn: 0000000000384004
SigCgt: 0000000008013003
CapInh: 0000000000000000
CapPrm: 0000000000000000
CapEff: 0000000000000000
CapBnd: 000001ffffffffff
CapAmb: 0000000000000000
NoNewPrivs:     0
Seccomp:        2
Seccomp_filters:        1
Speculation_Store_Bypass:       thread force mitigated
SpeculationIndirectBranch:      conditional force disabled
Cpus_allowed:   fff
Cpus_allowed_list:      0-11
Mems_allowed:   1
Mems_allowed_list:      0
voluntary_ctxt_switches:        1207
nonvoluntary_ctxt_switches:     152
)";
  result = ParseStatus(kTestInput);

  // Spot check some values.
  EXPECT_EQ("zsh", result["Name"]);
  EXPECT_EQ("1738", result["Pid"]);
}

}  // namespace linux
}  // namespace debug_agent

namespace debug_agent::linux {
TEST(LinuxUtils, AuxvBounds) {
  const uint64_t words[] = {AT_PHENT, sizeof(Elf64_Phdr), AT_BASE, 123,
                            AT_NULL, 0, AT_ENTRY, 456};
  std::string storage(1, 'x');
  storage.append(reinterpret_cast<const char*>(words), sizeof(words));
  auto bytes = std::string_view(storage).substr(1);
  auto parsed = ParseAuxv(bytes);
  ASSERT_EQ(2u, parsed.size());
  EXPECT_EQ(123u, parsed.at(AT_BASE));
  EXPECT_TRUE(ParseAuxv({}).empty());
  for (size_t size = 1; size < 16; ++size)
    EXPECT_TRUE(ParseAuxv(bytes.substr(0, size)).empty());
  EXPECT_EQ(123u, ParseAuxv(bytes.substr(0, 32)).at(AT_BASE));
}

TEST(LinuxUtils, AuxvRejectsUnsupportedLayout) {
  const uint32_t compat[] = {AT_PHENT, sizeof(Elf32_Phdr), AT_BASE, 123,
                             AT_ENTRY, 456, AT_NULL, 0};
  EXPECT_TRUE(ParseAuxv(std::string_view(reinterpret_cast<const char*>(compat),
                                       sizeof(compat))).empty());
  const uint64_t wrong_size[] = {AT_PHENT, sizeof(Elf32_Phdr), AT_NULL, 0};
  EXPECT_TRUE(ParseAuxv(std::string_view(reinterpret_cast<const char*>(wrong_size),
                                       sizeof(wrong_size))).empty());
  const uint64_t missing[] = {AT_BASE, 123, AT_NULL, 0};
  EXPECT_TRUE(ParseAuxv(std::string_view(reinterpret_cast<const char*>(missing),
                                       sizeof(missing))).empty());
  auto native = ReadAuxv(getpid());
  ASSERT_NE(native.end(), native.find(AT_PHENT));
  EXPECT_EQ(sizeof(Elf64_Phdr), native.at(AT_PHENT));
}

TEST(LinuxUtils, LargeInodeAndMalformedMaps) {
  auto maps = ParseMaps("1000-2000 r--p 0 00:01 4294967296 /large inode\n"
                        "2000-3000 rw-p 0 00:00 0\n");
  ASSERT_EQ(2u, maps.size());
  EXPECT_EQ(4294967296ull, maps[0].inode);
  EXPECT_EQ("/large inode", maps[0].path);
  EXPECT_TRUE(maps[1].path.empty());
  EXPECT_TRUE(ParseMaps("1000+2000 r--p 0 00:00 0").empty());
  EXPECT_TRUE(ParseMaps("2000-1000 r--p 0 00:00 0").empty());
  EXPECT_TRUE(ParseMaps("1000-2000 r--p 0 00:00").empty());
}
}  // namespace debug_agent::linux


namespace debug_agent {
TEST(LinuxMemory, PartialReadAndInvalidRanges) {
  debug::MessageLoopLinux loop;
  std::string error;
  ASSERT_TRUE(loop.Init(&error));
  auto cleanup = fit::defer([&] { loop.Cleanup(); });
  {
    auto task = fxl::MakeRefCounted<LinuxTask>(getpid());
    LinuxProcessHandle process(task);
    size_t page = sysconf(_SC_PAGESIZE);
    void* mapping = mmap(nullptr, page * 3, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(MAP_FAILED, mapping);
    auto unmap = fit::defer([&] { munmap(mapping, page * 3); });
    auto bytes = static_cast<uint8_t*>(mapping);
    bytes[page - 1] = 0xab;
    ASSERT_EQ(0, mprotect(bytes + page, page, PROT_NONE));
    auto blocks = process.ReadMemoryBlocks(reinterpret_cast<uintptr_t>(bytes + page - 1), 2);
    ASSERT_EQ(2u, blocks.size());
    EXPECT_TRUE(blocks[0].valid);
    EXPECT_EQ(1u, blocks[0].size);
    EXPECT_EQ(std::vector<uint8_t>({0xab}), blocks[0].data);
    EXPECT_FALSE(blocks[1].valid);
    EXPECT_EQ(1u, blocks[1].size);
    EXPECT_EQ(blocks[0].address + 1, blocks[1].address);
    EXPECT_TRUE(blocks[1].data.empty());
    EXPECT_EQ(0u, blocks[1].data.capacity());
    bytes[page * 2] = 0xcd;
    auto base = reinterpret_cast<uintptr_t>(bytes);
    blocks = process.ReadMemoryBlocks(base + page - 1, page + 2);
    ASSERT_EQ(3u, blocks.size());
    EXPECT_EQ(std::vector<uint8_t>({0xab}), blocks[0].data);
    EXPECT_FALSE(blocks[1].valid);
    EXPECT_EQ(0u, blocks[1].data.capacity());
    EXPECT_EQ(page, blocks[1].size);
    EXPECT_TRUE(blocks[2].valid);
    EXPECT_EQ(base + page * 2, blocks[2].address);
    EXPECT_EQ(std::vector<uint8_t>({0xcd}), blocks[2].data);
    blocks = process.ReadMemoryBlocks(base + page, page + 1);
    ASSERT_EQ(2u, blocks.size());
    EXPECT_FALSE(blocks[0].valid);
    EXPECT_EQ(0u, blocks[0].data.capacity());
    EXPECT_EQ(page, blocks[0].size);
    EXPECT_TRUE(blocks[1].valid);
    EXPECT_EQ(std::vector<uint8_t>({0xcd}), blocks[1].data);
    // Repeat with an unmapped hole instead of a protected mapping.
    ASSERT_EQ(0, munmap(bytes + page, page));
    blocks = process.ReadMemoryBlocks(base + page - 1, page + 2);
    ASSERT_EQ(3u, blocks.size());
    EXPECT_TRUE(blocks[0].valid);
    EXPECT_FALSE(blocks[1].valid);
    EXPECT_EQ(0u, blocks[1].data.capacity());
    EXPECT_EQ(page, blocks[1].size);
    EXPECT_EQ(std::vector<uint8_t>({0xcd}), blocks[2].data);
    size_t actual = 123;
    uint8_t dest;
    EXPECT_TRUE(process.ReadMemory(0, &dest, 1, &actual).has_error());
    EXPECT_EQ(0u, actual);
    EXPECT_FALSE(process.ReadMemoryBlocks(UINT64_MAX, 2)[0].valid);
    EXPECT_TRUE(process.ReadMemoryBlocks(0, 0)[0].valid);
  }
}

TEST(LinuxCapabilities, NoHardwareSlotsUntilImplemented) {
  EXPECT_EQ(0u, arch::GetHardwareBreakpointCount());
  EXPECT_EQ(0u, arch::GetHardwareWatchpointCount());
}
}  // namespace debug_agent

namespace debug_agent {
namespace {
uintptr_t memory_words[4];
int fail_write_at = -1;
int write_count = 0;
debug::Status ReadTestWord(pid_t, uintptr_t address, uintptr_t* value) {
  *value = memory_words[address / sizeof(uintptr_t)];
  return debug::Status();
}
debug::Status WriteTestWord(pid_t, uintptr_t address, uintptr_t value) {
  if (write_count++ == fail_write_at)
    return debug::Status("Injected write failure");
  memory_words[address / sizeof(uintptr_t)] = value;
  return debug::Status();
}
}  // namespace

TEST(LinuxMemory, WordWritesPreserveEdgesAndReportProgress) {
  std::vector<uint8_t> input(16, 0xab);
  for (size_t offset = 0; offset < sizeof(uintptr_t); ++offset) {
    for (size_t length = 0; length <= input.size(); ++length) {
      memset(memory_words, 0x12, sizeof(memory_words));
      fail_write_at = -1;
      write_count = 0;
      size_t actual = 123;
      EXPECT_FALSE(LinuxProcessHandle::WriteMemoryByWord(
          0, offset, input.data(), length, ReadTestWord, WriteTestWord, &actual).has_error());
      EXPECT_EQ(length, actual);
      auto bytes = reinterpret_cast<uint8_t*>(memory_words);
      for (size_t i = 0; i < sizeof(memory_words); ++i)
        EXPECT_EQ(i >= offset && i < offset + length ? 0xab : 0x12, bytes[i]);
    }
  }
  size_t actual = 123;
  fail_write_at = 1;
  write_count = 0;
  EXPECT_TRUE(LinuxProcessHandle::WriteMemoryByWord(
      0, 0, input.data(), 16, ReadTestWord, WriteTestWord, &actual).has_error());
  EXPECT_EQ(sizeof(uintptr_t), actual);
  actual = 123;
  EXPECT_TRUE(LinuxProcessHandle::WriteMemoryByWord(
      0, UINTPTR_MAX, input.data(), 2, ReadTestWord, WriteTestWord, &actual).has_error());
  EXPECT_EQ(0u, actual);
}
}  // namespace debug_agent
