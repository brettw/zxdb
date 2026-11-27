// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DWARF_SECTIONS_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DWARF_SECTIONS_H_

#include <stdint.h>

#include <span>

#include "src/developer/debug/zxdb/common/endian.h"

namespace zxdb {

// Non-owning views of DWARF bytes. A file-backed binary owns the ELF mapping and decompressed
// buffers through elflib. An in-memory fixture owns its section buffers.
struct DwarfSections {
  std::span<const uint8_t> info;
  std::span<const uint8_t> abbrev;
  std::span<const uint8_t> str;
  std::span<const uint8_t> str_offsets;
  std::span<const uint8_t> line;
  std::span<const uint8_t> line_str;
  std::span<const uint8_t> addr;
  std::span<const uint8_t> ranges;
  std::span<const uint8_t> rnglists;
  std::span<const uint8_t> loc;
  std::span<const uint8_t> loclists;
  std::span<const uint8_t> aranges;
  std::span<const uint8_t> names;
  std::span<const uint8_t> types;

  std::span<const uint8_t> info_dwo;
  std::span<const uint8_t> abbrev_dwo;
  std::span<const uint8_t> str_dwo;
  std::span<const uint8_t> str_offsets_dwo;
  std::span<const uint8_t> line_dwo;
  std::span<const uint8_t> loclists_dwo;
  std::span<const uint8_t> rnglists_dwo;
  std::span<const uint8_t> types_dwo;

  std::span<const uint8_t> cu_index;
  std::span<const uint8_t> tu_index;

  Endian endian = Endian::kLittle;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DWARF_SECTIONS_H_
