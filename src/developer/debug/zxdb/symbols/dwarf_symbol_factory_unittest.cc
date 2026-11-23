// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_symbol_factory.h"

#include <filesystem>
#include <initializer_list>

#include <gtest/gtest.h>

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "src/developer/debug/shared/string_util.h"
#include "src/developer/debug/zxdb/symbols/array_type.h"
#include "src/developer/debug/zxdb/symbols/base_type.h"
#include "src/developer/debug/zxdb/symbols/collection.h"
#include "src/developer/debug/zxdb/symbols/compile_unit.h"
#include "src/developer/debug/zxdb/symbols/data_member.h"
#include "src/developer/debug/zxdb/symbols/dwarf_binary_impl.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"
#include "src/developer/debug/zxdb/symbols/dwarf_op.h"
#include "src/developer/debug/zxdb/symbols/dwarf_test_util.h"
#include "src/developer/debug/zxdb/symbols/enumeration.h"
#include "src/developer/debug/zxdb/symbols/function.h"
#include "src/developer/debug/zxdb/symbols/function_type.h"
#include "src/developer/debug/zxdb/symbols/inherited_from.h"
#include "src/developer/debug/zxdb/symbols/input_location.h"
#include "src/developer/debug/zxdb/symbols/modified_type.h"
#include "src/developer/debug/zxdb/symbols/module_symbols_impl.h"
#include "src/developer/debug/zxdb/symbols/symbol.h"
#include "src/developer/debug/zxdb/symbols/template_parameter.h"
#include "src/developer/debug/zxdb/symbols/test_symbol_module.h"
#include "src/developer/debug/zxdb/symbols/variable.h"
#include "src/developer/debug/zxdb/symbols/variant.h"
#include "src/developer/debug/zxdb/symbols/variant_part.h"

