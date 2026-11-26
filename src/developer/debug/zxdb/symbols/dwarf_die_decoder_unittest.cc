// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"

#include <functional>

#include <gtest/gtest.h>

#include "llvm/ADT/StringMap.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/Support/MemoryBuffer.h"
#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"
#include "src/developer/debug/zxdb/symbols/dwarf_op.h"
#include "src/developer/debug/zxdb/symbols/dwarf_tag.h"
#include "src/developer/debug/zxdb/symbols/line_program_test_util.h"

namespace zxdb {

namespace {

// Abbreviation codes of the test unit.
enum Abbrev : uint8_t {
  kCompileUnitAbbrev = 1,
  kNamespaceAbbrev,
  kOriginAbbrev,
  kConcreteAbbrev,
  kFormsAbbrev,
  kOtherFormsAbbrev,
  kCycleAbbrev,
  kDuplicateAbbrev,
  kClassAbbrev,
  kDeclAbbrev,
  kSpecAbstractAbbrev,
  kSpecConcreteAbbrev,
  kNameLineAbbrev,
  kBothLinksAbbrev,
  kSpecCycleAbbrev,
  kOriginCycleAbbrev,
  kConstantsAbbrev,
  kSkeletonUnitAbbrev,
  kAddressesAbbrev,
  kCorruptAbbrev,
};

// The address table in .debug_addr. Its entries start after the 8-byte header.
constexpr uint64_t kAddrBase = 8;
constexpr uint64_t kTableAddresses[] = {0x5000, 0x6000, 0xffffffffffffffff};

constexpr int64_t kImplicitByteSize = 4;
constexpr int64_t kImplicitCount = 7;

// Writes a DWARF 5 unit with these DIEs:
//
//   DW_TAG_compile_unit "unit"
//     DW_TAG_namespace "ns"
//       DW_TAG_subprogram "Func" (the abstract origin)
//     DW_TAG_subprogram (the concrete instance of "Func")
//     DW_TAG_variable with many forms
//     DW_TAG_variable with other forms
//     DW_TAG_variable whose abstract origin is itself
//     DW_TAG_variable with a duplicate DW_AT_name
//     DW_TAG_structure_type "Class"
//       DW_TAG_subprogram "Method" (the declaration)
//     DW_TAG_subprogram (the abstract instance, specification is "Method")
//     DW_TAG_subprogram (the concrete instance of the abstract instance)
//     DW_TAG_variable "origin_name" and DW_TAG_variable "spec_name"
//     DW_TAG_variable with both links, to "origin_name" and "spec_name"
//     DW_TAG_variable "cycle_a" whose specification has cycle_a as its abstract origin
//     DW_TAG_variable (that abstract origin)
//     DW_TAG_variable with constants in unusual forms
//
// And three more units:
//
//   DW_TAG_compile_unit "skeleton" with DW_AT_addr_base, like the skeleton unit of a .dwo file
//   DW_TAG_compile_unit "dwo" without DW_AT_addr_base, like a .dwo unit
//     DW_TAG_subprogram with addresses in the DW_FORM_addrx forms
//   DW_TAG_compile_unit "corrupt"
//     DW_TAG_variable whose DW_AT_location block ends past the end of the unit
class DwarfDieDecoderTest : public testing::Test {
 public:
  DwarfDieDecoderTest() {
    WriteAbbrevs();
    WriteInfo();

    buffers_["debug_info"] = MakeBuffer(info_, "debug_info");
    buffers_["debug_abbrev"] = MakeBuffer(abbrev_, "debug_abbrev");
    buffers_["debug_addr"] = MakeBuffer(addr_, "debug_addr");
    context_ = llvm::DWARFContext::create(buffers_, 8, true);
  }

  llvm::DWARFContext* context() { return context_.get(); }

  // Returns the DIE at the offset in the unit with the given index.
  llvm::DWARFDie GetDieInUnit(unsigned unit_index, uint64_t offset) {
    return context_->getUnitAtIndex(unit_index)->getDIEForOffset(offset);
  }

  // Returns the unit with the given index, with its unit DIE extracted so its DW_AT_addr_base is
  // known.
  llvm::DWARFUnit* GetUnit(unsigned unit_index) {
    llvm::DWARFUnit* unit = context_->getUnitAtIndex(unit_index);
    unit->getUnitDIE();
    return unit;
  }

  llvm::DWARFDie GetDie(uint64_t offset) {
    return context_->getUnitAtIndex(0)->getDIEForOffset(offset);
  }

  // Offsets of the DIEs from the start of the section. The unit is at offset 0, so these are also
  // the unit-relative offsets.
  uint64_t unit_offset() const { return unit_offset_; }
  uint64_t namespace_offset() const { return namespace_offset_; }
  uint64_t origin_offset() const { return origin_offset_; }
  uint64_t concrete_offset() const { return concrete_offset_; }
  uint64_t forms_offset() const { return forms_offset_; }
  uint64_t other_forms_offset() const { return other_forms_offset_; }
  uint64_t cycle_offset() const { return cycle_offset_; }
  uint64_t duplicate_offset() const { return duplicate_offset_; }
  uint64_t class_offset() const { return class_offset_; }
  uint64_t decl_offset() const { return decl_offset_; }
  uint64_t spec_abstract_offset() const { return spec_abstract_offset_; }
  uint64_t spec_concrete_offset() const { return spec_concrete_offset_; }
  uint64_t origin_target_offset() const { return origin_target_offset_; }
  uint64_t spec_target_offset() const { return spec_target_offset_; }
  uint64_t both_links_offset() const { return both_links_offset_; }
  uint64_t cycle_a_offset() const { return cycle_a_offset_; }
  uint64_t cycle_b_offset() const { return cycle_b_offset_; }
  uint64_t constants_offset() const { return constants_offset_; }
  uint64_t addresses_offset() const { return addresses_offset_; }
  uint64_t corrupt_offset() const { return corrupt_offset_; }

  // Unit indices.
  static constexpr unsigned kSkeletonUnit = 1;
  static constexpr unsigned kDwoUnit = 2;
  static constexpr unsigned kCorruptUnit = 3;

 private:
  static std::unique_ptr<llvm::MemoryBuffer> MakeBuffer(const std::vector<uint8_t>& data,
                                                        const char* name) {
    return llvm::MemoryBuffer::getMemBufferCopy(
        llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size()), name);
  }

  static void AppendString(std::vector<uint8_t>& out, const std::string& str) {
    out.insert(out.end(), str.begin(), str.end());
    out.push_back(0);
  }

