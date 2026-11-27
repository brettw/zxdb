// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_die_scanner.h"

#include <lib/syslog/cpp/macros.h>

#include "src/developer/debug/zxdb/symbols/dwarf_tag.h"
#include "src/developer/debug/zxdb/symbols/dwarf_unit.h"

namespace zxdb {

DwarfDieScanner::DwarfDieScanner(const DwarfUnit& unit)
    : current_die_(unit.GetUnitDie()), die_count_(current_die_.unit_die_count()) {
  // We prefer not to reallocate and normally the C++ component depth is < 8.
  tree_stack_.reserve(8);
}

DwarfDieScanner::~DwarfDieScanner() = default;

const DwarfDie& DwarfDieScanner::Prepare() {
  FX_DCHECK(!done());
  current_die_.SetIndexUnchecked(die_index_);
  uint32_t parent_idx = current_die_.parent_index().value_or(kNoParent);

  while (!tree_stack_.empty() && tree_stack_.back().index != parent_idx)
    tree_stack_.pop_back();
  tree_stack_.emplace_back(die_index_, false);

  // Fix up the inside function flag.
  switch (current_die_.tag()) {
    case DwarfTag::kLexicalBlock:
    case DwarfTag::kVariable:
      // Inherits from previous. For a block and a variable there should always be the parent,
      // since at least there's the unit root DIE.
      if (tree_stack_.size() >= 2)
        tree_stack_.back().inside_function = tree_stack_[tree_stack_.size() - 2].inside_function;
      break;
    case DwarfTag::kSubprogram:
    case DwarfTag::kInlinedSubroutine:
      tree_stack_.back().inside_function = true;
      break;
    default:
      tree_stack_.back().inside_function = false;
      break;
  }

  return current_die_;
}

void DwarfDieScanner::Advance() {
  FX_DCHECK(!done());

  die_index_++;
}

uint32_t DwarfDieScanner::GetParentIndex(uint32_t index) const {
  return current_die_.parent_index_at(index).value_or(kNoParent);
}

}  // namespace zxdb