namespace zxdb {

namespace {

const char kMyNsName[] = "my_ns";
const char kDoStructCallName[] = "DoStructCall";
const char kGetIntPtrName[] = "GetIntPtr";
const char kGetStructName[] = "GetStruct";
const char kGetStructMemberPtrName[] = "GetStructMemberPtr";
const char kPassRValueRefName[] = "PassRValueRef";
const char kCallInlineMemberName[] = "CallInlineMember";
const char kCallInlineName[] = "CallInline";

// Returns the function symbol with the given name. The name is assumed to exist as this function
// will EXPECT_* it to be valid.
//
// The name is passed as an initializer list representing the different identifier components, so
// for one function, use {"Foo"} and for one inside a namespace {"my_namespace", "Foo"}.
fxl::RefPtr<Function> GetFunctionWithName(ModuleSymbolsImpl* module_symbols,
                                          std::initializer_list<std::string> name_comps) {
  EXPECT_TRUE(name_comps.size() > 0);
  Identifier name_identifier;
  for (const auto& comp : name_comps)
    name_identifier.AppendComponent(IdentifierComponent(comp));

  std::vector<Location> locs = module_symbols->ResolveInputLocation(
      SymbolContext::ForRelativeAddresses(), InputLocation(name_identifier));
  EXPECT_EQ(1u, locs.size()) << "Expected exactly one function with this name: "
                             << name_identifier.GetFullName();
  if (locs.size() != 1)
    return nullptr;

  return RefPtrTo(locs[0].symbol().Get()->As<Function>());
}

}  // namespace

TEST(DwarfSymbolFactory, Function) {
  TestSymbolModule setup(TestSymbolModule::kBuilt);
  ASSERT_TRUE(setup.Init("/build_dir").ok());

  // Find the GetIntPtr function.
  fxl::RefPtr<Function> function = GetFunctionWithName(setup.symbols(), {kGetIntPtrName});
  ASSERT_TRUE(function);

  // Unmangled name.
  EXPECT_EQ(kGetIntPtrName, function->GetAssignedName());

  // Mangled name. This tries not to depend on the exact name mangling rules while validating that
  // it's reasonable. The mangled name shouldn't be exactly the same as the unmangled name, but
  // should at least contain it.
  EXPECT_NE(kGetIntPtrName, function->linkage_name());
  EXPECT_TRUE(debug::StringContains(function->linkage_name(), kGetIntPtrName));

  // Declaration location.
  EXPECT_TRUE(function->decl_line().is_valid());
  EXPECT_TRUE(debug::StringEndsWith(function->decl_line().file(), "/type_test.cc"))
      << function->decl_line().file();
  EXPECT_EQ(15, function->decl_line().line());
  EXPECT_EQ("/build_dir", function->decl_line().comp_dir());

  // Note: return type tested by ModifiedBaseType.
}

TEST(DwarfSymbolFactory, PtrToMemberFunction) {
  TestSymbolModule setup(TestSymbolModule::kBuilt);
  ASSERT_TRUE(setup.Init().ok());

  // Find the GetIntPtr function.
  fxl::RefPtr<const Function> get_function =
      GetFunctionWithName(setup.symbols(), {kMyNsName, kGetStructMemberPtrName});
  ASSERT_TRUE(get_function);

  // Get the return type, this is a typedef (because functions can't return pointers to member
  // functions).
  auto return_typedef = get_function->return_type().Get()->As<ModifiedType>();
  ASSERT_TRUE(return_typedef);

  // The typedef references the member pointer. The type name encapsulates all return values and
  // parameters so this tests everything at once.
  const Symbol* member = return_typedef->modified().Get();
  EXPECT_EQ("int (my_ns::Struct::*)(my_ns::Struct*, char)", member->GetFullName());
}

TEST(DwarfSymbolFactory, InlinedMemberFunction) {
  TestSymbolModule setup(TestSymbolModule::kBuilt);
  ASSERT_TRUE(setup.Init().ok());

  // Find the CallInline function.
  fxl::RefPtr<const Function> call_function =
      GetFunctionWithName(setup.symbols(), {kCallInlineMemberName});
  ASSERT_TRUE(call_function);

  // It should have one inner block that's the inline function.
  ASSERT_EQ(1u, call_function->inner_blocks().size());
  const Function* inline_func = call_function->inner_blocks()[0].Get()->As<Function>();
  ASSERT_TRUE(inline_func);
  EXPECT_EQ(DwarfTag::kInlinedSubroutine, inline_func->tag());

  EXPECT_EQ("ForInline::InlinedFunction", inline_func->GetFullName());

  // The inline function should have two parameters, "this" and "param".  ASSERT_EQ(2u,
  // inline_func->parameters().size());
  const Variable* param0 = inline_func->parameters()[0].Get()->As<Variable>();
  ASSERT_TRUE(param0);
  const Variable* param1 = inline_func->parameters()[1].Get()->As<Variable>();
  ASSERT_TRUE(param1);

  // They can appear in either order. Because it's an inlined function, the abtract origin can
  // contribute to these parameters which can mean the iteration order is not clearly defined.
  // Normally the parameters are in order.
  const Variable* this_param = nullptr;
  if (param0->GetAssignedName() == "this") {
    this_param = param0;
    EXPECT_EQ("param", param1->GetAssignedName());
  } else {
    EXPECT_EQ("param", param0->GetAssignedName());
    EXPECT_EQ("this", param1->GetAssignedName());
    this_param = param1;
  }

  // The object pointer on the function should refer to the "this" pointer retrieved above. This is
  // tricky because normally the object pointer is on the "abstract origin" of the inlined routine,
  // and will refer to a "this" parameter specified on the abstract origin. We need to correlate it
  // to the one on the inlined instance to get the location correct.
  EXPECT_EQ(this_param, inline_func->GetObjectPointerVariable());
}

TEST(DwarfSymbolFactory, InlinedFunction) {
  TestSymbolModule setup(TestSymbolModule::kBuilt);
  ASSERT_TRUE(setup.Init().ok());

  // Find the CallInline function.
  fxl::RefPtr<const Function> call_function =
      GetFunctionWithName(setup.symbols(), {kCallInlineName});
  ASSERT_TRUE(call_function);

  // It should have one inner block that's the inline function.
  ASSERT_EQ(1u, call_function->inner_blocks().size());
  const Function* inline_func = call_function->inner_blocks()[0].Get()->As<Function>();
  ASSERT_TRUE(inline_func);
  EXPECT_EQ(DwarfTag::kInlinedSubroutine, inline_func->tag());

  // Parameter decoding is tested by the InlinedMemberFunction test above. Here we care that the
  // enclosing namespace is correct because of the different ways these inlined routines are
  // declared.
  EXPECT_EQ("my_ns::InlinedFunction", inline_func->GetFullName());

  // Check the declaration location. This mostly verifies that the path name is relative to the
  // build directory and that there aren't any unnecessary "." or "..".
  EXPECT_EQ("../../src/developer/debug/zxdb/symbols/test_data/cpp/type_test.cc",
            inline_func->decl_line().file());

  // The containing block of the inline function should be the calling function. Note that the
  // objects may not be the same.
  ASSERT_TRUE(inline_func->containing_block());
  auto containing_block = inline_func->containing_block().Get();
  auto containing_func = containing_block->As<Function>();
  ASSERT_TRUE(containing_func);
  EXPECT_EQ(kCallInlineName, containing_func->GetFullName());
}

TEST(DwarfSymbolFactory, ModifiedBaseType) {
  TestSymbolModule setup(TestSymbolModule::kBuilt);
  ASSERT_TRUE(setup.Init().ok());

  // Find the GetIntPtr function.
  fxl::RefPtr<const Function> function = GetFunctionWithName(setup.symbols(), {kGetIntPtrName});
  ASSERT_TRUE(function);

  // Get the return type, this references a "pointer" modifier.
  EXPECT_TRUE(function->return_type().is_valid());
  const ModifiedType* ptr_mod = function->return_type().Get()->As<ModifiedType>();
  ASSERT_TRUE(ptr_mod) << "Tag = " << static_cast<int>(function->return_type().Get()->tag());
  EXPECT_EQ(DwarfTag::kPointerType, ptr_mod->tag());
  EXPECT_EQ("const int*", ptr_mod->GetFullName());

  // The modified type should be a "const" modifier.
  const ModifiedType* const_mod = ptr_mod->modified().Get()->As<ModifiedType>();
  ASSERT_TRUE(const_mod) << "Tag = " << static_cast<int>(function->return_type().Get()->tag());
  EXPECT_EQ(DwarfTag::kConstType, const_mod->tag());
  EXPECT_EQ("const int", const_mod->GetFullName());

  // The modified type should be the int base type.
  const BaseType* base = const_mod->modified().Get()->As<BaseType>();
  ASSERT_TRUE(base);
  EXPECT_EQ(DwarfTag::kBaseType, base->tag());
  EXPECT_EQ("int", base->GetFullName());

  // Validate the BaseType parameters.
  EXPECT_EQ(BaseType::kBaseTypeSigned, base->base_type());
  EXPECT_EQ("int", base->GetAssignedName());
  // Try to be flexible about the size of ints on the platform.
  EXPECT_TRUE(base->byte_size() == 4 || base->byte_size() == 8);
}

TEST(DwarfSymbolFactory, RValueRef) {
  TestSymbolModule setup(TestSymbolModule::kBuilt);
  ASSERT_TRUE(setup.Init().ok());

  // Find the GetIntPtr function.
  fxl::RefPtr<const Function> function =
      GetFunctionWithName(setup.symbols(), {kMyNsName, kPassRValueRefName});
  ASSERT_TRUE(function);

  // Should have one parameter of rvalue ref type.
  ASSERT_EQ(1u, function->parameters().size());
  const Variable* var = function->parameters()[0].Get()->As<Variable>();
  ASSERT_TRUE(var);
  const ModifiedType* modified = var->type().Get()->As<ModifiedType>();
  ASSERT_TRUE(modified);
  EXPECT_EQ(DwarfTag::kRvalueReferenceType, modified->tag());

  EXPECT_EQ("int&&", modified->GetFullName());
}

TEST(DwarfSymbolFactory, ArrayType) {
  TestSymbolModule setup(TestSymbolModule::kBuilt);
  ASSERT_TRUE(setup.Init().ok());

  // Find the GetString function.
  const char kGetString[] = "GetString";
  fxl::RefPtr<const Function> function = GetFunctionWithName(setup.symbols(), {kGetString});
  ASSERT_TRUE(function);

  // Find the "str_array" variable in the function.
  ASSERT_EQ(1u, function->variables().size());
  const Variable* str_array = function->variables()[0].Get()->As<Variable>();
  ASSERT_TRUE(str_array);
  EXPECT_EQ("str_array", str_array->GetAssignedName());

  // It should be an array type with length 14.
  const ArrayType* array_type = str_array->type().Get()->As<ArrayType>();
  ASSERT_TRUE(array_type);
  EXPECT_EQ(14u, array_type->num_elts());
  EXPECT_EQ("const char[14]", array_type->GetFullName());

  // The inner type should be a "char".
  const Type* elt_type = array_type->value_type();
  ASSERT_TRUE(elt_type);
  EXPECT_EQ("const char", elt_type->GetFullName());
}

TEST(DwarfSymbolFactory, Array2D) {
  TestSymbolModule setup(TestSymbolModule::kBuilt);
  ASSERT_TRUE(setup.Init().ok());

  // Find the My2DArray function.
  const char kMy2DArray[] = "My2DArray";
  fxl::RefPtr<const Function> function = GetFunctionWithName(setup.symbols(), {kMy2DArray});
  ASSERT_TRUE(function);

  // Find the "array" variable in the function. It's declared as:
  //
  //   volatile int array[3][4]
  //
  ASSERT_EQ(1u, function->variables().size());
  const Variable* array = function->variables()[0].Get()->As<Variable>();
  ASSERT_TRUE(array);
  EXPECT_EQ("array", array->GetAssignedName());

  // It should be an array type with length 3 (outer dimension).
  const ArrayType* outer_array_type = array->type().Get()->As<ArrayType>();
  ASSERT_TRUE(outer_array_type);
  EXPECT_EQ(3u, outer_array_type->num_elts());
  EXPECT_EQ("volatile int[3][4]", outer_array_type->GetFullName());

  // The inner array type should be a int[4].
  const Type* inner_type = outer_array_type->value_type();
  ASSERT_TRUE(inner_type);
  const ArrayType* inner_array_type = inner_type->As<ArrayType>();
  ASSERT_TRUE(inner_array_type);
  EXPECT_EQ(4u, inner_array_type->num_elts());
  EXPECT_EQ("volatile int[4]", inner_array_type->GetFullName());

  // The final contained type type should be a "volatile int".
  const Type* elt_type = inner_array_type->value_type();
  ASSERT_TRUE(elt_type);
  EXPECT_EQ("volatile int", elt_type->GetFullName());
}

// TODO(https://fxbug.dev/42085764): A recent compiler change modified the way the DWARF is
// emitted for DW_AT_const_value, causing this test to fail. There's discussion going on
// upstream (see the linked upstream llvm bug) that will restore the emitted DWARF to be the
// same as it was before, so no action is being taken to change the behavior here until that's
// figured out.
//
// The new output changes the struct to have only 3 members, where "kConstInt" and
// "kConstLongDouble" to be declared with a DW_TAG_variable outside of the struct:
//
// clang-format off
// 0x00000136:   DW_TAG_variable
//                 DW_AT_specification	(... "kConstInt")
//                 DW_AT_const_value	(...)
//
// and in the struct:
//
// 0x00000175:   DW_TAG_variable
//                 DW_AT_name	("kConstInt")
//                 DW_AT_type	(0x0000027f "const int")
//                 DW_AT_decl_file ("./../../src/developer/debug/zxdb/symbols/test_data/cpp/type_test.cc")
//                 DW_AT_decl_line	(36)
//                 DW_AT_external	(true)
//                 DW_AT_declaration  (true)
// clang-format on
//
// "kConstLongDouble" is treated similarly.
//
// Once the upstream changes are finalized either with the new model or restoring the old
// model, we can enable this test again.
TEST(DwarfSymbolFactory, DISABLED_Collection) {
  TestSymbolModule setup(TestSymbolModule::kBuilt);
  ASSERT_TRUE(setup.Init().ok());

  // Find the GetStruct function.
  fxl::RefPtr<const Function> function =
      GetFunctionWithName(setup.symbols(), {kMyNsName, kGetStructName});
  ASSERT_TRUE(function);

  // The return type should be the struct.
  auto* struct_type = function->return_type().Get()->As<Collection>();
  ASSERT_TRUE(struct_type);
  EXPECT_EQ("my_ns::Struct", struct_type->GetFullName());

  // The struct has five data members and two base classes.
  ASSERT_EQ(5u, struct_type->data_members().size());
  ASSERT_EQ(2u, struct_type->inherited_from().size());

  // The first thing should be Base1 at offset 0.
  auto* base1 = struct_type->inherited_from()[0].Get()->As<InheritedFrom>();
  ASSERT_TRUE(base1);
  auto* base1_type = base1->from().Get()->As<Type>();
  EXPECT_EQ("my_ns::Base1", base1_type->GetFullName());
  EXPECT_EQ(InheritedFrom::kConstant, base1->kind());
  EXPECT_EQ(0u, base1->offset());

  // It should be followed by Base2. To allow flexibility in packing without breaking this test, all
  // offsets below check only that the offset is greater than the previous one and a multiple of 4.
  auto* base2 = struct_type->inherited_from()[1].Get()->As<InheritedFrom>();
  ASSERT_TRUE(base2);
  auto* base2_type = base2->from().Get()->As<Type>();
  EXPECT_EQ("my_ns::Base2", base2_type->GetFullName());
  EXPECT_EQ(InheritedFrom::kConstant, base2->kind());
  EXPECT_LT(0u, base2->offset());
  EXPECT_TRUE(base2->offset() % 4 == 0);

  // The base classes should be followed by the data members on the struct.
  auto* member_a = struct_type->data_members()[0].Get()->As<DataMember>();
  ASSERT_TRUE(member_a);
  auto* member_a_type = member_a->type().Get()->As<Type>();
  EXPECT_EQ("int", member_a_type->GetFullName());
  EXPECT_LT(base2->offset(), member_a->member_location());
  EXPECT_TRUE(member_a->member_location() % 4 == 0);

  // The second data member should be "Struct* member_b".
  auto* member_b = struct_type->data_members()[1].Get()->As<DataMember>();
  ASSERT_TRUE(member_b);
  auto* member_b_type = member_b->type().Get()->As<Type>();
  EXPECT_EQ("my_ns::Struct*", member_b_type->GetFullName());
  EXPECT_LT(member_a->member_location(), member_b->member_location());
  EXPECT_TRUE(member_b->member_location() % 4 == 0);

  // The third data member is "const void* v". Void is weird because it will be represented as a
  // modified pointer type of nothing.
  auto* member_v = struct_type->data_members()[2].Get()->As<DataMember>();
  ASSERT_TRUE(member_v);
  auto* member_v_type = member_v->type().Get()->As<Type>();
  EXPECT_EQ("const void*", member_v_type->GetFullName());
  EXPECT_LT(member_b->member_location(), member_v->member_location());
  EXPECT_TRUE(member_v->member_location() % 4 == 0);

  // The next data member should be kConstInt = -2. This is stored in a ConstValue as a
  // little-endian 64-bit signed value.
  //
  // This assumes the compiler has encoded the constexpr as a DW_AT_const_value. It's theoretically
  // possible for the constant value to be encoded as a DWARF expression but none of our compilers
  // currently do that and we really want to test ConstValue here.
  auto* member_ci = struct_type->data_members()[3].Get()->As<DataMember>();
  ASSERT_TRUE(member_ci);
  EXPECT_TRUE(member_ci->is_external());
  EXPECT_EQ("const int", member_ci->type().Get()->As<Type>()->GetFullName());
  EXPECT_TRUE(member_ci->const_value().has_value());
  std::vector<uint8_t> expected_minus_two{0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  EXPECT_EQ(expected_minus_two, member_ci->const_value().GetConstValue(8));

  // kConstLongDouble (see kConstInt above for notes).
  auto* member_cd = struct_type->data_members()[4].Get()->As<DataMember>();
  ASSERT_TRUE(member_cd);
  EXPECT_TRUE(member_cd->is_external());
  EXPECT_EQ("const long double", member_cd->type().Get()->As<Type>()->GetFullName());
  EXPECT_TRUE(member_cd->const_value().has_value());
  // This is a "long double" which can vary according to platform and compiler settings. Accept the
  // standard encodings for 64, 80, and 128 bits.
  std::vector<uint8_t> expected_64bit{0x1f, 0x85, 0xeb, 0x51, 0xb8, 0x1e, 0x09, 0x40};
  std::vector<uint8_t> expected_80bit{0, 0xf8, 0x28, 0x5c, 0x8f, 0xc2, 0xf5, 0xc8, 0, 0x40};
  std::vector<uint8_t> expected_128bit{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0,
                                       0x51, 0xb8, 0x1e, 0x85, 0xeb, 0x91, 0x00, 0x40};
  EXPECT_TRUE(expected_64bit == member_cd->const_value().GetConstValue(8) ||
              expected_80bit == member_cd->const_value().GetConstValue(10) ||
              expected_128bit == member_cd->const_value().GetConstValue(16));
}

// Covers cases of InheritedFrom not covered by the collection test above.
TEST(DwarfSymbolFactory, InheritedFrom) {
  TestSymbolModule setup(TestSymbolModule::kBuilt);
  ASSERT_TRUE(setup.Init().ok());

  const char kGetVirtualDerived[] = "GetVirtualDerived";
  fxl::RefPtr<const Function> function = GetFunctionWithName(setup.symbols(), {kGetVirtualDerived});
  ASSERT_TRUE(function);

  auto* derived_type = function->return_type().Get()->As<Collection>();
  ASSERT_TRUE(derived_type);
  EXPECT_EQ("VirtualDerived", derived_type->GetFullName());

  ASSERT_EQ(1u, derived_type->inherited_from().size());
  const InheritedFrom* inherited = derived_type->inherited_from()[0].Get()->As<InheritedFrom>();
  ASSERT_TRUE(inherited);

  // Validate that it has a nonempty expression. This test doesn't require that the expression
  // be a specific thing.
  EXPECT_EQ(InheritedFrom::kExpression, inherited->kind());
  EXPECT_FALSE(inherited->location_expression().data().empty());
}

TEST(DwarfSymbolFactory, Enum) {
  TestSymbolModule setup(TestSymbolModule::kBuilt);
  ASSERT_TRUE(setup.Init().ok());

  // Find the GetStruct function.
  const char kGetStruct[] = "GetStructWithEnums";
  fxl::RefPtr<const Function> function = GetFunctionWithName(setup.symbols(), {kGetStruct});
  ASSERT_TRUE(function);

  // The return type should be the struct.
  auto* struct_type = function->return_type().Get()->As<Collection>();
  ASSERT_TRUE(struct_type);
  EXPECT_EQ("StructWithEnums", struct_type->GetFullName());

  // There are three enum members defined in the struct.
  ASSERT_EQ(3u, struct_type->data_members().size());

  // First is a regular enum with no values.
  auto regular_enum =
      struct_type->data_members()[0].Get()->As<DataMember>()->type().Get()->As<Enumeration>();
  ASSERT_TRUE(regular_enum);
  EXPECT_EQ("StructWithEnums::RegularEnum", regular_enum->GetFullName());
  EXPECT_TRUE(regular_enum->values().empty());

  // Second is an anonymous signed enum with two values. We don't bother to test the enumerator
  // values on this one since some aspects will be compiler-dependent.
  auto anon_enum =
      struct_type->data_members()[1].Get()->As<DataMember>()->type().Get()->As<Enumeration>();
  ASSERT_TRUE(anon_enum);
  EXPECT_EQ("StructWithEnums::(anon enum)", anon_enum->GetFullName());
  EXPECT_TRUE(anon_enum->is_signed());
  EXPECT_EQ(2u, anon_enum->values().size());

  // Third is a type enum with two values.
  auto typed_enum =
      struct_type->data_members()[2].Get()->As<DataMember>()->type().Get()->As<Enumeration>();
  ASSERT_TRUE(typed_enum);
  EXPECT_EQ("StructWithEnums::TypedEnum", typed_enum->GetFullName());
  EXPECT_TRUE(typed_enum->is_signed());
  ASSERT_EQ(2u, typed_enum->values().size());
  EXPECT_EQ(1u, typed_enum->byte_size());

  // Since this is typed, the values should be known. The map contains the signed values casted to
  // an unsigned int.
  auto first_value = *typed_enum->values().begin();
  auto second_value = *(++typed_enum->values().begin());
  EXPECT_EQ(1u, first_value.first);
  EXPECT_EQ("TYPED_B", first_value.second);
  EXPECT_EQ(static_cast<uint64_t>(-1), second_value.first);
  EXPECT_EQ("TYPED_A", second_value.second);
}

// Tests nested code blocks, variables, and parameters.
TEST(DwarfSymbolFactory, CodeBlocks) {
  TestSymbolModule setup(TestSymbolModule::kBuilt);
  ASSERT_TRUE(setup.Init().ok());

  // Find the DoStructCall function.
  fxl::RefPtr<const Function> function =
      GetFunctionWithName(setup.symbols(), {kMyNsName, kDoStructCallName});
  ASSERT_TRUE(function);

  // It should have two parameters, arg1 and arg2.
  const Variable* struct_arg = nullptr;
  const Variable* int_arg = nullptr;
  ASSERT_EQ(2u, function->parameters().size());
  for (const auto& param : function->parameters()) {
    const Variable* cur_var = param.Get()->As<Variable>();
    ASSERT_TRUE(cur_var);  // Each parameter should decode to a variable.
    if (cur_var->GetAssignedName() == "arg1")
      struct_arg = cur_var;
    else if (cur_var->GetAssignedName() == "arg2")
      int_arg = cur_var;
  }

  // Both args should have valid locations with non-empty expressions. This doesn't test the actual
  // programs because that could vary by build.
  EXPECT_FALSE(struct_arg->location().is_null());
  EXPECT_FALSE(int_arg->location().is_null());

  // Validate the arg1 type (const Struct&).
  ASSERT_TRUE(struct_arg);
  const Type* struct_arg_type = struct_arg->type().Get()->As<Type>();
  ASSERT_TRUE(struct_arg_type);
  EXPECT_EQ("const my_ns::Struct&", struct_arg_type->GetFullName());

  // Validate the arg2 type (int).
  ASSERT_TRUE(int_arg);
  const Type* int_arg_type = int_arg->type().Get()->As<Type>();
  ASSERT_TRUE(int_arg_type);
  EXPECT_EQ("int", int_arg_type->GetFullName());

  // The function block should have one variable (var1).
  ASSERT_EQ(1u, function->variables().size());
  const Variable* var1 = function->variables()[0].Get()->As<Variable>();
  ASSERT_TRUE(var1);
  const Type* var1_type = var1->type().Get()->As<Type>();
  ASSERT_TRUE(var1_type);
  EXPECT_EQ("volatile int", var1_type->GetFullName());

  // There should be one child lexical scope.
  ASSERT_EQ(1u, function->inner_blocks().size());
  const CodeBlock* inner = function->inner_blocks()[0].Get()->As<CodeBlock>();

  // The lexical scope should have one child variable.
  ASSERT_EQ(1u, inner->variables().size());
  const Variable* var2 = inner->variables()[0].Get()->As<Variable>();
  ASSERT_TRUE(var2);
  const Type* var2_type = var2->type().Get()->As<Type>();
  ASSERT_TRUE(var2_type);
  EXPECT_EQ("volatile my_ns::Struct", var2_type->GetFullName());

  // The lexical scope should have no other children.
  EXPECT_TRUE(inner->inner_blocks().empty());
}

// Tests both nullptr_t and typedef decoding (which is how it's encoded).
TEST(DwarfSymbolFactory, NullPtrTTypedef) {
  TestSymbolModule setup(TestSymbolModule::kBuilt);
  ASSERT_TRUE(setup.Init().ok());

  // Find the GetNullPtrT function.
  const char kGetNullPtrT[] = "GetNullPtrT";
  fxl::RefPtr<const Function> function = GetFunctionWithName(setup.symbols(), {kGetNullPtrT});
  ASSERT_TRUE(function);

  // TODO(https://fxbug.dev/380087647): Reenable the rest of the test after toolchain roll.

  // The return type should be nullptr_t.
  // auto* nullptr_t_type = function->return_type().Get()->As<Type>();
  // ASSERT_TRUE(nullptr_t_type);
  // EXPECT_EQ("nullptr_t", nullptr_t_type->GetFullName());

  // The standard defined nullptr_t as "typedef decltype(nullptr) nullptr_t"
  // auto* typedef_type = nullptr_t_type->As<ModifiedType>();
  // ASSERT_TRUE(typedef_type);
  // EXPECT_EQ(DwarfTag::kTypedef, typedef_type->tag());

  // Check the type underlying the typedef.
  // auto* underlying = typedef_type->modified().Get()->As<Type>();
  // ASSERT_TRUE(underlying);
  // EXPECT_EQ("decltype(nullptr)", underlying->GetFullName());

  // Currently Clang defines this as an "unspecified" type. Since this isn't specified, it's
  // possible this may change in the future, but if it does we need to check to make sure everything
  // works properly.
  // EXPECT_EQ(DwarfTag::kUnspecifiedType, underlying->tag());

  // // The decoder should have forced the size to be the size of a pointer.
  // EXPECT_EQ(8u, underlying->byte_size());
}

TEST(DwarfSymbolFactory, TemplateParams) {
  TestSymbolModule setup(TestSymbolModule::kBuilt);
  ASSERT_TRUE(setup.Init().ok());

  // Find the GetTemplate() function.
  const char kGetTemplate[] = "GetTemplate";
  fxl::RefPtr<const Function> function = GetFunctionWithName(setup.symbols(), {kGetTemplate});
  ASSERT_TRUE(function);

  // The return type should be our collection
  auto* my_template = function->return_type().Get()->As<Collection>();
  ASSERT_TRUE(my_template);
  EXPECT_EQ("MyTemplate<my_ns::Struct, 42>", my_template->GetFullName());

  // There should be two template parameters.
  ASSERT_EQ(2u, my_template->template_params().size());

  // The first one is "T = my_ns::Struct".
  auto first_param = my_template->template_params()[0].Get()->As<TemplateParameter>();
  ASSERT_TRUE(first_param);
  EXPECT_EQ("T", first_param->GetAssignedName());
  EXPECT_EQ("T", first_param->GetFullName());

  auto first_type = first_param->type().Get()->As<Type>();
  ASSERT_TRUE(first_type);
  EXPECT_EQ("my_ns::Struct", first_type->GetFullName());

  // The second one is "i = int(42)".
  auto second_param = my_template->template_params()[1].Get()->As<TemplateParameter>();
  ASSERT_TRUE(second_param);
  EXPECT_EQ("i", second_param->GetAssignedName());
  EXPECT_EQ("i", second_param->GetFullName());

  auto second_type = second_param->type().Get()->As<Type>();
  ASSERT_TRUE(second_type);
  EXPECT_EQ("int", second_type->GetFullName());
}

TEST(DwarfSymbolFactory, TypeUnitReference) {
  // GCC references types in type units directly with DW_FORM_ref_sig8. The DWARF 4 fixture has the
  // type units in .debug_types and the DWARF 5 fixture has them in .debug_info.
  bool found_fixture = false;
  for (const char* name : {"gcc_dwarf4", "gcc_dwarf5"}) {
    SCOPED_TRACE(name);
    std::string path = TestSymbolModule::GetTestDataDir() + "type_unit/" + name + ".so";
    if (!std::filesystem::exists(path))
      continue;  // The GCC fixtures are optional.
    found_fixture = true;

    TestSymbolModule setup(path, "");
    ASSERT_TRUE(setup.Init().ok());

    // Check that the fixture has the reference form that this test is for.
    llvm::DWARFContext* context = setup.symbols()->binary()->GetLLVMContext();
    llvm::DWARFDie var_die;
    for (const std::unique_ptr<llvm::DWARFUnit>& unit : context->info_section_units()) {
      if (unit->isTypeUnit())
        continue;
      var_die =
          GetFirstDieOfTagAndName(context, unit.get(), DwarfTag::kVariable, "type_unit_outer");
      if (var_die)
        break;
    }
    ASSERT_TRUE(var_die);
    static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kType};
    DwarfDieDecoder decoder(context, kAttrs);
    ASSERT_TRUE(decoder.Decode(var_die));
    const DwarfDieDecoder::AttributeValue* type_value = decoder.GetValue(DwarfAttr::kType);
    ASSERT_TRUE(type_value);
    ASSERT_EQ(DwarfForm::kRefSig8, static_cast<DwarfForm>(type_value->form.getForm()));

    ResolveOptions options;
    options.symbolize = true;
    std::vector<Location> locs = setup.symbols()->ResolveInputLocation(
        SymbolContext::ForRelativeAddresses(),
        InputLocation(TestSymbolModule::SplitName("type_unit_outer")), options);
    ASSERT_EQ(1u, locs.size());
    const Variable* var = locs[0].symbol().Get()->As<Variable>();
    ASSERT_TRUE(var);

    const Collection* outer = var->type().Get()->As<Collection>();
    ASSERT_TRUE(outer);
    EXPECT_EQ("TypeUnitOuter", outer->GetFullName());
    ASSERT_EQ(2u, outer->data_members().size());

    const DataMember* inner = outer->data_members()[0].Get()->As<DataMember>();
    ASSERT_TRUE(inner);
    EXPECT_EQ("inner", inner->GetAssignedName());
    EXPECT_EQ("TypeUnitInner", inner->type().Get()->GetFullName());

    const DataMember* ptr = outer->data_members()[1].Get()->As<DataMember>();
    ASSERT_TRUE(ptr);
    EXPECT_EQ("ptr", ptr->GetAssignedName());
    EXPECT_EQ("TypeUnitInner*", ptr->type().Get()->GetFullName());
  }
  if (!found_fixture)
    GTEST_SKIP() << "The GCC type unit fixtures weren't built.";
}

TEST(DwarfSymbolFactory, AddrxOffsetLowPc) {
  // A DW_AT_low_pc of DW_FORM_LLVM_addrx_offset is an address table entry plus an offset.
  std::string path = TestSymbolModule::GetTestDataDir() + "addrx_offset/clang_addrx_offset.so";
  if (!std::filesystem::exists(path))
    GTEST_SKIP() << "The fixture needs Clang.";

  TestSymbolModule setup(path, "");
  ASSERT_TRUE(setup.Init().ok());
  llvm::DWARFContext* context = setup.symbols()->binary()->GetLLVMContext();
  const std::map<std::string, llvm::ELF::Elf64_Sym>& elf_symbols =
      setup.symbols()->binary()->GetELFSymbols();

  int addrx_offset_count = 0;
  for (const char* name : {"AddrxOffsetFirst", "AddrxOffsetSecond", "AddrxOffsetThird"}) {
    SCOPED_TRACE(name);

    // Count the functions whose fixture uses the form that this test is for.
    llvm::DWARFDie die =
        GetFirstDieOfTagAndName(context, context->getUnitAtIndex(0), DwarfTag::kSubprogram, name);
    ASSERT_TRUE(die);
    static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kLowPc};
    DwarfDieDecoder decoder(context, kAttrs);
    ASSERT_TRUE(decoder.Decode(die));
    const DwarfDieDecoder::AttributeValue* low_pc = decoder.GetValue(DwarfAttr::kLowPc);
    ASSERT_TRUE(low_pc);
    if (static_cast<DwarfForm>(low_pc->form.getForm()) == DwarfForm::kLlvmAddrxOffset)
      addrx_offset_count++;

    // The function starts at the address of its ELF symbol.
    fxl::RefPtr<Function> function = GetFunctionWithName(setup.symbols(), {name});
    ASSERT_TRUE(function);
    auto elf_symbol = elf_symbols.find(function->linkage_name());
    ASSERT_NE(elf_symbols.end(), elf_symbol);
    ASSERT_FALSE(function->code_ranges().empty());
    EXPECT_EQ(elf_symbol->second.st_value, function->code_ranges()[0].begin());
  }
  EXPECT_EQ(2, addrx_offset_count);
}

