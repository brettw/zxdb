// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_ATTR_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_ATTR_H_

#include <string>

namespace zxdb {

// DWARF attribute names. The values are the DW_AT_* values of the DWARF 5 standard.
//
// An attribute can have a value that isn't one of these constants, for example an attribute of a
// newer DWARF version or of a vendor. Code that reads attributes from symbol files must accept
// these values.
enum class DwarfAttr : int {
  // Not a DWARF attribute, this is used to indicate "not present."
  kNone = 0x00,

  // Reference to the next sibling DIE. Producers can add this so consumers can skip children.
  kSibling = 0x01,

  // Location description of a variable or parameter. Can be an expression or a location list.
  kLocation = 0x02,

  // Name of the declared entity as it appears in the source.
  kName = 0x03,

  kOrdering = 0x09,

  // Size in bytes of a type, a data member, or the storage of a value.
  kByteSize = 0x0b,

  // DWARF 2 and 3 bitfield offset, counted from the most significant bit of the storage. DWARF 4
  // replaces it with kDataBitOffset.
  kBitOffset = 0x0c,

  // Size in bits of a bitfield data member.
  kBitSize = 0x0d,

  // Section offset of the line table of a unit.
  kStmtList = 0x10,

  // Address of the first instruction of a code range.
  kLowPc = 0x11,

  // End of a code range. If the form is of class "constant," it is an offset from kLowPc.
  // Otherwise it is an address.
  kHighPc = 0x12,

  // Source language of a unit. See DwarfLang.
  kLanguage = 0x13,

  // Reference to the data member of a VariantPart that selects the active Variant.
  kDiscr = 0x15,

  // Discriminant value that selects this Variant.
  kDiscrValue = 0x16,

  kVisibility = 0x17,

  // Reference to the declaration, module or unit that an imported entry brings in.
  kImport = 0x18,

  kStringLength = 0x19,
  kCommonReference = 0x1a,

  // Compilation directory of a unit.
  kCompDir = 0x1b,

  // Constant value of a variable, a template value parameter, or an enumerator.
  kConstValue = 0x1c,

  // For a pointer to member type, the class that contains the member.
  kContainingType = 0x1d,

  kDefaultValue = 0x1e,
  kInline = 0x20,
  kIsOptional = 0x21,
  kLowerBound = 0x22,
  kProducer = 0x25,
  kPrototyped = 0x27,
  kReturnAddr = 0x2a,
  kStartScope = 0x2c,
  kBitStride = 0x2e,

  // Upper bound of an array subrange. GCC generates this. See kCount.
  kUpperBound = 0x2f,

  // Reference to the DIE that this one is a concrete instance of, for example the definition of an
  // inlined function. The decoder underlays the attributes of the abstract origin.
  kAbstractOrigin = 0x31,

  kAccessibility = 0x32,
  kAddressClass = 0x33,

  // Set for entities that the compiler generates, such as "this" parameters.
  kArtificial = 0x34,

  kBaseTypes = 0x35,

  // Calling convention of a function, or how a type is passed (by value or by reference).
  kCallingConvention = 0x36,

  // Number of elements in an array subrange. Clang generates this. See kUpperBound.
  kCount = 0x37,

  // Offset of a data member or a base class in its containing type. Can be a constant or an
  // expression.
  kDataMemberLocation = 0x38,

  kDeclColumn = 0x39,

  // Index of the declaration file in the line table of the unit.
  kDeclFile = 0x3a,

  // Line of the declaration in kDeclFile.
  kDeclLine = 0x3b,

  // Set when the DIE is a declaration that is not a definition.
  kDeclaration = 0x3c,

  kDiscrList = 0x3d,

  // Encoding of a base type, such as signed, unsigned, or float.
  kEncoding = 0x3e,

  // Set for entities with external linkage.
  kExternal = 0x3f,

  // Location of the frame base of a function. Variable locations can be relative to it.
  kFrameBase = 0x40,

  kFriend = 0x41,
  kIdentifierCase = 0x42,
  kMacroInfo = 0x43,
  kNamelistItem = 0x44,
  kPriority = 0x45,
  kSegment = 0x46,

  // Reference to the declaration that this definition completes, for example the declaration of a
  // member function in its class.
  kSpecification = 0x47,

