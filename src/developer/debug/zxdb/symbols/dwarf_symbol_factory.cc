// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_symbol_factory.h"

#include <lib/syslog/cpp/macros.h>

#include <limits>

#include "src/developer/debug/zxdb/common/int128_t.h"
#include "src/developer/debug/zxdb/common/ref_ptr_to.h"
#include "src/developer/debug/zxdb/symbols/array_type.h"
#include "src/developer/debug/zxdb/symbols/base_type.h"
#include "src/developer/debug/zxdb/symbols/call_site.h"
#include "src/developer/debug/zxdb/symbols/call_site_parameter.h"
#include "src/developer/debug/zxdb/symbols/code_block.h"
#include "src/developer/debug/zxdb/symbols/collection.h"
#include "src/developer/debug/zxdb/symbols/compile_unit.h"
#include "src/developer/debug/zxdb/symbols/data_member.h"
#include "src/developer/debug/zxdb/symbols/dwarf_abstract_child_iterator.h"
#include "src/developer/debug/zxdb/symbols/dwarf_binary.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"
#include "src/developer/debug/zxdb/symbols/dwarf_lang.h"
#include "src/developer/debug/zxdb/symbols/dwarf_location.h"
#include "src/developer/debug/zxdb/symbols/dwarf_op.h"
#include "src/developer/debug/zxdb/symbols/dwarf_unit.h"
#include "src/developer/debug/zxdb/symbols/enumeration.h"
#include "src/developer/debug/zxdb/symbols/function.h"
#include "src/developer/debug/zxdb/symbols/function_type.h"
#include "src/developer/debug/zxdb/symbols/inherited_from.h"
#include "src/developer/debug/zxdb/symbols/member_ptr.h"
#include "src/developer/debug/zxdb/symbols/modified_type.h"
#include "src/developer/debug/zxdb/symbols/namespace.h"
#include "src/developer/debug/zxdb/symbols/symbol.h"
#include "src/developer/debug/zxdb/symbols/template_parameter.h"
#include "src/developer/debug/zxdb/symbols/variable.h"
#include "src/developer/debug/zxdb/symbols/variant.h"
#include "src/developer/debug/zxdb/symbols/variant_part.h"

namespace zxdb {

namespace {

// Extracts a FileLine if possible from the given input. If the optional values aren't present, or
// are empty, returns an empty FileLine.
FileLine MakeFileLine(const DwarfBinary* binary, const DwarfDie& die,
                      const std::optional<std::string>& file, const std::optional<uint64_t>& line,
                      std::string compilation_dir) {
  if (!file || file->empty() || !line || *line == 0)
    return FileLine();
  if (compilation_dir.empty()) {
    if (auto context = binary->GetUnitContext(die))
      compilation_dir = context.value()->compilation_dir().value_or(std::string_view());
  }
  return FileLine(*file, compilation_dir, static_cast<int>(*line));
}

// The attributes of an array subrange. Clang writes DW_AT_count, and GCC writes DW_AT_upper_bound.
constexpr DwarfAttr kArraySubrangeAttrs[] = {DwarfAttr::kCount, DwarfAttr::kUpperBound,
                                             DwarfAttr::kLowerBound, DwarfAttr::kType};

}  // namespace

DwarfSymbolFactory::DwarfSymbolFactory(fxl::WeakPtr<DwarfBinary> binary,
                                       fxl::WeakPtr<Delegate> delegate, FileType file_type)
    : binary_(std::move(binary)), delegate_(std::move(delegate)), file_type_(file_type) {}
DwarfSymbolFactory::~DwarfSymbolFactory() = default;

fxl::RefPtr<Symbol> DwarfSymbolFactory::CreateSymbol(DwarfDieRef die_ref) const {
  if (!delegate_ || !binary_)
    return fxl::MakeRefCounted<Symbol>();

  DwarfDie die = binary_->GetDieAtOffset(die_ref, file_type_ == kDWO);
  if (!die.is_valid())
    return fxl::MakeRefCounted<Symbol>();

  return DecodeSymbol(die);
}

VariableLocation DwarfSymbolFactory::DecodeLocation(const DwarfDie& die,
                                                    const FormValue& form) const {
  if (!binary_ || !die.belongs_to(*binary_))
    return {};
  return DecodeVariableLocation(*binary_.get(), die, form, MakeUncachedLazy(die),
                                GetSkeletonUnit());
}

DwarfSymbolFactory::ArraySubrange DwarfSymbolFactory::ReadArraySubrange(
    DwarfDieDecoder& range_decoder, const DwarfDie& subrange_die,
    int64_t default_lower_bound) const {
  if (!range_decoder.Decode(subrange_die))
    return ArraySubrange();

  // The basis type of the subrange decides the signedness of the bounds. Without a type, the basis
  // type is a signed integer (DWARF 5 section 5.13).
  bool is_signed = true;
  if (DwarfDie type = range_decoder.GetReference(DwarfAttr::kType))
    is_signed = IsSignedIntegerType(std::move(type)).value_or(false);

  std::optional<int128_t> lower = range_decoder.GetInteger(DwarfAttr::kLowerBound, is_signed);
  if (!lower && range_decoder.Has(DwarfAttr::kLowerBound))
    return ArraySubrange();  // A dynamic lower bound isn't supported.
  int64_t lower_bound = lower ? static_cast<int64_t>(*lower) : default_lower_bound;

  int128_t count = 0;
  if (std::optional<int128_t> count_value =
          range_decoder.GetInteger(DwarfAttr::kCount, is_signed)) {
    count = *count_value;
  } else if (range_decoder.Has(DwarfAttr::kCount)) {
    return ReadDynamicBound(range_decoder, DwarfAttr::kCount, DynamicArrayBound::Kind::kCount, 0);
  } else if (std::optional<int128_t> upper =
                 range_decoder.GetInteger(DwarfAttr::kUpperBound, is_signed)) {
    // The upper bound is the index of the last element, so an upper bound one below the lower bound
    // is an empty array.
    count = *upper - lower_bound + 1;
  } else if (range_decoder.Has(DwarfAttr::kUpperBound)) {
    return ReadDynamicBound(range_decoder, DwarfAttr::kUpperBound,
                            DynamicArrayBound::Kind::kUpperBound, lower_bound);
  } else {
    return ArraySubrange();  // No bound, so the size is unknown.
  }

  if (count < 0 || count > static_cast<int128_t>(std::numeric_limits<size_t>::max()))
    return ArraySubrange();
  return ArraySubrange{.size = static_cast<size_t>(count)};
}

DwarfSymbolFactory::ArraySubrange DwarfSymbolFactory::ReadDynamicBound(
    const DwarfDieDecoder& range_decoder, DwarfAttr attribute, DynamicArrayBound::Kind kind,
    int64_t lower_bound) const {
  const DwarfDieDecoder::AttributeValue* value = range_decoder.GetValue(attribute);
  if (!value)
    return ArraySubrange();

  DynamicArrayBound bound;
  bound.kind = kind;
  bound.lower_bound = lower_bound;
  if (value->form.form == DwarfForm::kExprloc) {
    // The expression computes the value (DWARF 5 section 2.19). Its source is the subrange DIE.
    std::optional<std::vector<uint8_t>> block = range_decoder.GetBlock(attribute);
    if (!block)
      return ArraySubrange();
    bound.expression = DwarfExpr(std::move(*block), MakeUncachedLazy(value->die()));
  } else if (DwarfDie referenced = range_decoder.GetReference(attribute)) {
    // A reference to a variable that holds the value.
    bound.variable = MakeLazy(referenced);
  } else {
    return ArraySubrange();
  }
  return ArraySubrange{.dynamic_bound = std::move(bound)};
}

std::optional<uint64_t> DwarfSymbolFactory::GetDiscriminantByteSize(const DwarfDie& variant) const {
  // The parent of the variant is the variant part, whose DW_AT_discr references the data member
  // that holds the discriminant.
  DwarfDie variant_part = variant.parent();
  if (!variant_part || variant_part.tag() != DwarfTag::kVariantPart)
    return std::nullopt;

  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kDiscr, DwarfAttr::kType, DwarfAttr::kByteSize};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(variant_part))
    return std::nullopt;
  DwarfDie member = decoder.GetReference(DwarfAttr::kDiscr);
  if (!member || !decoder.Decode(member))
    return std::nullopt;

  // The size is on the type of the member, after typedefs and qualifiers. The limit stops loops in
  // corrupt symbols.
  DwarfDie type = decoder.GetReference(DwarfAttr::kType);
  for (int i = 0; i < 8 && type; i++) {
    if (!decoder.Decode(type))
      return std::nullopt;
    if (std::optional<uint64_t> byte_size = decoder.GetUnsignedConstant(DwarfAttr::kByteSize))
      return byte_size;
    type = decoder.GetReference(DwarfAttr::kType);
  }
  return std::nullopt;
}