namespace {

// Loads a binary of hand-written DWARF from test_data/<name>/<name>_fixture.s. See that file for
// the DIEs.
class AssemblyFixture {
 public:
  explicit AssemblyFixture(const std::string& name)
      : setup_(TestSymbolModule::GetTestDataDir() + name + "/" + name + ".so", "") {}

  bool Init() {
    if (!setup_.Init().ok())
      return false;
    context_ = setup_.symbols()->binary()->GetLLVMContext();
    return context_ && context_->getNumCompileUnits() == 2;
  }

  // Returns the symbol for the child with the given index of the given unit.
  fxl::RefPtr<Symbol> UnitChild(unsigned unit, size_t index) {
    llvm::DWARFDie die = context_->getUnitAtIndex(unit)->getUnitDIE(false).getFirstChild();
    for (size_t i = 0; i < index && die; i++)
      die = die.getSibling();
    if (!die)
      return nullptr;
    return setup_.symbols()->GetSymbolFactory()->CreateSymbol(DwarfDieRef::Main(die.getOffset()));
  }

 private:
  TestSymbolModule setup_;
  llvm::DWARFContext* context_ = nullptr;
};

// Indices of the children of the units in the fixture.
constexpr size_t kInnerDefinitionIndex = 2;
constexpr size_t kConcreteMethodIndex = 4;
constexpr size_t kStaticDefinitionIndex = 5;
constexpr size_t kPairOnlyIndex = 6;

}  // namespace

