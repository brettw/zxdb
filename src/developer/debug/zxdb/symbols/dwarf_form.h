// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_FORM_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_FORM_H_

#include <string>

namespace zxdb {

// DWARF attribute forms. A form says how a value is encoded in the data. The values are the
// DW_FORM_* values of the DWARF 5 standard.
//
// A form can have a value that isn't one of these constants, for example a form of a newer DWARF
// version or of a vendor. Code that reads forms from symbol files must accept these values. A form
// is an unsigned LEB128 value in the data, so code must also check that a value is in the range of
// the enum before it casts the value.
enum class DwarfForm : int {
  // Not a DWARF form, this is used to indicate "not present."
  kNone = 0x00,

  // An address of the target address size.
  kAddr = 0x01,

  // Blocks of bytes with a 2 or 4-byte length. See also kBlock1 and kBlock.
  kBlock2 = 0x03,
  kBlock4 = 0x04,

  // Constants of 2, 4, and 8 bytes. See also kData1 and kData16. In DWARF 2 and 3, kData4 and
  // kData8 can also be section offsets.
  kData2 = 0x05,
  kData4 = 0x06,
  kData8 = 0x07,

  // A null-terminated string in the DIE data.
  kString = 0x08,

  // A block of bytes with an unsigned LEB128 length.
  kBlock = 0x09,

  // A block of bytes with a 1-byte length.
  kBlock1 = 0x0a,

  // A 1-byte constant.
  kData1 = 0x0b,

  // A 1-byte flag. Zero is false and other values are true.
  kFlag = 0x0c,

  // A signed LEB128 constant.
  kSdata = 0x0d,

  // An offset into .debug_str.
  kStrp = 0x0e,

  // An unsigned LEB128 constant.
  kUdata = 0x0f,

  // A reference to a DIE by its offset in .debug_info. The DIE can be in another unit.
  kRefAddr = 0x10,

  // References to a DIE in the same unit, as an offset from the start of the unit.
  kRef1 = 0x11,
  kRef2 = 0x12,
  kRef4 = 0x13,
  kRef8 = 0x14,
  kRefUdata = 0x15,

  // The form is an unsigned LEB128 value in the data, followed by the value of that form.
  kIndirect = 0x16,

  // An offset into another section, for example a line table or a range list.
  kSecOffset = 0x17,

  // A DWARF expression with an unsigned LEB128 length.
  kExprloc = 0x18,

  // A flag that is true. It has no data.
  kFlagPresent = 0x19,

  // An index into the string offsets table of the unit, as an unsigned LEB128 value. See also
  // kStrx1 to kStrx4.
  kStrx = 0x1a,

  // An index into the address table of the unit, as an unsigned LEB128 value. See also kAddrx1
  // to kAddrx4.
  kAddrx = 0x1b,

  // A 4-byte reference to a DIE in the supplementary object file.
  kRefSup4 = 0x1c,

  // An offset into .debug_str of the supplementary object file.
  kStrpSup = 0x1d,

  // A 16-byte constant, for example an MD5 checksum.
  kData16 = 0x1e,

  // An offset into .debug_line_str.
  kLineStrp = 0x1f,

  // A reference to the type unit with the given 8-byte signature.
  kRefSig8 = 0x20,

  // A signed constant that is stored in the abbreviation instead of in the DIE data.
  kImplicitConst = 0x21,

  // An index into the location list table of the unit, as an unsigned LEB128 value.
  kLoclistx = 0x22,

  // An index into the range list table of the unit, as an unsigned LEB128 value.
  kRnglistx = 0x23,

  // An 8-byte reference to a DIE in the supplementary object file.
  kRefSup8 = 0x24,

  // Indices into the string offsets table of the unit, of 1 to 4 bytes.
  kStrx1 = 0x25,
  kStrx2 = 0x26,
  kStrx3 = 0x27,
  kStrx4 = 0x28,

  // Indices into the address table of the unit, of 1 to 4 bytes.
  kAddrx1 = 0x29,
  kAddrx2 = 0x2a,
  kAddrx3 = 0x2b,
  kAddrx4 = 0x2c,

  // -----------------------------------------------------------------------------------------------

  // Identifies one-past-the-end of the forms where we define constants. For range checking.
  kLastDefined,

  // GNU extensions for split DWARF and for supplementary object files. kGnuAddrIndex and
  // kGnuStrIndex were used before DWARF 5 added kAddrx and kStrx.
  kGnuAddrIndex = 0x1f01,
  kGnuStrIndex = 0x1f02,
  kGnuRefAlt = 0x1f20,
  kGnuStrpAlt = 0x1f21,

  // LLVM extension: an index into the address table followed by a 4-byte offset from that address.
  kLlvmAddrxOffset = 0x2001,
};

// Converts the form to a string, optionally including the numeric value. Invalid or unknown values
// will be formatted as "<undefined>" and will always include the numeric value.
std::string DwarfFormToString(DwarfForm form, bool include_number = false);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_FORM_H_
