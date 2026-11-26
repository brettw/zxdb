// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_die.h"

#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "llvm/ADT/StringMap.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Support/MemoryBuffer.h"
#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"
#include "src/developer/debug/zxdb/symbols/dwarf_attr.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"

namespace zxdb {
namespace {

// The unit has a namespace with one variable, followed by a subprogram at unit scope.
class DieTree {
 public:
  DieTree() {
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
    table.EndAbbrevTable();

    std::vector<uint8_t> info;
    DwarfSectionBuilder writer(info, kEndian);
    auto unit = writer.BeginUnit({});
    root_offset = info.size();
    writer.Uleb(1);
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
    context_ = llvm::DWARFContext::create(buffers_, 8, kEndian == Endian::kLittle);
  }

  DwarfDie root() { return DwarfDie(context_->getUnitAtIndex(0)->getUnitDIE(false)); }

  size_t root_offset;
  size_t namespace_offset;
  size_t variable_offset;
  size_t function_offset;

 private:
  llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> buffers_;
  std::unique_ptr<llvm::DWARFContext> context_;
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

TEST(DwarfDie, Identity) {
  DwarfDie invalid;
  EXPECT_FALSE(invalid.is_valid());
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