TEST(DwarfSymbolFactory, SpecificationOfType) {
  // A type definition outside its class gets the class as its parent from its declaration.
  AssemblyFixture fixture("specification");
  ASSERT_TRUE(fixture.Init());
  fxl::RefPtr<Symbol> symbol = fixture.UnitChild(0, kInnerDefinitionIndex);
  ASSERT_TRUE(symbol);
  const Collection* inner = symbol->As<Collection>();
  ASSERT_TRUE(inner);
  EXPECT_EQ("Outer::Inner", inner->GetFullName());
  EXPECT_EQ(4u, inner->byte_size());
  EXPECT_EQ(1u, inner->data_members().size());
}

TEST(DwarfSymbolFactory, SpecificationThroughAbstractOrigin) {
  // concrete instance -> (abstract origin) abstract instance -> (specification) declaration.
  AssemblyFixture fixture("specification");
  ASSERT_TRUE(fixture.Init());
  fxl::RefPtr<Symbol> symbol = fixture.UnitChild(0, kConcreteMethodIndex);
  ASSERT_TRUE(symbol);
  const Function* function = symbol->As<Function>();
  ASSERT_TRUE(function);
  EXPECT_EQ("Outer::Method", function->GetFullName());
  ASSERT_EQ(1u, function->code_ranges().size());
  EXPECT_EQ(4u, function->code_ranges()[0].size());

  // The abstract instance has a static local, so the concrete instance has it too.
  ASSERT_EQ(1u, function->variables().size());
  const Variable* var = function->variables()[0].Get()->As<Variable>();
  ASSERT_TRUE(var);
  EXPECT_EQ("local_static", var->GetAssignedName());
}

