// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/client/arch_info.h"

#include "src/developer/debug/zxdb/expr/abi_arm64.h"
#include "src/developer/debug/zxdb/expr/abi_riscv64.h"
#include "src/developer/debug/zxdb/expr/abi_x64.h"

namespace zxdb {

ArchInfo::ArchInfo() = default;
ArchInfo::~ArchInfo() = default;

Err ArchInfo::Init(debug::Arch arch, uint64_t page_size) {
  arch_ = arch;
  page_size_ = page_size;

  switch (arch) {
    case debug::Arch::kUnknown:
      return Err();
    case debug::Arch::kX64:
      abi_ = std::make_shared<AbiX64>();
      is_fixed_instr_ = false;
      max_instr_len_ = 15;
      instr_align_ = 1;
      break;
    case debug::Arch::kArm64:
      abi_ = std::make_shared<AbiArm64>();
      is_fixed_instr_ = true;
      max_instr_len_ = 4;
      instr_align_ = 4;
      break;
    case debug::Arch::kRiscv64:
      abi_ = std::make_shared<AbiRiscv64>();
      is_fixed_instr_ = false;
      max_instr_len_ = 4;
      instr_align_ = 2;
      break;
  }

  return Err();
}

}  // namespace zxdb
