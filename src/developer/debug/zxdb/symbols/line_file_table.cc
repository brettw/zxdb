// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/line_file_table.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"
#include "src/developer/debug/zxdb/common/file_util.h"

namespace zxdb {

std::optional<std::string> ResolveLineFileName(LineFileEntries files, uint64_t file_index) {
  const LineFileEntry* entry = files.GetFileEntry(file_index);
  if (!entry || !entry->name)
    return std::nullopt;
  std::string_view name = *entry->name;

  // Debug info can come from any OS, so an absolute name is absolute for either path style. LLVM
  // returns such names without a directory.
  if (llvm::sys::path::is_absolute(name, llvm::sys::path::Style::posix) ||
      llvm::sys::path::is_absolute(name, llvm::sys::path::Style::windows))
    return NormalizePath(name);

  // Relative names don't include the compilation directory, so the directory can be empty.
  // llvm::sys::path::append() skips empty components.
  std::string_view dir = files.GetFileDirectory(*entry).value_or(std::string_view());
  llvm::SmallString<128> path;
  llvm::sys::path::append(path, dir, name);
  return NormalizePath(path.str());
}

// static
LineFileTable LineFileTable::FromEntries(LineFileEntries files) {
  size_t count = files.size();
  std::vector<std::string> names(count);
  for (size_t i = 0; i < count; i++) {
    if (std::optional<std::string> name = ResolveLineFileName(files, i))
      names[i] = std::move(*name);
  }
  return LineFileTable(std::move(names));
}

}  // namespace zxdb