std::optional<bool> DwarfSymbolFactory::IsSignedIntegerType(DwarfDie type) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kEncoding, DwarfAttr::kType};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);

  // Typedefs, qualifiers, and enumerations can wrap the base type. The limit stops loops in corrupt
  // symbols.
  for (int i = 0; i < 8 && type; i++) {
    if (!decoder.Decode(type))
      return std::nullopt;
    switch (type.tag()) {
      case DwarfTag::kBaseType: {
        std::optional<uint64_t> encoding = decoder.GetUnsignedConstant(DwarfAttr::kEncoding);
        if (!encoding)
          return std::nullopt;
        switch (*encoding) {
          case BaseType::kBaseTypeSigned:
          case BaseType::kBaseTypeSignedChar:
            return true;
          case BaseType::kBaseTypeUnsigned:
          case BaseType::kBaseTypeUnsignedChar:
          case BaseType::kBaseTypeBoolean:
          case BaseType::kBaseTypeUTF:
            return false;
          default:
            return std::nullopt;
        }
      }
      case DwarfTag::kTypedef:
      case DwarfTag::kConstType:
      case DwarfTag::kVolatileType:
      case DwarfTag::kEnumerationType:
        type = decoder.GetReference(DwarfAttr::kType);
        break;
      default:
        return std::nullopt;
    }
  }
  return std::nullopt;
}

DwarfLang DwarfSymbolFactory::GetLanguage(const DwarfDie& die) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kLanguage};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  DwarfDie root = die.unit_root();
  if (!root || !decoder.Decode(root))
    return DwarfLang::kNone;
  std::optional<uint64_t> language = decoder.GetUnsignedConstant(DwarfAttr::kLanguage);
  if (!language || *language >= static_cast<uint64_t>(DwarfLang::kLast))
    return DwarfLang::kNone;
  return static_cast<DwarfLang>(*language);
}

fxl::RefPtr<CompileUnit> DwarfSymbolFactory::GetCompileUnitForDie(DwarfDieRef die_ref) const {
  if (!delegate_ || !binary_)
    return fxl::RefPtr<CompileUnit>();
  DwarfDie root = binary_->GetUnitDieForRef(die_ref, file_type_ == kDWO);
  if (!root)
    return fxl::RefPtr<CompileUnit>();

  fxl::RefPtr<Symbol> symbol = DecodeSymbol(root);
  if (const CompileUnit* compile_unit = symbol->As<CompileUnit>())
    return RefPtrTo(compile_unit);
  return fxl::RefPtr<CompileUnit>();
}

UnitContextSource DwarfSymbolFactory::GetSkeletonUnit() const {
  if (file_type_ == kDWO && delegate_) {
    if (CompileUnit* skeleton = delegate_->GetSkeletonCompileUnit()) {
      auto* unit = skeleton->dwarf_unit();
      if (auto* binary = unit->GetBinary())
        return {binary, unit->GetUnitIndex()};
    }
  }
  return {};
}