  void AddAbbrev(uint8_t code, DwarfTag tag, bool has_children,
                 const std::vector<std::pair<DwarfAttr, DwarfForm>>& specs,
                 std::optional<int64_t> implicit_const = std::nullopt) {
    AppendUleb(abbrev_, code);
    AppendUleb(abbrev_, static_cast<uint64_t>(tag));
    abbrev_.push_back(has_children ? 1 : 0);
    for (const auto& [attr, form] : specs) {
      AppendUleb(abbrev_, static_cast<uint64_t>(attr));
      AppendUleb(abbrev_, form);
      if (form == DwarfForm::kImplicitConst) {
        // The test values are small positive numbers, so the SLEB128 encoding is one byte.
        FX_CHECK(implicit_const && *implicit_const >= 0 && *implicit_const < 64);
        abbrev_.push_back(static_cast<uint8_t>(*implicit_const));
      }
    }
    abbrev_.push_back(0);
    abbrev_.push_back(0);
  }

  void WriteAbbrevs() {
    AddAbbrev(kCompileUnitAbbrev, DwarfTag::kCompileUnit, true,
              {{DwarfAttr::kName, DwarfForm::kString}});
    AddAbbrev(kNamespaceAbbrev, DwarfTag::kNamespace, true,
              {{DwarfAttr::kName, DwarfForm::kString}});
    AddAbbrev(kOriginAbbrev, DwarfTag::kSubprogram, false,
              {{DwarfAttr::kName, DwarfForm::kString},
               {DwarfAttr::kDeclLine, DwarfForm::kData1},
               {DwarfAttr::kExternal, DwarfForm::kFlagPresent},
               {DwarfAttr::kByteSize, DwarfForm::kImplicitConst}},
              kImplicitByteSize);
    AddAbbrev(kConcreteAbbrev, DwarfTag::kSubprogram, false,
              {{DwarfAttr::kAbstractOrigin, DwarfForm::kRef4},
               {DwarfAttr::kDeclLine, DwarfForm::kData1},
               {DwarfAttr::kLowPc, DwarfForm::kAddr},
               {DwarfAttr::kHighPc, DwarfForm::kData4}});
    AddAbbrev(kFormsAbbrev, DwarfTag::kVariable, false,
              {{DwarfAttr::kDeclaration, DwarfForm::kFlag},
               {DwarfAttr::kArtificial, DwarfForm::kFlag},
               {DwarfAttr::kConstValue, DwarfForm::kSdata},
               {DwarfAttr::kLocation, DwarfForm::kExprloc},
               {DwarfAttr::kAddrBase, DwarfForm::kSecOffset},
               {DwarfAttr::kType, DwarfForm::kRef4},
               {DwarfAttr::kCount, DwarfForm::kImplicitConst}},
              kImplicitCount);
    AddAbbrev(kOtherFormsAbbrev, DwarfTag::kVariable, false,
              {{DwarfAttr::kConstValue, DwarfForm::kUdata},
               {DwarfAttr::kHighPc, DwarfForm::kAddr},
               {DwarfAttr::kLocation, DwarfForm::kBlock1}});
    AddAbbrev(
        kCycleAbbrev, DwarfTag::kVariable, false,
        {{DwarfAttr::kAbstractOrigin, DwarfForm::kRef4}, {DwarfAttr::kName, DwarfForm::kString}});
    AddAbbrev(kDuplicateAbbrev, DwarfTag::kVariable, false,
              {{DwarfAttr::kName, DwarfForm::kString}, {DwarfAttr::kName, DwarfForm::kString}});
    AddAbbrev(kClassAbbrev, DwarfTag::kStructureType, true,
              {{DwarfAttr::kName, DwarfForm::kString}});
    AddAbbrev(kDeclAbbrev, DwarfTag::kSubprogram, false,
              {{DwarfAttr::kName, DwarfForm::kString},
               {DwarfAttr::kDeclaration, DwarfForm::kFlagPresent},
               {DwarfAttr::kExternal, DwarfForm::kFlagPresent},
               {DwarfAttr::kDeclLine, DwarfForm::kData1},
               {DwarfAttr::kSibling, DwarfForm::kRef4}});
    AddAbbrev(
        kSpecAbstractAbbrev, DwarfTag::kSubprogram, false,
        {{DwarfAttr::kSpecification, DwarfForm::kRef4}, {DwarfAttr::kDeclLine, DwarfForm::kData1}});
    AddAbbrev(
        kSpecConcreteAbbrev, DwarfTag::kSubprogram, false,
        {{DwarfAttr::kAbstractOrigin, DwarfForm::kRef4}, {DwarfAttr::kLowPc, DwarfForm::kAddr}});
    AddAbbrev(kNameLineAbbrev, DwarfTag::kVariable, false,
              {{DwarfAttr::kName, DwarfForm::kString}, {DwarfAttr::kDeclLine, DwarfForm::kData1}});
    AddAbbrev(kBothLinksAbbrev, DwarfTag::kVariable, false,
              {{DwarfAttr::kSpecification, DwarfForm::kRef4},
               {DwarfAttr::kAbstractOrigin, DwarfForm::kRef4}});
    AddAbbrev(
        kSpecCycleAbbrev, DwarfTag::kVariable, false,
        {{DwarfAttr::kSpecification, DwarfForm::kRef4}, {DwarfAttr::kName, DwarfForm::kString}});
    AddAbbrev(kOriginCycleAbbrev, DwarfTag::kVariable, false,
              {{DwarfAttr::kAbstractOrigin, DwarfForm::kRef4},
               {DwarfAttr::kDeclLine, DwarfForm::kData1}});
    AddAbbrev(kConstantsAbbrev, DwarfTag::kVariable, false,
              {{DwarfAttr::kConstValue, DwarfForm::kSdata},
               {DwarfAttr::kByteSize, DwarfForm::kData1},
               {DwarfAttr::kBitSize, DwarfForm::kData16},
               {DwarfAttr::kExternal, DwarfForm::kFlag}});
    AddAbbrev(
        kSkeletonUnitAbbrev, DwarfTag::kCompileUnit, false,
        {{DwarfAttr::kName, DwarfForm::kString}, {DwarfAttr::kAddrBase, DwarfForm::kSecOffset}});
    AddAbbrev(kAddressesAbbrev, DwarfTag::kSubprogram, false,
              {{DwarfAttr::kLowPc, DwarfForm::kAddrx1},
               {DwarfAttr::kHighPc, DwarfForm::kLlvmAddrxOffset},
               {DwarfAttr::kEntryPc, DwarfForm::kAddrx},
               {DwarfAttr::kCallReturnPc, DwarfForm::kAddr}});
    AddAbbrev(kCorruptAbbrev, DwarfTag::kVariable, false,
              {{DwarfAttr::kName, DwarfForm::kString}, {DwarfAttr::kLocation, DwarfForm::kBlock1}});
    abbrev_.push_back(0);  // End of the abbreviations.
  }

