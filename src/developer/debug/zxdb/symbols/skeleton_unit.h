// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_SKELETON_UNIT_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_SKELETON_UNIT_H_

#include <cstdint>
#include <optional>
#include <string>

namespace zxdb {

// Identifies a skeleton in the main binary and its companion .dwo file.
struct SkeletonUnit {
  // Offset in the main binary of the DIE defining the skeleton unit. This is used later to
  // reconstruct the symbol if we need it (normally it's not needed).
  uint64_t skeleton_die_offset = 0;

  std::optional<uint64_t> dwo_id;
  std::string dwo_name;
  std::string comp_dir;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_SKELETON_UNIT_H_
