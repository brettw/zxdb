// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_attr.h"

#include <iterator>

#include "src/lib/fxl/strings/string_printf.h"

namespace zxdb {

namespace {

// Undefined values will be null.
const char* kDwarfAttrNames[] = {
    "<none>",                         // 0x00
    "DW_AT_sibling",                  // 0x01
    "DW_AT_location",                 // 0x02
    "DW_AT_name",                     // 0x03
    nullptr,                          // 0x04
    nullptr,                          // 0x05
    nullptr,                          // 0x06
    nullptr,                          // 0x07
    nullptr,                          // 0x08
    "DW_AT_ordering",                 // 0x09
    nullptr,                          // 0x0a
    "DW_AT_byte_size",                // 0x0b
    "DW_AT_bit_offset",               // 0x0c
    "DW_AT_bit_size",                 // 0x0d
    nullptr,                          // 0x0e
    nullptr,                          // 0x0f
    "DW_AT_stmt_list",                // 0x10
    "DW_AT_low_pc",                   // 0x11
    "DW_AT_high_pc",                  // 0x12
    "DW_AT_language",                 // 0x13
    nullptr,                          // 0x14
    "DW_AT_discr",                    // 0x15
    "DW_AT_discr_value",              // 0x16
    "DW_AT_visibility",               // 0x17
    "DW_AT_import",                   // 0x18
    "DW_AT_string_length",            // 0x19
    "DW_AT_common_reference",         // 0x1a
    "DW_AT_comp_dir",                 // 0x1b
    "DW_AT_const_value",              // 0x1c
    "DW_AT_containing_type",          // 0x1d
    "DW_AT_default_value",            // 0x1e
    nullptr,                          // 0x1f
    "DW_AT_inline",                   // 0x20
    "DW_AT_is_optional",              // 0x21
    "DW_AT_lower_bound",              // 0x22
    nullptr,                          // 0x23
    nullptr,                          // 0x24
    "DW_AT_producer",                 // 0x25
    nullptr,                          // 0x26
    "DW_AT_prototyped",               // 0x27
    nullptr,                          // 0x28
    nullptr,                          // 0x29
    "DW_AT_return_addr",              // 0x2a
    nullptr,                          // 0x2b
    "DW_AT_start_scope",              // 0x2c
    nullptr,                          // 0x2d
    "DW_AT_bit_stride",               // 0x2e
    "DW_AT_upper_bound",              // 0x2f
    nullptr,                          // 0x30
    "DW_AT_abstract_origin",          // 0x31
    "DW_AT_accessibility",            // 0x32
    "DW_AT_address_class",            // 0x33
    "DW_AT_artificial",               // 0x34
    "DW_AT_base_types",               // 0x35
    "DW_AT_calling_convention",       // 0x36
    "DW_AT_count",                    // 0x37
    "DW_AT_data_member_location",     // 0x38
    "DW_AT_decl_column",              // 0x39
    "DW_AT_decl_file",                // 0x3a
    "DW_AT_decl_line",                // 0x3b
    "DW_AT_declaration",              // 0x3c
    "DW_AT_discr_list",               // 0x3d
    "DW_AT_encoding",                 // 0x3e
    "DW_AT_external",                 // 0x3f
    "DW_AT_frame_base",               // 0x40
    "DW_AT_friend",                   // 0x41
    "DW_AT_identifier_case",          // 0x42
    "DW_AT_macro_info",               // 0x43
    "DW_AT_namelist_item",            // 0x44
    "DW_AT_priority",                 // 0x45
    "DW_AT_segment",                  // 0x46
    "DW_AT_specification",            // 0x47
    "DW_AT_static_link",              // 0x48
    "DW_AT_type",                     // 0x49
    "DW_AT_use_location",             // 0x4a
    "DW_AT_variable_parameter",       // 0x4b
    "DW_AT_virtuality",               // 0x4c
    "DW_AT_vtable_elem_location",     // 0x4d
    "DW_AT_allocated",                // 0x4e
    "DW_AT_associated",               // 0x4f
    "DW_AT_data_location",            // 0x50
    "DW_AT_byte_stride",              // 0x51
    "DW_AT_entry_pc",                 // 0x52
    "DW_AT_use_UTF8",                 // 0x53
    "DW_AT_extension",                // 0x54
    "DW_AT_ranges",                   // 0x55
    "DW_AT_trampoline",               // 0x56
    "DW_AT_call_column",              // 0x57
    "DW_AT_call_file",                // 0x58
    "DW_AT_call_line",                // 0x59
    "DW_AT_description",              // 0x5a
    "DW_AT_binary_scale",             // 0x5b
    "DW_AT_decimal_scale",            // 0x5c
    "DW_AT_small",                    // 0x5d
    "DW_AT_decimal_sign",             // 0x5e
    "DW_AT_digit_count",              // 0x5f
    "DW_AT_picture_string",           // 0x60
    "DW_AT_mutable",                  // 0x61
    "DW_AT_threads_scaled",           // 0x62
    "DW_AT_explicit",                 // 0x63
    "DW_AT_object_pointer",           // 0x64
    "DW_AT_endianity",                // 0x65
    "DW_AT_elemental",                // 0x66
    "DW_AT_pure",                     // 0x67
    "DW_AT_recursive",                // 0x68
    "DW_AT_signature",                // 0x69
    "DW_AT_main_subprogram",          // 0x6a
    "DW_AT_data_bit_offset",          // 0x6b
    "DW_AT_const_expr",               // 0x6c
    "DW_AT_enum_class",               // 0x6d
    "DW_AT_linkage_name",             // 0x6e
    "DW_AT_string_length_bit_size",   // 0x6f
    "DW_AT_string_length_byte_size",  // 0x70
    "DW_AT_rank",                     // 0x71
    "DW_AT_str_offsets_base",         // 0x72
    "DW_AT_addr_base",                // 0x73
    "DW_AT_rnglists_base",            // 0x74
    nullptr,                          // 0x75
    "DW_AT_dwo_name",                 // 0x76
    "DW_AT_reference",                // 0x77
    "DW_AT_rvalue_reference",         // 0x78
    "DW_AT_macros",                   // 0x79
    "DW_AT_call_all_calls",           // 0x7a
    "DW_AT_call_all_source_calls",    // 0x7b
    "DW_AT_call_all_tail_calls",      // 0x7c
    "DW_AT_call_return_pc",           // 0x7d
    "DW_AT_call_value",               // 0x7e
    "DW_AT_call_origin",              // 0x7f
    "DW_AT_call_parameter",           // 0x80
    "DW_AT_call_pc",                  // 0x81
    "DW_AT_call_tail_call",           // 0x82
    "DW_AT_call_target",              // 0x83
    "DW_AT_call_target_clobbered",    // 0x84
    "DW_AT_call_data_location",       // 0x85
    "DW_AT_call_data_value",          // 0x86
    "DW_AT_noreturn",                 // 0x87
    "DW_AT_alignment",                // 0x88
    "DW_AT_export_symbols",           // 0x89
    "DW_AT_deleted",                  // 0x8a
    "DW_AT_defaulted",                // 0x8b
    "DW_AT_loclists_base",            // 0x8c
};

constexpr size_t kDwarfAttrNameCount = static_cast<size_t>(DwarfAttr::kLastDefined);
static_assert(std::size(kDwarfAttrNames) == kDwarfAttrNameCount,
              "Update DWARF attribute name array for changes to enum.");

// Returns the name of a vendor attribute that has a constant, or null.
const char* GetVendorAttrName(DwarfAttr attr) {
  switch (attr) {
    case DwarfAttr::kMipsLinkageName:
      return "DW_AT_MIPS_linkage_name";
    case DwarfAttr::kGnuDwoName:
      return "DW_AT_GNU_dwo_name";
    case DwarfAttr::kGnuRangesBase:
      return "DW_AT_GNU_ranges_base";
    case DwarfAttr::kGnuAddrBase:
      return "DW_AT_GNU_addr_base";
    default:
      return nullptr;
  }
}

}  // namespace

std::string DwarfAttrToString(DwarfAttr attr, bool include_number) {
  unsigned int_attr = static_cast<unsigned>(attr);
  const char* name = nullptr;
  if (int_attr < kDwarfAttrNameCount)
    name = kDwarfAttrNames[int_attr];
  else
    name = GetVendorAttrName(attr);

  if (name) {
    if (include_number)
      return fxl::StringPrintf("%s (0x%02x)", name, int_attr);
    return name;
  }
  return fxl::StringPrintf("<undefined (0x%x)>", int_attr);
}

}  // namespace zxdb