DwarfDieDecoder DwarfSymbolFactory::MakeDecoder(std::span<const DwarfAttr> attributes) const {
  // A .dwo unit uses the address table in the main binary.
  return DwarfDieDecoder(*binary_.get(), attributes, GetSkeletonUnit());
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeSymbol(const DwarfDie& die) const {
  DwarfTag tag = die.tag();
  if (DwarfTagIsTypeModifier(tag))
    return DecodeModifiedType(die);

  fxl::RefPtr<Symbol> symbol;
  switch (tag) {
    case DwarfTag::kArrayType:
      symbol = DecodeArrayType(die);
      break;
    case DwarfTag::kBaseType:
      symbol = DecodeBaseType(die);
      break;
    case DwarfTag::kCallSite:
      symbol = DecodeCallSite(die);
      break;
    case DwarfTag::kCallSiteParameter:
      symbol = DecodeCallSiteParameter(die);
      break;
    case DwarfTag::kCompileUnit:
    case DwarfTag::kSkeletonUnit:
    case DwarfTag::kTypeUnit:
      symbol = DecodeCompileUnit(die, tag);
      break;
    case DwarfTag::kEnumerationType:
      symbol = DecodeEnum(die);
      break;
    case DwarfTag::kFormalParameter:
    case DwarfTag::kVariable:
      symbol = DecodeVariable(die);
      break;
    case DwarfTag::kSubroutineType:
      symbol = DecodeFunctionType(die);
      break;
    case DwarfTag::kImportedDeclaration:
      symbol = DecodeImportedDeclaration(die);
      break;
    case DwarfTag::kInheritance:
      symbol = DecodeInheritedFrom(die);
      break;
    case DwarfTag::kLexicalBlock:
      symbol = DecodeLexicalBlock(die);
      break;
    case DwarfTag::kMember:
      symbol = DecodeDataMember(die);
      break;
    case DwarfTag::kNamespace:
      symbol = DecodeNamespace(die);
      break;
    case DwarfTag::kPtrToMemberType:
      symbol = DecodeMemberPtr(die);
      break;
    case DwarfTag::kInlinedSubroutine:
    case DwarfTag::kSubprogram:
      symbol = DecodeFunction(die, tag);
      break;
    case DwarfTag::kStructureType:
    case DwarfTag::kClassType:
    case DwarfTag::kUnionType:
      symbol = DecodeCollection(die);
      break;
    case DwarfTag::kTemplateTypeParameter:
    case DwarfTag::kTemplateValueParameter:
      symbol = DecodeTemplateParameter(die, tag);
      break;
    case DwarfTag::kVariantPart:
      symbol = DecodeVariantPart(die);
      break;
    case DwarfTag::kVariant:
      symbol = DecodeVariant(die);
      break;
    case DwarfTag::kUnspecifiedType:
      symbol = DecodeUnspecifiedType(die);
      break;
    default:
      // All unhandled Tag types get a Symbol that has the correct tag, but no other data.
      symbol = fxl::MakeRefCounted<Symbol>(tag);
  }

  symbol->set_lazy_this(MakeUncachedLazy(die));

  // Set the parent block if it hasn't been set already by the type-specific factory. In particular,
  // we want the function/variable specification's parent block if there was a specification since
  // it will contain the namespace and class stuff.
  if (!symbol->parent()) {
    DwarfDie parent = die.parent();
    if (parent)
      symbol->set_parent(MakeUncachedLazy(parent));
  }

  return symbol;
}

LazySymbol DwarfSymbolFactory::MakeLazy(const DwarfDie& die) const { return MakeLazy(die.ref()); }

UncachedLazySymbol DwarfSymbolFactory::MakeUncachedLazy(const DwarfDie& die) const {
  return MakeUncachedLazy(die.ref());
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeFunction(const DwarfDie& die, DwarfTag tag) const {
  // The decoder takes the attributes that this DIE doesn't have from its abstract origin and from
  // the declaration that DW_AT_specification references, so one decode gives all of them.
  static constexpr DwarfAttr kAttrs[] = {
      DwarfAttr::kName,     DwarfAttr::kLinkageName, DwarfAttr::kType,
      DwarfAttr::kDeclFile, DwarfAttr::kDeclLine,    DwarfAttr::kCallFile,
      DwarfAttr::kCallLine, DwarfAttr::kFrameBase,   DwarfAttr::kObjectPointer,
      DwarfAttr::kLowPc,    DwarfAttr::kHighPc,      DwarfAttr::kRanges};

  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  // The parent is the namespace or class of the declaration. For an inlined subroutine, the
  // containing block is set separately below.
  DwarfDie parent = decoder.GetContextParent();
  std::optional<const char*> name = decoder.GetCString(DwarfAttr::kName);
  std::optional<const char*> linkage_name = decoder.GetCString(DwarfAttr::kLinkageName);
  DwarfDie return_type = decoder.GetReference(DwarfAttr::kType);

  // Declaration location.
  std::optional<std::string> decl_file = decoder.GetFile(DwarfAttr::kDeclFile);
  std::optional<uint64_t> decl_line = decoder.GetUnsignedConstant(DwarfAttr::kDeclLine);

  // Call location (inline functions only).
  std::optional<std::string> call_file;
  std::optional<uint64_t> call_line;
  if (tag == DwarfTag::kInlinedSubroutine) {
    call_file = decoder.GetFile(DwarfAttr::kCallFile);
    call_line = decoder.GetUnsignedConstant(DwarfAttr::kCallLine);
  }

  // The expression source is the DIE that has the attribute, so the expression uses the address
  // table of that DIE's unit.
  VariableLocation frame_base;
  if (const DwarfDieDecoder::AttributeValue* value = decoder.GetValue(DwarfAttr::kFrameBase))
    frame_base = DecodeLocation(value->die(), value->form);

  DwarfDie object_ptr = decoder.GetReference(DwarfAttr::kObjectPointer);

  auto function = fxl::MakeRefCounted<Function>(tag);

  if (name)
    function->set_assigned_name(*name);
  if (linkage_name)
    function->set_linkage_name(*linkage_name);
  function->set_code_ranges(decoder.GetCodeRanges());
  if (decl_file) {
    // The file name comes from the unit of the DIE that has DW_AT_decl_file.
    function->set_decl_line(MakeFileLine(binary_.get(),
                                         decoder.GetValue(DwarfAttr::kDeclFile)->die(), decl_file,
                                         decl_line, delegate_->GetBuildDirForSymbolFactory()));
  }
  function->set_call_line(MakeFileLine(binary_.get(), die, call_file, call_line,
                                       delegate_->GetBuildDirForSymbolFactory()));
  if (return_type)
    function->set_return_type(MakeLazy(return_type));
  function->set_frame_base(std::move(frame_base));
  if (object_ptr)
    function->set_object_pointer(MakeLazy(object_ptr));

  // Handle sub-DIEs: parameters, child blocks, and variables.
  //
  // Note: this partially duplicates a similar loop in DecodeLexicalBlock().
  std::vector<LazySymbol> parameters;
  std::vector<LazySymbol> inner_blocks;
  std::vector<LazySymbol> variables;
  std::vector<LazySymbol> template_params;
  std::vector<LazySymbol> call_sites;
  auto add_child = [&](const DwarfDie& child) {
    switch (child.tag()) {
      case DwarfTag::kFormalParameter:
        parameters.push_back(MakeLazy(child));
        break;
      case DwarfTag::kVariable:
        variables.push_back(MakeLazy(child));
        break;
      case DwarfTag::kInlinedSubroutine:
      case DwarfTag::kLexicalBlock:
        inner_blocks.push_back(MakeLazy(child));
        break;
      case DwarfTag::kTemplateTypeParameter:
      case DwarfTag::kTemplateValueParameter:
        template_params.push_back(MakeLazy(child));
        break;
      case DwarfTag::kCallSite:
        call_sites.push_back(MakeLazy(child));
        break;
      default:
        break;  // Skip everything else.
    }
  };
  // Direct children can be used immediately when there is no abstract tree to merge.
  DwarfDie origin = die.abstract_origin();
  if (origin && origin != die) {
    auto get_origin = [&](const DwarfDie& child) {
      return child == die ? origin : child.abstract_origin();
    };
    for (const DwarfDie& child : DwarfAbstractChildIterator(die, get_origin))
      add_child(child);
  } else {
    for (const DwarfDie& child : die.children())
      add_child(child);
  }
  function->set_parameters(std::move(parameters));
  function->set_inner_blocks(std::move(inner_blocks));
  function->set_variables(std::move(variables));
  function->set_template_params(std::move(template_params));
  function->set_call_sites(std::move(call_sites));

  if (parent)
    function->set_parent(MakeUncachedLazy(parent));

  if (tag == DwarfTag::kInlinedSubroutine) {
    // In contrast to the logic for parent() above, the direct containing block of the inlined
    // subroutine will save the CodeBlock inlined functions are embedded in.
    DwarfDie direct_parent = die.parent();
    if (direct_parent)
      function->set_containing_block(MakeUncachedLazy(direct_parent));
  }

  return function;
}

// We expect array types to have two things:
// - An attribute linking to the underlying type of the array.
// - One or more DW_TAG_subrange_type children that hold the size of the array in a DW_AT_count
//   attribute.
//
// Clang can emit a synthetic __ARRAY_SIZE_TYPE__ referenced by the subrange's DW_AT_type.
// The decoder uses the subrange's bounds to compute the element count.
//
// One might expect 2-dimensional arrays to be expressed as an array of one dimension where the
// contained type is an array of another. But both Clang and GCC generate one array entry with two
// subrange children. The order of these represents the declaration order in the code.
fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeArrayType(const DwarfDie& die) const {
  // Extract the type attribute from the root DIE (should be a DW_TAG_array_type).
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kType};
  DwarfDieDecoder array_decoder = MakeDecoder(kAttrs);
  if (!array_decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();
  DwarfDie type = array_decoder.GetReference(DwarfAttr::kType);
  if (!type)
    return fxl::MakeRefCounted<Symbol>();

  // Need the concrete symbol for the contained type for the array constructor.
  auto contained = DecodeSymbol(type);
  if (!contained)
    return fxl::MakeRefCounted<Symbol>();
  Type* contained_type = contained->As<Type>();
  if (!contained_type)
    return fxl::MakeRefCounted<Symbol>();

  // Find all subranges stored in the declaration order. More than one means a multi-dimensional
  // array.
  int64_t default_lower_bound = DwarfLangDefaultLowerBound(GetLanguage(die));
  DwarfDieDecoder range_decoder = MakeDecoder(kArraySubrangeAttrs);
  std::vector<ArraySubrange> subranges;
  for (const DwarfDie& child : die.children()) {
    if (child.tag() == DwarfTag::kSubrangeType)
      subranges.push_back(ReadArraySubrange(range_decoder, child, default_lower_bound));
  }

  // Require a subrange with a count in it. If we find cases where this isn't the case, we could add
  // support for array types with unknown lengths, but currently ArrayType requires a size.
  if (subranges.empty())
    return fxl::MakeRefCounted<Symbol>();

  // Work backwards in the array dimensions generating nested array definitions. The innermost
  // definition refers to the contained type.
  fxl::RefPtr<Type> cur(contained_type);
  for (int i = static_cast<int>(subranges.size()) - 1; i >= 0; i--) {
    if (subranges[i].dynamic_bound) {
      cur = fxl::MakeRefCounted<ArrayType>(std::move(cur), std::move(*subranges[i].dynamic_bound));
    } else {
      cur = fxl::MakeRefCounted<ArrayType>(std::move(cur), subranges[i].size);
    }
  }
  return cur;
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeBaseType(const DwarfDie& die) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName, DwarfAttr::kEncoding,
                                         DwarfAttr::kByteSize};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  // Types must always use the context parent so they can be nested in the correct namespace or
  // class. It comes from the abstract origin or the specification, if the DIE has one.
  DwarfDie parent = decoder.GetContextParent();

  std::optional<const char*> name = decoder.GetCString(DwarfAttr::kName);
  std::optional<uint64_t> encoding = decoder.GetUnsignedConstant(DwarfAttr::kEncoding);
  std::optional<uint64_t> byte_size = decoder.GetUnsignedConstant(DwarfAttr::kByteSize);

  auto base_type = fxl::MakeRefCounted<BaseType>();
  if (name)
    base_type->set_assigned_name(*name);
  if (encoding)
    base_type->set_base_type(static_cast<int>(*encoding));
  if (byte_size)
    base_type->set_byte_size(static_cast<uint32_t>(*byte_size));

  if (parent)
    base_type->set_parent(MakeUncachedLazy(parent));

  return base_type;
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeCallSite(const DwarfDie& die) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kCallReturnPc};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  std::optional<uint64_t> return_pc = decoder.GetAddress(DwarfAttr::kCallReturnPc);

  // Decode parameters.
  std::vector<LazySymbol> parameters;
  for (const DwarfDie& child : die.children()) {
    if (child.tag() == DwarfTag::kCallSiteParameter)
      parameters.push_back(MakeLazy(child));
  }

  return fxl::MakeRefCounted<CallSite>(return_pc, std::move(parameters));
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeCallSiteParameter(const DwarfDie& die) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kLocation, DwarfAttr::kCallValue};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  // We currently assume that the location and value for call site parameters are always blocks
  // having inline location expressions. Technically, these are location expressions that could
  // be an offset to a location list, but our current supported compilers don't express it this way,
  // and there would seem to no point in doing so for this case.
  std::optional<std::vector<uint8_t>> location = decoder.GetBlock(DwarfAttr::kLocation);
  std::optional<std::vector<uint8_t>> value = decoder.GetBlock(DwarfAttr::kCallValue);

  // Decode the register number. We currently expect the location to be exactly one DWARF opcode
  // indicating the register (see call_site_parameter.h for more).
  std::optional<uint32_t> register_num;
  if (location && location->size() == 1) {
    uint8_t location_op = location->at(0);
    if (location_op >= DwarfOp::kReg0 && location_op <= DwarfOp::kReg31)
      register_num = location_op - DwarfOp::kReg0;
  }

  // Convert a nonexistent value to one with an empty block. These are equivalent for our needs.
  if (!value)
    value.emplace();

  return fxl::MakeRefCounted<CallSiteParameter>(
      register_num, DwarfExpr(std::move(*value), MakeUncachedLazy(die)));
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeCollection(const DwarfDie& die) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kSignature, DwarfAttr::kName,
                                         DwarfAttr::kByteSize, DwarfAttr::kDeclaration,
                                         DwarfAttr::kCallingConvention};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  // Types must always use the context parent so they can be nested in the correct namespace or
  // class. It comes from the abstract origin or the specification, if the DIE has one.
  DwarfDie parent = decoder.GetContextParent();

  if (const auto* signature = decoder.GetValue(DwarfAttr::kSignature);
      signature && signature->form.AsReference()) {
    DwarfDie type = signature->form.form == DwarfForm::kRefSig8
                        ? decoder.GetReference(DwarfAttr::kSignature)
                        : DwarfDie();
    if (type && type != die)
      return DecodeSymbol(type);
    return fxl::MakeRefCounted<Symbol>();
  }

  std::optional<const char*> name = decoder.GetCString(DwarfAttr::kName);
  std::optional<uint64_t> byte_size = decoder.GetUnsignedConstant(DwarfAttr::kByteSize);
  std::optional<bool> is_declaration = decoder.GetBool(DwarfAttr::kDeclaration);
  std::optional<uint64_t> calling_convention =
      decoder.GetUnsignedConstant(DwarfAttr::kCallingConvention);

  auto result = fxl::MakeRefCounted<Collection>(die.tag());
  if (name)
    result->set_assigned_name(*name);
  if (byte_size)
    result->set_byte_size(static_cast<uint32_t>(*byte_size));

  // Handle sub-DIEs: data members and inheritance.
  std::vector<LazySymbol> data;
  std::vector<LazySymbol> inheritance;
  std::vector<LazySymbol> template_params;
  LazySymbol variant_part;
  for (const DwarfDie& child : die.children()) {
    switch (child.tag()) {
      case DwarfTag::kInheritance:
        inheritance.push_back(MakeLazy(child));
        break;
      case DwarfTag::kMember:
        data.push_back(MakeLazy(child));
        break;
      case DwarfTag::kVariantPart:
        // Currently we only support one variant_part per struct. This could be expanded to a vector
        // if a compiler generates such a structure.
        variant_part = MakeLazy(child);
        break;
      case DwarfTag::kTemplateTypeParameter:
      case DwarfTag::kTemplateValueParameter:
        template_params.push_back(MakeLazy(child));
        break;
      default:
        break;  // Skip everything else.
    }
  }
  result->set_data_members(std::move(data));
  result->set_inherited_from(std::move(inheritance));
  result->set_template_params(std::move(template_params));
  result->set_variant_part(variant_part);
  if (is_declaration)
    result->set_is_declaration(*is_declaration);

  if (calling_convention) {
    if (*calling_convention == Collection::kNormalCall ||
        *calling_convention == Collection::kPassByReference ||
        *calling_convention == Collection::kPassByValue) {
      result->set_calling_convention(
          static_cast<Collection::CallingConvention>(*calling_convention));
    }
  }

  if (parent)
    result->set_parent(MakeUncachedLazy(parent));

  return result;
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeCompileUnit(const DwarfDie& die, DwarfTag tag) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName, DwarfAttr::kLanguage,
                                         DwarfAttr::kAddrBase};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  std::optional<const char*> name = decoder.GetCString(DwarfAttr::kName);
  std::optional<uint64_t> language = decoder.GetUnsignedConstant(DwarfAttr::kLanguage);
  std::optional<uint64_t> addr_base = decoder.GetSectionOffset(DwarfAttr::kAddrBase);

  std::string name_str;
  if (name)
    name_str = *name;

  DwarfLang lang_enum = DwarfLang::kNone;
  if (language && *language >= 0 && *language < static_cast<int>(DwarfLang::kLast))
    lang_enum = static_cast<DwarfLang>(*language);

  auto dwarf_unit = binary_->CreateUncachedUnit(die);
  if (!dwarf_unit)
    return fxl::MakeRefCounted<Symbol>();

  // We know the delegate_ is valid, that was checked on entry to CreateSymbol().
  return fxl::MakeRefCounted<CompileUnit>(tag, delegate_->GetModuleSymbols(), std::move(dwarf_unit),
                                          lang_enum, std::move(name_str), addr_base);
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeDataMember(const DwarfDie& die) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName,
                                         DwarfAttr::kType,
                                         DwarfAttr::kArtificial,
                                         DwarfAttr::kExternal,
                                         DwarfAttr::kDataMemberLocation,
                                         DwarfAttr::kByteSize,
                                         DwarfAttr::kBitSize,
                                         DwarfAttr::kBitOffset,
                                         DwarfAttr::kDataBitOffset,
                                         DwarfAttr::kConstValue,
                                         DwarfAttr::kDeclFile,
                                         DwarfAttr::kDeclLine};

  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  std::optional<const char*> name = decoder.GetCString(DwarfAttr::kName);
  DwarfDie type = decoder.GetReference(DwarfAttr::kType);
  std::optional<bool> artificial = decoder.GetBool(DwarfAttr::kArtificial);
  std::optional<bool> external = decoder.GetBool(DwarfAttr::kExternal);
  std::optional<uint64_t> member_offset =
      decoder.GetUnsignedConstant(DwarfAttr::kDataMemberLocation);
  std::optional<uint64_t> byte_size = decoder.GetUnsignedConstant(DwarfAttr::kByteSize);
  std::optional<uint64_t> bit_size = decoder.GetUnsignedConstant(DwarfAttr::kBitSize);
  std::optional<int64_t> bit_offset = decoder.GetSignedConstant(DwarfAttr::kBitOffset);
  std::optional<uint64_t> data_bit_offset = decoder.GetUnsignedConstant(DwarfAttr::kDataBitOffset);
  ConstValue const_value = decoder.GetConstValue(DwarfAttr::kConstValue);

  // These are set for Rust generators.
  std::optional<std::string> decl_file = decoder.GetFile(DwarfAttr::kDeclFile);
  std::optional<uint64_t> decl_line = decoder.GetUnsignedConstant(DwarfAttr::kDeclLine);

  auto result = fxl::MakeRefCounted<DataMember>();
  if (name)
    result->set_assigned_name(*name);
  if (type)
    result->set_type(MakeLazy(type));
  if (artificial)
    result->set_artificial(*artificial);
  if (external)
    result->set_is_external(*external);
  if (member_offset)
    result->set_member_location(static_cast<uint32_t>(*member_offset));
  if (byte_size)
    result->set_byte_size(static_cast<uint32_t>(*byte_size));
  if (bit_offset)
    result->set_bit_offset(static_cast<int32_t>(*bit_offset));
  if (bit_size)
    result->set_bit_size(static_cast<uint32_t>(*bit_size));
  if (data_bit_offset)
    result->set_data_bit_offset(static_cast<uint32_t>(*data_bit_offset));
  result->set_const_value(std::move(const_value));
  if (decl_file) {
    result->set_decl_line(MakeFileLine(binary_.get(), die, decl_file, decl_line,
                                       delegate_->GetBuildDirForSymbolFactory()));
  }
  return result;
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeEnum(const DwarfDie& die) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kSignature, DwarfAttr::kName,
                                         DwarfAttr::kByteSize, DwarfAttr::kDeclaration,
                                         DwarfAttr::kType};
  DwarfDieDecoder main_decoder = MakeDecoder(kAttrs);
  if (!main_decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  if (const auto* signature = main_decoder.GetValue(DwarfAttr::kSignature);
      signature && signature->form.AsReference()) {
    DwarfDie type = signature->form.form == DwarfForm::kRefSig8
                        ? main_decoder.GetReference(DwarfAttr::kSignature)
                        : DwarfDie();
    if (type && type != die)
      return DecodeSymbol(type);
    return fxl::MakeRefCounted<Symbol>();
  }

  // Types must always use the context parent so they can be nested in the correct namespace or
  // class. It comes from the abstract origin or the specification, if the DIE has one.
  DwarfDie parent = main_decoder.GetContextParent();

  // Name is optional (enums can be anonymous).
  std::optional<const char*> type_name = main_decoder.GetCString(DwarfAttr::kName);
  std::optional<uint64_t> byte_size = main_decoder.GetUnsignedConstant(DwarfAttr::kByteSize);
  std::optional<bool> is_declaration = main_decoder.GetBool(DwarfAttr::kDeclaration);

  // The type is optional for an enumeration.
  DwarfDie type = main_decoder.GetReference(DwarfAttr::kType);

  FX_CHECK(byte_size.has_value());

  // For decoding the individual enum values.
  static constexpr DwarfAttr kEnumeratorAttrs[] = {DwarfAttr::kName, DwarfAttr::kConstValue};
  DwarfDieDecoder enumerator_decoder = MakeDecoder(kEnumeratorAttrs);

  // Enum values can be signed or unsigned. The underlying type decides, as in LLDB. It also decides
  // how to read DW_FORM_data<n> values, which GCC uses for non-negative values (DWARF 5 section
  // 7.5.5).
  //
  // Without a known underlying type, the enumeration is signed if any value has DW_FORM_sdata. A
  // small positive integer could be represented either way but a signed value must be encoded
  // differently.
  std::optional<bool> type_is_signed;
  if (type)
    type_is_signed = IsSignedIntegerType(type);
  bool is_signed = type_is_signed.value_or(false);

  Enumeration::Map map;
  for (const DwarfDie& child : die.children()) {
    if (child.tag() != DwarfTag::kEnumerator)
      continue;
    if (!enumerator_decoder.Decode(child))
      continue;

    if (!type_is_signed) {
      if (const DwarfDieDecoder::AttributeValue* value =
              enumerator_decoder.GetValue(DwarfAttr::kConstValue)) {
        if (value->form.form == DwarfForm::kSdata)
          is_signed = true;
      }
    }

    // The map stores signed values as their unsigned bits. Other forms are corrupted symbols or an
    // unsupported format, so skip them.
    std::optional<int128_t> value =
        enumerator_decoder.GetInteger(DwarfAttr::kConstValue, type_is_signed.value_or(false));
    std::optional<const char*> enumerator_name = enumerator_decoder.GetCString(DwarfAttr::kName);
    if (enumerator_name && value)
      map[static_cast<uint64_t>(*value)] = std::string(*enumerator_name);
  }

  LazySymbol lazy_type;
  if (type)
    lazy_type = MakeLazy(type);
  const char* type_name_str = type_name ? *type_name : "";
  auto result = fxl::MakeRefCounted<Enumeration>(type_name_str, std::move(lazy_type), *byte_size,
                                                 is_signed, std::move(map));
  if (parent)
    result->set_parent(MakeUncachedLazy(parent));
  if (is_declaration)
    result->set_is_declaration(*is_declaration);
  return result;
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeFunctionType(const DwarfDie& die) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kType};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  // Types must always use the context parent so they can be nested in the correct namespace or
  // class. It comes from the abstract origin or the specification, if the DIE has one.
  DwarfDie parent = decoder.GetContextParent();
  DwarfDie return_type = decoder.GetReference(DwarfAttr::kType);

  // Handle sub-DIEs (this only has parameters).
  std::vector<LazySymbol> parameters;
  for (const DwarfDie& child : die.children()) {
    switch (child.tag()) {
      case DwarfTag::kFormalParameter:
        parameters.push_back(MakeLazy(child));
        break;
      default:
        break;  // Skip everything else.
    }
  }

  LazySymbol lazy_return_type;
  if (return_type)
    lazy_return_type = MakeLazy(return_type);

  auto function = fxl::MakeRefCounted<FunctionType>(lazy_return_type, std::move(parameters));
  if (parent)
    function->set_parent(MakeUncachedLazy(parent));
  return function;
}

