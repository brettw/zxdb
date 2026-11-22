// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"

#include <gtest/gtest.h>

#include "llvm/ADT/StringMap.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/Support/MemoryBuffer.h"
#include "src/developer/debug/zxdb/symbols/dwarf_op.h"
#include "src/developer/debug/zxdb/symbols/line_program_test_util.h"

namespace zxdb {

namespace {

using namespace llvm::dwarf;

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
};

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
class DwarfDieDecoderTest : public testing::Test {
 public:
  DwarfDieDecoderTest() {
    WriteAbbrevs();
    WriteInfo();

    buffers_["debug_info"] = MakeBuffer(info_, "debug_info");
    buffers_["debug_abbrev"] = MakeBuffer(abbrev_, "debug_abbrev");
    context_ = llvm::DWARFContext::create(buffers_, 8, true);
  }

  llvm::DWARFContext* context() { return context_.get(); }

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

  void AddAbbrev(uint8_t code, Tag tag, bool has_children,
                 const std::vector<std::pair<DwarfAttr, Form>>& specs,
                 std::optional<int64_t> implicit_const = std::nullopt) {
    AppendUleb(abbrev_, code);
    AppendUleb(abbrev_, tag);
    abbrev_.push_back(has_children ? 1 : 0);
    for (const auto& [attr, form] : specs) {
      AppendUleb(abbrev_, static_cast<uint64_t>(attr));
      AppendUleb(abbrev_, form);
      if (form == DW_FORM_implicit_const) {
        // The test values are small positive numbers, so the SLEB128 encoding is one byte.
        FX_CHECK(implicit_const && *implicit_const >= 0 && *implicit_const < 64);
        abbrev_.push_back(static_cast<uint8_t>(*implicit_const));
      }
    }
    abbrev_.push_back(0);
    abbrev_.push_back(0);
  }

  void WriteAbbrevs() {
    AddAbbrev(kCompileUnitAbbrev, DW_TAG_compile_unit, true, {{DwarfAttr::kName, DW_FORM_string}});
    AddAbbrev(kNamespaceAbbrev, DW_TAG_namespace, true, {{DwarfAttr::kName, DW_FORM_string}});
    AddAbbrev(kOriginAbbrev, DW_TAG_subprogram, false,
              {{DwarfAttr::kName, DW_FORM_string},
               {DwarfAttr::kDeclLine, DW_FORM_data1},
               {DwarfAttr::kExternal, DW_FORM_flag_present},
               {DwarfAttr::kByteSize, DW_FORM_implicit_const}},
              kImplicitByteSize);
    AddAbbrev(kConcreteAbbrev, DW_TAG_subprogram, false,
              {{DwarfAttr::kAbstractOrigin, DW_FORM_ref4},
               {DwarfAttr::kDeclLine, DW_FORM_data1},
               {DwarfAttr::kLowPc, DW_FORM_addr},
               {DwarfAttr::kHighPc, DW_FORM_data4}});
    AddAbbrev(kFormsAbbrev, DW_TAG_variable, false,
              {{DwarfAttr::kDeclaration, DW_FORM_flag},
               {DwarfAttr::kArtificial, DW_FORM_flag},
               {DwarfAttr::kConstValue, DW_FORM_sdata},
               {DwarfAttr::kLocation, DW_FORM_exprloc},
               {DwarfAttr::kAddrBase, DW_FORM_sec_offset},
               {DwarfAttr::kType, DW_FORM_ref4},
               {DwarfAttr::kCount, DW_FORM_implicit_const}},
              kImplicitCount);
    AddAbbrev(kOtherFormsAbbrev, DW_TAG_variable, false,
              {{DwarfAttr::kConstValue, DW_FORM_udata},
               {DwarfAttr::kHighPc, DW_FORM_addr},
               {DwarfAttr::kLocation, DW_FORM_block1}});
    AddAbbrev(kCycleAbbrev, DW_TAG_variable, false,
              {{DwarfAttr::kAbstractOrigin, DW_FORM_ref4}, {DwarfAttr::kName, DW_FORM_string}});
    AddAbbrev(kDuplicateAbbrev, DW_TAG_variable, false,
              {{DwarfAttr::kName, DW_FORM_string}, {DwarfAttr::kName, DW_FORM_string}});
    abbrev_.push_back(0);  // End of the abbreviations.
  }

