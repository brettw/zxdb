// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_die.h"

#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "llvm/ADT/StringMap.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Support/MemoryBuffer.h"
#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"
#include "src/developer/debug/zxdb/symbols/dwarf_attr.h"
#include "src/developer/debug/zxdb/symbols/dwarf_binary_impl.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"

namespace zxdb {
namespace {

static_assert(sizeof(DwarfDie) <= 3 * sizeof(void*));

// The unit has a namespace with one variable, followed by a subprogram at unit scope.
class DieTree {
 public:
  explicit DieTree(bool leading_unknown_tag = false) {
    constexpr Endian kEndian = Endian::kLittle;
    std::vector<uint8_t> abbrev;
    DwarfSectionBuilder table(abbrev, kEndian);
    table.AbbrevDeclaration(1, static_cast<uint64_t>(DwarfTag::kCompileUnit), true);
    table.AbbrevDeclaration(2, static_cast<uint64_t>(DwarfTag::kNamespace), true);
    table.AbbrevDeclaration(3, static_cast<uint64_t>(DwarfTag::kVariable), false);
    const AbbrevTable::AttributeSpec specs[] = {
        {static_cast<uint64_t>(DwarfAttr::kName), static_cast<uint64_t>(DwarfForm::kString), {}},
        {static_cast<uint64_t>(DwarfAttr::kDeclLine),
         static_cast<uint64_t>(DwarfForm::kImplicitConst), 17},
        {static_cast<uint64_t>(DwarfAttr::kLinkageName),
         static_cast<uint64_t>(DwarfForm::kString),
         {}}};
    table.AbbrevDeclaration(4, static_cast<uint64_t>(DwarfTag::kSubprogram), false, specs);
    if (leading_unknown_tag)
      table.AbbrevDeclaration(5, uint64_t{1} << 32, false);
    table.EndAbbrevTable();

    std::vector<uint8_t> info;
    DwarfSectionBuilder writer(info, kEndian);
    auto unit = writer.BeginUnit({});
    root_offset = info.size();
    writer.Uleb(1);
    if (leading_unknown_tag)
      writer.Uleb(5);
    namespace_offset = info.size();
    writer.Uleb(2);
    variable_offset = info.size();
    writer.Uleb(3);
    writer.Uleb(0);  // End of namespace children.
    function_offset = info.size();
    writer.Uleb(4);
    writer.CString("f");
    writer.CString("_Z1fv");
    writer.Uleb(0);  // End of unit children.
    writer.EndUnit(unit);

    buffers_["debug_info"] = llvm::MemoryBuffer::getMemBufferCopy(AsStringView(info));
    buffers_["debug_abbrev"] = llvm::MemoryBuffer::getMemBufferCopy(AsStringView(abbrev));
    binary_ = std::make_unique<DwarfBinaryImpl>(
        llvm::DWARFContext::create(buffers_, 8, kEndian == Endian::kLittle));
  }

  DwarfBinaryImpl& binary() { return *binary_; }
  fxl::RefPtr<DwarfUnit> unit() { return binary_->GetUnitAtIndex(UnitIndex(false, 0)); }
  DwarfDie root(DwarfDie::Load load = DwarfDie::Load::kAllDies) { return unit()->GetUnitDie(load); }

  size_t root_offset;
  size_t namespace_offset;
  size_t variable_offset;
  size_t function_offset;