// Imported declarations are "using" statements that don't provide a new name like
// "using std::vector;".
//
// Type renames like "using Foo = std::vector;" is encoded as a typedef.
fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeImportedDeclaration(const DwarfDie& die) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kImport};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  DwarfDie imported = decoder.GetReference(DwarfAttr::kImport);
  if (!imported)
    return fxl::MakeRefCounted<Symbol>();

  return fxl::MakeRefCounted<ModifiedType>(DwarfTag::kImportedDeclaration, MakeLazy(imported));
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeInheritedFrom(const DwarfDie& die) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kType, DwarfAttr::kDataMemberLocation};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  DwarfDie type = decoder.GetReference(DwarfAttr::kType);

  // The DW_AT_data_member_location can either be a constant or an expression.
  std::optional<uint64_t> member_offset;
  std::vector<uint8_t> offset_expression;
  if (const DwarfDieDecoder::AttributeValue* value =
          decoder.GetValue(DwarfAttr::kDataMemberLocation)) {
    DwarfForm form = value->form.form;
    if (form == DwarfForm::kExprloc) {
      // Location expression.
      auto block = *value->form.AsBlock();
      offset_expression.assign(block.data(), block.data() + block.size());
    } else if (DwarfFormIsConstant(form)) {
      // Constant value.
      member_offset = value->form.AsUnsignedConstant();
    }
    // Otherwise leave both empty.
  }

  LazySymbol lazy_type;
  if (type)
    lazy_type = MakeLazy(type);

  if (member_offset)
    return fxl::MakeRefCounted<InheritedFrom>(std::move(lazy_type), *member_offset);
  if (!offset_expression.empty()) {
    return fxl::MakeRefCounted<InheritedFrom>(std::move(lazy_type),
                                              DwarfExpr(std::move(offset_expression)));
  }
  return fxl::MakeRefCounted<Symbol>();  // Missing location.
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeLexicalBlock(const DwarfDie& die) const {
  auto block = fxl::MakeRefCounted<CodeBlock>(DwarfTag::kLexicalBlock);
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kLowPc, DwarfAttr::kHighPc, DwarfAttr::kRanges};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (decoder.Decode(die))
    block->set_code_ranges(decoder.GetCodeRanges());

  // Handle sub-DIEs: child blocks and variables.
  //
  // Note: this partially duplicates a similar loop in DecodeFunction().
  std::vector<LazySymbol> inner_blocks;
  std::vector<LazySymbol> variables;
  std::vector<LazySymbol> call_sites;
  for (const DwarfDie& child : die.children()) {
    switch (child.tag()) {
      case DwarfTag::kVariable:
        variables.push_back(MakeLazy(child));
        break;
      case DwarfTag::kInlinedSubroutine:
      case DwarfTag::kLexicalBlock:
        inner_blocks.push_back(MakeLazy(child));
        break;
      case DwarfTag::kCallSite:
        call_sites.push_back(MakeLazy(child));
        break;
      default:
        break;  // Skip everything else.
    }
  }
  block->set_inner_blocks(std::move(inner_blocks));
  block->set_variables(std::move(variables));
  block->set_call_sites(std::move(call_sites));

  return block;
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeMemberPtr(const DwarfDie& die) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kContainingType, DwarfAttr::kType};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  DwarfDie container_type = decoder.GetReference(DwarfAttr::kContainingType);
  DwarfDie type = decoder.GetReference(DwarfAttr::kType);
  if (!container_type || !type)
    return fxl::MakeRefCounted<Symbol>();

  auto member_ptr = fxl::MakeRefCounted<MemberPtr>(MakeLazy(container_type), MakeLazy(type));
  return member_ptr;
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeModifiedType(const DwarfDie& die) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName, DwarfAttr::kType};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  // Types must always use the context parent so they can be nested in the correct namespace or
  // class. It comes from the abstract origin or the specification, if the DIE has one.
  DwarfDie parent = decoder.GetContextParent();
  std::optional<const char*> name = decoder.GetCString(DwarfAttr::kName);
  DwarfDie modified = decoder.GetReference(DwarfAttr::kType);

  // Modified type may be null for "void*".
  LazySymbol lazy_modified;
  if (modified)
    lazy_modified = MakeLazy(modified);

  auto result = fxl::MakeRefCounted<ModifiedType>(die.tag(), std::move(lazy_modified));
  if (name)
    result->set_assigned_name(*name);

  result->set_lazy_this(MakeUncachedLazy(die));
  if (parent)
    result->set_parent(MakeUncachedLazy(parent));

  return result;
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeNamespace(const DwarfDie& die) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  // Types must always use the context parent so they can be nested in the correct namespace or
  // class. It comes from the abstract origin or the specification, if the DIE has one.
  DwarfDie parent = decoder.GetContextParent();
  std::optional<const char*> name = decoder.GetCString(DwarfAttr::kName);

  auto result = fxl::MakeRefCounted<Namespace>();
  if (name)
    result->set_assigned_name(*name);

  if (parent)
    result->set_parent(MakeUncachedLazy(parent));
  return result;
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeTemplateParameter(const DwarfDie& die,
                                                                DwarfTag tag) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName, DwarfAttr::kType,
                                         DwarfAttr::kConstValue};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  std::optional<const char*> name = decoder.GetCString(DwarfAttr::kName);
  DwarfDie type = decoder.GetReference(DwarfAttr::kType);
  if (!type)
    return fxl::MakeRefCounted<Symbol>();

  // Account for DW_TAG_template_value_parameter. This will change the behavior to print the value
  // of the template parameter for things like template <int i> struct Foo {};.
  ConstValue const_value = decoder.GetConstValue(DwarfAttr::kConstValue);

  std::string name_str = name ? *name : "";
  fxl::RefPtr<TemplateParameter> result = fxl::MakeRefCounted<TemplateParameter>(
      name_str, MakeLazy(type), tag == DwarfTag::kTemplateValueParameter);

  result->set_const_value(std::move(const_value));

  return result;
}

