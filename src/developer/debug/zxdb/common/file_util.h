// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_COMMON_FILE_UTIL_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_COMMON_FILE_UTIL_H_

#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// TODO: modernize this file using std::filesystem.
namespace zxdb {

// Extracts the substring into the given file path of the last path component (the stuff following
// the last slash). If the path ends in a slash, it will return an empty StringView. If the input
// has no slash, it will return the whole thing.
std::string_view ExtractLastFileComponent(std::string_view path);

// Returns true if the given file path is absolute (begins with a slash). The contents could still
// have relative components ("/foo/../bar" is still absolute).
bool IsPathAbsolute(const std::string& path);

// Returns true if the path is absolute in the POSIX style or in the Windows style. Debug info can
// come from any OS, so its paths can have either style.
//
// A POSIX absolute path starts with a slash. A path that starts with exactly two slashes and then a
// name ("//server") has a network root name. Such a path is absolute only if a slash follows the
// name.
//
// A Windows absolute path has a root name and then a separator, which is a slash or a backslash.
// The root name is a drive letter ("C:"), a network name ("\\server"), or any other first
// component that ends with a colon.
bool IsPathAbsoluteInAnyStyle(std::string_view path);

// Returns true if the given |path| matches the |right_query| from the right-hand side. This
// requires both that the |path| end in |right_query| (case-sensitive) AND the start of the match is
// either the beginning of |path| or immediately following a path separator.
//
// Examples:
//   path = "foo.cc", right_query = "foo.cc" => TRUE
//   path = "bar/foo.cc", right_query = "foo.cc" => TRUE
//   path = "foo.cc", right_query = "o.cc" => FALSE
bool PathEndsWith(std::string_view path, std::string_view right_query);

// Concatenates the two path components with a slash in between them. "first" can end with a slash
// or not. The second component shouldn't begin with a slash.
std::string CatPathComponents(const std::string& first, const std::string& second);

// Appends a file name to a directory, with a slash between them. Unlike CatPathComponents(), the
// name can start with a slash, and the result keeps an empty name. Only a slash is a separator.
//
// - An empty directory gives the name.
// - If the directory ends with a slash, the leading slashes of the name are removed.
// - Otherwise, a slash is added if the name doesn't start with one.
//
// Examples:
//   dir = "src", name = "a.c" => "src/a.c"
//   dir = "src", name = "" => "src/"
//   dir = "src/", name = "//a.c" => "src/a.c"
//   dir = "src", name = "//a.c" => "src//a.c"
std::string AppendPathName(std::string_view dir, std::string_view name);

// Resolves "." and ".." components to the extent possible. If there are leading "..", then they
// will be preserved.
std::string NormalizePath(std::string_view path);

// Returns the modification time of the given file, or 0 if it could not be determined.
std::time_t GetFileModificationTime(const std::string& path);

// Check if a path starts with another path. Return false if either one is relative.
// The input may not be normalized, e.g.,
//   PathStartsWith("/path/to/build/dir/../../source.cc", "/path/to/build/dir") => true
bool PathStartsWith(const std::filesystem::path& path, const std::filesystem::path& base);

// Compute the relative path from base. Both inputs must be absolute.
std::filesystem::path PathRelativeTo(const std::filesystem::path& path,
                                     const std::filesystem::path& base);

// Returns the expanded and normalized path. It handles the following cases:
// - "~" and "$HOME" are expanded to the home directory.
// - "." and ".." components are resolved.
std::optional<std::string> ExpandAndNormalizePath(const std::string& path);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_COMMON_FILE_UTIL_H_