 private:
  llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> buffers_;
  std::unique_ptr<DwarfBinaryImpl> binary_;
};

TEST(DwarfDie, Navigation) {
  DieTree tree;
  DwarfDie root = tree.root();
  ASSERT_TRUE(root);
  EXPECT_EQ(DwarfTag::kCompileUnit, root.tag());
  EXPECT_EQ(tree.root_offset, root.offset());
  EXPECT_FALSE(root.parent());
  EXPECT_EQ(std::nullopt, root.parent_index());

  DwarfDie ns = root.first_child();
  ASSERT_TRUE(ns);
  EXPECT_EQ(DwarfTag::kNamespace, ns.tag());
  EXPECT_EQ(tree.namespace_offset, ns.offset());
  EXPECT_EQ(root, ns.unit_root());
  EXPECT_EQ(DwarfDieRef::Main(tree.namespace_offset), ns.ref());
  EXPECT_EQ(root, ns.parent());
  EXPECT_EQ(0u, ns.parent_index());

  DwarfDie var = ns.first_child();
  ASSERT_TRUE(var);
  EXPECT_EQ(tree.variable_offset, var.offset());
  EXPECT_EQ(ns, var.parent());
  EXPECT_EQ(1u, var.parent_index());
  EXPECT_FALSE(var.first_child());
  EXPECT_TRUE(var.children().empty());

  DwarfDie function = ns.sibling();
  ASSERT_TRUE(function);
  EXPECT_EQ(tree.function_offset, function.offset());
  EXPECT_STREQ("f", function.short_name());
  EXPECT_STREQ("_Z1fv", function.linkage_name());

  std::vector<uint64_t> offsets;
  for (DwarfDie child : root.children())
    offsets.push_back(child.offset());
  EXPECT_EQ((std::vector<uint64_t>{tree.namespace_offset, tree.function_offset}), offsets);
  offsets.clear();
  for (DwarfDie child : ns.children())
    offsets.push_back(child.offset());
  EXPECT_EQ((std::vector<uint64_t>{tree.variable_offset}), offsets);
}

TEST(DwarfDie, UnknownTagKeepsLaterChildren) {
  DieTree tree(/*leading_unknown_tag=*/true);
  const auto root = tree.root();
  ASSERT_TRUE(root);
  std::vector<DwarfTag> tags;
  for (const DwarfDie& child : root.children())
    tags.push_back(child.tag());
  EXPECT_EQ((std::vector{DwarfTag::kNone, DwarfTag::kNamespace, DwarfTag::kSubprogram}), tags);
}

TEST(DwarfDie, UnitWalkKeepsSnapshotThroughEviction) {
  EXPECT_TRUE(DwarfDie().unit_dies().empty());
  DieTree tree;
  auto unit = tree.unit();
  DwarfDie root_only = unit->GetUnitDie(DwarfDie::Load::kRootOnly);
  DwarfDie retained;
  {
    auto dies = unit->GetDies();
    unit->ClearDieCache();

    std::vector<DwarfTag> tags;
    for (const DwarfDie& die : dies) {
      EXPECT_EQ(tags.size(), die.index());
      tags.push_back(die.tag());
      if (die.tag() == DwarfTag::kSubprogram)
        retained = die;
    }
    EXPECT_EQ((std::vector{DwarfTag::kCompileUnit, DwarfTag::kNamespace, DwarfTag::kVariable,
                           DwarfTag::kNone, DwarfTag::kSubprogram, DwarfTag::kNone}),
              tags);
  }

  ASSERT_TRUE(retained);
  EXPECT_EQ(tree.function_offset, retained.offset());
  EXPECT_STREQ("f", retained.short_name());
  EXPECT_EQ(1, std::ranges::distance(root_only.unit_dies()));
  EXPECT_FALSE(unit->GetUnitDie(DwarfDie::Load::kRootOnly).first_child());
}

TEST(DwarfDie, Attributes) {
  DieTree tree;
  DwarfDie root = tree.root();
  ASSERT_TRUE(root);
  auto empty = root.attributes();
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->empty());

  DwarfDie function = root.first_child().sibling();
  ASSERT_TRUE(function);
  auto specs = function.attributes();
  ASSERT_TRUE(specs);
  std::vector<DwarfAttr> attrs;
  std::vector<DwarfForm> forms;
  std::vector<std::optional<int64_t>> constants;
  for (const auto spec : *specs) {
    attrs.push_back(static_cast<DwarfAttr>(spec.attribute));
    forms.push_back(static_cast<DwarfForm>(spec.form));
    constants.push_back(spec.implicit_const);
  }
  EXPECT_EQ((std::vector{DwarfAttr::kName, DwarfAttr::kDeclLine, DwarfAttr::kLinkageName}), attrs);
  EXPECT_EQ((std::vector{DwarfForm::kString, DwarfForm::kImplicitConst, DwarfForm::kString}),
            forms);
  EXPECT_EQ((std::vector<std::optional<int64_t>>{std::nullopt, 17, std::nullopt}), constants);