  // Returns the offset that the next DIE will have.
  uint64_t NextOffset() const { return info_.size(); }

  void WriteInfo() {
    AppendInteger(info_, 0, 4, Endian::kLittle);  // Unit length, set below.
    AppendInteger(info_, 5, 2, Endian::kLittle);  // Version.
    info_.push_back(kUnitTypeCompile);
    info_.push_back(8);          // Address size.
    AppendInteger(info_, 0, 4, Endian::kLittle);  // Abbreviation offset.

    unit_offset_ = NextOffset();
    AppendUleb(info_, kCompileUnitAbbrev);
    AppendString(info_, "unit");

    namespace_offset_ = NextOffset();
    AppendUleb(info_, kNamespaceAbbrev);
    AppendString(info_, "ns");

    origin_offset_ = NextOffset();
    AppendUleb(info_, kOriginAbbrev);
    AppendString(info_, "Func");
    info_.push_back(10);  // DW_AT_decl_line.

    info_.push_back(0);  // End of the namespace children.

    concrete_offset_ = NextOffset();
    AppendUleb(info_, kConcreteAbbrev);
    AppendInteger(info_, origin_offset_, 4, Endian::kLittle);
    info_.push_back(20);              // DW_AT_decl_line overrides the origin.
    AppendInteger(info_, 0x1000, 8, Endian::kLittle);  // DW_AT_low_pc.
    AppendInteger(info_, 0x20, 4, Endian::kLittle);    // DW_AT_high_pc.

    forms_offset_ = NextOffset();
    AppendUleb(info_, kFormsAbbrev);
    info_.push_back(0);     // DW_AT_declaration is false.
    info_.push_back(1);     // DW_AT_artificial is true.
    info_.push_back(0x7b);  // DW_AT_const_value is -5 in SLEB128.
    AppendUleb(info_, 2);   // DW_AT_location length.
    info_.push_back(DwarfOp::kReg0 + 3);
    info_.push_back(DwarfOp::kStackValue);
    AppendInteger(info_, 0, 4, Endian::kLittle);  // DW_AT_addr_base is offset 0, which is valid.
    AppendInteger(info_, namespace_offset_, 4, Endian::kLittle);  // DW_AT_type.

    other_forms_offset_ = NextOffset();
    AppendUleb(info_, kOtherFormsAbbrev);
    AppendUleb(info_, 9);             // DW_AT_const_value.
    AppendInteger(info_, 0x2000, 8, Endian::kLittle);  // DW_AT_high_pc.
    info_.push_back(1);               // DW_AT_location length.
    info_.push_back(DwarfOp::kReg0 + 5);

    cycle_offset_ = NextOffset();
    AppendUleb(info_, kCycleAbbrev);
    AppendInteger(info_, cycle_offset_, 4, Endian::kLittle);
    AppendString(info_, "cycle");

    duplicate_offset_ = NextOffset();
    AppendUleb(info_, kDuplicateAbbrev);
    AppendString(info_, "first");
    AppendString(info_, "second");

    class_offset_ = NextOffset();
    AppendUleb(info_, kClassAbbrev);
    AppendString(info_, "Class");

    decl_offset_ = NextOffset();
    AppendUleb(info_, kDeclAbbrev);
    AppendString(info_, "Method");
    info_.push_back(30);                     // DW_AT_decl_line.
    AppendInteger(info_, class_offset_, 4,
                  Endian::kLittle);  // DW_AT_sibling. Its value isn't used.

    info_.push_back(0);  // End of the class children.

    spec_abstract_offset_ = NextOffset();
    AppendUleb(info_, kSpecAbstractAbbrev);
    AppendInteger(info_, decl_offset_, 4, Endian::kLittle);
    info_.push_back(31);  // DW_AT_decl_line overrides the declaration.

    spec_concrete_offset_ = NextOffset();
    AppendUleb(info_, kSpecConcreteAbbrev);
    AppendInteger(info_, spec_abstract_offset_, 4, Endian::kLittle);
    AppendInteger(info_, 0x3000, 8, Endian::kLittle);  // DW_AT_low_pc.

    origin_target_offset_ = NextOffset();
    AppendUleb(info_, kNameLineAbbrev);
    AppendString(info_, "origin_name");
    info_.push_back(50);

    spec_target_offset_ = NextOffset();
    AppendUleb(info_, kNameLineAbbrev);
    AppendString(info_, "spec_name");
    info_.push_back(60);

    // The specification comes first in the DIE, but the abstract origin wins.
    both_links_offset_ = NextOffset();
    AppendUleb(info_, kBothLinksAbbrev);
    AppendInteger(info_, spec_target_offset_, 4, Endian::kLittle);
    AppendInteger(info_, origin_target_offset_, 4, Endian::kLittle);

    // cycle_a references cycle_b, which comes after it.
    cycle_a_offset_ = NextOffset();
    AppendUleb(info_, kSpecCycleAbbrev);
    size_t cycle_b_ref = info_.size();
    AppendInteger(info_, 0, 4, Endian::kLittle);  // Set below.
    AppendString(info_, "cycle_a");

    cycle_b_offset_ = NextOffset();
    AppendUleb(info_, kOriginCycleAbbrev);
    AppendInteger(info_, cycle_a_offset_, 4, Endian::kLittle);
    info_.push_back(70);
    for (size_t i = 0; i < 4; i++)
      info_[cycle_b_ref + i] = static_cast<uint8_t>(cycle_b_offset_ >> (i * 8));

    constants_offset_ = NextOffset();
    AppendUleb(info_, kConstantsAbbrev);
    info_.push_back(7);                   // DW_AT_const_value is 7 in SLEB128.
    info_.push_back(0xff);                // DW_AT_byte_size.
    info_.insert(info_.end(), 16, 0x11);  // DW_AT_bit_size, 16 bytes.
    info_.push_back(1);                   // DW_AT_external.

    info_.push_back(0);  // End of the unit children.

    uint64_t unit_length = info_.size() - 4;
    for (size_t i = 0; i < 4; i++)
      info_[i] = static_cast<uint8_t>(unit_length >> (i * 8));

    WriteAddressUnits();
    WriteCorruptUnit();
  }

  // Writes a DWARF 5 unit header and returns the offset of the unit.
  size_t BeginUnit() {
    size_t start = info_.size();
    AppendInteger(info_, 0, 4, Endian::kLittle);  // Unit length, set by EndUnit().
    AppendInteger(info_, 5, 2, Endian::kLittle);
    info_.push_back(kUnitTypeCompile);
    info_.push_back(8);
    AppendInteger(info_, 0, 4, Endian::kLittle);
    return start;
  }

