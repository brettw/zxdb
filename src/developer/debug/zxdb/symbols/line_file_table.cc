// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/line_file_table.h"

#include "src/developer/debug/zxdb/common/file_util.h"

namespace zxdb {

std::optional<std::string> ResolveLineFileName(const llvm::DWARFDebugLine::Prologue& prologue,
                                               uint64_t file_index) {
  // Pass an empty compilation directory so names stay relative to it. The debugger joins them
  // with the build directory later.
  std::string name;
  if (!prologue.getFileNameByIndex(file_index, "",
                                   llvm::DILineInfoSpecifier::FileLineInfoKind::RelativeFilePath,
                                   name)) {
    return std::nullopt;
  }
  return NormalizePath(name);
}

size_t GetLineFileCount(const llvm::DWARFDebugLine::Prologue& prologue) {
  // DWARF 2 to 4 count file entries from 1 so the table needs one extra slot.
  size_t count = prologue.FileNames.size();
  if (prologue.getVersion() < 5 && count > 0)
    count++;
  return count;
}

// static
LineFileTable LineFileTable::FromPrologue(const llvm::DWARFDebugLine::Prologue& prologue) {
  size_t count = GetLineFileCount(prologue);
  std::vector<std::string> names(count);
  for (size_t i = 0; i < count; i++) {
    if (std::optional<std::string> name = ResolveLineFileName(prologue, i))
      names[i] = std::move(*name);
  }
  return LineFileTable(std::move(names));
}

}  // namespace zxdb