TEST(DwarfSymbolFactory, AbstractOriginInOtherUnit) {
  // The concrete instance in unit B has an abstract origin in unit A.
  AssemblyFixture fixture("specification");
  ASSERT_TRUE(fixture.Init());
  fxl::RefPtr<Symbol> symbol = fixture.UnitChild(1, 0);
  ASSERT_TRUE(symbol);
  const Function* function = symbol->As<Function>();
  ASSERT_TRUE(function);
  EXPECT_EQ("Outer::Method", function->GetFullName());
  ASSERT_EQ(1u, function->code_ranges().size());
  EXPECT_EQ(8u, function->code_ranges()[0].size());

  // The first variable in unit B has no attributes except its abstract origin in unit A. The second
  // variable has its own location.
  ASSERT_EQ(2u, function->variables().size());
  const Variable* var = function->variables()[0].Get()->As<Variable>();
  ASSERT_TRUE(var);
  EXPECT_EQ("local_static", var->GetAssignedName());
  EXPECT_EQ("int", var->type().Get()->GetFullName());

  // The function and its variable are in unit B. The parent of the function is the class of its
  // declaration, which is in unit A.
  fxl::RefPtr<CompileUnit> function_unit = function->GetCompileUnit();
  ASSERT_TRUE(function_unit);
  EXPECT_EQ("specification_b.cc", function_unit->name());
  fxl::RefPtr<CompileUnit> var_unit = var->GetCompileUnit();
  ASSERT_TRUE(var_unit);
  EXPECT_EQ("specification_b.cc", var_unit->name());

  // The variable is in the scope of the concrete instance.
  fxl::RefPtr<Symbol> parent = var->parent().Get();
  EXPECT_EQ(DwarfTag::kSubprogram, parent->tag());
  const Function* parent_function = parent->As<Function>();
  ASSERT_TRUE(parent_function);
  ASSERT_EQ(1u, parent_function->code_ranges().size());
  EXPECT_EQ(8u, parent_function->code_ranges()[0].size());

  // The location comes from unit A, so its DW_OP_addrx uses the address table of unit A.
  const DwarfExpr* expr = var->location().default_expr();
  ASSERT_TRUE(expr);
  EXPECT_EQ(0x8u, expr->GetAddrBase());

  // The location of the second variable is in unit B, so it uses the address table of unit B. The
  // parents of the variable reach unit A: the parent of the concrete instance is the class of its
  // declaration.
  const Variable* var_b = function->variables()[1].Get()->As<Variable>();
  ASSERT_TRUE(var_b);
  EXPECT_EQ("local_b", var_b->GetAssignedName());
  const DwarfExpr* expr_b = var_b->location().default_expr();
  ASSERT_TRUE(expr_b);
  EXPECT_EQ(0x18u, expr_b->GetAddrBase());
}

