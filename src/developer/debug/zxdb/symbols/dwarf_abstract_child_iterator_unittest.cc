// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_abstract_child_iterator.h"

#include <gtest/gtest.h>

namespace zxdb {

namespace {

// Stand-in for DwarfDie that emulates enough of the API to be used in the
// DwarfAbstractChildIterator.
class TestDie {
 public:
  using Vector = std::vector<TestDie>;

  // Constructs a null DIE.
  TestDie() = default;

  // Constructs a non-null DIE. This will report the given offset to uniquely identify it.
  explicit TestDie(uint64_t offset) : is_valid_(true), offset_(offset) {}

  TestDie(const TestDie& other)
      : is_valid_(other.is_valid_), offset_(other.offset_), children_(other.children_) {
    if (other.abstract_origin_)
      abstract_origin_ = std::make_unique<TestDie>(*other.abstract_origin_);
  }

  const TestDie& operator=(const TestDie& other) {
    is_valid_ = other.is_valid_;
    offset_ = other.offset_;
    children_ = other.children_;
    abstract_origin_.reset();
    if (other.abstract_origin_)
      abstract_origin_ = std::make_unique<TestDie>(*other.abstract_origin_);
    return *this;
  }

  bool operator==(const TestDie& other) const {
    // Assume the offsets uniquely identify a DIE.
    return offset_ == other.offset_;
  }

  void set_is_null(bool iv) { is_valid_ = iv; }
  TestDie& abstract_origin() {
    // Lazily create.
    if (!abstract_origin_)
      abstract_origin_ = std::make_unique<TestDie>();
    return *abstract_origin_;
  }
  Vector& children() { return children_; }

  const Vector& children() const { return children_; }

  // DwarfDie implementation (enough for the iterator).
  bool is_valid() const { return is_valid_; }
  uint64_t offset() const { return offset_; }

 private:
  bool is_valid_ = false;
  uint64_t offset_ = 0;
  std::unique_ptr<TestDie> abstract_origin_;