  // Returns the offset that the next DIE will have.
  uint64_t NextOffset() const { return info_.size(); }

  void WriteInfo() {
    AppendInteger(info_, 0, 4);  // Unit length, set below.
    AppendInteger(info_, 5, 2);  // Version.
    info_.push_back(DW_UT_compile);
    info_.push_back(8);          // Address size.
    AppendInteger(info_, 0, 4);  // Abbreviation offset.

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
    AppendInteger(info_, origin_offset_, 4);
    info_.push_back(20);              // DW_AT_decl_line overrides the origin.
    AppendInteger(info_, 0x1000, 8);  // DW_AT_low_pc.
    AppendInteger(info_, 0x20, 4);    // DW_AT_high_pc.

    forms_offset_ = NextOffset();
    AppendUleb(info_, kFormsAbbrev);
    info_.push_back(0);     // DW_AT_declaration is false.
    info_.push_back(1);     // DW_AT_artificial is true.
    info_.push_back(0x7b);  // DW_AT_const_value is -5 in SLEB128.
    AppendUleb(info_, 2);   // DW_AT_location length.
    info_.push_back(DwarfOp::kReg0 + 3);
    info_.push_back(DwarfOp::kStackValue);
    AppendInteger(info_, 0, 4);                  // DW_AT_addr_base is offset 0, which is valid.
    AppendInteger(info_, namespace_offset_, 4);  // DW_AT_type.

    other_forms_offset_ = NextOffset();
    AppendUleb(info_, kOtherFormsAbbrev);
    AppendUleb(info_, 9);             // DW_AT_const_value.
    AppendInteger(info_, 0x2000, 8);  // DW_AT_high_pc.
    info_.push_back(1);               // DW_AT_location length.
    info_.push_back(DwarfOp::kReg0 + 5);

    cycle_offset_ = NextOffset();
    AppendUleb(info_, kCycleAbbrev);
    AppendInteger(info_, cycle_offset_, 4);
    AppendString(info_, "cycle");

    duplicate_offset_ = NextOffset();
    AppendUleb(info_, kDuplicateAbbrev);
    AppendString(info_, "first");
    AppendString(info_, "second");

    info_.push_back(0);  // End of the unit children.

    uint64_t unit_length = info_.size() - 4;
    for (size_t i = 0; i < 4; i++)
      info_[i] = static_cast<uint8_t>(unit_length >> (i * 8));
  }

  std::vector<uint8_t> abbrev_;
  std::vector<uint8_t> info_;

  uint64_t unit_offset_ = 0;
  uint64_t namespace_offset_ = 0;
  uint64_t origin_offset_ = 0;
  uint64_t concrete_offset_ = 0;
  uint64_t forms_offset_ = 0;
  uint64_t other_forms_offset_ = 0;
  uint64_t cycle_offset_ = 0;
  uint64_t duplicate_offset_ = 0;

  // The context references these buffers, so they must outlive it.
  llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> buffers_;
  std::unique_ptr<llvm::DWARFContext> context_;
};

}  // namespace

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
  EXPECT_EQ(GetDie(namespace_offset()), decoder.GetAbstractParent());

  // Without an abstract origin, the abstract parent is the regular parent.
  ASSERT_TRUE(decoder.Decode(GetDie(origin_offset())));
  EXPECT_FALSE(decoder.Has(DwarfAttr::kAbstractOrigin));
  EXPECT_EQ(10u, decoder.GetUnsignedConstant(DwarfAttr::kDeclLine));
  EXPECT_EQ(GetDie(namespace_offset()), decoder.GetAbstractParent());

  // The unit DIE has no parent.
  ASSERT_TRUE(decoder.Decode(GetDie(unit_offset())));
  EXPECT_STREQ("unit", decoder.GetCString(DwarfAttr::kName).value_or(""));
  EXPECT_FALSE(decoder.GetAbstractParent().isValid());
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
  EXPECT_EQ(DW_FORM_exprloc, value->form.getForm());

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
  EXPECT_EQ(GetDie(unit_offset()), decoder.GetAbstractParent());
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
  EXPECT_EQ(GetDie(namespace_offset()), decoder.GetAbstractParent());
}

}  // namespace zxdb
