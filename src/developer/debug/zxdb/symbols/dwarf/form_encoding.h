// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_FORM_ENCODING_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_FORM_ENCODING_H_

#include <stdint.h>

#include <optional>

#include "src/developer/debug/zxdb/symbols/dwarf_form.h"

namespace zxdb {

class DataReader;
struct FormParams;

// The byte layout of a form before interpreting its value.
struct FormEncoding {
  enum class Kind : uint8_t {
    kFixed,
    kAddress,
    kOffset,
    kRefAddr,
    kUleb,
    kSleb,
    kCString,
    kBlockFixed,
    kBlockUleb,
    kZeroWidth,
    kAddressIndexOffset,
    kIndirect,
    kInvalid,
  };

  Kind kind;
  uint8_t width = 0;  // Width of a fixed payload or a block's fixed length field.
};

// Classifies a form before the caller interprets the attribute. A section offset can
// belong to several DWARF classes depending on its attribute.
enum class FormClass : uint8_t {
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

struct FormDescription {
  FormEncoding encoding;
  FormClass form_class = FormClass::kConstant;
};

// Describes a form from its code. Set indirect if the code was read from the DIE.
FormDescription DescribeForm(uint64_t form, bool indirect = false);

// Reads the final form code from the payload of a DW_FORM_indirect.
std::optional<uint64_t> ReadIndirectForm(DataReader& reader);

// Moves past one attribute value. Returns false on malformed input or an unknown form.
// The raw overload accepts an unvalidated form code from an abbreviation declaration.
// On failure, the cursor may have consumed part of the value.
bool SkipForm(FormEncoding encoding, const FormParams& params, DataReader& reader);
bool SkipForm(uint64_t form, const FormParams& params, DataReader& reader);
inline bool SkipForm(DwarfForm form, const FormParams& params, DataReader& reader) {
  return SkipForm(static_cast<uint64_t>(form), params, reader);
}

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_FORM_ENCODING_H_
