// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/line_file_table.h"

#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace zxdb {

namespace {

// Resolves one file entry of a DWARF 5 line table. Directory 0 is the compilation directory, which
// relative names don't use. A null directory uses directory 0.
std::optional<std::string> Resolve(std::optional<std::string_view> dir,
                                   std::optional<std::string_view> name) {
  LineProgramHeader header;
  header.version = 5;
  header.include_dirs = {"/build"};
  uint64_t dir_index = 0;
  if (dir) {
    header.include_dirs.push_back(*dir);
    dir_index = 1;
  }
  header.files = {LineFileEntry{.name = name, .dir_index = dir_index}};
  return ResolveLineFileName(LineFileEntries(header), 0);
}

TEST(LineFileTable, ResolveRelativeNames) {
  EXPECT_EQ("src/a.c", Resolve("src", "a.c"));
  EXPECT_EQ("/abs/src/a.c", Resolve("/abs/src", "a.c"));
  EXPECT_EQ("a.c", Resolve(std::nullopt, "a.c"));
  EXPECT_EQ("a.c", Resolve("", "a.c"));
  EXPECT_EQ("src/b.c", Resolve("src", "a/../b.c"));

  // An empty name gives the directory.
  EXPECT_EQ("src/", Resolve("src", ""));
  EXPECT_EQ("src/", Resolve("src/", ""));
  EXPECT_EQ("", Resolve("", ""));

  // A backslash isn't a separator for a relative name.
  EXPECT_EQ("C:\\src\\/a.c", Resolve("C:\\src\\", "a.c"));
  EXPECT_EQ("src/\\\\server", Resolve("src", "\\\\server"));

  // A network root name without a root directory isn't absolute.
  EXPECT_EQ("src/server", Resolve("src", "//server"));
}

TEST(LineFileTable, ResolveAbsoluteNames) {
  // An absolute name in either style doesn't use the directory.
  EXPECT_EQ("/abs/a.c", Resolve("src", "/abs/a.c"));
  EXPECT_EQ("/server/share", Resolve("src", "//server/share"));
  EXPECT_EQ("/x", Resolve("src", "///x"));
  EXPECT_EQ("\\\\server\\share", Resolve("src", "\\\\server\\share"));
  EXPECT_EQ("C:\\a.c", Resolve("src", "C:\\a.c"));
  EXPECT_EQ("C:/a.c", Resolve("src", "C:/a.c"));
}

TEST(LineFileTable, ResolveInvalidEntries) {
  EXPECT_EQ(std::nullopt, Resolve("src", std::nullopt));

  LineProgramHeader header;
  header.version = 5;
  EXPECT_EQ(std::nullopt, ResolveLineFileName(LineFileEntries(header), 0));
}

}  // namespace

}  // namespace zxdb
