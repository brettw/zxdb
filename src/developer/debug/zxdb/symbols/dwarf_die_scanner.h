// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DIE_SCANNER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DIE_SCANNER_H_

#include <stdint.h>

#include <vector>

#include "src/developer/debug/zxdb/symbols/dwarf_die.h"

namespace zxdb {

class DwarfUnit;

// This is a helper class for generating a zxdb::Index.
//
// It works in two phases. In the first, it linearly iterates through the DIEs of a unit. The
// calling code does:
//
//    while (!scanner.done()) {
//      current_die = scanner.Prepare();
//      ... work on current_die ...
//      scanner.Advance();
//    }
//
// In the second phase, the scanner can provide extra information for a DIE in the unit in constant
// time.
//
// The scan tracks which entries are inside a function. The second pass reads parent indices
// directly from the unit's DIE table.
class DwarfDieScanner {
 public:
  static constexpr uint32_t kNoParent = DieTable::kNoIndex;

  // The binary must keep the unit's section data alive.
  explicit DwarfDieScanner(const DwarfUnit& unit);

  ~DwarfDieScanner();

  // Requires !done(). Call before Advance() to compute the current DIE's scope. The reference
  // is valid until the scanner advances or is destroyed.
  const DwarfDie& Prepare();

  // Advances to the next DIE.
  void Advance() { ++current_; }

  uint32_t die_count() const { return root_.unit_die_count(); }
  DwarfDie GetDieAtIndex(uint64_t index) const { return root_.die_at_index(index); }

  bool done() const { return current_ == std::default_sentinel; }

  // Returns true if the current stack position is considered to be directly inside a function.
  // Lexical blocks count as being inside a function, but if a new type is defined inside a function
  // the children of that type are no longer considered to be inside a function.
  //
  // This is used to avoid indexing function-local variables.
  bool is_inside_function() const { return tree_stack_.back().inside_function; }

  // Return the parent's index of a DIE in constant time. Will return kNoParent for the root.
  uint32_t GetParentIndex(uint32_t index) const;

 private:
  // Stores the list of parent indices according to the current depth in the tree. At any given
  // point, the parent index of the current node will be tree_stack.back(). inside_function should
  // be set if this node or any parent node is a function.
  struct StackEntry {
    StackEntry(unsigned i, bool f) : index(i), inside_function(f) {}

    unsigned index;

    // Tracks whether this node is a child of a function with no intermediate types. This is to
    // avoid indexing local variables inside functions or inside blocks inside functions.
    bool inside_function;
  };

  // Keep the snapshot for random access after the iterator reaches the end.
  DwarfDie root_;
  DwarfDie::UnitIterator current_;

  std::vector<StackEntry> tree_stack_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DIE_SCANNER_H_