  void EndUnit(size_t start) {
    uint64_t unit_length = info_.size() - start - 4;
    for (size_t i = 0; i < 4; i++)
      info_[start + i] = static_cast<uint8_t>(unit_length >> (i * 8));
  }

  void WriteAddressUnits() {
    // The address table.
    AppendInteger(addr_, 4 + 8 * std::size(kTableAddresses), 4, Endian::kLittle);  // Unit length.
    AppendInteger(addr_, 5, 2, Endian::kLittle);                                   // Version.
    addr_.push_back(8);                                           // Address size.
    addr_.push_back(0);                                           // Segment selector size.
    for (uint64_t address : kTableAddresses)
      AppendInteger(addr_, address, 8, Endian::kLittle);

    size_t skeleton = BeginUnit();
    AppendUleb(info_, kSkeletonUnitAbbrev);
    AppendString(info_, "skeleton");
    AppendInteger(info_, kAddrBase, 4, Endian::kLittle);
    EndUnit(skeleton);

    size_t dwo = BeginUnit();
    AppendUleb(info_, kCompileUnitAbbrev);
    AppendString(info_, "dwo");
    addresses_offset_ = NextOffset();
    AppendUleb(info_, kAddressesAbbrev);
    info_.push_back(1);                           // DW_AT_low_pc is entry 1.
    AppendUleb(info_, 0);                         // DW_AT_high_pc is entry 0...
    AppendInteger(info_, 0x10, 4, Endian::kLittle);  // ...plus 0x10.
    AppendUleb(info_, 2);                         // DW_AT_entry_pc is the tombstone.
    AppendInteger(info_, 0xffffffffffffffff, 8, Endian::kLittle);  // DW_AT_call_return_pc too.
    info_.push_back(0);                           // End of the unit children.
    EndUnit(dwo);
  }

  void WriteCorruptUnit() {
    // This unit is last, so the block length is also past the end of the section. LLVM reads the
    // DIE because it doesn't check block lengths.
    size_t corrupt = BeginUnit();
    AppendUleb(info_, kCompileUnitAbbrev);
    AppendString(info_, "corrupt");
    corrupt_offset_ = NextOffset();
    AppendUleb(info_, kCorruptAbbrev);
    AppendString(info_, "var");
    info_.push_back(0xff);  // DW_AT_location length, past the end.
    info_.push_back(DwarfOp::kReg0);
    EndUnit(corrupt);
  }

  std::vector<uint8_t> abbrev_;
  std::vector<uint8_t> info_;
  std::vector<uint8_t> addr_;

  uint64_t unit_offset_ = 0;
  uint64_t namespace_offset_ = 0;
  uint64_t origin_offset_ = 0;
  uint64_t concrete_offset_ = 0;
  uint64_t forms_offset_ = 0;
  uint64_t other_forms_offset_ = 0;
  uint64_t cycle_offset_ = 0;
  uint64_t duplicate_offset_ = 0;
  uint64_t class_offset_ = 0;
  uint64_t decl_offset_ = 0;
  uint64_t spec_abstract_offset_ = 0;
  uint64_t spec_concrete_offset_ = 0;
  uint64_t origin_target_offset_ = 0;
  uint64_t spec_target_offset_ = 0;
  uint64_t both_links_offset_ = 0;
  uint64_t cycle_a_offset_ = 0;
  uint64_t cycle_b_offset_ = 0;
  uint64_t constants_offset_ = 0;
  uint64_t addresses_offset_ = 0;
  uint64_t corrupt_offset_ = 0;

  // The context references these buffers, so they must outlive it.
  llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> buffers_;
  std::unique_ptr<llvm::DWARFContext> context_;
};

}  // namespace

TEST(DwarfDieDecoder, RefAddrInDwo) {
  // DW_FORM_ref_addr in a .dwo file references a DIE in another unit of .debug_info.dwo.
  std::vector<uint8_t> abbrev = {
      1, 0x11, 1, 0x03, 0x08, 0, 0,  // 1: DW_TAG_compile_unit with children, DW_AT_name.
      2, 0x34, 0, 0x03, 0x08, 0, 0,  // 2: DW_TAG_variable, DW_AT_name.
      3, 0x34, 0, 0x49, 0x10, 0, 0,  // 3: DW_TAG_variable, DW_AT_type in DW_FORM_ref_addr.
      0};

  // Writes a DWARF 4 unit with one variable. Returns the offset of the variable.
  std::vector<uint8_t> info;
  auto write_unit = [&info](uint8_t variable_abbrev, const std::function<void()>& write_variable) {
    size_t start = info.size();
    AppendInteger(info, 0, 4, Endian::kLittle);  // Unit length, set below.
    AppendInteger(info, 4, 2, Endian::kLittle);  // Version.
    AppendInteger(info, 0, 4, Endian::kLittle);  // Abbreviation offset.
    info.push_back(8);          // Address size.
    AppendUleb(info, 1);
    info.insert(info.end(), {'u', 0});
    uint64_t variable_offset = info.size();
    AppendUleb(info, variable_abbrev);
    write_variable();
    info.push_back(0);  // End of the unit children.
    uint64_t length = info.size() - start - 4;
    for (size_t i = 0; i < 4; i++)
      info[start + i] = static_cast<uint8_t>(length >> (i * 8));
    return variable_offset;
  };
  uint64_t target_offset = write_unit(2, [&info]() { info.insert(info.end(), {'t', 0}); });
  uint64_t source_offset = write_unit(
      3, [&info, target_offset]() { AppendInteger(info, target_offset, 4, Endian::kLittle); });

  llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> buffers;
  auto make_buffer = [](const std::vector<uint8_t>& data, const char* name) {
    return llvm::MemoryBuffer::getMemBufferCopy(
        llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size()), name);
  };
  buffers["debug_info.dwo"] = make_buffer(info, "debug_info.dwo");
  buffers["debug_abbrev.dwo"] = make_buffer(abbrev, "debug_abbrev.dwo");
  std::unique_ptr<llvm::DWARFContext> context = llvm::DWARFContext::create(buffers, 8, true);

  const llvm::DWARFUnitVector& units = context->getDWOUnitsVector();
  ASSERT_EQ(2u, units.size());
  ASSERT_TRUE(units[1]->isDWOUnit());
  llvm::DWARFDie source = units[1]->getDIEForOffset(source_offset);
  ASSERT_TRUE(source);

  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kType};
  DwarfDieDecoder decoder(context.get(), kAttrs);
  ASSERT_TRUE(decoder.Decode(source));
  llvm::DWARFDie target = decoder.GetReference(DwarfAttr::kType);
  ASSERT_TRUE(target);
  EXPECT_EQ(target_offset, target.getOffset());
  EXPECT_EQ(units[0].get(), target.getDwarfUnit());
}

