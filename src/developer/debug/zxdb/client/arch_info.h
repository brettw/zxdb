// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_ARCH_INFO_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_ARCH_INFO_H_

#include <memory>

#include "src/developer/debug/shared/arch.h"
#include "src/developer/debug/zxdb/common/err.h"
#include "src/lib/fxl/macros.h"

namespace zxdb {

class Abi;

class ArchInfo {
 public:
  ArchInfo();
  ~ArchInfo();

  Err Init(debug::Arch arch, uint64_t page_size);

  debug::Arch arch() const { return arch_; }
  uint64_t page_size() const { return page_size_; }

  const std::shared_ptr<Abi>& abi() const { return abi_; }

  // Returns true if the instruction length is fixed.
  bool is_fixed_instr() const { return is_fixed_instr_; }

  // Minimum instruction alignment in bytes.
  size_t instr_align() const { return instr_align_; }

  // Longest possible instruction in bytes.
  size_t max_instr_len() const { return max_instr_len_; }

 private:
  debug::Arch arch_ = debug::Arch::kUnknown;
  uint64_t page_size_ = 0;
  std::shared_ptr<Abi> abi_;

  bool is_fixed_instr_ = false;
  size_t instr_align_ = 1;
  size_t max_instr_len_ = 1;

  FXL_DISALLOW_COPY_AND_ASSIGN(ArchInfo);
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_ARCH_INFO_H_