TEST(DwarfSymbolFactory, StaticMemberDefinition) {
  // The definition of a static data member takes its name and type from the declaration in the
  // class, and the class is its parent.
  AssemblyFixture fixture("specification");
  ASSERT_TRUE(fixture.Init());
  fxl::RefPtr<Symbol> symbol = fixture.UnitChild(0, kStaticDefinitionIndex);
  ASSERT_TRUE(symbol);
  const Variable* var = symbol->As<Variable>();
  ASSERT_TRUE(var);
  EXPECT_EQ("Outer::kStatic", var->GetFullName());
  EXPECT_TRUE(var->is_external());
  EXPECT_EQ("int", var->type().Get()->GetFullName());
  EXPECT_FALSE(var->location().is_null());
}

TEST(DwarfSymbolFactory, LowHighPcFromDifferentDies) {
  // DW_AT_low_pc is on the DIE and DW_AT_high_pc is on its abstract origin. They don't make a
  // range together.
  AssemblyFixture fixture("specification");
  ASSERT_TRUE(fixture.Init());
  fxl::RefPtr<Symbol> symbol = fixture.UnitChild(0, kPairOnlyIndex);
  ASSERT_TRUE(symbol);
  const Function* function = symbol->As<Function>();
  ASSERT_TRUE(function);
  EXPECT_EQ("PairOnly", function->GetFullName());
  EXPECT_TRUE(function->code_ranges().empty());
}

