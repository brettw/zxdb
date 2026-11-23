// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/expr/resolve_array.h"

#include <inttypes.h>

#include "src/developer/debug/shared/logging/logging.h"
#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/common/ref_ptr_to.h"
#include "src/developer/debug/zxdb/expr/eval_context.h"
#include "src/developer/debug/zxdb/expr/eval_dwarf_expr.h"
#include "src/developer/debug/zxdb/expr/expr_value.h"
#include "src/developer/debug/zxdb/expr/pretty_type.h"
#include "src/developer/debug/zxdb/expr/pretty_type_manager.h"
#include "src/developer/debug/zxdb/expr/resolve_ptr_ref.h"
#include "src/developer/debug/zxdb/symbols/arch.h"
#include "src/developer/debug/zxdb/symbols/array_type.h"
#include "src/developer/debug/zxdb/symbols/base_type.h"
#include "src/developer/debug/zxdb/symbols/collection.h"
#include "src/developer/debug/zxdb/symbols/data_member.h"
#include "src/developer/debug/zxdb/symbols/modified_type.h"
#include "src/developer/debug/zxdb/symbols/symbol_data_provider.h"
#include "src/developer/debug/zxdb/symbols/type.h"
#include "src/developer/debug/zxdb/symbols/unit_symbol_factory.h"
#include "src/developer/debug/zxdb/symbols/variable.h"

namespace zxdb {

namespace {

using IntCallback = fit::callback<void(ErrOr<int128_t>)>;

// Computes the value of a dynamic array bound in the current frame.
void EvalDynamicArrayBound(const fxl::RefPtr<EvalContext>& eval_context,
                           const SymbolContext& symbol_context, const DynamicArrayBound& bound,
                           IntCallback cb) {
  if (bound.expression) {
    // The result of the expression is the value (DWARF 5 section 2.19), so it's used for both
    // result types.
    auto on_complete = [cb = std::move(cb)](DwarfExprEval& eval, const Err& err) mutable {
      if (err.has_error())
        return cb(err);
      if (eval.GetResultType() == DwarfExprEval::ResultType::kData)
        return cb(Err("Unexpected result type from the array bound expression."));
      DwarfStackEntry result = eval.GetResult();
      if (result.TreatAsSigned())
        return cb(static_cast<int128_t>(result.signed_value()));
      cb(static_cast<int128_t>(result.unsigned_value()));
    };
    fxl::RefPtr<Symbol> source = bound.expression->source().Get();
    auto async_eval = fxl::MakeRefCounted<AsyncDwarfExprEval>(
        UnitSymbolFactory(source.get()), eval_context->GetDataProvider(), symbol_context,
        std::move(on_complete));
    async_eval->Eval(*bound.expression);
    return;
  }

  // The bound references a variable that holds the value.
  const Value* value = bound.variable.Get()->As<Value>();
  if (!value)
    return cb(Err("The size of the array isn't in a variable."));
  eval_context->GetVariableValue(
      RefPtrTo(value), [eval_context, cb = std::move(cb)](ErrOrValue result) mutable {
        if (result.has_error())
          return cb(result.err());

        // The type of the variable decides the signedness.
        fxl::RefPtr<Type> type = eval_context->GetConcreteType(result.value().type());
        const BaseType* base_type = type ? type->As<BaseType>() : nullptr;
        if (base_type && (base_type->base_type() == BaseType::kBaseTypeSigned ||
                          base_type->base_type() == BaseType::kBaseTypeSignedChar)) {
          int128_t signed_value = 0;
          if (Err err = result.value().PromoteTo128(&signed_value); err.has_error())
            return cb(err);
          return cb(signed_value);
        }
        uint128_t unsigned_value = 0;
        if (Err err = result.value().PromoteTo128(&unsigned_value); err.has_error())
          return cb(err);
        cb(static_cast<int128_t>(unsigned_value));
      });
}

enum class ArrayKind {
  kError,
  kStatic,   // int[4]
  kPointer,  // int*
};

struct ArrayInfo {
  ArrayKind kind = ArrayKind::kError;

  // Guaranteed for kind != kError.
  fxl::RefPtr<Type> original_value_type;  // Use for error messages.
  fxl::RefPtr<Type> concrete_value_type;  // Use to get the element count, etc.
  fxl::RefPtr<Type> erased_type;          // Use to get the size of one element, if the type does
                                          // not match |concrete_value_type|.