TEST(DwarfDieDecoder, SupplementaryStrings) {
  // A supplementary offset can coincide with a valid string offset in the local section.
  DwarfStringSectionBuilder strings;
  uint64_t offset = strings.AddString("local name");

  std::vector<uint8_t> info;
  DwarfSectionBuilder info_builder(info, Endian::kLittle);
  auto unit = info_builder.BeginUnit({});
  info_builder.Uleb(1);
  info_builder.Offset(offset, 4);
  info_builder.EndUnit(unit);

  auto make_buffer = [](std::span<const uint8_t> data) {
    return llvm::MemoryBuffer::getMemBufferCopy(
        llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size()));
  };
  for (DwarfForm form : {DwarfForm::kStrpSup, DwarfForm::kGnuStrpAlt}) {
    SCOPED_TRACE(DwarfFormToString(form));
    std::vector<uint8_t> abbrev;
    DwarfSectionBuilder abbrev_builder(abbrev, Endian::kLittle);
    const AbbrevTable::AttributeSpec attrs[] = {
        {static_cast<uint64_t>(DwarfAttr::kName), static_cast<uint64_t>(form), std::nullopt}};
    abbrev_builder.AbbrevDeclaration(1, static_cast<uint64_t>(DwarfTag::kCompileUnit), false,
                                     attrs);
    abbrev_builder.EndAbbrevTable();

    llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> buffers;
    buffers["debug_abbrev"] = make_buffer(abbrev);
    buffers["debug_info"] = make_buffer(info);
    buffers["debug_str"] = make_buffer(strings.bytes());
    auto context = llvm::DWARFContext::create(buffers, 8, true);

    static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName};
    DwarfDieDecoder decoder(context.get(), kAttrs);
    ASSERT_TRUE(decoder.Decode(context->getUnitAtIndex(0)->getUnitDIE()));
    ASSERT_TRUE(decoder.Has(DwarfAttr::kName));
    EXPECT_EQ(std::nullopt, decoder.GetCString(DwarfAttr::kName));
  }
}

TEST_F(DwarfDieDecoderTest, AbstractOrigin) {
  static constexpr DwarfAttr kAttrs[] = {
      DwarfAttr::kName,  DwarfAttr::kDeclLine, DwarfAttr::kExternal,      DwarfAttr::kByteSize,
      DwarfAttr::kLowPc, DwarfAttr::kHighPc,   DwarfAttr::kAbstractOrigin};
  DwarfDieDecoder decoder(context(), kAttrs);
  ASSERT_TRUE(decoder.Decode(GetDie(concrete_offset())));

  // The concrete DIE gives these values.
  EXPECT_EQ(20u, decoder.GetUnsignedConstant(DwarfAttr::kDeclLine));
  EXPECT_EQ(0x1000u, decoder.GetAddress(DwarfAttr::kLowPc));
  std::optional<DwarfDieDecoder::HighPC> high_pc = decoder.GetHighPC();
  ASSERT_TRUE(high_pc);
  EXPECT_TRUE(high_pc->is_constant);
  EXPECT_EQ(0x20u, high_pc->value);

  // The abstract origin gives these values. DW_AT_byte_size is an implicit constant.
  EXPECT_STREQ("Func", decoder.GetCString(DwarfAttr::kName).value_or(""));
  EXPECT_EQ(true, decoder.GetBool(DwarfAttr::kExternal));
  EXPECT_EQ(static_cast<uint64_t>(kImplicitByteSize),
            decoder.GetUnsignedConstant(DwarfAttr::kByteSize));

  EXPECT_TRUE(decoder.Has(DwarfAttr::kAbstractOrigin));
  EXPECT_EQ(GetDie(origin_offset()), decoder.GetReference(DwarfAttr::kAbstractOrigin));

  // The parent of the abstract origin is the namespace. The parent of the concrete DIE is the unit.
  EXPECT_EQ(GetDie(namespace_offset()), decoder.GetContextParent());

  // Without an abstract origin, the abstract parent is the regular parent.
  ASSERT_TRUE(decoder.Decode(GetDie(origin_offset())));
  EXPECT_FALSE(decoder.Has(DwarfAttr::kAbstractOrigin));
  EXPECT_EQ(10u, decoder.GetUnsignedConstant(DwarfAttr::kDeclLine));
  EXPECT_EQ(GetDie(namespace_offset()), decoder.GetContextParent());

  // The unit DIE has no parent.
  ASSERT_TRUE(decoder.Decode(GetDie(unit_offset())));
  EXPECT_STREQ("unit", decoder.GetCString(DwarfAttr::kName).value_or(""));
  EXPECT_FALSE(decoder.GetContextParent().isValid());
}

TEST_F(DwarfDieDecoderTest, Specification) {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName,           DwarfAttr::kDeclLine,
                                         DwarfAttr::kDeclaration,    DwarfAttr::kSibling,
                                         DwarfAttr::kExternal,       DwarfAttr::kLowPc,
                                         DwarfAttr::kAbstractOrigin, DwarfAttr::kSpecification};
  DwarfDieDecoder decoder(context(), kAttrs);

  // concrete instance -> (abstract origin) abstract instance -> (specification) declaration.
  ASSERT_TRUE(decoder.Decode(GetDie(spec_concrete_offset())));

  // Each value records the DIE that has it.
  ASSERT_TRUE(decoder.GetValue(DwarfAttr::kLowPc));
  EXPECT_EQ(0x3000u, decoder.GetAddress(DwarfAttr::kLowPc));
  EXPECT_EQ(GetDie(spec_concrete_offset()), decoder.GetValue(DwarfAttr::kLowPc)->die);

  // The abstract instance overrides the declaration.
  EXPECT_EQ(31u, decoder.GetUnsignedConstant(DwarfAttr::kDeclLine));
  EXPECT_EQ(GetDie(spec_abstract_offset()), decoder.GetValue(DwarfAttr::kDeclLine)->die);

  // The declaration gives the attributes that the other DIEs don't have.
  EXPECT_STREQ("Method", decoder.GetCString(DwarfAttr::kName).value_or(""));
  EXPECT_EQ(GetDie(decl_offset()), decoder.GetValue(DwarfAttr::kName)->die);
  EXPECT_EQ(true, decoder.GetBool(DwarfAttr::kExternal));

  // DW_AT_declaration and DW_AT_sibling apply only to the decoded DIE.
  EXPECT_FALSE(decoder.Has(DwarfAttr::kDeclaration));
  EXPECT_FALSE(decoder.Has(DwarfAttr::kSibling));

  // The links themselves are values too.
  EXPECT_EQ(GetDie(spec_abstract_offset()), decoder.GetReference(DwarfAttr::kAbstractOrigin));
  EXPECT_EQ(GetDie(decl_offset()), decoder.GetReference(DwarfAttr::kSpecification));

  // The declaration gives the context: the class.
  EXPECT_EQ(GetDie(class_offset()), decoder.GetContextParent());

  // A definition with only a specification gets the same context.
  ASSERT_TRUE(decoder.Decode(GetDie(spec_abstract_offset())));
  EXPECT_STREQ("Method", decoder.GetCString(DwarfAttr::kName).value_or(""));
  EXPECT_FALSE(decoder.Has(DwarfAttr::kDeclaration));
  EXPECT_EQ(GetDie(class_offset()), decoder.GetContextParent());

  // The declaration itself has DW_AT_declaration and DW_AT_sibling.
  ASSERT_TRUE(decoder.Decode(GetDie(decl_offset())));
  EXPECT_EQ(true, decoder.GetBool(DwarfAttr::kDeclaration));
  EXPECT_TRUE(decoder.Has(DwarfAttr::kSibling));
  EXPECT_EQ(GetDie(class_offset()), decoder.GetContextParent());
}