namespace {

// Returns the number of elements of each dimension of the array type, outermost first. A dimension
// of unknown size is -1.
std::vector<int64_t> GetArrayDimensions(const fxl::RefPtr<Symbol>& symbol) {
  std::vector<int64_t> result;
  const Type* type = symbol ? symbol->As<Type>() : nullptr;
  while (type) {
    const ArrayType* array = type->As<ArrayType>();
    if (!array)
      break;
    result.push_back(array->num_elts() ? static_cast<int64_t>(*array->num_elts()) : -1);
    type = array->value_type();
  }
  return result;
}

}  // namespace

TEST(DwarfSymbolFactory, ArrayBounds) {
  AssemblyFixture fixture("data_forms");
  ASSERT_TRUE(fixture.Init());

  // The upper bound is the index of the last element.
  EXPECT_EQ(std::vector<int64_t>({10}), GetArrayDimensions(fixture.UnitChild(0, 3)));
  EXPECT_EQ(std::vector<int64_t>({10}), GetArrayDimensions(fixture.UnitChild(0, 4)));

  // A data1 upper bound of 0xff is 255 for an unsigned basis type.
  EXPECT_EQ(std::vector<int64_t>({256}), GetArrayDimensions(fixture.UnitChild(0, 5)));

  // An upper bound of -1 with the lower bound of C is an empty array.
  EXPECT_EQ(std::vector<int64_t>({0}), GetArrayDimensions(fixture.UnitChild(0, 6)));

  // An explicit lower bound.
  EXPECT_EQ(std::vector<int64_t>({10}), GetArrayDimensions(fixture.UnitChild(0, 7)));

  // Two dimensions.
  EXPECT_EQ(std::vector<int64_t>({3, 5}), GetArrayDimensions(fixture.UnitChild(0, 8)));

  // A dynamic bound gives an unknown size, and the dimension keeps the bound so the expression
  // evaluator can compute the size. The upper bound is an expression here.
  fxl::RefPtr<Symbol> expression_symbol = fixture.UnitChild(0, 9);
  EXPECT_EQ(std::vector<int64_t>({-1}), GetArrayDimensions(expression_symbol));
  const ArrayType* expression_array = expression_symbol->As<ArrayType>();
  ASSERT_TRUE(expression_array);
  ASSERT_TRUE(expression_array->dynamic_bound());
  EXPECT_EQ(DynamicArrayBound::Kind::kUpperBound, expression_array->dynamic_bound()->kind);
  EXPECT_EQ(0, expression_array->dynamic_bound()->lower_bound);
  ASSERT_TRUE(expression_array->dynamic_bound()->expression);
  EXPECT_EQ(std::vector<uint8_t>({DwarfOp::kLit0}),
            expression_array->dynamic_bound()->expression->data());

  // The count references a variable.
  fxl::RefPtr<Symbol> variable_symbol = fixture.UnitChild(0, 21);
  EXPECT_EQ(std::vector<int64_t>({-1}), GetArrayDimensions(variable_symbol));
  const ArrayType* variable_array = variable_symbol->As<ArrayType>();
  ASSERT_TRUE(variable_array);
  ASSERT_TRUE(variable_array->dynamic_bound());
  EXPECT_EQ(DynamicArrayBound::Kind::kCount, variable_array->dynamic_bound()->kind);
  EXPECT_FALSE(variable_array->dynamic_bound()->expression);
  EXPECT_EQ("const_char", variable_array->dynamic_bound()->variable.Get()->GetAssignedName());
  EXPECT_EQ("int[]", variable_array->GetFullName());

  // The default lower bound of Fortran is 1.
  EXPECT_EQ(std::vector<int64_t>({10}), GetArrayDimensions(fixture.UnitChild(1, 1)));
}

