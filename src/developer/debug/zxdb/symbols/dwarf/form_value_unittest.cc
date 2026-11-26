// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/form_value.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_header.h"

namespace zxdb {

namespace {

using Class = FormValue::Class;
using Spec = AbbrevTable::AttributeSpec;

constexpr uint8_t kBlock[] = {0x12, 0x80, 0x00};
constexpr uint8_t kData16[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                               0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
constexpr FormParams kDwarf5{.version = 5, .offset_size = 4, .address_size = 8};

Spec Attribute(DwarfForm form, std::optional<int64_t> implicit_const = std::nullopt) {
  return {0x03, static_cast<uint64_t>(form), implicit_const};
}

struct FormCase {
  FormParams params;
  Endian endian;
  Spec spec;
  std::vector<uint8_t> bytes;
  FormValue expected;
};

void AddFormCases(std::vector<FormCase>& cases, const FormParams& params, Endian endian) {
  auto add = [&](DwarfForm form, Class form_class, FormValue::Value value,
                 std::vector<uint8_t> bytes, std::optional<int64_t> implicit_const = std::nullopt) {
    cases.push_back({params,
                     endian,
                     Attribute(form, implicit_const),
                     std::move(bytes),
                     {form, form_class, value}});
  };
  auto fixed = [&](DwarfForm form, Class form_class, uint64_t value, size_t width) {
    std::vector<uint8_t> bytes;
    DwarfSectionBuilder(bytes, endian).Fixed(value, width);
    add(form, form_class, value, std::move(bytes));
  };
  auto fixed_block = [&](DwarfForm form, size_t width) {
    std::vector<uint8_t> bytes;
    DwarfSectionBuilder writer(bytes, endian);
    writer.Fixed(std::size(kBlock), width);
    writer.Bytes(kBlock);
    add(form, Class::kBlock, std::span<const uint8_t>(kBlock), std::move(bytes));
  };

  // DWARF 2 through 4 section 7.5.4 and DWARF 5 section 7.5.5 specify these encodings.
  // Use different address and offset sizes to distinguish the two DW_FORM_ref_addr layouts.
  const uint64_t address = params.address_size == 8 ? 0x123456789abcdef0 : 0x76543210;
  const uint64_t offset = params.offset_size == 8 ? 0xfedcba9876543210 : 0x89abcdef;
  fixed(DwarfForm::kAddr, Class::kAddress, address, params.address_size);
  if (params.version == 2)
    fixed(DwarfForm::kRefAddr, Class::kReference, address, params.address_size);
  else
    fixed(DwarfForm::kRefAddr, Class::kReference, offset, params.offset_size);
  fixed(DwarfForm::kStrp, Class::kString, offset, params.offset_size);
  fixed(DwarfForm::kData1, Class::kConstant, 0x81, 1);
  fixed(DwarfForm::kData2, Class::kConstant, 0x8123, 2);
  fixed(DwarfForm::kData4, Class::kConstant, 0x81234567, 4);
  fixed(DwarfForm::kData8, Class::kConstant, 0x8123456789abcdef, 8);
  fixed(DwarfForm::kRef1, Class::kReference, 0x91, 1);
  fixed(DwarfForm::kRef2, Class::kReference, 0x9123, 2);
  fixed(DwarfForm::kRef4, Class::kReference, 0x91234567, 4);
  fixed(DwarfForm::kRef8, Class::kReference, 0x9123456789abcdef, 8);
  add(DwarfForm::kString, Class::kString, std::string_view("foo"), {'f', 'o', 'o', 0});
  add(DwarfForm::kFlag, Class::kFlag, uint64_t{1}, {0x01});
  add(DwarfForm::kUdata, Class::kConstant, uint64_t{12857}, {0xb9, 0x64});
  add(DwarfForm::kSdata, Class::kConstant, int64_t{-129}, {0xff, 0x7e});
  add(DwarfForm::kRefUdata, Class::kReference, uint64_t{129}, {0x81, 0x01});
  fixed_block(DwarfForm::kBlock1, 1);
  fixed_block(DwarfForm::kBlock2, 2);
  fixed_block(DwarfForm::kBlock4, 4);
  add(DwarfForm::kBlock, Class::kBlock, std::span<const uint8_t>(kBlock), {3, 0x12, 0x80, 0x00});
  cases.push_back({params,
                   endian,
                   Attribute(DwarfForm::kIndirect),
                   {0x16, 0x0b, 0x81},
                   {DwarfForm::kData1, Class::kConstant, uint64_t{0x81}}});

  if (params.version >= 4) {
    fixed(DwarfForm::kSecOffset, Class::kSectionOffset, offset, params.offset_size);
    fixed(DwarfForm::kRefSig8, Class::kReference, 0xa123456789abcdef, 8);
    add(DwarfForm::kExprloc, Class::kExprloc, std::span<const uint8_t>(kBlock),
        {3, 0x12, 0x80, 0x00});
    add(DwarfForm::kFlagPresent, Class::kFlag, uint64_t{1}, {});
  }

  if (params.version >= 5) {
    add(DwarfForm::kData16, Class::kConstant, std::span<const uint8_t>(kData16),
        {std::begin(kData16), std::end(kData16)});
    add(DwarfForm::kImplicitConst, Class::kConstant, int64_t{-128}, {}, -128);
    fixed(DwarfForm::kLineStrp, Class::kString, offset, params.offset_size);
    fixed(DwarfForm::kStrpSup, Class::kString, offset, params.offset_size);
    fixed(DwarfForm::kRefSup4, Class::kReference, 0xb1234567, 4);
    fixed(DwarfForm::kRefSup8, Class::kReference, 0xb123456789abcdef, 8);
    add(DwarfForm::kLoclistx, Class::kLoclist, uint64_t{128}, {0x80, 0x01});
    add(DwarfForm::kRnglistx, Class::kRnglist, uint64_t{129}, {0x81, 0x01});
    add(DwarfForm::kStrx, Class::kString, uint64_t{130}, {0x82, 0x01});
    fixed(DwarfForm::kStrx1, Class::kString, 0xa1, 1);
    fixed(DwarfForm::kStrx2, Class::kString, 0xa123, 2);
    fixed(DwarfForm::kStrx3, Class::kString, 0xa12345, 3);
    fixed(DwarfForm::kStrx4, Class::kString, 0xa1234567, 4);
    add(DwarfForm::kAddrx, Class::kAddress, uint64_t{131}, {0x83, 0x01});
    fixed(DwarfForm::kAddrx1, Class::kAddress, 0xb1, 1);
    fixed(DwarfForm::kAddrx2, Class::kAddress, 0xb123, 2);
    fixed(DwarfForm::kAddrx3, Class::kAddress, 0xb12345, 3);
    fixed(DwarfForm::kAddrx4, Class::kAddress, 0xb1234567, 4);
  }

  // The GNU split DWARF and alternate object extensions specify these encodings.
  add(DwarfForm::kGnuAddrIndex, Class::kAddress, uint64_t{132}, {0x84, 0x01});
  add(DwarfForm::kGnuStrIndex, Class::kString, uint64_t{133}, {0x85, 0x01});
  fixed(DwarfForm::kGnuRefAlt, Class::kReference, offset, params.offset_size);
  fixed(DwarfForm::kGnuStrpAlt, Class::kString, offset, params.offset_size);

  // The LLVM extension stores a ULEB128 address index followed by a four-byte offset.
  std::vector<uint8_t> indexed_address = {0x80, 0x80, 0x80, 0x80, 0x10};  // 2^32.
  DwarfSectionBuilder(indexed_address, endian).Fixed(0xc1234567, 4);
  add(DwarfForm::kLlvmAddrxOffset, Class::kAddress,
      FormValue::IndexedAddress{0x100000000, 0xc1234567}, std::move(indexed_address));
}

std::vector<FormCase> AllFormCases() {
  std::vector<FormCase> cases;
  for (uint16_t version : {2, 3, 4, 5}) {
    for (const FormParams params : {FormParams{version, 4, 8}, FormParams{version, 8, 4}}) {
      for (Endian endian : {Endian::kLittle, Endian::kBig})
        AddFormCases(cases, params, endian);
    }
  }
  return cases;
}

void ExpectForm(const FormValue& expected, const FormValue& actual) {
  EXPECT_EQ(expected.form, actual.form);
  EXPECT_EQ(expected.form_class, actual.form_class);
  std::visit(
      [&actual](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        const auto* decoded = std::get_if<T>(&actual.value);
        ASSERT_NE(nullptr, decoded);
        if constexpr (std::is_same_v<T, std::span<const uint8_t>>) {
          EXPECT_TRUE(std::ranges::equal(value, *decoded));
        } else if constexpr (std::is_same_v<T, FormValue::IndexedAddress>) {
          EXPECT_EQ(value.index, decoded->index);
          EXPECT_EQ(value.offset, decoded->offset);
        } else {
          EXPECT_EQ(value, *decoded);
        }
      },
      expected.value);
}

void ExpectMalformed(const Spec& spec, const FormParams& params, DataReader reader) {
  DataReader skip = reader;
  EXPECT_FALSE(SkipForm(spec.form, params, skip));
  const auto read = ReadForm(spec, params, reader);
  EXPECT_TRUE(read.has_error());
  EXPECT_EQ(skip.offset(), reader.offset());
  EXPECT_EQ(skip.ok(), reader.ok());
}

class FormEncodingTest : public testing::TestWithParam<FormCase> {};

TEST_P(FormEncodingTest, ReadAndSkip) {
  const FormCase& form = GetParam();
  std::vector<uint8_t> bytes = {0xfa};
  bytes.insert(bytes.end(), form.bytes.begin(), form.bytes.end());
  bytes.push_back(0xfb);
  DataReader reader(bytes, form.endian);
  reader.Seek(1);
  DataReader skip = reader;

  auto read = ReadForm(form.spec, form.params, reader);
  ASSERT_TRUE(read.ok()) << read.err_or_empty().msg();
  ASSERT_TRUE(SkipForm(form.spec.form, form.params, skip));
  ExpectForm(form.expected, read.value());
  EXPECT_EQ(1 + form.bytes.size(), reader.offset());
  EXPECT_EQ(reader.offset(), skip.offset());
  EXPECT_EQ(0xfbu, reader.Fixed(1));
  EXPECT_EQ(0xfbu, skip.Fixed(1));
}

TEST_P(FormEncodingTest, Truncation) {
  const FormCase& form = GetParam();
  DataReader input(form.bytes, form.endian);
  if (form.bytes.empty()) {
    // The two zero-byte forms can be read and skipped even at the end of the unit.
    EXPECT_TRUE(SkipForm(form.spec.form, form.params, input));
    EXPECT_TRUE(ReadForm(form.spec, form.params, input).ok());
    EXPECT_EQ(0u, input.offset());
  }
  for (size_t limit = 0; limit < form.bytes.size(); ++limit) {
    SCOPED_TRACE(limit);
    DataReader truncated = input;
    truncated.set_limit(limit);
    ExpectMalformed(form.spec, form.params, truncated);
  }
}

INSTANTIATE_TEST_SUITE_P(Dwarf2To5, FormEncodingTest, testing::ValuesIn(AllFormCases()),
                         [](const testing::TestParamInfo<FormCase>& info) {
                           const auto& form = info.param;
                           return DwarfFormToString(static_cast<DwarfForm>(form.spec.form)) + "_V" +
                                  std::to_string(form.params.version) +
                                  (form.params.offset_size == 4 ? "_32" : "_64") +
                                  (form.endian == Endian::kLittle ? "_LE" : "_BE");
                         });

TEST(FormValue, BlocksPastTheLimit) {
  // An unchecked addition of UINT64_MAX to the cursor would wrap in both ULEB128 cases.
  for (DwarfForm form : {DwarfForm::kBlock, DwarfForm::kExprloc}) {
    constexpr uint8_t kHugeBlock[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                      0xff, 0xff, 0xff, 0x01, 0x12};
    ExpectMalformed(Attribute(form), kDwarf5, DataReader(kHugeBlock, Endian::kLittle));
  }
  constexpr uint8_t kBlock1[] = {5, 0x12};
  constexpr uint8_t kBlock2[] = {0x00, 0x05, 0x12};
  constexpr uint8_t kBlock4[] = {0x05, 0x00, 0x00, 0x00, 0x12};
  ExpectMalformed(Attribute(DwarfForm::kBlock1), kDwarf5, DataReader(kBlock1, Endian::kLittle));
  ExpectMalformed(Attribute(DwarfForm::kBlock2), kDwarf5, DataReader(kBlock2, Endian::kBig));
  ExpectMalformed(Attribute(DwarfForm::kBlock4), kDwarf5, DataReader(kBlock4, Endian::kLittle));
}

TEST(FormValue, UnknownForms) {
  constexpr uint64_t kUnknown[] = {0, 2, 0x2d, 0x2002, 0x10001, 0x100000001, UINT64_MAX};
  for (uint64_t form : kUnknown) {
    SCOPED_TRACE(form);
    std::vector<uint8_t> bytes;
    DwarfSectionBuilder(bytes, Endian::kLittle).Uleb(form);
    ExpectMalformed(Spec{0x03, form, {}}, kDwarf5, DataReader(bytes, Endian::kLittle));
    ExpectMalformed(Attribute(DwarfForm::kIndirect), kDwarf5, DataReader(bytes, Endian::kLittle));
  }
}

TEST(FormValue, IndirectDepthLimit) {
  constexpr unsigned kDepthLimit = 8;
  for (unsigned depth : {kDepthLimit, kDepthLimit + 1}) {
    SCOPED_TRACE(depth);
    // The abbreviation supplies the first indirect form, so the DIE has one fewer.
    std::vector<uint8_t> bytes(depth - 1, 0x16);
    bytes.push_back(0x19);  // DW_FORM_flag_present.
    DataReader reader(bytes, Endian::kLittle);
    if (depth > kDepthLimit) {
      ExpectMalformed(Attribute(DwarfForm::kIndirect), kDwarf5, reader);
    } else {
      EXPECT_TRUE(SkipForm(DwarfForm::kIndirect, kDwarf5, reader));
      EXPECT_EQ(bytes.size(), reader.offset());
      reader.Seek(0);
      auto read = ReadForm(Attribute(DwarfForm::kIndirect), kDwarf5, reader);
      ASSERT_TRUE(read.ok()) << read.err_or_empty().msg();
      ExpectForm({DwarfForm::kFlagPresent, Class::kFlag, uint64_t{1}}, read.value());
      EXPECT_EQ(bytes.size(), reader.offset());
    }
  }
}

TEST(FormValue, IndirectImplicitConst) {
  // DWARF 5 errata section 7.5.3 stores the SLEB128 after the indirect form code.
  constexpr uint8_t kBytes[] = {0x16, 0x21, 0xff, 0x7e, 0xfb};
  DataReader reader(kBytes, Endian::kLittle);
  const auto spec = Attribute(DwarfForm::kIndirect);
  auto read = ReadForm(spec, kDwarf5, reader);
  ASSERT_TRUE(read.ok()) << read.err_or_empty().msg();
  ExpectForm({DwarfForm::kImplicitConst, Class::kConstant, int64_t{-129}}, read.value());
  EXPECT_EQ(4u, reader.offset());
  reader.Seek(0);
  EXPECT_TRUE(SkipForm(spec.form, kDwarf5, reader));
  EXPECT_EQ(4u, reader.offset());

  for (size_t limit : {2, 3})
    ExpectMalformed(spec, kDwarf5, DataReader(std::span(kBytes).first(limit), Endian::kLittle));

  DataReader empty({}, Endian::kLittle);
  EXPECT_TRUE(ReadForm(Attribute(DwarfForm::kImplicitConst), kDwarf5, empty).has_error());
  auto zero = ReadForm(Attribute(DwarfForm::kImplicitConst, 0), kDwarf5, empty);
  ASSERT_TRUE(zero.ok()) << zero.err_or_empty().msg();
  ExpectForm({DwarfForm::kImplicitConst, Class::kConstant, int64_t{0}}, zero.value());
  EXPECT_EQ(0u, empty.offset());
}

TEST(FormValue, ZeroValues) {
  constexpr uint8_t kZero[8] = {};
  for (const FormValue& expected : {
           FormValue{DwarfForm::kRefAddr, Class::kReference, uint64_t{0}},
           FormValue{DwarfForm::kRef1, Class::kReference, uint64_t{0}},
           FormValue{DwarfForm::kRefSig8, Class::kReference, uint64_t{0}},
           FormValue{DwarfForm::kSecOffset, Class::kSectionOffset, uint64_t{0}},
           FormValue{DwarfForm::kStrx, Class::kString, uint64_t{0}},
           FormValue{DwarfForm::kAddr, Class::kAddress, uint64_t{0}},
           FormValue{DwarfForm::kFlag, Class::kFlag, uint64_t{0}},
           FormValue{DwarfForm::kString, Class::kString, std::string_view{}},
           FormValue{DwarfForm::kBlock, Class::kBlock, std::span<const uint8_t>{}},
       }) {
    SCOPED_TRACE(DwarfFormToString(expected.form));
    DataReader reader(kZero, Endian::kLittle);
    auto read = ReadForm(Attribute(expected.form), kDwarf5, reader);
    ASSERT_TRUE(read.ok()) << read.err_or_empty().msg();
    ExpectForm(expected, read.value());
    DataReader skip(kZero, Endian::kLittle);
    EXPECT_TRUE(SkipForm(expected.form, kDwarf5, skip));
    EXPECT_EQ(reader.offset(), skip.offset());
  }
}

TEST(FormValue, KnownEncodingInOlderVersion) {
  constexpr uint8_t kIndex[] = {0x12, 0x34, 0x56};
  constexpr FormParams kDwarf4{.version = 4, .offset_size = 4, .address_size = 8};
  DataReader reader(kIndex, Endian::kBig);
  auto read = ReadForm(Attribute(DwarfForm::kStrx3), kDwarf4, reader);
  ASSERT_TRUE(read.ok()) << read.err_or_empty().msg();
  ExpectForm({DwarfForm::kStrx3, Class::kString, uint64_t{0x123456}}, read.value());
  reader.Seek(0);
  EXPECT_TRUE(SkipForm(DwarfForm::kStrx3, kDwarf4, reader));
  EXPECT_EQ(3u, reader.offset());
}

TEST(FormValue, AsInteger) {
  const FormValue data1{DwarfForm::kData1, Class::kConstant, uint64_t{0x80}};
  const FormValue data2{DwarfForm::kData2, Class::kConstant, uint64_t{0x8000}};
  const FormValue data4{DwarfForm::kData4, Class::kConstant, uint64_t{0x80000000}};
  const FormValue data8{DwarfForm::kData8, Class::kConstant, uint64_t{0x8000000000000000}};
  EXPECT_EQ(int128_t{-128}, data1.AsInteger(true));
  EXPECT_EQ(int128_t{-32768}, data2.AsInteger(true));
  EXPECT_EQ(int128_t{-2147483648}, data4.AsInteger(true));
  EXPECT_EQ(int128_t{std::numeric_limits<int64_t>::min()}, data8.AsInteger(true));
  EXPECT_EQ(int128_t{0x8000000000000000}, data8.AsInteger(false));

  const FormValue sdata{DwarfForm::kSdata, Class::kConstant, int64_t{-5}};
  const FormValue implicit{DwarfForm::kImplicitConst, Class::kConstant, int64_t{-7}};
  const FormValue udata{DwarfForm::kUdata, Class::kConstant, std::numeric_limits<uint64_t>::max()};
  EXPECT_EQ(int128_t{-5}, sdata.AsInteger(false));
  EXPECT_EQ(int128_t{-7}, implicit.AsInteger(false));
  EXPECT_EQ(int128_t{std::numeric_limits<uint64_t>::max()}, udata.AsInteger(true));

  const FormValue flag{DwarfForm::kFlag, Class::kFlag, uint64_t{1}};
  const FormValue data16{DwarfForm::kData16, Class::kConstant, std::span(kData16)};
  EXPECT_FALSE(flag.AsInteger(false));
  EXPECT_FALSE(data16.AsInteger(true));
}

TEST(FormValue, AsUnsignedConstant) {
  const FormValue implicit{DwarfForm::kImplicitConst, Class::kConstant, int64_t{-1}};
  const FormValue sdata{DwarfForm::kSdata, Class::kConstant, int64_t{1}};
  const FormValue data16{DwarfForm::kData16, Class::kConstant, std::span(kData16)};
  const FormValue ref{DwarfForm::kRef1, Class::kReference, uint64_t{1}};
  EXPECT_EQ(std::numeric_limits<uint64_t>::max(), implicit.AsUnsignedConstant());
  EXPECT_FALSE(sdata.AsUnsignedConstant());
  EXPECT_EQ(16u, data16.AsUnsignedConstant());
  EXPECT_FALSE(ref.AsUnsignedConstant());

  constexpr uint8_t kFlag[] = {0x80};
  DataReader reader(kFlag, Endian::kLittle);
  auto flag = ReadForm(Attribute(DwarfForm::kFlag), kDwarf5, reader);
  ASSERT_TRUE(flag.ok()) << flag.err_or_empty().msg();
  EXPECT_EQ(0x80u, flag.value().AsUnsignedConstant());
}

TEST(FormValue, AsSectionOffset) {
  for (DwarfForm form : {DwarfForm::kData4, DwarfForm::kData8}) {
    SCOPED_TRACE(DwarfFormToString(form));
    FormValue value{form, Class::kConstant, uint64_t{0}};
    EXPECT_EQ(0u, value.AsSectionOffset(2));
    EXPECT_EQ(0u, value.AsSectionOffset(3));
    EXPECT_FALSE(value.AsSectionOffset(4));
    EXPECT_FALSE(value.AsSectionOffset(5));
  }

  const FormValue sec_offset{DwarfForm::kSecOffset, Class::kSectionOffset, uint64_t{0}};
  const FormValue strp{DwarfForm::kStrp, Class::kString, uint64_t{1}};
  const FormValue line_strp{DwarfForm::kLineStrp, Class::kString, uint64_t{2}};
  const FormValue loclistx{DwarfForm::kLoclistx, Class::kLoclist, uint64_t{3}};
  const FormValue rnglistx{DwarfForm::kRnglistx, Class::kRnglist, uint64_t{4}};
  const FormValue strx{DwarfForm::kStrx, Class::kString, uint64_t{5}};
  EXPECT_EQ(0u, sec_offset.AsSectionOffset(5));
  EXPECT_EQ(1u, strp.AsSectionOffset(5));
  EXPECT_EQ(2u, line_strp.AsSectionOffset(5));
  EXPECT_EQ(3u, loclistx.AsSectionOffset(5));
  EXPECT_EQ(4u, rnglistx.AsSectionOffset(5));
  EXPECT_FALSE(strx.AsSectionOffset(5));
}

TEST(FormValue, AsReference) {
  const FormValue relative{DwarfForm::kRef4, Class::kReference, uint64_t{1}};
  const FormValue absolute{DwarfForm::kRefAddr, Class::kReference, uint64_t{0}};
  const FormValue signature{DwarfForm::kRefSig8, Class::kReference, uint64_t{0}};
  const FormValue offset{DwarfForm::kSecOffset, Class::kSectionOffset, uint64_t{1}};
  EXPECT_EQ(1u, relative.AsReference());
  EXPECT_EQ(0u, absolute.AsReference());
  EXPECT_EQ(0u, signature.AsReference());
  EXPECT_FALSE(offset.AsReference());
}

TEST(FormValue, AsBlock) {
  const FormValue block{DwarfForm::kBlock1, Class::kBlock, std::span(kBlock)};
  const FormValue exprloc{DwarfForm::kExprloc, Class::kExprloc, std::span<const uint8_t>{}};
  const FormValue data16{DwarfForm::kData16, Class::kConstant, std::span(kData16)};
  const FormValue data8{DwarfForm::kData8, Class::kConstant, uint64_t{0}};
  ASSERT_TRUE(block.AsBlock());
  EXPECT_TRUE(std::ranges::equal(kBlock, *block.AsBlock()));
  ASSERT_TRUE(exprloc.AsBlock());
  EXPECT_TRUE(exprloc.AsBlock()->empty());
  ASSERT_TRUE(data16.AsBlock());
  EXPECT_TRUE(std::ranges::equal(kData16, *data16.AsBlock()));
  EXPECT_FALSE(data8.AsBlock());
}

TEST(FormValue, InvalidPayloadsAndWidths) {
  constexpr uint8_t kOverflow[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02};
  for (DwarfForm form :
       {DwarfForm::kUdata, DwarfForm::kSdata, DwarfForm::kBlock, DwarfForm::kIndirect})
    ExpectMalformed(Attribute(form), kDwarf5, DataReader(kOverflow, Endian::kLittle));

  constexpr uint8_t kBytes[8] = {};
  constexpr FormParams kBadAddress{.version = 2, .offset_size = 4, .address_size = 3};
  constexpr FormParams kBadOffset{.version = 3, .offset_size = 2, .address_size = 4};
  ExpectMalformed(Attribute(DwarfForm::kAddr), kBadAddress, DataReader(kBytes, Endian::kLittle));
  ExpectMalformed(Attribute(DwarfForm::kRefAddr), kBadAddress, DataReader(kBytes, Endian::kLittle));
  ExpectMalformed(Attribute(DwarfForm::kRefAddr), kBadOffset, DataReader(kBytes, Endian::kLittle));
  ExpectMalformed(Attribute(DwarfForm::kStrp), kBadOffset, DataReader(kBytes, Endian::kLittle));

  DataReader failed(kBytes, Endian::kLittle);
  failed.Skip(sizeof(kBytes) + 1);
  ExpectMalformed(Attribute(DwarfForm::kFlagPresent), kDwarf5, failed);
}

}  // namespace

}  // namespace zxdb