TEST_F(DwarfDieDecoderTest, DontFollowSpecifications) {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName, DwarfAttr::kDeclLine,
                                         DwarfAttr::kSpecification};
  DwarfDieDecoder decoder(context(), kAttrs);
  decoder.set_follow_specifications(false);
  ASSERT_TRUE(decoder.Decode(GetDie(spec_concrete_offset())));

  // The abstract origin is followed, but not its specification.
  EXPECT_EQ(31u, decoder.GetUnsignedConstant(DwarfAttr::kDeclLine));
  EXPECT_FALSE(decoder.Has(DwarfAttr::kName));
  EXPECT_EQ(GetDie(decl_offset()), decoder.GetReference(DwarfAttr::kSpecification));
  EXPECT_EQ(GetDie(unit_offset()), decoder.GetContextParent());
}

TEST_F(DwarfDieDecoderTest, AbstractOriginBeforeSpecification) {
  // A DIE with both links takes values from the abstract origin first.
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName, DwarfAttr::kDeclLine};
  DwarfDieDecoder decoder(context(), kAttrs);
  ASSERT_TRUE(decoder.Decode(GetDie(both_links_offset())));
  EXPECT_STREQ("origin_name", decoder.GetCString(DwarfAttr::kName).value_or(""));
  EXPECT_EQ(50u, decoder.GetUnsignedConstant(DwarfAttr::kDeclLine));
  EXPECT_EQ(GetDie(origin_target_offset()), decoder.GetValue(DwarfAttr::kName)->die);

  // The specification is visited last, so it gives the context.
  EXPECT_EQ(GetDie(unit_offset()), decoder.GetContextParent());
}

TEST_F(DwarfDieDecoderTest, CycleThroughBothLinks) {
  // cycle_a -> (specification) cycle_b -> (abstract origin) cycle_a.
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName, DwarfAttr::kDeclLine};
  DwarfDieDecoder decoder(context(), kAttrs);
  ASSERT_TRUE(decoder.Decode(GetDie(cycle_a_offset())));
  EXPECT_STREQ("cycle_a", decoder.GetCString(DwarfAttr::kName).value_or(""));
  EXPECT_EQ(70u, decoder.GetUnsignedConstant(DwarfAttr::kDeclLine));
  EXPECT_EQ(GetDie(cycle_b_offset()), decoder.GetValue(DwarfAttr::kDeclLine)->die);
}

TEST_F(DwarfDieDecoderTest, DontFollowAbstractOrigins) {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName, DwarfAttr::kDeclLine,
                                         DwarfAttr::kAbstractOrigin};
  DwarfDieDecoder decoder(context(), kAttrs);
  decoder.set_follow_abstract_origins(false);
  ASSERT_TRUE(decoder.Decode(GetDie(concrete_offset())));

  // Only the concrete DIE gives values. The name is on the abstract origin.
  EXPECT_FALSE(decoder.Has(DwarfAttr::kName));
  EXPECT_EQ(20u, decoder.GetUnsignedConstant(DwarfAttr::kDeclLine));
  EXPECT_EQ(GetDie(origin_offset()), decoder.GetReference(DwarfAttr::kAbstractOrigin));

  // The abstract parent is the parent of the concrete DIE.
  EXPECT_EQ(GetDie(unit_offset()), decoder.GetContextParent());

  // A DIE that is its own abstract origin is decoded once.
  ASSERT_TRUE(decoder.Decode(GetDie(cycle_offset())));
  EXPECT_STREQ("cycle", decoder.GetCString(DwarfAttr::kName).value_or(""));
}

TEST_F(DwarfDieDecoderTest, GetAbstractOrigin) {
  // Only the first link in the chain is followed.
  EXPECT_EQ(GetDie(origin_offset()), GetAbstractOrigin(GetDie(concrete_offset())));
  EXPECT_EQ(GetDie(cycle_offset()), GetAbstractOrigin(GetDie(cycle_offset())));

  // DIEs without an abstract origin.
  EXPECT_FALSE(GetAbstractOrigin(GetDie(origin_offset())).isValid());
  EXPECT_FALSE(GetAbstractOrigin(GetDie(unit_offset())).isValid());
  EXPECT_FALSE(GetAbstractOrigin(llvm::DWARFDie()).isValid());
}

