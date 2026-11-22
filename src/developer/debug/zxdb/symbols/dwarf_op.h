// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_OP_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_OP_H_

#include <stdint.h>

#include <string>

namespace zxdb {

// DWARF expression opcodes. The values are the DW_OP_* values of the DWARF 5 standard, plus the GNU
// extensions that zxdb knows.
//
// An expression is a sequence of bytes, and code often does arithmetic on opcodes. For example, it
// gets the register number of kReg0 to kReg31 by subtraction. As a result, these are byte constants
// instead of an enum class like DwarfTag. The struct only groups the constants under one name.
//
// An expression can have an opcode that isn't one of these constants, for example a vendor
// extension. Code that reads expressions from symbol files must accept these values.
struct DwarfOp {
  DwarfOp() = delete;

  // Pushes an address operand. The address is relative to the module.
  static constexpr uint8_t kAddr = 0x03;

  // Pops an address and pushes the pointer-sized value at that address.
  static constexpr uint8_t kDeref = 0x06;

  // Push constants of the given size and signedness. "u" constants are unsigned and "s" constants
  // are signed.
  static constexpr uint8_t kConst1u = 0x08;
  static constexpr uint8_t kConst1s = 0x09;
  static constexpr uint8_t kConst2u = 0x0a;
  static constexpr uint8_t kConst2s = 0x0b;
  static constexpr uint8_t kConst4u = 0x0c;
  static constexpr uint8_t kConst4s = 0x0d;
  static constexpr uint8_t kConst8u = 0x0e;
  static constexpr uint8_t kConst8s = 0x0f;
  static constexpr uint8_t kConstu = 0x10;
  static constexpr uint8_t kConsts = 0x11;

  // Stack operations.
  static constexpr uint8_t kDup = 0x12;
  static constexpr uint8_t kDrop = 0x13;
  static constexpr uint8_t kOver = 0x14;
  static constexpr uint8_t kPick = 0x15;
  static constexpr uint8_t kSwap = 0x16;
  static constexpr uint8_t kRot = 0x17;

  // Like kDeref, but also pops an address space identifier.
  static constexpr uint8_t kXderef = 0x18;

  // Arithmetic and logical operations. These pop their operands and push the result.
  static constexpr uint8_t kAbs = 0x19;
  static constexpr uint8_t kAnd = 0x1a;
  static constexpr uint8_t kDiv = 0x1b;
  static constexpr uint8_t kMinus = 0x1c;
  static constexpr uint8_t kMod = 0x1d;
  static constexpr uint8_t kMul = 0x1e;
  static constexpr uint8_t kNeg = 0x1f;
  static constexpr uint8_t kNot = 0x20;
  static constexpr uint8_t kOr = 0x21;
  static constexpr uint8_t kPlus = 0x22;
  static constexpr uint8_t kPlusUconst = 0x23;
  static constexpr uint8_t kShl = 0x24;
  static constexpr uint8_t kShr = 0x25;
  static constexpr uint8_t kShra = 0x26;
  static constexpr uint8_t kXor = 0x27;

  // Pops a value and branches by a signed 2-byte offset if the value is nonzero.
  static constexpr uint8_t kBra = 0x28;

  // Comparisons. These pop two values and push 1 if the comparison is true and 0 otherwise.
  static constexpr uint8_t kEq = 0x29;
  static constexpr uint8_t kGe = 0x2a;
  static constexpr uint8_t kGt = 0x2b;
  static constexpr uint8_t kLe = 0x2c;
  static constexpr uint8_t kLt = 0x2d;
  static constexpr uint8_t kNe = 0x2e;

  // Unconditional branch by a signed 2-byte offset.
  static constexpr uint8_t kSkip = 0x2f;