  DwarfDie terminator = function.sibling();
  ASSERT_TRUE(terminator);
  EXPECT_EQ(DwarfTag::kNone, terminator.tag());
  EXPECT_FALSE(terminator.attributes());
}

TEST(DwarfDie, NameLookupPrecedence) {
  constexpr Endian kEndian = Endian::kLittle;
  constexpr AbbrevTable::AttributeSpec kOriginAttrs[] = {
      {static_cast<uint64_t>(DwarfAttr::kName), static_cast<uint64_t>(DwarfForm::kString)},
      {static_cast<uint64_t>(DwarfAttr::kMipsLinkageName),
       static_cast<uint64_t>(DwarfForm::kString)},
  };
  constexpr AbbrevTable::AttributeSpec kLinkedAttrs[] = {
      {static_cast<uint64_t>(DwarfAttr::kSpecification), static_cast<uint64_t>(DwarfForm::kRef4)},
      {static_cast<uint64_t>(DwarfAttr::kAbstractOrigin), static_cast<uint64_t>(DwarfForm::kRef4)},
      {static_cast<uint64_t>(DwarfAttr::kLinkageName), static_cast<uint64_t>(DwarfForm::kString)},
  };
  std::vector<uint8_t> abbrev;
  DwarfSectionBuilder table(abbrev, kEndian);
  table.AbbrevDeclaration(1, static_cast<uint64_t>(DwarfTag::kCompileUnit), true);
  table.AbbrevDeclaration(2, static_cast<uint64_t>(DwarfTag::kSubprogram), false, kOriginAttrs);
  table.AbbrevDeclaration(3, static_cast<uint64_t>(DwarfTag::kSubprogram), false, kLinkedAttrs);
  table.AbbrevDeclaration(4, static_cast<uint64_t>(DwarfTag::kSubprogram), false,
                          std::span(kLinkedAttrs).first<1>());
  table.AbbrevDeclaration(5, static_cast<uint64_t>(DwarfTag::kSubprogram), false,
                          std::span(kLinkedAttrs).first<2>());
  table.EndAbbrevTable();

  std::vector<uint8_t> info;
  DwarfSectionBuilder writer(info, kEndian);
  auto unit = writer.BeginUnit({});
  writer.Uleb(1);
  uint64_t origin = info.size();
  writer.Uleb(2);
  writer.CString("origin");
  writer.CString("legacy_origin");
  uint64_t spec = info.size();
  writer.Uleb(2);
  writer.CString("spec");
  writer.CString("legacy_spec");

  // A reference chain can reach the declaration through several intermediate entries.
  for (size_t i = 0; i < 40; ++i) {
    uint64_t link = info.size();
    writer.Uleb(4);
    writer.Offset(spec, 4);
    spec = link;
  }

  uint64_t target = info.size();
  writer.Uleb(3);
  writer.Offset(spec, 4);
  writer.Offset(origin, 4);
  writer.CString("concrete_linkage");

  uint64_t no_direct_linkage = info.size();
  writer.Uleb(5);
  writer.Offset(spec, 4);
  writer.Offset(origin, 4);

  // Both paths to the origin exist. The path through the specification gives it priority over
  // that entry's abstract origin.
  uint64_t spec_path = info.size();
  writer.Uleb(5);
  writer.Offset(origin, 4);
  writer.Offset(spec, 4);
  uint64_t converging_paths = info.size();
  writer.Uleb(5);
  writer.Offset(spec_path, 4);
  writer.Offset(origin, 4);
  writer.Uleb(0);
  writer.EndUnit(unit);

  llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> buffers;
  buffers["debug_info"] = llvm::MemoryBuffer::getMemBufferCopy(AsStringView(info));
  buffers["debug_abbrev"] = llvm::MemoryBuffer::getMemBufferCopy(AsStringView(abbrev));
  DwarfBinaryImpl binary(llvm::DWARFContext::create(buffers, 8, true));
  auto die = binary.GetDieAtOffset(DwarfDieRef::Main(target));
  ASSERT_TRUE(die);
  EXPECT_STREQ("spec", die.short_name());
  EXPECT_STREQ("concrete_linkage", die.linkage_name());

  auto inherited = binary.GetDieAtOffset(DwarfDieRef::Main(no_direct_linkage));
  ASSERT_TRUE(inherited);
  EXPECT_STREQ("legacy_spec", inherited.linkage_name());

  auto converging = binary.GetDieAtOffset(DwarfDieRef::Main(converging_paths));
  ASSERT_TRUE(converging);
  EXPECT_STREQ("origin", converging.short_name());
  EXPECT_STREQ("legacy_origin", converging.linkage_name());
}

TEST(DwarfDie, NameFromTypeSignature) {
  constexpr Endian kEndian = Endian::kLittle;
  constexpr uint64_t kSignature = 0;
  constexpr uint64_t kTypeOffset = 25;  // Type unit header plus the root's abbreviation code.
  constexpr uint64_t kStructureTag = static_cast<uint64_t>(DwarfTag::kStructureType);
  constexpr AbbrevTable::AttributeSpec kLinks[] = {
      {static_cast<uint64_t>(DwarfAttr::kSignature), static_cast<uint64_t>(DwarfForm::kRefSig8)},
      {static_cast<uint64_t>(DwarfAttr::kAbstractOrigin), static_cast<uint64_t>(DwarfForm::kRef4)},
      {static_cast<uint64_t>(DwarfAttr::kSpecification), static_cast<uint64_t>(DwarfForm::kRef4)},
  };
  constexpr AbbrevTable::AttributeSpec kNames[] = {
      {static_cast<uint64_t>(DwarfAttr::kName), static_cast<uint64_t>(DwarfForm::kString)},
      {static_cast<uint64_t>(DwarfAttr::kLinkageName), static_cast<uint64_t>(DwarfForm::kString)},
  };
  std::vector<uint8_t> abbrev;
  DwarfSectionBuilder table(abbrev, kEndian);
  table.AbbrevDeclaration(1, static_cast<uint64_t>(DwarfTag::kCompileUnit), true);
  table.AbbrevDeclaration(2, kStructureTag, false, std::span(kLinks).first<1>());
  table.AbbrevDeclaration(3, static_cast<uint64_t>(DwarfTag::kTypeUnit), true);
  table.AbbrevDeclaration(4, kStructureTag, false, kNames);
  table.AbbrevDeclaration(5, kStructureTag, false, std::span(kLinks).first<2>());
  table.AbbrevDeclaration(6, kStructureTag, false, kLinks);
  table.EndAbbrevTable();

  std::vector<uint8_t> info;
  DwarfSectionBuilder writer(info, kEndian);
  auto compile_unit = writer.BeginUnit({});
  writer.Uleb(1);
  uint64_t declaration_offset = info.size();
  writer.Uleb(2);
  writer.Fixed(kSignature, 8);

  uint64_t origin_offset = info.size();
  writer.Uleb(4);
  writer.CString("OriginType");
  writer.CString("origin_linkage");
  uint64_t specification_offset = info.size();
  writer.Uleb(4);
  writer.CString("SpecificationType");
  writer.CString("specification_linkage");

  uint64_t origin_and_signature_offset = info.size();
  writer.Uleb(5);
  writer.Fixed(kSignature, 8);
  writer.Offset(origin_offset, 4);
  uint64_t all_links_offset = info.size();
  writer.Uleb(6);
  writer.Fixed(kSignature, 8);
  writer.Offset(origin_offset, 4);
  writer.Offset(specification_offset, 4);
  writer.Uleb(0);
  writer.EndUnit(compile_unit);
  auto type_unit = writer.BeginUnit(
      {.unit_type = kUnitTypeType, .type_signature = kSignature, .type_offset = kTypeOffset});
  writer.Uleb(3);
  writer.Uleb(4);
  writer.CString("SignatureType");
  writer.CString("signature_linkage");
  writer.Uleb(0);
  writer.EndUnit(type_unit);

  llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> buffers;
  buffers["debug_info"] = llvm::MemoryBuffer::getMemBufferCopy(AsStringView(info));
  buffers["debug_abbrev"] = llvm::MemoryBuffer::getMemBufferCopy(AsStringView(abbrev));
  DwarfBinaryImpl binary(llvm::DWARFContext::create(buffers, 8, true));
  auto declaration = binary.GetDieAtOffset(DwarfDieRef::Main(declaration_offset));
  ASSERT_TRUE(declaration);
  EXPECT_STREQ("SignatureType", declaration.short_name());
  EXPECT_STREQ("signature_linkage", declaration.linkage_name());

  auto origin_and_signature = binary.GetDieAtOffset(DwarfDieRef::Main(origin_and_signature_offset));
  ASSERT_TRUE(origin_and_signature);
  EXPECT_STREQ("OriginType", origin_and_signature.short_name());
  EXPECT_STREQ("origin_linkage", origin_and_signature.linkage_name());

  auto all_links = binary.GetDieAtOffset(DwarfDieRef::Main(all_links_offset));
  ASSERT_TRUE(all_links);
  EXPECT_STREQ("SpecificationType", all_links.short_name());
  EXPECT_STREQ("specification_linkage", all_links.linkage_name());
}

TEST(DwarfDie, CacheEvictionPreservesSnapshots) {
  DieTree tree;
  const auto unit = tree.unit();
  auto root_only = tree.root(DwarfDie::Load::kRootOnly);
  ASSERT_TRUE(root_only);
  EXPECT_EQ(1u, root_only.unit_die_count());
  EXPECT_FALSE(root_only.first_child());
  EXPECT_FALSE(root_only.GetUnitRelativeDie(0));
  EXPECT_FALSE(root_only.GetUnitRelativeDie(UINT64_MAX));
  EXPECT_EQ(DwarfDieRef::Main(tree.function_offset),
            root_only.GetUnitRelativeDie(tree.function_offset).ref());

  auto full = tree.root();
  ASSERT_TRUE(full.first_child());
  EXPECT_EQ(root_only, full);
  EXPECT_FALSE(root_only.first_child());
  EXPECT_EQ(full, tree.root(DwarfDie::Load::kRootOnly));
  EXPECT_EQ(full.unit_die_count(), tree.root(DwarfDie::Load::kRootOnly).unit_die_count());
  EXPECT_FALSE(unit->GetDieAtIndex(full.unit_die_count()));
  EXPECT_FALSE(unit->GetDieAtIndex(uint64_t{1} << 32));
  EXPECT_FALSE(unit->GetDieAtOffset(DwarfDieRef::Type(tree.function_offset)));
  EXPECT_FALSE(unit->GetDieAtOffset(DwarfDieRef::Main(tree.function_offset + 1)));

  DwarfDieRef root_ref = full.ref();
  std::latch holding_table(1);
  std::latch evicted(1);
  std::thread reader([&] {
    DwarfDie die = tree.binary().GetDieAtOffset(DwarfDieRef::Main(tree.function_offset));
    holding_table.count_down();
    evicted.wait();
    ASSERT_TRUE(die);
    EXPECT_EQ(DwarfTag::kSubprogram, die.tag());
    EXPECT_EQ(root_ref, die.parent().ref());
    EXPECT_EQ(die, tree.binary().GetDieAtOffset(die.ref()));
  });
  holding_table.wait();

  root_only = {};
  full = {};
  unit->ClearDieCache();
  EXPECT_FALSE(tree.root(DwarfDie::Load::kRootOnly).first_child());
  tree.binary().ClearDwarfCaches();
  EXPECT_FALSE(tree.root(DwarfDie::Load::kRootOnly).first_child());

  evicted.count_down();
  reader.join();
}

TEST(DwarfDie, Identity) {
  DwarfDie invalid;
  EXPECT_FALSE(invalid.is_valid());
  EXPECT_FALSE(invalid.unit_root());
  EXPECT_FALSE(invalid.ref());
  EXPECT_EQ(DwarfDie(), invalid);

  DieTree tree;
  DieTree other;
  DwarfDie root = tree.root();
  ASSERT_TRUE(root);
  EXPECT_EQ(root, tree.root());
  EXPECT_NE(root, invalid);
  EXPECT_NE(root, root.first_child());
  EXPECT_NE(root, other.root());  // Same section offset in another unit.
}

}  // namespace
}  // namespace zxdb