TEST_F(DwarfDieDecoderTest, Forms) {
  static constexpr DwarfAttr kAttrs[] = {
      DwarfAttr::kDeclaration, DwarfAttr::kArtificial, DwarfAttr::kConstValue, DwarfAttr::kLocation,
      DwarfAttr::kAddrBase,    DwarfAttr::kType,       DwarfAttr::kCount,      DwarfAttr::kHighPc};
  DwarfDieDecoder decoder(context(), kAttrs);
  ASSERT_TRUE(decoder.Decode(GetDie(forms_offset())));

  // A DW_FORM_flag of 0 is false.
  EXPECT_EQ(false, decoder.GetBool(DwarfAttr::kDeclaration));
  EXPECT_EQ(true, decoder.GetBool(DwarfAttr::kArtificial));

  EXPECT_EQ(-5, decoder.GetSignedConstant(DwarfAttr::kConstValue));
  EXPECT_EQ(ConstValue(-5).GetConstValue(8),
            decoder.GetConstValue(DwarfAttr::kConstValue).GetConstValue(8));

  std::vector<uint8_t> expected_location = {DwarfOp::kReg0 + 3, DwarfOp::kStackValue};
  EXPECT_EQ(expected_location, decoder.GetBlock(DwarfAttr::kLocation));

  // Offset 0 is a valid value.
  EXPECT_EQ(0u, decoder.GetSectionOffset(DwarfAttr::kAddrBase));

  EXPECT_EQ(GetDie(namespace_offset()), decoder.GetReference(DwarfAttr::kType));
  EXPECT_EQ(static_cast<uint64_t>(kImplicitCount), decoder.GetUnsignedConstant(DwarfAttr::kCount));

  // The raw value has the unit and form.
  const DwarfDieDecoder::AttributeValue* value = decoder.GetValue(DwarfAttr::kLocation);
  ASSERT_TRUE(value);
  EXPECT_EQ(context()->getUnitAtIndex(0), value->unit);
  EXPECT_EQ(DwarfForm::kExprloc, value->form.form);

  // A missing attribute has no value.
  EXPECT_FALSE(decoder.Has(DwarfAttr::kHighPc));
  EXPECT_FALSE(decoder.GetValue(DwarfAttr::kHighPc));
  EXPECT_FALSE(decoder.GetHighPC());

  ASSERT_TRUE(decoder.Decode(GetDie(other_forms_offset())));
  EXPECT_EQ(ConstValue(9).GetConstValue(8),
            decoder.GetConstValue(DwarfAttr::kConstValue).GetConstValue(8));
  std::optional<DwarfDieDecoder::HighPC> high_pc = decoder.GetHighPC();
  ASSERT_TRUE(high_pc);
  EXPECT_FALSE(high_pc->is_constant);
  EXPECT_EQ(0x2000u, high_pc->value);
  std::vector<uint8_t> expected_block = {DwarfOp::kReg0 + 5};
  EXPECT_EQ(expected_block, decoder.GetBlock(DwarfAttr::kLocation));
}

TEST_F(DwarfDieDecoderTest, GetInteger) {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kConstValue, DwarfAttr::kCount,
                                         DwarfAttr::kDeclaration, DwarfAttr::kLocation};
  DwarfDieDecoder decoder(context(), kAttrs);
  ASSERT_TRUE(decoder.Decode(GetDie(forms_offset())));

  // DW_FORM_sdata and DW_FORM_implicit_const are signed in both cases.
  EXPECT_EQ(-5, decoder.GetInteger(DwarfAttr::kConstValue, false));
  EXPECT_EQ(-5, decoder.GetInteger(DwarfAttr::kConstValue, true));
  EXPECT_EQ(kImplicitCount, decoder.GetInteger(DwarfAttr::kCount, false));

  // Flags and blocks aren't integers.
  EXPECT_EQ(std::nullopt, decoder.GetInteger(DwarfAttr::kDeclaration, false));
  EXPECT_EQ(std::nullopt, decoder.GetInteger(DwarfAttr::kLocation, false));

  // DW_FORM_udata is unsigned in both cases.
  ASSERT_TRUE(decoder.Decode(GetDie(other_forms_offset())));
  EXPECT_EQ(9, decoder.GetInteger(DwarfAttr::kConstValue, true));

  // A missing attribute.
  EXPECT_EQ(std::nullopt, decoder.GetInteger(DwarfAttr::kCount, false));
}

TEST_F(DwarfDieDecoderTest, GetIntegerData) {
  // DW_AT_decl_line is DW_FORM_data1, and DW_AT_high_pc is DW_FORM_data4.
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kDeclLine, DwarfAttr::kHighPc};
  DwarfDieDecoder decoder(context(), kAttrs);
  ASSERT_TRUE(decoder.Decode(GetDie(concrete_offset())));
  EXPECT_EQ(20, decoder.GetInteger(DwarfAttr::kDeclLine, false));
  EXPECT_EQ(20, decoder.GetInteger(DwarfAttr::kDeclLine, true));
  EXPECT_EQ(0x20, decoder.GetInteger(DwarfAttr::kHighPc, true));

  // The constant of a data form is its bytes.
  static constexpr DwarfAttr kLineAttrs[] = {DwarfAttr::kDeclLine};
  DwarfDieDecoder line_decoder(context(), kLineAttrs);
  ASSERT_TRUE(line_decoder.Decode(GetDie(concrete_offset())));
  EXPECT_EQ(std::vector<uint8_t>({20}),
            line_decoder.GetConstValue(DwarfAttr::kDeclLine).GetConstValue(1));
}

TEST_F(DwarfDieDecoderTest, ConstantAccessors) {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kConstValue, DwarfAttr::kByteSize,
                                         DwarfAttr::kBitSize, DwarfAttr::kExternal};
  DwarfDieDecoder decoder(context(), kAttrs);
  ASSERT_TRUE(decoder.Decode(GetDie(constants_offset())));

  // A non-negative DW_FORM_sdata value is unsigned too.
  EXPECT_EQ(7u, decoder.GetUnsignedConstant(DwarfAttr::kConstValue));
  EXPECT_EQ(7, decoder.GetSignedConstant(DwarfAttr::kConstValue));

  // The data forms are zero-extended for unsigned values and sign-extended for signed values.
  EXPECT_EQ(255u, decoder.GetUnsignedConstant(DwarfAttr::kByteSize));
  EXPECT_EQ(-1, decoder.GetSignedConstant(DwarfAttr::kByteSize));

  // DW_FORM_data16 doesn't fit in 64 bits.
  EXPECT_EQ(std::nullopt, decoder.GetUnsignedConstant(DwarfAttr::kBitSize));
  EXPECT_EQ(std::nullopt, decoder.GetSignedConstant(DwarfAttr::kBitSize));
  EXPECT_EQ(std::nullopt, decoder.GetInteger(DwarfAttr::kBitSize, false));
  EXPECT_EQ(std::vector<uint8_t>(16, 0x11),
            decoder.GetConstValue(DwarfAttr::kBitSize).GetConstValue(16));

  // A flag isn't a constant.
  EXPECT_EQ(std::nullopt, decoder.GetUnsignedConstant(DwarfAttr::kExternal));
  EXPECT_EQ(true, decoder.GetBool(DwarfAttr::kExternal));

  // A negative DW_FORM_sdata value isn't unsigned.
  static constexpr DwarfAttr kFormsAttrs[] = {DwarfAttr::kConstValue};
  DwarfDieDecoder forms_decoder(context(), kFormsAttrs);
  ASSERT_TRUE(forms_decoder.Decode(GetDie(forms_offset())));
  EXPECT_EQ(std::nullopt, forms_decoder.GetUnsignedConstant(DwarfAttr::kConstValue));
  EXPECT_EQ(-5, forms_decoder.GetSignedConstant(DwarfAttr::kConstValue));
}

