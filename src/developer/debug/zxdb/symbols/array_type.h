// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_ARRAY_TYPE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_ARRAY_TYPE_H_

#include <stdint.h>

#include <optional>

#include "src/developer/debug/zxdb/symbols/dwarf_expr.h"
#include "src/developer/debug/zxdb/symbols/lazy_symbol.h"
#include "src/developer/debug/zxdb/symbols/type.h"

namespace zxdb {

// A bound of an array dimension whose value is only known when the program runs (DWARF 5 section
// 2.19), for example the size of a C variable-length array. The value is the value of a variable,
// or the result of a DWARF expression. The expression evaluator computes it in a frame. See
// ResolveDynamicArrayType().
struct DynamicArrayBound {
  // Clang gives the number of elements (DW_AT_count). GCC gives the index of the last element
  // (DW_AT_upper_bound).
  enum class Kind { kCount, kUpperBound };
  Kind kind = Kind::kCount;

  // For kUpperBound, the number of elements is the upper bound - lower_bound + 1.
  int64_t lower_bound = 0;

  // Exactly one of these is set. Clang and GCC reference an artificial variable, and GCC can also
  // give an expression.
  LazySymbol variable;
  std::optional<DwarfExpr> expression;
};

// Represents an array. An array is similar to a pointer but we specifically know that it is an
// array and often know its length. Not much can be done with arrays with unknown lengths.
//
// DWARF says an array *may* have a length. Clang and GCC define int[] as a pointer so we expect
// all "real" arrays to have a length.
//
// The case that may not have lengths are extern definitions that refer to arrays. For example:
//
//   extern const char kFoo[];
//
// will be marked as an "external" variable with an array type with no length. Resolving the
// extern to getting the real variable definition will give an array type with a real length.
//
// A C variable-length array also has no length in its type. Its dimension has a dynamic_bound()
// instead, and its size is 0. The expression evaluator makes an array type with the length for a
// specific frame.
class ArrayType final : public Type {
 public:
  const Type* value_type() const { return value_type_.get(); }
  std::optional<size_t> num_elts() const { return num_elts_; }

  // Set for a dimension whose length is only known when the program runs. Then num_elts() is
  // nullopt.
  const std::optional<DynamicArrayBound>& dynamic_bound() const { return dynamic_bound_; }

 protected:
  // Symbol protected override.
  const ArrayType* AsArrayType() const override;

 private:
  FRIEND_REF_COUNTED_THREAD_SAFE(ArrayType);
  FRIEND_MAKE_REF_COUNTED(ArrayType);

  // The actual type (rather than a LazySymbol) is passed to this constructor because all Types
  // expect to have their size set as a member, and we can't compute the size of an array without
  // knowing the size of the contained elements.
  ArrayType(fxl::RefPtr<Type> value_type, std::optional<size_t> num_elts);
  ArrayType(fxl::RefPtr<Type> value_type, DynamicArrayBound dynamic_bound);
  ~ArrayType() override;

  std::string ComputeFullName() const override;

  // Normally array names are the contained type with a "[...]" on the end, but nested array
  // dimensions work in the other direction, so it will look like "array[outer][inner]". This
  // function takes a previously computed substring for what should be "[outer]" and creates the
  // final type name.
  std::string ComputeFullNameOfNestedArray(const std::string& outer_dims) const;

  const fxl::RefPtr<Type> value_type_;
  const std::optional<size_t> num_elts_;
  const std::optional<DynamicArrayBound> dynamic_bound_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_ARRAY_TYPE_H_
