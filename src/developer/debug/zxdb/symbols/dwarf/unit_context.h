// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_CONTEXT_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_CONTEXT_H_

#include <stdint.h>

#include <optional>
#include <span>
#include <string_view>

#include "src/developer/debug/zxdb/common/endian.h"
#include "src/developer/debug/zxdb/common/err_or.h"
#include "src/developer/debug/zxdb/symbols/dwarf/form_value.h"
#include "src/developer/debug/zxdb/symbols/dwarf/package_index.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"

namespace zxdb {

struct DwarfSections;
class RangeListReader;

// Values from the first DIE of a unit. The context borrows section bytes and has no lazy state.
// Keep the unit and skeleton section bytes alive while using it.
class UnitContext {
 public:
  struct PackageContributions {
    std::optional<PackageIndex::Contribution> str_offsets;
    std::optional<PackageIndex::Contribution> loclists;
    std::optional<PackageIndex::Contribution> rnglists;
  };

  struct Options {
    bool is_dwo = false;
    const UnitContext* skeleton = nullptr;
    // Present for a unit in a DWARF package. The offsets are relative to the sections.
    std::optional<PackageContributions> package;
  };

  struct StringOffsets {
    uint64_t base;  // Section offset of the first entry.
    uint64_t end;   // Section offset just after the last entry.
    uint8_t entry_size;
  };

  // Reads the unit DIE within the bounds of the supplied header. An invalid abbreviation or
  // attribute encoding returns an error. A missing attribute or a failed section lookup gives
  // an empty optional for the affected value.
  //
  // If supplied, abbrevs must describe this unit's abbreviation table and remain alive for
  // the call. The returned context only borrows the section bytes.
  static ErrOr<UnitContext> Parse(const DwarfSections& sections, const UnitHeader& header,
                                  UnitSection section, const Options& options,
                                  const ErrOr<AbbrevTable>* abbrevs = nullptr);
  static ErrOr<UnitContext> Parse(const DwarfSections& sections, const UnitHeader& header,
                                  UnitSection section = UnitSection::kInfo) {
    return Parse(sections, header, section, Options{});
  }

  std::optional<uint64_t> dwo_id() const { return dwo_id_; }
  std::optional<uint64_t> str_offsets_base() const { return str_offsets_base_; }
  std::optional<uint64_t> addr_base() const { return addr_base_; }
  std::optional<uint64_t> rnglists_base() const { return rnglists_base_; }
  std::optional<uint64_t> loclists_base() const { return loclists_base_; }

  // The DWARF 4 .debug_ranges base supplied by the skeleton for the split unit.
  std::optional<uint64_t> ranges_base() const { return ranges_base_; }

  std::optional<StringOffsets> string_offsets() const { return string_offsets_; }
  std::optional<std::string_view> compilation_dir() const { return compilation_dir_; }
  std::optional<uint64_t> base_address() const { return base_address_; }
  std::optional<uint64_t> low_pc() const { return low_pc_; }
  std::optional<uint64_t> stmt_list() const { return stmt_list_; }
  // The form distinguishes a section offset from a DW_FORM_rnglistx index.
  const std::optional<FormValue>& ranges() const { return ranges_; }

  // Resolves string and address forms using this unit's sections and bases.
  std::optional<std::string_view> ResolveString(const FormValue& value) const;
  std::optional<uint64_t> ResolveAddress(const FormValue& value) const;
  std::optional<uint64_t> ReadAddress(uint64_t index) const;

  // The offset and loclists_base() are relative to this view. For a package, the view is
  // limited to the unit's contribution.
  std::span<const uint8_t> loclists() const { return loclists_; }
  std::optional<uint64_t> ReadLoclistOffset(uint64_t index) const;

 private:
  friend class RangeListReader;

  UnitContext() = default;

  std::span<const uint8_t> str_;
  std::span<const uint8_t> line_str_;
  std::span<const uint8_t> str_offsets_;
  std::span<const uint8_t> addr_;
  std::span<const uint8_t> loclists_;
  std::span<const uint8_t> ranges_section_;
  std::span<const uint8_t> rnglists_;
  uint64_t rnglists_section_offset_ = 0;
  bool is_dwo_ = false;
  FormParams params_;
  Endian endian_;
  uint8_t addr_entry_size_;
  Endian addr_endian_;

  std::optional<uint64_t> dwo_id_;
  std::optional<uint64_t> str_offsets_base_;
  std::optional<uint64_t> addr_base_;
  std::optional<uint64_t> rnglists_base_;
  std::optional<uint64_t> loclists_base_;
  std::optional<uint64_t> ranges_base_;
  std::optional<uint64_t> gnu_ranges_base_;

  std::optional<StringOffsets> string_offsets_;
  std::optional<std::string_view> compilation_dir_;
  std::optional<uint64_t> base_address_;
  std::optional<uint64_t> low_pc_;
  std::optional<FormValue> high_pc_;
  std::optional<uint64_t> stmt_list_;
  std::optional<FormValue> ranges_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_UNIT_CONTEXT_H_