  // Push the literal values 0 to 31. Code gets the value with "op - kLit0".
  static constexpr uint8_t kLit0 = 0x30;
  static constexpr uint8_t kLit1 = 0x31;
  static constexpr uint8_t kLit2 = 0x32;
  static constexpr uint8_t kLit3 = 0x33;
  static constexpr uint8_t kLit4 = 0x34;
  static constexpr uint8_t kLit5 = 0x35;
  static constexpr uint8_t kLit6 = 0x36;
  static constexpr uint8_t kLit7 = 0x37;
  static constexpr uint8_t kLit8 = 0x38;
  static constexpr uint8_t kLit9 = 0x39;
  static constexpr uint8_t kLit10 = 0x3a;
  static constexpr uint8_t kLit11 = 0x3b;
  static constexpr uint8_t kLit12 = 0x3c;
  static constexpr uint8_t kLit13 = 0x3d;
  static constexpr uint8_t kLit14 = 0x3e;
  static constexpr uint8_t kLit15 = 0x3f;
  static constexpr uint8_t kLit16 = 0x40;
  static constexpr uint8_t kLit17 = 0x41;
  static constexpr uint8_t kLit18 = 0x42;
  static constexpr uint8_t kLit19 = 0x43;
  static constexpr uint8_t kLit20 = 0x44;
  static constexpr uint8_t kLit21 = 0x45;
  static constexpr uint8_t kLit22 = 0x46;
  static constexpr uint8_t kLit23 = 0x47;
  static constexpr uint8_t kLit24 = 0x48;
  static constexpr uint8_t kLit25 = 0x49;
  static constexpr uint8_t kLit26 = 0x4a;
  static constexpr uint8_t kLit27 = 0x4b;
  static constexpr uint8_t kLit28 = 0x4c;
  static constexpr uint8_t kLit29 = 0x4d;
  static constexpr uint8_t kLit30 = 0x4e;
  static constexpr uint8_t kLit31 = 0x4f;

  // The value is in register 0 to 31. Code gets the register number with "op - kReg0".
  static constexpr uint8_t kReg0 = 0x50;
  static constexpr uint8_t kReg1 = 0x51;
  static constexpr uint8_t kReg2 = 0x52;
  static constexpr uint8_t kReg3 = 0x53;
  static constexpr uint8_t kReg4 = 0x54;
  static constexpr uint8_t kReg5 = 0x55;
  static constexpr uint8_t kReg6 = 0x56;
  static constexpr uint8_t kReg7 = 0x57;
  static constexpr uint8_t kReg8 = 0x58;
  static constexpr uint8_t kReg9 = 0x59;
  static constexpr uint8_t kReg10 = 0x5a;
  static constexpr uint8_t kReg11 = 0x5b;
  static constexpr uint8_t kReg12 = 0x5c;
  static constexpr uint8_t kReg13 = 0x5d;
  static constexpr uint8_t kReg14 = 0x5e;
  static constexpr uint8_t kReg15 = 0x5f;
  static constexpr uint8_t kReg16 = 0x60;
  static constexpr uint8_t kReg17 = 0x61;
  static constexpr uint8_t kReg18 = 0x62;
  static constexpr uint8_t kReg19 = 0x63;
  static constexpr uint8_t kReg20 = 0x64;
  static constexpr uint8_t kReg21 = 0x65;
  static constexpr uint8_t kReg22 = 0x66;
  static constexpr uint8_t kReg23 = 0x67;
  static constexpr uint8_t kReg24 = 0x68;
  static constexpr uint8_t kReg25 = 0x69;
  static constexpr uint8_t kReg26 = 0x6a;
  static constexpr uint8_t kReg27 = 0x6b;
  static constexpr uint8_t kReg28 = 0x6c;
  static constexpr uint8_t kReg29 = 0x6d;
  static constexpr uint8_t kReg30 = 0x6e;
  static constexpr uint8_t kReg31 = 0x6f;

  // Push the value of register 0 to 31 plus a signed LEB128 offset. Code gets the register
  // number with "op - kBreg0".
  static constexpr uint8_t kBreg0 = 0x70;
  static constexpr uint8_t kBreg1 = 0x71;
  static constexpr uint8_t kBreg2 = 0x72;
  static constexpr uint8_t kBreg3 = 0x73;
  static constexpr uint8_t kBreg4 = 0x74;
  static constexpr uint8_t kBreg5 = 0x75;
  static constexpr uint8_t kBreg6 = 0x76;
  static constexpr uint8_t kBreg7 = 0x77;
  static constexpr uint8_t kBreg8 = 0x78;
  static constexpr uint8_t kBreg9 = 0x79;
  static constexpr uint8_t kBreg10 = 0x7a;
  static constexpr uint8_t kBreg11 = 0x7b;
  static constexpr uint8_t kBreg12 = 0x7c;
  static constexpr uint8_t kBreg13 = 0x7d;
  static constexpr uint8_t kBreg14 = 0x7e;
  static constexpr uint8_t kBreg15 = 0x7f;
  static constexpr uint8_t kBreg16 = 0x80;
  static constexpr uint8_t kBreg17 = 0x81;
  static constexpr uint8_t kBreg18 = 0x82;
  static constexpr uint8_t kBreg19 = 0x83;
  static constexpr uint8_t kBreg20 = 0x84;
  static constexpr uint8_t kBreg21 = 0x85;
  static constexpr uint8_t kBreg22 = 0x86;
  static constexpr uint8_t kBreg23 = 0x87;
  static constexpr uint8_t kBreg24 = 0x88;
  static constexpr uint8_t kBreg25 = 0x89;
  static constexpr uint8_t kBreg26 = 0x8a;
  static constexpr uint8_t kBreg27 = 0x8b;
  static constexpr uint8_t kBreg28 = 0x8c;
  static constexpr uint8_t kBreg29 = 0x8d;
  static constexpr uint8_t kBreg30 = 0x8e;
  static constexpr uint8_t kBreg31 = 0x8f;