  kStaticLink = 0x48,

  // Reference to the type of the entity.
  kType = 0x49,

  kUseLocation = 0x4a,
  kVariableParameter = 0x4b,
  kVirtuality = 0x4c,
  kVtableElemLocation = 0x4d,
  kAllocated = 0x4e,
  kAssociated = 0x4f,
  kDataLocation = 0x50,
  kByteStride = 0x51,
  kEntryPc = 0x52,
  kUseUtf8 = 0x53,
  kExtension = 0x54,

  // Section offset or index of a list of code ranges, for entities with code in more than one
  // range.
  kRanges = 0x55,

  kTrampoline = 0x56,
  kCallColumn = 0x57,

  // Index of the file of the call to an inlined subroutine.
  kCallFile = 0x58,

  // Line of the call to an inlined subroutine.
  kCallLine = 0x59,

  kDescription = 0x5a,
  kBinaryScale = 0x5b,
  kDecimalScale = 0x5c,
  kSmall = 0x5d,
  kDecimalSign = 0x5e,
  kDigitCount = 0x5f,
  kPictureString = 0x60,
  kMutable = 0x61,
  kThreadsScaled = 0x62,
  kExplicit = 0x63,

  // Reference to the "this" parameter of a member function.
  kObjectPointer = 0x64,

  kEndianity = 0x65,
  kElemental = 0x66,
  kPure = 0x67,
  kRecursive = 0x68,

  // 8-byte signature of a type unit that contains the definition of this type.
  kSignature = 0x69,

  // Set on the main function of a program.
  kMainSubprogram = 0x6a,

  // Bitfield offset from the start of the containing type. Replaces kBitOffset in DWARF 4.
  kDataBitOffset = 0x6b,

  kConstExpr = 0x6c,
  kEnumClass = 0x6d,

  // Mangled name of an entity with linkage.
  kLinkageName = 0x6e,

  kStringLengthBitSize = 0x6f,
  kStringLengthByteSize = 0x70,
  kRank = 0x71,

  // Offset of the string offsets table of a unit. See DW_FORM_strx.
  kStrOffsetsBase = 0x72,

  // Offset of the address table of a unit. See DW_FORM_addrx.
  kAddrBase = 0x73,

  // Offset of the range list table of a unit.
  kRnglistsBase = 0x74,

  // Name of the .dwo file of a split DWARF skeleton unit.
  kDwoName = 0x76,

  kReference = 0x77,
  kRvalueReference = 0x78,
  kMacros = 0x79,
  kCallAllCalls = 0x7a,
  kCallAllSourceCalls = 0x7b,
  kCallAllTailCalls = 0x7c,

  // Return address of a call site.
  kCallReturnPc = 0x7d,

  // Value of a call site parameter at the time of the call.
  kCallValue = 0x7e,

  kCallOrigin = 0x7f,
  kCallParameter = 0x80,
  kCallPc = 0x81,
  kCallTailCall = 0x82,
  kCallTarget = 0x83,
  kCallTargetClobbered = 0x84,
  kCallDataLocation = 0x85,
  kCallDataValue = 0x86,
  kNoreturn = 0x87,
  kAlignment = 0x88,
  kExportSymbols = 0x89,
  kDeleted = 0x8a,
  kDefaulted = 0x8b,

  // Offset of the location list table of a unit.
  kLoclistsBase = 0x8c,

  // -----------------------------------------------------------------------------------------------

  // Identifies one-past-the-end of the attributes where we define constants. For range checking.
  kLastDefined,

  // User-defined range.
  kLoUser = 0x2000,

  // Legacy vendor attribute for the mangled name.
  kMipsLinkageName = 0x2007,

  // GNU extension for the name of the .dwo file of a split DWARF skeleton unit. Used before DWARF 5
  // added kDwoName.
  kGnuDwoName = 0x2130,
  kGnuDwoId = 0x2131,
  kGnuRangesBase = 0x2132,
  kGnuAddrBase = 0x2133,

  kHiUser = 0x3fff,
};

// Converts the attribute to a string, optionally including the numeric value. Invalid or unknown
// values will be formatted as "<undefined>" and will always include the numeric value.
std::string DwarfAttrToString(DwarfAttr attr, bool include_number = false);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_ATTR_H_