TEST(DwarfSymbolFactory, EnumDataForms) {
  AssemblyFixture fixture("data_forms");
  ASSERT_TRUE(fixture.Init());

  // The unsigned type makes the data1 value 0xff 255.
  fxl::RefPtr<Symbol> unsigned_symbol = fixture.UnitChild(0, 10);
  const Enumeration* unsigned_enum = unsigned_symbol->As<Enumeration>();
  ASSERT_TRUE(unsigned_enum);
  EXPECT_FALSE(unsigned_enum->is_signed());
  EXPECT_EQ(Enumeration::Map({{1, "kOne"}, {255, "kHigh"}, {0xffffffffffffffff, "kMax"}}),
            unsigned_enum->values());

  // The signed type makes the data1 value 0xff -1. The map stores signed values as their unsigned
  // bits.
  fxl::RefPtr<Symbol> signed_symbol = fixture.UnitChild(0, 11);
  const Enumeration* signed_enum = signed_symbol->As<Enumeration>();
  ASSERT_TRUE(signed_enum);
  EXPECT_TRUE(signed_enum->is_signed());
  EXPECT_EQ(Enumeration::Map({{2, "kTwo"},
                              {static_cast<uint64_t>(-1), "kMinusOne"},
                              {static_cast<uint64_t>(-3), "kMinusThree"}}),
            signed_enum->values());

  // Without a type, data forms are unsigned and a DW_FORM_sdata value makes the enumeration signed.
  fxl::RefPtr<Symbol> untyped_symbol = fixture.UnitChild(0, 12);
  const Enumeration* untyped_enum = untyped_symbol->As<Enumeration>();
  ASSERT_TRUE(untyped_enum);
  EXPECT_TRUE(untyped_enum->is_signed());
  EXPECT_EQ(Enumeration::Map({{0x80, "kA"}, {static_cast<uint64_t>(-1), "kB"}}),
            untyped_enum->values());
}

TEST(DwarfSymbolFactory, ConstValueDataForms) {
  AssemblyFixture fixture("data_forms");
  ASSERT_TRUE(fixture.Init());

  // Returns the bytes of the constant of the variable, read with the given size.
  auto const_bytes = [&fixture](size_t index, size_t size) -> std::vector<uint8_t> {
    fxl::RefPtr<Symbol> symbol = fixture.UnitChild(0, index);
    const Variable* var = symbol->As<Variable>();
    if (!var || !var->const_value().has_value())
      return {};
    return var->const_value().GetConstValue(size);
  };

  // The data forms give their bytes in target byte order. A larger read is zero-filled.
  EXPECT_EQ(std::vector<uint8_t>({0x61}), const_bytes(13, 1));
  EXPECT_EQ(std::vector<uint8_t>({0x61, 0, 0, 0}), const_bytes(13, 4));
  EXPECT_EQ(std::vector<uint8_t>(8, 0xff), const_bytes(14, 8));
  EXPECT_EQ(std::vector<uint8_t>({0x34, 0x12, 0, 0}), const_bytes(15, 4));
  EXPECT_EQ(std::vector<uint8_t>({0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                                  0x0b, 0x0c, 0x0d, 0x0e, 0x0f}),
            const_bytes(16, 16));
}

TEST(DwarfSymbolFactory, VariantDiscriminants) {
  // The discriminant values are the bits of the discriminant member, so a negative value matches
  // the member however the producer encodes it.
  AssemblyFixture fixture("data_forms");
  ASSERT_TRUE(fixture.Init());

  // Returns the discriminant values of the variants of the structure. A default variant is -1.
  auto discriminants = [&fixture](size_t index) -> std::vector<int64_t> {
    std::vector<int64_t> result;
    fxl::RefPtr<Symbol> symbol = fixture.UnitChild(0, index);
    const Collection* collection = symbol->As<Collection>();
    if (!collection)
      return result;
    const VariantPart* part = collection->variant_part().Get()->As<VariantPart>();
    if (!part)
      return result;
    for (const LazySymbol& lazy : part->variants()) {
      const Variant* variant = lazy.Get()->As<Variant>();
      if (variant)
        result.push_back(variant->discr_value() ? static_cast<int64_t>(*variant->discr_value())
                                                : -1);
    }
    return result;
  };

  // i8: sdata -1, data2 0x00ff, data1 2, and the default.
  EXPECT_EQ(std::vector<int64_t>({0xff, 0xff, 2, -1}), discriminants(19));

  // i32: sdata -5 and data8 0xfffffffb.
  EXPECT_EQ(std::vector<int64_t>({0xfffffffb, 0xfffffffb}), discriminants(20));
}

TEST(DwarfSymbolFactory, InlinedParameterOrder) {
  // Clang omits unused parameters from concrete inlined instances. The parameters keep the order of
  // the declaration.
  std::string path = TestSymbolModule::GetTestDataDir() + "inline_params/clang_inline_params.so";
  if (!std::filesystem::exists(path))
    GTEST_SKIP() << "The fixture needs Clang.";

  TestSymbolModule setup(path, "");
  ASSERT_TRUE(setup.Init().ok());
  llvm::DWARFContext* context = setup.symbols()->binary()->GetLLVMContext();

  struct Case {
    const char* caller;
    std::vector<std::string> expected;
  };
  bool found_omitted = false;
  for (const Case& c : {Case{"InlineParamsCaller", {"unused_a", "b", "c"}},
                        Case{"InlineParamsMethodCaller", {"this", "unused", "param"}}}) {
    SCOPED_TRACE(c.caller);
    fxl::RefPtr<Function> caller = GetFunctionWithName(setup.symbols(), {c.caller});
    ASSERT_TRUE(caller);

    const Function* inlined = nullptr;
    for (const LazySymbol& block : caller->inner_blocks()) {
      const Function* function = block.Get()->As<Function>();
      if (function && function->tag() == DwarfTag::kInlinedSubroutine)
        inlined = function;
    }
    ASSERT_TRUE(inlined);

    // Find whether the fixture has the case this test is for: the concrete instance has fewer
    // parameter entries than the function.
    llvm::DWARFDie die = context->getDIEForOffset(inlined->GetLazySymbol().die_ref().offset());
    ASSERT_TRUE(die);
    size_t concrete_parameters = 0;
    for (const llvm::DWARFDie& child : die) {
      if (static_cast<DwarfTag>(child.getTag()) == DwarfTag::kFormalParameter)
        concrete_parameters++;
    }
    if (concrete_parameters < c.expected.size())
      found_omitted = true;

    std::vector<std::string> names;
    for (const LazySymbol& param : inlined->parameters())
      names.push_back(param.Get()->GetAssignedName());
    EXPECT_EQ(c.expected, names);
  }
  if (!found_omitted)
    GTEST_SKIP() << "Clang wrote all parameters, so the fixture doesn't test omitted ones.";
}

// TODO(brettw) test using statements. See GetUsing() in type_test.cc

}  // namespace zxdb
