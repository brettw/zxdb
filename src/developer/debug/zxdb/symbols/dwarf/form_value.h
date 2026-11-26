// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_FORM_VALUE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_FORM_VALUE_H_

#include <stdint.h>

#include <span>
#include <string_view>
#include <variant>

#include "src/developer/debug/zxdb/common/err_or.h"
#include "src/developer/debug/zxdb/symbols/dwarf/abbrev_table.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"

namespace zxdb {

class DataReader;
struct FormParams;

// An attribute's raw value. The form identifies the encoding and the meaning of numeric payloads,
// including the base of a reference and the table for an index. The reader decodes the bytes
// without applying relocations or resolving references, addresses, or strings in other sections.
struct FormValue {
  // Forms such as DW_FORM_sec_offset can belong to several DWARF classes depending on the
  // attribute. This classification records what the form tells us before that interpretation.
  enum class Class {
    kConstant,
    kAddress,
    kBlock,
    kString,
    kReference,
    kSectionOffset,
    kFlag,
    kExprloc,
    kLoclist,
    kRnglist,
  };

  struct IndexedAddress {
    uint64_t index;
    uint32_t offset;
  };

  // Byte and string views borrow the reader's input. DW_FORM_data16 preserves its 16 bytes in
  // file order. A uint64_t holds an unsigned constant, an address, an offset, an index, or a
  // type signature as specified by the form.
  using Value = std::variant<uint64_t, int64_t, std::span<const uint8_t>, std::string_view, bool,
                             IndexedAddress>;

  DwarfForm form;  // The final form after following DW_FORM_indirect.
  Class form_class;
  Value value;
};

// Moves past one attribute value. Returns false on malformed input, including an unknown form.
// The raw overload accepts the full form code from an abbreviation declaration.
// On failure, the cursor may have consumed part of the value.
bool SkipForm(uint64_t form, const FormParams& params, DataReader& reader);
inline bool SkipForm(DwarfForm form, const FormParams& params, DataReader& reader) {
  return SkipForm(static_cast<uint64_t>(form), params, reader);
}

// Reads one attribute value. Direct implicit constants come from the attribute specification.
// On failure, the cursor may have consumed part of the value.
ErrOr<FormValue> ReadForm(const AbbrevTable::AttributeSpec& spec, const FormParams& params,
                          DataReader& reader);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_FORM_VALUE_H_
