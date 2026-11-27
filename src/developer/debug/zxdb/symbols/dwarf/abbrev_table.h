// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_ABBREV_TABLE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_ABBREV_TABLE_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <span>
#include <vector>

#include "src/developer/debug/zxdb/common/err_or.h"
#include "src/developer/debug/zxdb/symbols/dwarf/form_encoding.h"

namespace zxdb {

class DataReader;
struct FormParams;

// Owns the abbreviation declarations at one offset. Readers can share a parsed table across
// threads.
class AbbrevTable {
 public:
  struct AttributeSpec {
    uint64_t attribute;                     // DW_AT_*.
    uint64_t form;                          // DW_FORM_*.
    std::optional<int64_t> implicit_const;  // Present for DW_FORM_implicit_const.
  };

  struct Declaration {
    uint64_t code;
    uint64_t tag;  // DW_TAG_*.
    bool has_children;
    std::vector<AttributeSpec> attributes;

    // Returns the total size of the attributes in a DIE, excluding its abbreviation code.
    // Pass the parameters of the unit that uses this declaration. Variable or unknown sizes
    // return std::nullopt.
    std::optional<uint64_t> FixedAttributeSize(const FormParams& params) const;

    // Skips a DIE's attributes within the reader's limits.
    bool SkipAttributes(const FormParams& params, DataReader& reader) const;

   private:
    friend class AbbrevTable;

    struct FixedSize {
      bool AddForm(FormEncoding encoding);

      uint64_t bytes = 0;
      uint64_t addresses = 0;
      uint64_t ref_addrs = 0;
      uint64_t offsets = 0;
    };

    std::optional<FixedSize> fixed_size_ = FixedSize{};
    std::vector<FormEncoding> skip_forms_;  // Byte layouts for a mixed declaration.
  };

  AbbrevTable(const AbbrevTable&) = default;
  AbbrevTable(AbbrevTable&&) = default;

  // Reads one table in .debug_abbrev or .debug_abbrev.dwo at UnitHeader::abbrev_offset.
  static ErrOr<AbbrevTable> Parse(std::span<const uint8_t> section, uint64_t offset);

  // Returns a declaration owned by this table, or nullptr if the code has no declaration.
  // If a code appears more than once, the first declaration wins.
  const Declaration* Find(uint64_t code) const {
    if (!consecutive_)
      return FindSparse(code);
    if (code < declarations_.front().code)
      return nullptr;
    uint64_t index = code - declarations_.front().code;
    return index < declarations_.size() ? &declarations_[index] : nullptr;
  }

 private:
  AbbrevTable() = default;
  const Declaration* FindSparse(uint64_t code) const;

  std::vector<Declaration> declarations_;  // Sorted by code.
  bool consecutive_ = false;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_ABBREV_TABLE_H_
