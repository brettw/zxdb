// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_ABSTRACT_CHILD_ITERATOR_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_ABSTRACT_CHILD_ITERATOR_H_

#include <optional>
#include <vector>

#include "src/developer/debug/zxdb/symbols/dwarf_die.h"

namespace zxdb {

// This file allows iterating over a DWARF "DIE"'s children, taking into account "abstract origin"
// information.
//
// EXAMPLE USAGE
// 🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶
//
//   DwarfDie die;  // Passed in from somewhere.
//   for (const DwarfDie& child : DwarfAbstractChildIterator(die, get_origin)) {
//     DoSomething(child);
//   }
//
// BACKGROUND
// 🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶
// DWARF inline functions are normally split into two parts: the "concrete inlined instance" which
// is a per-inlined-location description of the call, and the "abstract origin" which is a shared
// description of parameters and declaration information common to all inlined instances. This
// prevents unnecessary duplication of information.
//
// When a DIE has a DW_AT_abstract_origin attribute, it indicates the abstract origin that
// corresponds to the current concrete instance. This affects both the attributes and children of
// the current DIE.
//
// ATTRIBUTE HANDLING
// 🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶
// For the attributes, concrete instance attributes shadow and abstract origin attributes (allowing
// the concrete instance to provide more specific information). But any attributes not specified on
// the concrete instance fall back to their values in the abstract origin.
//
// This attribute shadowing logic is transparently handled by the DwarfDieDecoder and does not
// concern this class.
//
// CHILD HANDLING
// 🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶
// This class handles the "child iteration" cases where you want to iterate the children of a DIE
// and also handle any additional children provided by the abstract origin.
//
// The concrete instance can have children that shadow children of the abstract origin. This is used
// to provide things like the precise location of a variable in the inlined instance, while keeping
// the general type and name of the variable common on the abstract origin. When this happens the
// DW_AT_abstract_origin will be set on the child of the concrete instance and the DwarfDieDecoder
// will handle everything.
//
// But there is an additional case where if there are no instance-specific overrides on a child,
// that child can be omitted and the child on the abstract origin should be used. This class allows
// iteration over the children, and will magically add children of the abstract origin that were
// not overridden by the concrete instance.
//
// ORDER
// 🭶🭶🭶🭶🭶
// The children are in the order of the abstract instance tree. The ordering rules for formal
// parameters and members apply to abstract entries (DWARF 5 section 3.3.8.1), and a concrete
// instance can omit entries (section 3.3.8.2). For example, Clang omits a parameter that an inlined
// function doesn't use. So the iterator returns each child of the abstract origin, or the concrete
// child that references it. The concrete children that reference no child of the abstract origin
// describe entities of only this instance (section 3.3.8.2), and they come after the others, in
// their order.
//
// In the example below, the result is the "this" parameter from the abstract origin, followed by
// the concrete "param" parameter.
//
// DWARF EXAMPLE
// 🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶🭶
// Abstract origin that provides the shared information:
//
//   0x00000555:   DW_TAG_subprogram
//                   DW_AT_specification (0x00000535 "_ZN9ForInline15InlinedFunctionEi")
//                   DW_AT_inline (DW_INL_inlined)
//                   DW_AT_object_pointer (0x0000055f)
//
//   0x0000055f:     DW_TAG_formal_parameter                 <=== THIS ONE IS ADDED
//                     DW_AT_name ("this")
//                     DW_AT_type (0x00000574 "ForInline*")
//                     DW_AT_artificial (true)
//
//   0x00000568:     DW_TAG_formal_parameter                 <=== THIS ONE IS NOT ITERATED OVER
//                     DW_AT_name ("param")                       (The attributes will be merged by
//                     DW_AT_decl_file ("type_test.cc")           the DwarfDieDecoder.)
//                     DW_AT_decl_line (84)
//                     DW_AT_type (0x000001a0 "int")
//
//   0x00000573:     NULL
//
// Concrete inlined instance. Note that the "this" parameter is not overridden here so the
// parameter from the abstract origin will "show through":
//
//   0x000005b1:     DW_TAG_inlined_subroutine
//                     DW_AT_abstract_origin (0x00000555 "_ZN9ForInline15InlinedFunctionEi")
//                     DW_AT_low_pc (0x0000000000001150)
//                     DW_AT_high_pc (0x0000000000001158)
//
//   0x000005c5:       DW_TAG_formal_parameter              <=== SHADOWS THE ABSTRACT ORIGIN ONE
//                       DW_AT_location (DW_OP_breg0 W0+1, DW_OP_stack_value)
//                       DW_AT_abstract_origin (0x00000568 "param")
//
//   0x000005ce:       NULL

// Tests supply a DIE type with the same tree operations and a lookup for abstract origins.
//
// The constructor computes all the children, so the DIE doesn't need to outlive this object.
template <class Die>
class DwarfAbstractChildIteratorBase {
 public:
  using const_iterator = typename std::vector<Die>::const_iterator;

  // Takes the DIE whose children to iterate over and the lookup for its abstract origin.
  template <typename OriginLookup>
  DwarfAbstractChildIteratorBase(const Die& die, const OriginLookup& get_origin)
      : children_(Collect(die, kMaxDepth, get_origin)) {}

  const_iterator begin() const { return children_.begin(); }
  const_iterator end() const { return children_.end(); }

 private:
  // The maximum number of abstract origins to follow. This stops cycles in corrupt symbols.
  static constexpr int kMaxDepth = 8;

  // Returns the children of the DIE, merged with the children of its abstract origin. See "ORDER"
  // above.
  template <typename OriginLookup>
  static std::vector<Die> Collect(const Die& die, int depth, const OriginLookup& get_origin);

  std::vector<Die> children_;
};

template <class Die>
template <typename OriginLookup>
std::vector<Die> DwarfAbstractChildIteratorBase<Die>::Collect(const Die& die, int depth,
                                                              const OriginLookup& get_origin) {
  std::vector<Die> own;
  if (!die.is_valid())
    return own;
  for (const auto& child : die.children())
    own.push_back(child);

  Die origin = get_origin(die);
  if (!origin.is_valid() || origin == die || depth == 0)
    return own;  // The common case.

  // The abstract origin can have its own abstract origin, so its children are merged first.
  std::vector<Die> base = Collect(origin, depth - 1, get_origin);

  // The offsets of the DIEs that the children of this DIE reference.
  std::vector<std::optional<uint64_t>> own_origins;
  own_origins.reserve(own.size());
  for (const Die& child : own) {
    Die child_origin = get_origin(child);
    own_origins.push_back(child_origin.is_valid() ? std::optional<uint64_t>(child_origin.offset())
                                                  : std::nullopt);
  }

  std::vector<Die> result;
  result.reserve(base.size() + own.size());
  std::vector<bool> used(own.size(), false);
  for (const Die& base_child : base) {
    // Replace the child of the abstract origin with the child of this DIE that references it. The
    // trees are small, so a linear search is fast enough.
    size_t found = own.size();
    for (size_t i = 0; i < own.size(); i++) {
      if (!used[i] && own_origins[i] == base_child.offset()) {
        found = i;
        break;
      }
    }
    if (found < own.size()) {
      used[found] = true;
      result.push_back(own[found]);
    } else {
      result.push_back(base_child);
    }
  }

  // The children of only this instance.
  for (size_t i = 0; i < own.size(); i++) {
    if (!used[i])
      result.push_back(own[i]);
  }
  return result;
}

using DwarfAbstractChildIterator = DwarfAbstractChildIteratorBase<DwarfDie>;

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_ABSTRACT_CHILD_ITERATOR_H_