  // Valid when kind == kStatic.
  fxl::RefPtr<ArrayType> static_type;
};

// On success, the ArrayInfo will have kind != kError.
ErrOr<ArrayInfo> ClassifyArray(const fxl::RefPtr<EvalContext>& eval_context, const ExprValue& array,
                               const Type* erased_type) {
  if (!array.type())
    return Err("No type information.");

  ArrayInfo info;
  info.erased_type = RefPtrTo(erased_type);
  fxl::RefPtr<Type> concrete = eval_context->GetConcreteType(array.type());

  if (const ArrayType* array_type = concrete->As<ArrayType>()) {
    info.kind = ArrayKind::kStatic;
    info.static_type = RefPtrTo(array_type);

    info.original_value_type = RefPtrTo(array_type->value_type());
    info.concrete_value_type = eval_context->GetConcreteType(info.original_value_type);
    if (!info.concrete_value_type)
      return Err("Bad type information for '%s'.", array.type()->GetFullName().c_str());

    return info;
  } else if (const ModifiedType* modified_type = concrete->As<ModifiedType>()) {
    if (modified_type->tag() == DwarfTag::kPointerType) {
      info.kind = ArrayKind::kPointer;

      info.original_value_type = RefPtrTo(modified_type->modified().Get()->As<Type>());
      info.concrete_value_type = eval_context->GetConcreteType(info.original_value_type);

      if (!info.concrete_value_type)
        return Err("Bad type information for '%s'.", array.type()->GetFullName().c_str());

      return info;
    }
  }
  return Err("Not an array type.");
}

void ArrayFromPointer(const fxl::RefPtr<EvalContext>& eval_context, uint64_t begin_address,
                      fxl::RefPtr<Type> element_type, size_t element_count, EvalCallback cb) {
  fxl::RefPtr<Type> concrete_element_type = eval_context->GetConcreteType(element_type);
  if (!concrete_element_type)
    return cb(Err("Bad type information."));

  auto array_type = fxl::MakeRefCounted<ArrayType>(element_type, element_count);

  eval_context->GetDataProvider()->GetMemoryAsync(
      begin_address, array_type->byte_size(),
      [array_type, begin_address, cb = std::move(cb)](const Err& err,
                                                      std::vector<uint8_t> data) mutable {
        if (err.has_error())
          return cb(err);
        if (data.size() < array_type->byte_size())
          return cb(Err("Array memory not valid."));  // A short read indicates invalid memory.
        FX_DCHECK(data.size() == array_type->byte_size());

        cb(ExprValue(array_type, std::move(data), ExprValueSource(begin_address)));
      });
}

// Handles the "int[4]" case.
ErrOrValueVector ResolveStaticArray(const ExprValue& array, const ArrayInfo& info,
                                    size_t begin_index, size_t end_index) {
  if (array.data().size() < info.static_type->byte_size()) {
    return Err(
        "Array data (%zu bytes) is too small for the expected size "
        "(%u bytes).",
        array.data().size(), info.static_type->byte_size());
  }

  uint32_t type_size = info.concrete_value_type->byte_size();

  std::vector<ExprValue> result;
  result.reserve(end_index - begin_index);
  for (size_t i = begin_index; i < end_index; i++) {
    size_t begin_offset = i * type_size;
    if (begin_offset + type_size > array.data().size())
      break;

    // Describe the source of this data.
    ExprValueSource source = array.source();
    if (source.type() == ExprValueSource::Type::kMemory) {
      source = source.GetOffsetInto(begin_offset);
    } else if (source.type() == ExprValueSource::Type::kRegister) {
      // Vector register, compute the bit shifts for this subset. This assumes little-endian
      // so we can compute the bit shifts to write to the register from the left.
      source = ExprValueSource(source.register_id(), type_size * 8,
                               source.bit_shift() + (begin_offset * 8));
    }
    // else keep as original temporary/constant source.

    // Extract the array element data.
    std::optional<TaggedData> data = array.data().Extract(begin_offset, type_size);
    if (!data)
      return Err("Array data out of range.");
    result.emplace_back(info.original_value_type, std::move(*data), source);
  }
  return result;
}

// Handles the "Foo*" case.
void ResolvePointerArray(const fxl::RefPtr<EvalContext>& eval_context, const ExprValue& array,
                         const ArrayInfo& info, size_t begin_index, size_t end_index,
                         fit::callback<void(ErrOrValueVector)> cb) {
  // The address is stored in the contents of the array value.
  auto pointer_value_or = ExtractPointerValue(array);
  if (pointer_value_or.has_error())
    return cb(pointer_value_or.err());
  TargetPointer base_address = pointer_value_or.value();

  fxl::RefPtr<Type> type;
  if (info.erased_type) {
    type = info.erased_type;
  } else {
    type = info.concrete_value_type;
  }

  uint32_t type_size = type->byte_size();
  TargetPointer begin_address = base_address + type_size * begin_index;
  TargetPointer end_address = base_address + type_size * end_index;

  eval_context->GetDataProvider()->GetMemoryAsync(
      begin_address, end_address - begin_address,
      [begin_address, type, count = end_index - begin_index, cb = std::move(cb)](
          const Err& err, std::vector<uint8_t> data) mutable {
        if (err.has_error())
          return cb(err);

        // Convert returned raw memory to ExprValues.
        std::vector<ExprValue> result;
        result.reserve(count);
        uint32_t type_size = type->byte_size();

        for (size_t i = 0; i < count; i++) {
          size_t begin_offset = i * type_size;
          if (begin_offset + type_size > data.size())
            break;  // Ran out of data, leave remaining results uninitialized.

          std::vector<uint8_t> item_data(&data[begin_offset], &data[begin_offset + type_size]);
          result.emplace_back(type, std::move(item_data),
                              ExprValueSource(begin_address + begin_offset));
        }
        cb(std::move(result));
      });
}

// Backend for the single-item and async multiple item array resolution.
void DoResolveArray(const fxl::RefPtr<EvalContext>& eval_context, const ExprValue& array,
                    const ArrayInfo& info, size_t begin_index, size_t end_index,
                    fit::callback<void(ErrOrValueVector)> cb) {
  switch (info.kind) {
    case ArrayKind::kStatic:
      cb(ResolveStaticArray(array, info, begin_index, end_index));
      break;
    case ArrayKind::kPointer:
      ResolvePointerArray(eval_context, array, info, begin_index, end_index, std::move(cb));
      break;
    default:
      FX_NOTREACHED();
      break;
  }
}

}  // namespace

void ResolveArray(const fxl::RefPtr<EvalContext>& eval_context, const Type* erased_type,
                  const ExprValue& array, size_t begin_index, size_t end_index,
                  fit::callback<void(ErrOrValueVector)> cb) {
  auto info_or = ClassifyArray(eval_context, array, erased_type);
  if (info_or.has_error())
    return cb(info_or.err());

  DoResolveArray(eval_context, array, info_or.value(), begin_index, end_index, std::move(cb));
}

void ResolveArrayItem(const fxl::RefPtr<EvalContext>& eval_context, const ExprValue& array,
                      size_t index, EvalCallback cb) {
  auto info_or = ClassifyArray(eval_context, array, nullptr);
  if (info_or.ok()) {
    // Do regular array access.
    DoResolveArray(eval_context, array, info_or.value(), index, index + 1,
                   [cb = std::move(cb)](ErrOrValueVector result) mutable {
                     if (result.has_error()) {
                       cb(result.err());
                     } else if (result.value().empty()) {
                       // Short read.
                       cb(Err("Invalid array index."));
                     } else {
                       cb(std::move(result.value()[0]));  // Should have only one value.
                     }
                   });
  } else {
    // Not an array, check for pretty types that support array access.
    if (const PrettyType* pretty = eval_context->GetPrettyTypeManager().GetForType(array.type())) {
      if (auto array_access = pretty->GetArrayAccess())
        return array_access(eval_context, array, index, std::move(cb));
    }

    cb(Err("Can't resolve an array access on type '%s'.",
           array.type() ? array.type()->GetFullName().c_str() : "<Unknown>"));
  }
}

void CoerceArraySize(const fxl::RefPtr<EvalContext>& eval_context, const ExprValue& array,
                     size_t new_size, EvalCallback cb) {
  auto info_or = ClassifyArray(eval_context, array, nullptr);
  if (info_or.has_error())
    return cb(info_or.err());
  ArrayInfo& info = info_or.value();

  switch (info.kind) {
    case ArrayKind::kError:
      FX_NOTREACHED();  // Errors should be caught above.
      break;

    case ArrayKind::kStatic: {
      if (info.static_type->num_elts() && new_size <= *info.static_type->num_elts()) {
        // Shrinking a static array, can just extract the subrange.
        auto new_array_type = fxl::MakeRefCounted<ArrayType>(info.original_value_type, new_size);

        auto extracted = array.data().Extract(0, new_array_type->byte_size());
        if (!extracted)
          return cb(Err("Array contains less data than expected."));

        cb(ExprValue(std::move(new_array_type), std::move(*extracted), array.source()));
      } else {
        // Expanding a static array. This requires the memory be re-fetched.
        if (array.source().type() != ExprValueSource::Type::kMemory)
          return cb(Err("Can not expand array that is not in memory."));
        ArrayFromPointer(eval_context, array.source().address(), info.original_value_type, new_size,
                         std::move(cb));
      }
      break;
    }

    case ArrayKind::kPointer: {
      // Fetch the memory to convert to an array.
      auto pointer_value_or = ExtractPointerValue(array);
      if (pointer_value_or.has_error())
        return cb(pointer_value_or.err());

      fxl::RefPtr<Type> type = nullptr;
      if (info.erased_type) {
        type = info.erased_type;
      } else {
        type = info.original_value_type;
      }

      ArrayFromPointer(eval_context, pointer_value_or.value(), type, new_size, std::move(cb));
      break;
    }
  }
}

bool HasDynamicArrayBound(const fxl::RefPtr<EvalContext>& eval_context, const Type* type) {
  fxl::RefPtr<Type> concrete = eval_context->GetConcreteType(type);
  const ArrayType* array = concrete ? concrete->As<ArrayType>() : nullptr;
  if (!array)
    return false;
  return array->dynamic_bound() || HasDynamicArrayBound(eval_context, array->value_type());
}

void ResolveDynamicArrayType(const fxl::RefPtr<EvalContext>& eval_context,
                             const SymbolContext& symbol_context, fxl::RefPtr<Type> type,
                             fit::callback<void(ErrOr<fxl::RefPtr<Type>>)> cb) {
  fxl::RefPtr<Type> concrete = eval_context->GetConcreteType(type.get());
  const ArrayType* array = concrete ? concrete->As<ArrayType>() : nullptr;
  if (!array)
    return cb(std::move(type));

  // The inner dimensions are resolved first, because the size of this dimension's elements depends
  // on them.
  fxl::RefPtr<ArrayType> array_ref = RefPtrTo(array);
  ResolveDynamicArrayType(
      eval_context, symbol_context, RefPtrTo(array->value_type()),
      [eval_context, symbol_context, type, array_ref,
       cb = std::move(cb)](ErrOr<fxl::RefPtr<Type>> inner) mutable {
        if (inner.has_error())
          return cb(inner.err());
        fxl::RefPtr<Type> inner_type = inner.take_value();

        if (!array_ref->dynamic_bound()) {
          if (inner_type.get() == array_ref->value_type())
            return cb(std::move(type));  // No dynamic dimension.
          return cb(fxl::RefPtr<Type>(
              fxl::MakeRefCounted<ArrayType>(std::move(inner_type), array_ref->num_elts())));
        }

        const DynamicArrayBound& bound = *array_ref->dynamic_bound();
        EvalDynamicArrayBound(
            eval_context, symbol_context, bound,
            [inner_type = std::move(inner_type), kind = bound.kind, lower = bound.lower_bound,
             cb = std::move(cb)](ErrOr<int128_t> value) mutable {
              if (value.has_error())
                return cb(value.err());

              int128_t count = value.value();
              if (kind == DynamicArrayBound::Kind::kUpperBound)
                count = count - lower + 1;
              if (count < 0)
                return cb(Err("The array has a negative size (%" PRId64 ").",
                              static_cast<int64_t>(count)));

              uint64_t elt_size = inner_type->byte_size();
              if (elt_size > 0 && count > static_cast<int128_t>(kMaxDynamicArrayBytes / elt_size)) {
                return cb(Err("The array has %" PRId64 " elements, which is too large to read.",
                              static_cast<int64_t>(count)));
              }
              cb(fxl::RefPtr<Type>(fxl::MakeRefCounted<ArrayType>(std::move(inner_type),
                                                                  static_cast<size_t>(count))));
            });
      });
}

}  // namespace zxdb
