// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_form.h"

#include <iterator>

#include "src/lib/fxl/strings/string_printf.h"

namespace zxdb {

namespace {

// Undefined values will be null.
const char* kDwarfFormNames[] = {
    "<none>",                  // 0x00
    "DW_FORM_addr",            // 0x01
    nullptr,                   // 0x02
    "DW_FORM_block2",          // 0x03
    "DW_FORM_block4",          // 0x04
    "DW_FORM_data2",           // 0x05
    "DW_FORM_data4",           // 0x06
    "DW_FORM_data8",           // 0x07
    "DW_FORM_string",          // 0x08
    "DW_FORM_block",           // 0x09
    "DW_FORM_block1",          // 0x0a
    "DW_FORM_data1",           // 0x0b
    "DW_FORM_flag",            // 0x0c
    "DW_FORM_sdata",           // 0x0d
    "DW_FORM_strp",            // 0x0e
    "DW_FORM_udata",           // 0x0f
    "DW_FORM_ref_addr",        // 0x10
    "DW_FORM_ref1",            // 0x11
    "DW_FORM_ref2",            // 0x12
    "DW_FORM_ref4",            // 0x13
    "DW_FORM_ref8",            // 0x14
    "DW_FORM_ref_udata",       // 0x15
    "DW_FORM_indirect",        // 0x16
    "DW_FORM_sec_offset",      // 0x17
    "DW_FORM_exprloc",         // 0x18
    "DW_FORM_flag_present",    // 0x19
    "DW_FORM_strx",            // 0x1a
    "DW_FORM_addrx",           // 0x1b
    "DW_FORM_ref_sup4",        // 0x1c
    "DW_FORM_strp_sup",        // 0x1d
    "DW_FORM_data16",          // 0x1e
    "DW_FORM_line_strp",       // 0x1f
    "DW_FORM_ref_sig8",        // 0x20
    "DW_FORM_implicit_const",  // 0x21
    "DW_FORM_loclistx",        // 0x22
    "DW_FORM_rnglistx",        // 0x23
    "DW_FORM_ref_sup8",        // 0x24
    "DW_FORM_strx1",           // 0x25
    "DW_FORM_strx2",           // 0x26
    "DW_FORM_strx3",           // 0x27
    "DW_FORM_strx4",           // 0x28
    "DW_FORM_addrx1",          // 0x29
    "DW_FORM_addrx2",          // 0x2a
    "DW_FORM_addrx3",          // 0x2b
    "DW_FORM_addrx4",          // 0x2c
};

constexpr size_t kDwarfFormNameCount = static_cast<size_t>(DwarfForm::kLastDefined);
static_assert(std::size(kDwarfFormNames) == kDwarfFormNameCount,
              "Update DWARF form name array for changes to enum.");

// Returns the name of a vendor form that has a constant, or null.
const char* GetVendorFormName(DwarfForm form) {
  switch (form) {
    case DwarfForm::kGnuAddrIndex:
      return "DW_FORM_GNU_addr_index";
    case DwarfForm::kGnuStrIndex:
      return "DW_FORM_GNU_str_index";
    case DwarfForm::kGnuRefAlt:
      return "DW_FORM_GNU_ref_alt";
    case DwarfForm::kGnuStrpAlt:
      return "DW_FORM_GNU_strp_alt";
    case DwarfForm::kLlvmAddrxOffset:
      return "DW_FORM_LLVM_addrx_offset";
    default:
      return nullptr;
  }
}

}  // namespace

bool DwarfFormIsBlock(DwarfForm form) {
  return form == DwarfForm::kBlock || form == DwarfForm::kBlock1 || form == DwarfForm::kBlock2 ||
         form == DwarfForm::kBlock4;
}

bool DwarfFormIsConstant(DwarfForm form) {
  return form == DwarfForm::kData1 || form == DwarfForm::kData2 || form == DwarfForm::kData4 ||
         form == DwarfForm::kData8 || form == DwarfForm::kData16 || form == DwarfForm::kSdata ||
         form == DwarfForm::kUdata || form == DwarfForm::kImplicitConst;
}

bool DwarfFormIsAddress(DwarfForm form) {
  return form == DwarfForm::kAddr || form == DwarfForm::kAddrx || form == DwarfForm::kAddrx1 ||
         form == DwarfForm::kAddrx2 || form == DwarfForm::kAddrx3 || form == DwarfForm::kAddrx4 ||
         form == DwarfForm::kGnuAddrIndex || form == DwarfForm::kLlvmAddrxOffset;
}

std::string DwarfFormToString(DwarfForm form, bool include_number) {
  unsigned int_form = static_cast<unsigned>(form);
  const char* name = nullptr;
  if (int_form < kDwarfFormNameCount)
    name = kDwarfFormNames[int_form];
  else
    name = GetVendorFormName(form);

  if (name) {
    if (include_number)
      return fxl::StringPrintf("%s (0x%02x)", name, int_form);
    return name;
  }
  return fxl::StringPrintf("<undefined (0x%x)>", int_form);
}

}  // namespace zxdb
