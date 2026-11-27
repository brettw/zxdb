// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_FORM_VALUE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_FORM_VALUE_H_

#include <stdint.h>

#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include "src/developer/debug/zxdb/common/err_or.h"
#include "src/developer/debug/zxdb/common/int128_t.h"
#include "src/developer/debug/zxdb/symbols/dwarf/abbrev_table.h"
#include "src/developer/debug/zxdb/symbols/dwarf/form_encoding.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"

namespace zxdb {

class DataReader;
struct FormParams;

// An attribute's raw value. The form identifies the encoding and the meaning of numeric payloads,
// including the base of a reference and the table for an index. The reader decodes the bytes
// without applying relocations or resolving references, addresses, or strings in other sections.
struct FormValue {
  using Class = FormClass;

  struct IndexedAddress {
    uint64_t index;
    uint32_t offset;
  };

  // Byte and string views borrow the reader's input. DW_FORM_data16 preserves its 16 bytes in
  // file order. A uint64_t holds an unsigned constant, a flag byte, an address, an offset, an
  // index, or a type signature as specified by the form.
  using Value =
      std::variant<uint64_t, int64_t, std::span<const uint8_t>, std::string_view, IndexedAddress>;

  DwarfForm form;  // The final form after following DW_FORM_indirect.
  Class form_class;
  Value value;

  // Reads an integer constant. The caller supplies the signedness of DW_FORM_data1 through
  // DW_FORM_data8. DW_FORM_data16 and flags aren't integer constants.
  std::optional<int128_t> AsInteger(bool data_is_signed) const;

  // The debugger's unsigned constant conversion also accepts flags, treats DW_FORM_implicit_const
  // as unsigned bits, and returns the byte count for DW_FORM_data16. It rejects DW_FORM_sdata.
  std::optional<uint64_t> AsUnsignedConstant() const;

  // Returns the raw value for section offsets, including the forms that can double as offsets
  // and the list index forms.
  std::optional<uint64_t> AsSectionOffset(uint16_t version) const;

  // Returns the raw offset or signature for a reference form.
  std::optional<uint64_t> AsReference() const;

  // Returns the bytes of a block, expression, or DW_FORM_data16 value.
  std::optional<std::span<const uint8_t>> AsBlock() const;
};

// Reads one attribute value. Direct implicit constants come from the attribute specification.
// On failure, the cursor may have consumed part of the value.
ErrOr<FormValue> ReadForm(const AbbrevTable::AttributeSpec& spec, const FormParams& params,
                          DataReader& reader);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_FORM_VALUE_H_