  Vector children_;
};

// The iterator finds this overload by argument-dependent lookup.
TestDie GetAbstractOrigin(const TestDie& die) {
  return const_cast<TestDie&>(die).abstract_origin();
}

using TestIterator = DwarfAbstractChildIteratorBase<TestDie>;

}  // namespace

TEST(DwarfAbstractChildIterator, Null) {
  TestDie empty;
  TestIterator iter(empty, GetAbstractOrigin);

  auto begin = iter.begin();
  EXPECT_EQ(iter.begin(), iter.end());
}

TEST(DwarfAbstractChildIterator, Empty) {
  TestDie empty(22);  // Give some arbitrary nonzero offset for not-null DIE.
  TestIterator iter(empty, GetAbstractOrigin);

  EXPECT_EQ(iter.begin(), iter.end());

  // Check that a range-based for loop works on the iterators and reports nothing.
  int count = 0;
  for (const auto& child : iter) {
    (void)child;
    count++;
  }
  EXPECT_EQ(0, count);
}

// Tests child iteration with no abstract origin.
TEST(DwarfAbstractChildIterator, NoAbstract) {
  TestDie root(1);

  uint64_t expected_child_offset = 100;
  root.children().emplace_back(expected_child_offset);
  root.children().emplace_back(expected_child_offset + 1);
  root.children().emplace_back(expected_child_offset + 2);

  EXPECT_EQ(102u, root.children().back().offset());

  // Validate the unique offsets of each child.
  for (const auto& child : TestIterator(root, GetAbstractOrigin)) {
    EXPECT_EQ(expected_child_offset, child.offset());
    expected_child_offset++;
  }
  EXPECT_EQ(103u, expected_child_offset);  // Should have seen them all.
}

TEST(DwarfAbstractChildIterator, Abstract) {
  // These are the DIEs in each class, with lines indicating the shadowing:
  //
  //   CONCRETE       ABSTRACT1      ABSTRACT2
  //                                   100
  //     301 ------------------------- 101
  //                    202 ---------- 102
  //     303 ---------- 203 ---------- 103
  //     304 ---------- 204
  //                    205
  //
  // As a result, iterating should show the children in the order of abstract2, with each one
  // replaced by the child that shadows it, then the children of only abstract1: 100, 301, 202, 303,
  // 304, 205. 304 shadows 204, which is a child of only abstract1, so it takes that position.

  TestDie concrete(1);
  TestDie abstract1(2);
  TestDie abstract2(3);

  abstract2.children().emplace_back(100);  // Visible, unique.
  abstract2.children().emplace_back(101);  // Shadowed by concrete.
  abstract2.children().emplace_back(102);  // Shadowed by abstract1.
  abstract2.children().emplace_back(103);  // Shadowed by both concrete and abstract1.

  abstract1.children().emplace_back(202);  // Visible, shadows abstract2.
  abstract1.children().back().abstract_origin() = abstract2.children()[2];  // 102
  abstract1.children().emplace_back(203);  // Shadows abstract2, shadowed by concrete.
  abstract1.children().back().abstract_origin() = abstract2.children()[3];  // 103
  abstract1.children().emplace_back(204);                                   // Shadowed by concrete.
  abstract1.children().emplace_back(205);                                   // Visible, unique.

  concrete.children().emplace_back(301);                                   // Shadows abstract2.
  concrete.children().back().abstract_origin() = abstract2.children()[1];  // 101
  concrete.children().emplace_back(303);  // Shadows abstract1 and abstract2.
  concrete.children().back().abstract_origin() = abstract1.children()[1];  // 203
  concrete.children().emplace_back(304);                                   // Shadows abstract1.
  concrete.children().back().abstract_origin() = abstract1.children()[2];  // 204

  // Connect the tree.
  //
  // This will COPY the values so changes to our local vars won't reflect in the abstract origin
  // hierarchy from here down.
  abstract1.abstract_origin() = abstract2;
  concrete.abstract_origin() = abstract1;

  // Iterate through the values.
  std::vector<uint64_t> expected{100, 301, 202, 303, 304, 205};  // See comment above.
  size_t i = 0;
  for (const auto& child : TestIterator(concrete, GetAbstractOrigin)) {
    ASSERT_LT(i, expected.size());
    EXPECT_EQ(expected[i], child.offset());
    ++i;
  }
  EXPECT_EQ(i, expected.size());
}

// This tests several combinations of DIEs in a hierarchy having no children.
TEST(DwarfAbstractChildIterator, ConcreteNoChildren) {
  TestDie concrete(1);
  TestDie abstract1(2);
  TestDie abstract2(3);
  TestDie abstract3(4);

  abstract2.children().emplace_back(101);
  abstract2.children().emplace_back(102);

  abstract2.abstract_origin() = abstract3;
  abstract1.abstract_origin() = abstract2;
  concrete.abstract_origin() = abstract1;

  TestIterator iter(concrete, GetAbstractOrigin);
  auto cur = iter.begin();

  // We should find both children on the double-abstract origin.
  ASSERT_NE(cur, iter.end());
  EXPECT_EQ(cur->offset(), 101u);
  ++cur;
  ASSERT_NE(cur, iter.end());
  EXPECT_EQ(cur->offset(), 102u);
  ++cur;
  EXPECT_EQ(cur, iter.end());
}

namespace {

// Returns the offsets of the children that the iterator gives.
std::vector<uint64_t> ChildOffsets(const TestDie& die) {
  std::vector<uint64_t> result;
  for (const auto& child : TestIterator(die, GetAbstractOrigin))
    result.push_back(child.offset());
  return result;
}

}  // namespace

TEST(DwarfAbstractChildIterator, OmittedChild) {
  // Clang omits a parameter that an inlined function doesn't use. The parameters keep the order of
  // the abstract origin:
  //
  //   CONCRETE       ABSTRACT
  //                    100 (unused parameter)
  //     201 ---------- 101
  //     202 ---------- 102
  TestDie concrete(1);
  TestDie abstract(2);
  abstract.children().emplace_back(100);
  abstract.children().emplace_back(101);
  abstract.children().emplace_back(102);
  concrete.children().emplace_back(201);
  concrete.children().back().abstract_origin() = abstract.children()[1];
  concrete.children().emplace_back(202);
  concrete.children().back().abstract_origin() = abstract.children()[2];
  concrete.abstract_origin() = abstract;

  EXPECT_EQ(std::vector<uint64_t>({100, 201, 202}), ChildOffsets(concrete));
}

TEST(DwarfAbstractChildIterator, ConcreteOnlyChildren) {
  // Children of only the concrete instance come after the others, in their order, also when they
  // come first in the concrete instance.
  TestDie concrete(1);
  TestDie abstract(2);
  abstract.children().emplace_back(100);
  abstract.children().emplace_back(101);
  concrete.children().emplace_back(200);  // Only in the concrete instance.
  concrete.children().emplace_back(201);
  concrete.children().back().abstract_origin() = abstract.children()[0];
  concrete.children().emplace_back(202);  // Only in the concrete instance.
  concrete.abstract_origin() = abstract;

  EXPECT_EQ(std::vector<uint64_t>({201, 101, 200, 202}), ChildOffsets(concrete));
}

TEST(DwarfAbstractChildIterator, DuplicateReference) {
  // Two concrete children that reference the same child. The first replaces it, and the second is
  // kept with the children of only the concrete instance.
  TestDie concrete(1);
  TestDie abstract(2);
  abstract.children().emplace_back(100);
  abstract.children().emplace_back(101);
  concrete.children().emplace_back(201);
  concrete.children().back().abstract_origin() = abstract.children()[1];
  concrete.children().emplace_back(202);
  concrete.children().back().abstract_origin() = abstract.children()[1];
  concrete.abstract_origin() = abstract;

  EXPECT_EQ(std::vector<uint64_t>({100, 201, 202}), ChildOffsets(concrete));
}

TEST(DwarfAbstractChildIterator, SelfOrigin) {
  // A DIE that is its own abstract origin gives its children once.
  TestDie die(1);
  die.children().emplace_back(100);
  die.children().emplace_back(101);
  die.abstract_origin() = die;

  EXPECT_EQ(std::vector<uint64_t>({100, 101}), ChildOffsets(die));
}

}  // namespace zxdb
