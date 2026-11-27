// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DIE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DIE_H_

#include <stdint.h>

#include <optional>

#include "llvm/ADT/STLExtras.h"
#include "llvm/DebugInfo/DWARF/DWARFAbbreviationDeclaration.h"
#include "llvm/DebugInfo/DWARF/DWARFDie.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "src/developer/debug/zxdb/symbols/dwarf/abbrev_table.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_ref.h"
#include "src/developer/debug/zxdb/symbols/dwarf_tag.h"

namespace zxdb {

// Borrows an entry in a unit's DIE array. Check is_valid() before reading the entry.
// Clearing the array or expanding a root-only array invalidates its handles.
class DwarfDie {
 private:
  using LLVMSpec = llvm::DWARFAbbreviationDeclaration::AttributeSpec;
  struct ReadSpec {
    AbbrevTable::AttributeSpec operator()(const LLVMSpec& spec) const {
      return {spec.Attr, spec.Form,
              spec.isImplicitConst() ? std::optional(spec.getImplicitConstValue()) : std::nullopt};
    }
  };
  using Attributes = llvm::iterator_range<llvm::mapped_iterator<const LLVMSpec*, ReadSpec>>;

 public:
  DwarfDie() = default;
  enum class Load { kRootOnly, kAllDies };

  // Entry points for LLVM-backed readers. A root-only lookup keeps any existing children.
  static DwarfDie FromUnitRoot(llvm::DWARFUnit* unit, Load load = Load::kRootOnly);
  // For an index in an already loaded DIE array.
  static DwarfDie FromIndex(llvm::DWARFUnit& unit, uint32_t index) {
    return DwarfDie(unit.getDIEAtIndex(index));
  }
  static DwarfDie FromSubroutineAddress(llvm::DWARFUnit& unit, uint64_t relative_address);
  static DwarfDie FromOffset(llvm::DWARFUnit* unit, uint64_t offset) {
    return unit ? DwarfDie(unit->getDIEForOffset(offset)) : DwarfDie();
  }

  std::optional<uint32_t> index_in(const llvm::DWARFUnit& unit) const {
    if (!is_valid() || die_.getDwarfUnit() != &unit)
      return std::nullopt;
    return unit.getDIEIndex(die_);
  }
  DwarfDie unit_root() const;
  bool unit_is_dwo() const { return die_.getDwarfUnit()->isDWOUnit(); }

  // Resolves a unit-relative offset. The offset must point to a DIE in this unit.
  DwarfDie GetUnitRelativeDie(uint64_t offset) const;

  DwarfDieRef ref() const {
    if (!is_valid())
      return {};
    auto* unit = die_.getDwarfUnit();
    return unit->isTypeUnit() ? DwarfDieRef::ForTypeUnit(unit->getVersion(), offset())
                              : DwarfDieRef::Main(offset());
  }

  bool is_valid() const { return die_.isValid(); }
  explicit operator bool() const { return is_valid(); }
  bool operator==(const DwarfDie&) const = default;

  DwarfTag tag() const { return static_cast<DwarfTag>(die_.getTag()); }
  uint64_t offset() const { return die_.getOffset(); }
  const char* short_name() const;
  const char* linkage_name() const;

  DwarfDie parent() const { return DwarfDie(die_.getParent()); }
  std::optional<uint32_t> parent_index() const { return die_.getDebugInfoEntry()->getParentIdx(); }
  DwarfDie first_child() const { return DwarfDie(die_.getFirstChild()); }
  DwarfDie sibling() const { return DwarfDie(die_.getSibling()); }

  // The child range excludes the null entry that ends a sibling chain.
  auto children() const {
    return llvm::map_range(die_.children(), [](llvm::DWARFDie child) { return DwarfDie(child); });
  }

  // The abbreviation specifies the attributes in encoding order. An entry without an
  // abbreviation (such as the end of a sibling chain) returns nullopt.
  std::optional<Attributes> attributes() const {
    if (const auto* abbrev = die_.getAbbreviationDeclarationPtr())
      return llvm::map_range(abbrev->attributes(), ReadSpec{});
    return std::nullopt;
  }

  // Temporary. Remove when the decoder, unit contexts and line programs use the new backend.
  llvm::DWARFUnit* llvm_unit() const { return die_.getDwarfUnit(); }

 private:
  explicit DwarfDie(llvm::DWARFDie die) : die_(die) {}

  llvm::DWARFDie die_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DIE_H_