  // The value is in the register given by an unsigned LEB128 operand.
  static constexpr uint8_t kRegx = 0x90;

  // Pushes the frame base of the function plus a signed LEB128 offset.
  static constexpr uint8_t kFbreg = 0x91;

  // Pushes the value of the register given by an unsigned LEB128 operand plus a signed LEB128
  // offset.
  static constexpr uint8_t kBregx = 0x92;

  // Describes a piece of a composite value. The operand is the size of the piece in bytes.
  static constexpr uint8_t kPiece = 0x93;

  // Pops an address and pushes the value at that address. The operand is the size of the value.
  static constexpr uint8_t kDerefSize = 0x94;
  static constexpr uint8_t kXderefSize = 0x95;

  // Does nothing.
  static constexpr uint8_t kNop = 0x96;

  // Pushes the address of the object that the expression is evaluated for.
  static constexpr uint8_t kPushObjectAddress = 0x97;

  // Calls the expression of another DIE.
  static constexpr uint8_t kCall2 = 0x98;
  static constexpr uint8_t kCall4 = 0x99;
  static constexpr uint8_t kCallRef = 0x9a;

  // Pops a thread-local storage offset and pushes the address of the variable for the current
  // thread.
  static constexpr uint8_t kFormTlsAddress = 0x9b;

  // Pushes the canonical frame address of the current frame.
  static constexpr uint8_t kCallFrameCfa = 0x9c;

  // Describes a piece of a composite value. The operands are the size and offset in bits.
  static constexpr uint8_t kBitPiece = 0x9d;

  // The value is the block that follows the opcode.
  static constexpr uint8_t kImplicitValue = 0x9e;

  // The value is on the top of the stack instead of at the address on the top of the stack.
  static constexpr uint8_t kStackValue = 0x9f;

  // The value is a pointer to a value that has no address, for example an optimized-out variable.
  static constexpr uint8_t kImplicitPointer = 0xa0;

  // Push an address or a constant from the address table of the unit. The operand is an index.
  static constexpr uint8_t kAddrx = 0xa1;
  static constexpr uint8_t kConstx = 0xa2;

  // Evaluates an expression with the register values at the entry of the current function.
  static constexpr uint8_t kEntryValue = 0xa3;

  // Typed stack operations. The operands reference a base type DIE.
  static constexpr uint8_t kConstType = 0xa4;
  static constexpr uint8_t kRegvalType = 0xa5;
  static constexpr uint8_t kDerefType = 0xa6;
  static constexpr uint8_t kXderefType = 0xa7;
  static constexpr uint8_t kConvert = 0xa8;
  static constexpr uint8_t kReinterpret = 0xa9;

  // -----------------------------------------------------------------------------------------------

  // Identifies one-past-the-end of the opcodes where we define constants. For range checking.
  static constexpr uint8_t kLastDefined = 0xaa;

  // User-defined range.
  static constexpr uint8_t kLoUser = 0xe0;
  static constexpr uint8_t kHiUser = 0xff;

  // GNU extensions. Most of these were used before DWARF 5 added the standard opcodes of the same
  // names.
  static constexpr uint8_t kGnuPushTlsAddress = 0xe0;
  static constexpr uint8_t kGnuUninit = 0xf0;
  static constexpr uint8_t kGnuEncodedAddr = 0xf1;
  static constexpr uint8_t kGnuImplicitPointer = 0xf2;
  static constexpr uint8_t kGnuEntryValue = 0xf3;
  static constexpr uint8_t kGnuConstType = 0xf4;
  static constexpr uint8_t kGnuRegvalType = 0xf5;
  static constexpr uint8_t kGnuDerefType = 0xf6;
  static constexpr uint8_t kGnuConvert = 0xf7;
  static constexpr uint8_t kGnuReinterpret = 0xf9;
  static constexpr uint8_t kGnuParameterRef = 0xfa;
  static constexpr uint8_t kGnuAddrIndex = 0xfb;
  static constexpr uint8_t kGnuConstIndex = 0xfc;
  static constexpr uint8_t kGnuVariableValue = 0xfd;
};

// Converts the opcode to a string, optionally including the numeric value. Invalid or unknown
// values will be formatted as "<undefined>" and will always include the numeric value.
std::string DwarfOpToString(uint8_t op, bool include_number = false);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_OP_H_
