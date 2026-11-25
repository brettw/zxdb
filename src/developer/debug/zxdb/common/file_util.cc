// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/common/file_util.h"

#include <lib/syslog/cpp/macros.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include "src/developer/debug/shared/logging/logging.h"
#include "src/developer/debug/shared/string_util.h"

namespace zxdb {

namespace {

bool IsWindowsSeparator(char c) { return c == '/' || c == '\\'; }

bool IsPosixPathAbsolute(std::string_view path) {
  if (path.empty() || path[0] != '/')
    return false;

  // A network root name needs a root directory after it.
  if (path.size() > 2 && path[1] == '/' && path[2] != '/')
    return path.find('/', 2) != std::string_view::npos;
  return true;
}

bool IsWindowsPathAbsolute(std::string_view path) {
  if (path.empty())
    return false;

  bool has_drive =
      path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':';
  if (has_drive)
    return path.size() > 2 && IsWindowsSeparator(path[2]);

  // A network root name needs a root directory after it. Both separators must be the same.
  if (path.size() > 2 && IsWindowsSeparator(path[0]) && path[1] == path[0] &&
      !IsWindowsSeparator(path[2]))
    return path.find_first_of("/\\", 2) != std::string_view::npos;

  // A root directory without a root name isn't absolute.
  if (IsWindowsSeparator(path[0]))
    return false;

  // A first component that ends with a colon is also a root name.
  size_t first_end = path.find_first_of("/\\");
  return first_end != std::string_view::npos && path[first_end - 1] == ':';
}

}  // namespace

std::string_view ExtractLastFileComponent(std::string_view path) {
  size_t last_slash = path.rfind('/');
  if (last_slash == std::string::npos)
    return path;
  return path.substr(last_slash + 1);
}

bool IsPathAbsolute(const std::string& path) { return !path.empty() && path[0] == '/'; }

bool IsPathAbsoluteInAnyStyle(std::string_view path) {
  return IsPosixPathAbsolute(path) || IsWindowsPathAbsolute(path);
}

bool PathEndsWith(std::string_view path, std::string_view right_query) {
  return debug::StringEndsWith(path, right_query) &&
         (path.size() == right_query.size() || path[path.size() - right_query.size() - 1] == '/');
}

std::string CatPathComponents(const std::string& first, const std::string& second) {
  // Second component shouldn't begin with a slash.
  FX_DCHECK(second.empty() || second[0] != '/');

  std::string result;
  result.reserve(first.size() + second.size() + 1);
  result.append(first);

  if (!first.empty() && !second.empty() && first.back() != '/')
    result.push_back('/');
  result.append(second);

  return result;
}

std::string AppendPathName(std::string_view dir, std::string_view name) {
  std::string result(dir);
  if (dir.empty())
    return result.append(name);

  if (dir.back() == '/') {
    size_t name_begin = name.find_first_not_of('/');
    if (name_begin != std::string_view::npos)
      result.append(name.substr(name_begin));
    return result;
  }

  if (name.empty() || name[0] != '/')
    result.push_back('/');
  return result.append(name);
}

std::string NormalizePath(std::string_view path) {
  return std::filesystem::path(path).lexically_normal();
}

std::time_t GetFileModificationTime(const std::string& path) {
  std::error_code ec;
  std::filesystem::file_time_type last_write = std::filesystem::last_write_time(path, ec);
  if (ec)
    return 0;

  // The duration of the file clock differs between standard libraries, so cast it to the duration
  // of the system clock.
  return std::chrono::system_clock::to_time_t(
      std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          std::chrono::file_clock::to_sys(last_write)));
}

bool PathStartsWith(const std::filesystem::path& path, const std::filesystem::path& base) {
  if (path.is_absolute() != base.is_absolute())
    return false;

  if (path.empty())
    return base.empty();

  auto path_it = path.begin();
  for (const auto& ancestor : base) {
    if (ancestor.empty() || ancestor == ".")
      continue;
    while (path_it->empty() || *path_it == ".")
      path_it++;
    if (path_it == path.end())
      return false;
    if (ancestor != *path_it)
      return false;
    path_it++;
  }
  return true;
}

std::filesystem::path PathRelativeTo(const std::filesystem::path& path,
                                     const std::filesystem::path& base) {
  FX_CHECK(path.is_absolute() == base.is_absolute());
  auto base_it = base.begin();
  auto path_it = path.begin();
  while (base_it != base.end() && path_it != path.end() && *base_it == *path_it) {
    base_it++;
    path_it++;
  }
  std::filesystem::path res;
  while (base_it != base.end()) {
    if (*base_it != ".") {
      res.append("..");
    }
    base_it++;
  }
  while (path_it != path.end()) {
    res.append(path_it->string());
    path_it++;
  }
  return res;
}

std::optional<std::string> ExpandAndNormalizePath(const std::string& path) {
  // Remove all the ".." and "." components from the path.
  std::string normalized_path = NormalizePath(path);
  std::filesystem::path p(normalized_path);
  if (p.is_absolute() || p.empty()) {
    return normalized_path;
  }

  std::filesystem::path expand;

  // Support "~", "~/", $HOME, and "$HOME/".
  if (PathStartsWith(p, "~") || PathStartsWith(p, "$HOME")) {
    const char* home = getenv("HOME");
    if (home) {
      expand = std::filesystem::path(home);
      // Collect the rest of the path.
      auto it = p.begin();
      for (++it; it != p.end(); ++it) {
        expand /= *it;
      }
    } else {
      LOGS(Warn) << "HOME environment variable is not set.";
      return std::nullopt;
    }
  } else if (normalized_path.starts_with('~')) {
    LOGS(Warn) << "~username is not supported yet.";
    return std::nullopt;
  } else {  // Start of a relative path.
    expand = std::filesystem::absolute(normalized_path);
  }
  return expand.string();
}

}  // namespace zxdb
