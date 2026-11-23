// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_VARIANT_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_VARIANT_H_

#include <optional>
#include <vector>

#include "src/developer/debug/zxdb/symbols/symbol.h"

namespace zxdb {

// A variant is one possible value of a "variant part".
//
// Each Variant contains a discriminant value which is a selector for this in the containing
// VariantPart, and the set of data inside it.
//
// See VariantPart for a full description.
class Variant final : public Symbol {
 public:
  // Construct with fxl::MakeRefCounted().

  // The discriminant value associated with this variant. See VariantPart.
  //
  // The discriminant value may be unset which indicates that this variant is the default one.
  //
  // The value is the bits of the discriminant data member of the VariantPart, zero-extended to 64
  // bits. This is how the evaluator reads the member, so a signed discriminant also matches: an
  // i8 of -1 is 0xff. The decoder keeps only the bits of the member's size, so the result doesn't
  // depend on how the producer encoded a negative value.
  const std::optional<uint64_t>& discr_value() const { return discr_value_; }

  // Data members. These should be DataMember objects. The offsets of the data members will be from
  // the structure containing the VariantPart.
  //
  // As of this writing, Rust (our only use-case for this) generates variants with exactly one data
  // member. If Rust has:
  //
  //   enum MyEnum {
  //     Foo,
  //     Bar(i32),
  //   }
  //
  // DWARF will define two structure types "MyEnum::Foo" (with no members) and "MyEnum::Bar" (with
  // one member) and each variant's data members will contain a DataMember of that type. The "name"
  // of these data members will match the type ("Foo" and "Bar" in this example).
  const std::vector<LazySymbol>& data_members() const { return data_members_; }

 protected:
  // Symbol overrides.
  const Variant* AsVariant() const override { return this; }

 private:
  FRIEND_REF_COUNTED_THREAD_SAFE(Variant);
  FRIEND_MAKE_REF_COUNTED(Variant);

  Variant(std::optional<uint64_t> discr_value, std::vector<LazySymbol> data_members);
  virtual ~Variant();

  std::optional<uint64_t> discr_value_;
  std::vector<LazySymbol> data_members_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_VARIANT_H_