TEST_F(DwarfDieDecoderTest, AddressTableUnit) {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kLowPc, DwarfAttr::kHighPc, DwarfAttr::kEntryPc,
                                         DwarfAttr::kCallReturnPc};
  llvm::DWARFDie die = GetDieInUnit(kDwoUnit, addresses_offset());
  ASSERT_TRUE(die);

  // The unit of the DIE has no address table, like a .dwo unit.
  DwarfDieDecoder own_table(context(), kAttrs);
  ASSERT_TRUE(own_table.Decode(die));
  EXPECT_EQ(std::nullopt, own_table.GetAddress(DwarfAttr::kLowPc));

  // With the address table of the skeleton unit, the indices resolve.
  DwarfDieDecoder decoder(context(), kAttrs, GetUnit(kSkeletonUnit));
  ASSERT_TRUE(decoder.Decode(die));
  EXPECT_EQ(0x6000u, decoder.GetAddress(DwarfAttr::kLowPc));

  // DW_FORM_LLVM_addrx_offset is an entry plus an offset.
  EXPECT_EQ(0x5010u, decoder.GetAddress(DwarfAttr::kHighPc));
  std::optional<DwarfDieDecoder::HighPC> high_pc = decoder.GetHighPC();
  ASSERT_TRUE(high_pc);
  EXPECT_FALSE(high_pc->is_constant);
  EXPECT_EQ(0x5010u, high_pc->value);

  // The tombstone value means removed code, in the table and in DW_FORM_addr.
  EXPECT_EQ(std::nullopt, decoder.GetAddress(DwarfAttr::kEntryPc));
  EXPECT_TRUE(decoder.Has(DwarfAttr::kCallReturnPc));
  EXPECT_EQ(std::nullopt, decoder.GetAddress(DwarfAttr::kCallReturnPc));
}

TEST_F(DwarfDieDecoderTest, CorruptBlockLength) {
  llvm::DWARFDie die = GetDieInUnit(kCorruptUnit, corrupt_offset());
  ASSERT_TRUE(die);

  // The block ends past the end of the unit, when the decoder reads it and when it skips it.
  static constexpr DwarfAttr kReadAttrs[] = {DwarfAttr::kName, DwarfAttr::kLocation};
  DwarfDieDecoder read_decoder(context(), kReadAttrs);
  EXPECT_FALSE(read_decoder.Decode(die));

  static constexpr DwarfAttr kSkipAttrs[] = {DwarfAttr::kName};
  DwarfDieDecoder skip_decoder(context(), kSkipAttrs);
  EXPECT_FALSE(skip_decoder.Decode(die));

  // The unit DIE before it is fine.
  EXPECT_TRUE(skip_decoder.Decode(GetUnit(kCorruptUnit)->getUnitDIE()));
  EXPECT_STREQ("corrupt", skip_decoder.GetCString(DwarfAttr::kName).value_or(""));
}

TEST_F(DwarfDieDecoderTest, WrongForm) {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kDeclaration, DwarfAttr::kAddrBase,
                                         DwarfAttr::kLocation};
  DwarfDieDecoder decoder(context(), kAttrs);
  ASSERT_TRUE(decoder.Decode(GetDie(forms_offset())));

  // The attributes are present but can't be converted.
  EXPECT_TRUE(decoder.Has(DwarfAttr::kDeclaration));
  EXPECT_EQ(std::nullopt, decoder.GetCString(DwarfAttr::kDeclaration));
  EXPECT_EQ(std::nullopt, decoder.GetBlock(DwarfAttr::kDeclaration));
  EXPECT_FALSE(decoder.GetReference(DwarfAttr::kDeclaration).isValid());
  EXPECT_FALSE(decoder.GetConstValue(DwarfAttr::kAddrBase).has_value());
  EXPECT_EQ(std::nullopt, decoder.GetAddress(DwarfAttr::kLocation));
}

TEST_F(DwarfDieDecoderTest, MissingAttributes) {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName, DwarfAttr::kType, DwarfAttr::kConstValue,
                                         DwarfAttr::kDeclFile};
  DwarfDieDecoder decoder(context(), kAttrs);
  ASSERT_TRUE(decoder.Decode(GetDie(namespace_offset())));

  EXPECT_FALSE(decoder.Has(DwarfAttr::kType));
  EXPECT_EQ(std::nullopt, decoder.GetUnsignedConstant(DwarfAttr::kType));
  EXPECT_FALSE(decoder.GetReference(DwarfAttr::kType).isValid());
  EXPECT_FALSE(decoder.GetConstValue(DwarfAttr::kConstValue).has_value());
  EXPECT_EQ(std::nullopt, decoder.GetFile(DwarfAttr::kDeclFile));
}

TEST_F(DwarfDieDecoderTest, Reuse) {
  // Decoding a second DIE clears the values of the first.
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName, DwarfAttr::kType};
  DwarfDieDecoder decoder(context(), kAttrs);

  ASSERT_TRUE(decoder.Decode(GetDie(origin_offset())));
  EXPECT_TRUE(decoder.Has(DwarfAttr::kName));
  EXPECT_FALSE(decoder.Has(DwarfAttr::kType));

  ASSERT_TRUE(decoder.Decode(GetDie(forms_offset())));
  EXPECT_FALSE(decoder.Has(DwarfAttr::kName));
  EXPECT_TRUE(decoder.Has(DwarfAttr::kType));
}

TEST_F(DwarfDieDecoderTest, AbstractOriginCycle) {
  // A DIE that is its own abstract origin stops after the maximum number of references.
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName, DwarfAttr::kAbstractOrigin};
  DwarfDieDecoder decoder(context(), kAttrs);
  ASSERT_TRUE(decoder.Decode(GetDie(cycle_offset())));
  EXPECT_STREQ("cycle", decoder.GetCString(DwarfAttr::kName).value_or(""));
  EXPECT_EQ(GetDie(cycle_offset()), decoder.GetReference(DwarfAttr::kAbstractOrigin));
  EXPECT_EQ(GetDie(unit_offset()), decoder.GetContextParent());
}

TEST_F(DwarfDieDecoderTest, DuplicateAttribute) {
  // The first of two duplicate attributes gives the value.
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName};
  DwarfDieDecoder decoder(context(), kAttrs);
  ASSERT_TRUE(decoder.Decode(GetDie(duplicate_offset())));
  EXPECT_STREQ("first", decoder.GetCString(DwarfAttr::kName).value_or(""));
}

TEST_F(DwarfDieDecoderTest, EmptyTable) {
  // A decoder with no attributes still checks the DIE and finds the abstract parent.
  DwarfDieDecoder decoder(context(), {});
  ASSERT_TRUE(decoder.Decode(GetDie(concrete_offset())));
  EXPECT_EQ(GetDie(namespace_offset()), decoder.GetContextParent());
}

}  // namespace zxdb