// Clang and GCC use "unspecified" types to encode "decltype(nullptr)". When used as a variable this
// appears as a pointer with 0 value, despite not having any declared size in the symbols.
// Therefore, we make up a byte size equal to the pointer size (8 bytes on our 64-bit systems).
fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeUnspecifiedType(const DwarfDie& die) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kName};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  // Types must always use the context parent so they can be nested in the correct namespace or
  // class. It comes from the abstract origin or the specification, if the DIE has one.
  DwarfDie parent = decoder.GetContextParent();
  std::optional<const char*> name = decoder.GetCString(DwarfAttr::kName);

  auto result = fxl::MakeRefCounted<Type>(DwarfTag::kUnspecifiedType);
  if (name)
    result->set_assigned_name(*name);
  result->set_byte_size(8);  // Assume pointer.
  if (parent)
    result->set_parent(MakeUncachedLazy(parent));

  return result;
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeVariable(const DwarfDie& die) const {
  // The decoder takes the attributes that this DIE doesn't have from its abstract origin and from
  // the declaration that DW_AT_specification references, so one decode gives all of them.
  static constexpr DwarfAttr kAttrs[] = {
      DwarfAttr::kSpecification, DwarfAttr::kName,       DwarfAttr::kLocation,  DwarfAttr::kType,
      DwarfAttr::kExternal,      DwarfAttr::kArtificial, DwarfAttr::kConstValue};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  DwarfDie specification = decoder.GetReference(DwarfAttr::kSpecification);
  std::optional<const char*> name = decoder.GetCString(DwarfAttr::kName);

  // The expression source is the DIE that has the attribute. A location can come from an abstract
  // origin in another unit, for example for a static local of an inline function.
  VariableLocation location;
  if (const DwarfDieDecoder::AttributeValue* value = decoder.GetValue(DwarfAttr::kLocation))
    location = DecodeLocation(value->die(), value->form);

  DwarfDie type = decoder.GetReference(DwarfAttr::kType);
  std::optional<bool> external = decoder.GetBool(DwarfAttr::kExternal);
  std::optional<bool> artificial = decoder.GetBool(DwarfAttr::kArtificial);
  ConstValue const_value = decoder.GetConstValue(DwarfAttr::kConstValue);

  auto variable = fxl::MakeRefCounted<Variable>(die.tag());

  if (name)
    variable->set_assigned_name(*name);
  if (type)
    variable->set_type(MakeLazy(type));
  if (external)
    variable->set_is_external(*external);
  if (artificial)
    variable->set_artificial(*artificial);
  variable->set_location(std::move(location));
  variable->set_const_value(std::move(const_value));

  // The definition of a static data member references its declaration in the class with
  // DW_AT_specification, so the class is the parent. Other variables use the parent of their own
  // DIE, which is the scope that contains them. This is also true for a variable in an inlined
  // subroutine: its abstract origin is in the scope of the abstract instance.
  DwarfDie parent = die.parent();
  if (specification)
    parent = specification.parent();
  if (parent)
    variable->set_parent(MakeUncachedLazy(parent));
  return variable;
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeVariant(const DwarfDie& die) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kDiscrValue};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  // The evaluator compares the bits of the discriminant member, zero-extended to 64 bits. So the
  // value must be the bits of the member's size. rustc writes a negative value as those bits in a
  // larger DW_FORM_data<n> (an i8 of -1 is data2 0x00ff). Other producers can write
  // DW_FORM_sdata. Keeping only the bits of the member's size gives the same result for both.
  std::optional<uint64_t> discr_value;
  if (std::optional<int128_t> value = decoder.GetInteger(DwarfAttr::kDiscrValue, false)) {
    uint64_t bits = static_cast<uint64_t>(*value);
    std::optional<uint64_t> size = GetDiscriminantByteSize(die);
    if (size && *size > 0 && *size < sizeof(uint64_t))
      bits &= (uint64_t{1} << (8 * *size)) - 1;
    discr_value = bits;
  }

  // Collect the data members.
  std::vector<LazySymbol> members;
  for (const DwarfDie& child : die.children()) {
    if (child.tag() == DwarfTag::kMember)
      members.push_back(MakeLazy(child));
  }

  std::optional<uint64_t> discr;
  if (discr_value)
    discr = *discr_value;

  return fxl::MakeRefCounted<Variant>(discr, std::move(members));
}

fxl::RefPtr<Symbol> DwarfSymbolFactory::DecodeVariantPart(const DwarfDie& die) const {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kDiscr};
  DwarfDieDecoder decoder = MakeDecoder(kAttrs);
  if (!decoder.Decode(die))
    return fxl::MakeRefCounted<Symbol>();

  // The discriminant is the DataMember in the variant whose value indicates which variant currently
  // applies.
  DwarfDie discriminant = decoder.GetReference(DwarfAttr::kDiscr);
  if (!discriminant)
    return fxl::MakeRefCounted<Symbol>();

  // Look for variants in this variant_part. It will also have a data member for the discriminant
  // but we will have already found that above via reference.
  std::vector<LazySymbol> variants;
  for (const DwarfDie& child : die.children()) {
    if (child.tag() == DwarfTag::kVariant)
      variants.push_back(MakeLazy(child));
  }

  return fxl::MakeRefCounted<VariantPart>(MakeLazy(discriminant), std::move(variants));
}

}  // namespace zxdb
