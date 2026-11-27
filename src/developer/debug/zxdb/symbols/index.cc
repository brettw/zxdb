// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/index.h"

#include <algorithm>
#include <initializer_list>
#include <string_view>
#include <variant>

#include "src/developer/debug/shared/string_util.h"
#include "src/developer/debug/zxdb/common/adapters.h"
#include "src/developer/debug/zxdb/common/file_util.h"
#include "src/developer/debug/zxdb/symbols/dwarf_binary.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_decoder.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_ref.h"
#include "src/developer/debug/zxdb/symbols/dwarf_die_scanner.h"
#include "src/developer/debug/zxdb/symbols/dwarf_tag.h"
#include "src/developer/debug/zxdb/symbols/dwarf_unit.h"
#include "src/developer/debug/zxdb/symbols/source_file_indexer.h"
#include "src/developer/debug/zxdb/symbols/symbol_utils.h"

namespace zxdb {

namespace {

// Don't index more than this number of levels to prevent infinite recursion.
constexpr size_t kMaxParentPath = 16;

// Stores a name with a SymbolRef for later indexing.
class NamedSymbolRef : public IndexNode::SymbolRef {
 public:
  struct Declaration {
    UnitIndex unit;
    uint32_t index = DieTable::kNoIndex;
    explicit operator bool() const { return index != DieTable::kNoIndex; }
  };

  NamedSymbolRef() = default;

  // Creates a SymbolRef we should index.
  //
  // WARNING: name is a non-owning string_view. Its backing storage must remain alive and unchanged
  // for this object's lifetime and the lifetime of every copy that borrows the same name.
  NamedSymbolRef(SymbolRef::Kind kind, int32_t dwo_index, DwarfDieRef die_ref, IndexNode::Kind k,
                 std::string_view name, const DwarfDie& declaration, bool has_abstract_origin)
      : SymbolRef(kind, dwo_index, die_ref),
        kind_(k),
        name_(name),
        declaration_(declaration ? Declaration{declaration.unit_index(), declaration.index()}
                                 : Declaration()),
        has_abstract_origin_(has_abstract_origin) {}

  bool should_index() const { return kind_ != IndexNode::Kind::kNone; }

  IndexNode::Kind kind() const { return kind_; }

  // The name associated with the DIE. Could be null.
  //
  // It's also possible for this to be valid for an otherwise !should_index() SymbolRef. In the case
  // of a function with a specification, the implementation will have should_index set, but we'll
  // traverse the specification to fill in the name. This will generate a valid but not indexable
  // item for the specification.
  std::string_view name() const {
    if (const auto* borrowed = std::get_if<std::string_view>(&name_))
      return *borrowed;
    return std::get<std::string>(name_);
  }
  void copy_name(const NamedSymbolRef& other) { name_ = other.name_; }
  void set_name(std::string n) { name_ = std::move(n); }
  // Borrows n with the same lifetime requirements as the constructor's name parameter.
  void set_name(const char* n) { name_ = std::string_view(n); }

  // A separate declaration supplies the name and lexical scope of a definition.
  const Declaration& declaration() const { return declaration_; }

  // The indexing layer uses this to cache the node found for a given thing. This allows us to
  // bypass lookup for the common case of things that are all in the same scope.
  IndexNode* index_node() const { return index_node_; }
  void set_index_node(IndexNode* n) { index_node_ = n; }

  // Sometimes we need to know whether an abstract origin is present for parent computations.
  //
  // When walking the dependency path, the abstract origin (if any) encodes the lexical scope.
  // As an example, DW_TAG_inlined_subroutine DIEs are inside of the function they're inlined into
  // (the calling function will be die.parent()). These will then have an abstract origin of a
  // DIE outside of the function containing the common info for all inlined instances.
  //
  // When there's no separate declaration, this abstract origin will be the scope that the function
  // was declared in where we index from. For:
  //
  //   namespace ns {
  //     inline void InlinedFunction() { ... }
  //   }
  //   void CallingFunction() {
  //     ns::InlinedFunction();
  //   }
  //
  // The DWARF would look like:
  //
  //   1: DW_TAG_namespace (name = "ns")
  //      2: DW_TAG_subprogram (name = "InlinedFunction")
  //   2: DW_TAG_subprogram (name = "CallingFunction")
  //      3: DW_TAG_inlined_subroutine (abstract_origin = 2)
  //
  // When there is a declaration, that declaration will encode the scope:
  //
  //   1: DW_TAG_namespace (name = "ns")
  //      2: DW_TAG_subprogram (name = "InlinedFunction", is_declaration = true)
  //   3: DW_TAG_subprogram (declaration = 2)
  //   2: DW_TAG_subprogram (name = "CallingFunction")
  //      3: DW_TAG_inlined_subroutine (abstract_origin = 3)
  bool has_abstract_origin() const { return has_abstract_origin_; }
  void set_has_abstract_origin(bool b) { has_abstract_origin_ = b; }

  // Set the flag that tells us we've modified the name with template types. This is primarily used
  // for dealing with symbols that have been generated without the simple-template-names compiler
  // flag, and the name already has the template types.
  bool has_modified_template_name() const { return has_modified_template_name_; }
  // Once we set this, it will never change.
  void set_has_modified_template_name() { has_modified_template_name_ = true; }

 private:
  IndexNode::Kind kind_ = IndexNode::Kind::kNone;
  // DWARF strings outlive the unit's indexing pass. Only synthesized template names need storage.
  std::variant<std::string_view, std::string> name_ = std::string_view();
  Declaration declaration_;
  IndexNode* index_node_ = nullptr;
  bool has_abstract_origin_ = false;
  bool has_modified_template_name_ = false;
};

bool HasAnyAttribute(const DwarfDie& die, std::initializer_list<DwarfAttr> attrs) {
  if (auto specs = die.attributes()) {
    for (const auto spec : *specs) {
      if (std::ranges::find(attrs, spec.attribute, [](DwarfAttr attr) {
            return static_cast<uint64_t>(attr);
          }) != attrs.end())
        return true;
    }
  }
  return false;
}

size_t RecursiveCountDies(const IndexNode& node) {
  size_t result = node.dies().size();

  for (int i = 0; i < static_cast<int>(IndexNode::Kind::kEndPhysical); i++) {
    for (const auto& pair : node.MapForKind(static_cast<IndexNode::Kind>(i)))
      result += RecursiveCountDies(pair.second);
  }
  return result;
}

// This helper class is used to index the symbols of one unit. It keeps some state to avoid
// reallocating for each call.
//
// Indexing is two passes. In the first pass we scan the DIEs in the unit. We identify which ones
// will need indexing and save information on the nesting. The parent chain information is important
// because we need to go from a DIE to its parent chain.
//
// In the second pass we actually index the items identified, using the saved parent and name
// information from the scan pass.
//
// In the second pass we can encounter some DIEs in the hierarchy chain that were not decoded in
// the first pass. An example is when going to a function declaration. We only identify the
// implementations in the first pass, but need to take the name from the declaration. These missing
// ones are filled in by FillDieInfo().
class UnitIndexer {
 public:
  // All passed-in objects must outlive this class.
  explicit UnitIndexer(const DwarfUnit& unit, int32_t dwo_index)
      : unit_(unit),
        dwo_index_(dwo_index),
        scanner_(unit),
        name_decoder_(*unit.GetBinary(), kNameAttrs),
        type_decoder_(*unit.GetBinary(), kTypeAttrs) {
    // The indexable array is 1:1 with the scanner entries.
    indexable_.resize(scanner_.die_count());

    path_.reserve(8);  // Don't want to reallocate.

    // The indexer finds names through declarations separately, so it doesn't need the attributes
    // of the DIEs that DW_AT_specification references. Not decoding them saves time.
    name_decoder_.set_follow_specifications(false);
    type_decoder_.set_follow_specifications(false);
  }

  // Forces indexing to go through the slow path (AddStandaloneEntryToIndex() instead of
  // AddEntryToIndex()) which can handle cross-unit references. This allows us to test the slow path
  // on the same data as the fast path and make sure they match.
  void set_force_slow_path(bool force) { force_slow_path_ = force; }

  // To use, first call Scan() to populate the indexable_ array, then call Index() to add the
  // items to the given index node root. The Scan pass will additionally add any entrypoint
  // functions it finds to the main_functions vector.
  void Scan(std::vector<IndexNode::SymbolRef>* main_functions, FunctionNameIndex* names);
  void Index(IndexNode* root);

 private:
  // Returns kNone for non-indexable items.
  //
  // The kVar case is also returned for collection members. These need to be treated as variables
  // when they have const data, but not otherwise, and this function does not decode the attributes.
  IndexNode::Kind GetKindForDie(const DwarfDie& die) const;

  // Looks for spellings that the enclosing scopes do not give us. Both names refer to the
  // definition, even when a separate declaration or abstract origin gives the scope.
  void IndexFunctionNames(FunctionNameIndex& names, const DwarfDie& die, const DwarfDie& scope,
                          const DwarfDieDecoder& decoder, std::optional<const char*> name);

  // Computes in the name and type for a DIE entry that wasn't filled in in the first pass (see
  // class-level comment). Returns empty string if there is no name (this is important for the
  // caller, see that code for more).
  //
  // This requires that the DIE be in the current unit_ (the decoder references the unit).
  const char* GetDieName(uint32_t index);

  // Adds the entry at the given index. If the entry is not in the current compilation unit, this
  // will fall back to the slow path: AddStandaloneEntryToIndex().
  void AddEntryToIndex(uint32_t index_me, IndexNode* root);

  // Slow path for adding an entry.
  //
  // This takes the index of a SymbolRef we want to index and adds it to the index without using
  // anything that references the compilation unit, notably the scanner_ which computes parent
  // information.
  //
  // This is used for the uncommon case of cross-unit references, where the declaration might be
  // in a different unit from the implementation. This means that the scanner's parent tree
  // doesn't cover the object we want. Walk the parent chain directly in that unit.
  void AddStandaloneEntryToIndex(uint32_t index_me, IndexNode* root);

  const DwarfUnit& unit_;
  int32_t dwo_index_ = IndexNode::SymbolRef::kMainBinary;

  bool force_slow_path_ = false;  // See setter above.

  DwarfDieScanner scanner_;
  std::vector<NamedSymbolRef> indexable_;

  // Variable used for collecting the path of parents in AddDIE. This would make more sense as a
  // local variable but having it here prevents reallocating each time.
  std::vector<NamedSymbolRef*> path_;

  // Used to decode names for DIEs in the second pass when we find one we need that wasn't extracted
  // in the first.
  static constexpr DwarfAttr kNameAttrs[] = {DwarfAttr::kName};
  DwarfDieDecoder name_decoder_;

  // Used to decode the types of template parameters.
  static constexpr DwarfAttr kTypeAttrs[] = {DwarfAttr::kType};
  DwarfDieDecoder type_decoder_;
};

// The symbol storage will be filled with the indexable entries.
void UnitIndexer::Scan(std::vector<IndexNode::SymbolRef>* main_functions,
                       FunctionNameIndex* names) {
  static constexpr DwarfAttr kCommonAttrs[] = {
      DwarfAttr::kSpecification,  DwarfAttr::kDeclaration, DwarfAttr::kConstValue,
      DwarfAttr::kMainSubprogram, DwarfAttr::kName,        DwarfAttr::kAbstractOrigin};
  static constexpr DwarfAttr kFunctionAttrs[] = {
      DwarfAttr::kSpecification,   DwarfAttr::kDeclaration, DwarfAttr::kConstValue,
      DwarfAttr::kMainSubprogram,  DwarfAttr::kName,        DwarfAttr::kAbstractOrigin,
      DwarfAttr::kMipsLinkageName, DwarfAttr::kLinkageName};
  DwarfDieDecoder common_decoder(*unit_.GetBinary(), kCommonAttrs);
  DwarfDieDecoder function_decoder(*unit_.GetBinary(), kFunctionAttrs);
  // See the constructor for why these don't follow specifications.
  common_decoder.set_follow_specifications(false);
  function_decoder.set_follow_specifications(false);

  for (; !scanner_.done(); scanner_.Advance()) {
    const DwarfDie& die = scanner_.Prepare();

    // Check whether we should consider this before decoding since decoding is slow.
    IndexNode::Kind kind = GetKindForDie(die);
    if (kind == IndexNode::Kind::kNone)
      continue;

    if (kind == IndexNode::Kind::kTemplateParameter) {
      auto parent_idx = die.parent_index();
      if (!parent_idx)
        continue;
      const auto& parent = indexable_[*parent_idx];
      // Names that already contain template arguments need no parameter decoding.
      if (!parent.has_modified_template_name() && debug::StringContains(parent.name(), "<"))
        continue;
    }

    // This DIE is of the type we want to index so decode.
    auto& decoder = kind == IndexNode::Kind::kFunction ? function_decoder : common_decoder;
    if (!decoder.Decode(die))
      continue;

    if (kind == IndexNode::Kind::kVar && die.tag() == DwarfTag::kMember &&
        !decoder.Has(DwarfAttr::kConstValue)) {
      // An abstract origin can supply the constant value, so confirm its presence after decoding.
      //
      // In C++ everything with a const_value will generally also be external (i.e. "static") which
      // are things we want to index. Theoretically the compiler could generated a const_value
      // member if it notices the member is never modified and optimize it. In that case, the user
      // would never expect to reference it outside of a known Collection object and it doesn't need
      // to be in the index. But that requires some extra work checking for the external flag in
      // this time-critical indexing step, and the worse thing is that "print MyClass::kMyConstant"
      // evaluates to a correct value where it might not be allowed in the actual language.
      //
      // As a result, we don't also check DW_AT_external.
      continue;
    } else if (kind == IndexNode::Kind::kTemplateParameter) {
      // This is a template type parameter. We don't need to index this, but we do need to fixup the
      // name of the parent DIE to reflect the template types. There can be any number of these for
      // a type.
      auto parent_idx = die.parent_index();
      if (!parent_idx)
        continue;

      NamedSymbolRef& parent = indexable_[*parent_idx];

      std::string fixup_template_name(parent.name());

      // Unfortunately, we need to decode again here to get the actual type instantiation. This
      // should be relatively quick and there should still be an overall speedup when using
      // -gsimple-template-names due to the drastically reduced symbol size.
      if (!type_decoder_.Decode(die))
        continue;

      if (DwarfDie type_die = type_decoder_.GetReference(DwarfAttr::kType)) {
        AddTemplateParameterToName(fixup_template_name, type_die.short_name());
        parent.set_name(std::move(fixup_template_name));
        parent.set_has_modified_template_name();
      }

      continue;
    }

    // Compute the offset of a separate declaration if this DIE has one. This can be unit-relative
    // or .debug_info-relative (global).
    DwarfDie decl = decoder.GetReference(DwarfAttr::kSpecification);

    FX_DCHECK(scanner_.die_index() < indexable_.size());
    std::optional<bool> is_declaration = decoder.GetBool(DwarfAttr::kDeclaration);
    auto ref_kind = is_declaration && *is_declaration ? IndexNode::SymbolRef::kDwarfDeclaration
                                                      : IndexNode::SymbolRef::kDwarf;

    std::optional<const char*> name = decoder.GetCString(DwarfAttr::kName);
    indexable_[scanner_.die_index()] =
        NamedSymbolRef(ref_kind, dwo_index_, die.ref(), kind, name ? *name : "", decl,
                       decoder.Has(DwarfAttr::kAbstractOrigin));

    if (kind == IndexNode::Kind::kFunction) {
      DwarfDie origin = decl ? DwarfDie() : decoder.GetReference(DwarfAttr::kAbstractOrigin);
      IndexFunctionNames(*names, die, decl ? decl : origin, decoder, name);
    }

    // Check for "main" function annotation.
    std::optional<bool> is_main_subprogram = decoder.GetBool(DwarfAttr::kMainSubprogram);
    if (kind == IndexNode::Kind::kFunction && is_main_subprogram && *is_main_subprogram)
      main_functions->emplace_back(IndexNode::SymbolRef::kDwarf, dwo_index_, die.ref());
  }
}

void UnitIndexer::IndexFunctionNames(FunctionNameIndex& names, const DwarfDie& die,
                                     const DwarfDie& scope, const DwarfDieDecoder& decoder,
                                     std::optional<const char*> name) {
  const NamedSymbolRef& ref = indexable_[scanner_.die_index()];
  auto is_direct = [&](DwarfAttr attr) {
    const auto* value = decoder.GetValue(attr);
    return value && value->die() == die;
  };
  auto short_name = [&]() -> const char* {
    // A direct attribute wins in both the attribute decoder and the name lookup.
    if (is_direct(DwarfAttr::kName))
      return name.value_or(nullptr);
    return scope ? die.short_name() : nullptr;
  };

  // The root is the first entry in the unit's DIE table. A declaration at this level may have
  // the whole qualified name in DW_AT_name.
  if ((scope ? scope : die).parent_index() == 0) {
    if (const char* spelling = short_name(); spelling && std::string_view(spelling).contains("::"))
      names.AddQualifiedName(spelling, ref);
  } else if (!ref.name().empty() || short_name()) {
    return;  // The enclosing declarations supply the scope.
  }

  // A linkage name can be the only name available.
  const char* linkage = nullptr;
  if (is_direct(DwarfAttr::kMipsLinkageName))
    linkage = decoder.GetCString(DwarfAttr::kMipsLinkageName).value_or(nullptr);
  else if (is_direct(DwarfAttr::kLinkageName))
    linkage = decoder.GetCString(DwarfAttr::kLinkageName).value_or(nullptr);
  else if (scope)
    linkage = die.linkage_name();
  if (linkage)
    names.AddLinkageName(linkage, ref, ref.name());
}

void UnitIndexer::Index(IndexNode* root) {
  if (force_slow_path_) {
    // Index everything with the slow path for testing purposes.
    for (uint32_t i = 0; i < indexable_.size(); i++) {
      if (indexable_[i].should_index())
        AddStandaloneEntryToIndex(i, root);
    }
  } else {
    // Normal fast-path. This is about 6x faster than the slow path for large programs.
    for (uint32_t i = 0; i < indexable_.size(); i++) {
      if (indexable_[i].should_index())
        AddEntryToIndex(i, root);
    }
  }
}

IndexNode::Kind UnitIndexer::GetKindForDie(const DwarfDie& die) const {
  switch (die.tag()) {
    case DwarfTag::kSubprogram:
    case DwarfTag::kInlinedSubroutine:
      if (HasAnyAttribute(die, {DwarfAttr::kLowPc, DwarfAttr::kHighPc, DwarfAttr::kRanges}))
        return IndexNode::Kind::kFunction;
      return IndexNode::Kind::kNone;  // Skip functions with no code.

    case DwarfTag::kNamespace:
      return IndexNode::Kind::kNamespace;

    case DwarfTag::kBaseType:
    case DwarfTag::kClassType:
    case DwarfTag::kEnumerationType:
    case DwarfTag::kPtrToMemberType:
    case DwarfTag::kStringType:
    case DwarfTag::kStructureType:
    case DwarfTag::kSubroutineType:
    case DwarfTag::kTypedef:
    case DwarfTag::kUnionType:
      return IndexNode::Kind::kType;

    case DwarfTag::kVariable:
      if (!scanner_.is_inside_function() && HasAnyAttribute(die, {DwarfAttr::kLocation})) {
        // Found variable storage outside of a function (variables inside functions are local so
        // don't get added to the global index).
        // TODO(bug 36671): index function-static variables.
        return IndexNode::Kind::kVar;
      }
      return IndexNode::Kind::kNone;  // Variable with no location.

    case DwarfTag::kMember:
      // Ordinary instance members are not part of the global symbol index.
      if (HasAnyAttribute(die, {DwarfAttr::kConstValue, DwarfAttr::kAbstractOrigin}))
        return IndexNode::Kind::kVar;
      return IndexNode::Kind::kNone;

    case DwarfTag::kTemplateTypeParameter:
    case DwarfTag::kTemplateValueParameter:
      return IndexNode::Kind::kTemplateParameter;

    default:
      // Don't index anything else.
      return IndexNode::Kind::kNone;
  }
}

const char* UnitIndexer::GetDieName(uint32_t index) {
  if (name_decoder_.Decode(scanner_.GetDieAtIndex(index))) {
    if (std::optional<const char*> name = name_decoder_.GetCString(DwarfAttr::kName))
      return *name;
  }
  return "";
}

// NOTE: Changes in this function may require updates in the slow path: AddStandaloneEntryToIndex().
void UnitIndexer::AddEntryToIndex(uint32_t index_me, IndexNode* root) {
  // The path to index always ends with the last thing being indexed (the path_ is in reverse).
  path_.clear();
  path_.push_back(&indexable_[index_me]);

  uint32_t cur = index_me;
  if (const auto& declaration = indexable_[index_me].declaration()) {
    // The declaration supplies the name. Most declarations belong to the current unit.
    if (declaration.unit != unit_.GetUnitIndex()) {
      AddStandaloneEntryToIndex(index_me, root);
      return;
    }
    cur = declaration.index;

    if (indexable_[index_me].name().empty()) {
      // When there's no name, take the name from the declaration.
      if (indexable_[cur].name().empty()) {
        // The declaration has no name because the first pass didn't need to index it. Compute
        // the name now. Caching it on both the declaration and the implementation is useful because
        // many implementation can share the same declaration and this saves multiple name
        // retrievals.
        //
        // Here GetDieName() returns the empty string if there's no name which allows us to cache
        // the lack of a name and not recompute.
        indexable_[cur].set_name(GetDieName(cur));
      }
      indexable_[index_me].copy_name(indexable_[cur]);
    }
  }

  // If at this point we still don't have a name for the thing being indexed, give up trying to
  // index it.
  if (indexable_[cur].name().empty())
    return;

  // Move to the abstract origin if present to start walking the scopes. The abstract origin (if
  // any) encodes the lexical scope (see has_abstract_origin() declaration above).
  if (indexable_[cur].has_abstract_origin()) {
    DwarfDie origin = scanner_.GetDieAtIndex(cur).abstract_origin();
    auto origin_index = unit_.GetIndexForDie(origin);
    if (!origin_index) {
      // The slow path handles an absent origin or an origin in another unit.
      AddStandaloneEntryToIndex(index_me, root);
      return;
    }
    cur = *origin_index;
  }

  // Goes to the parent. The first item was added above, the loop below will add going up the
  // parent chain from there.
  cur = scanner_.GetParentIndex(cur);

  // Start indexing from here. We may find a cached one that will prevent us from having to
  // go to the root.
  IndexNode* index_from = root;

  // Collect the path from the current item (path_[0]) to its ultimate parent (path_.back()).
  while (cur != DwarfDieScanner::kNoParent && indexable_[cur].should_index()) {
    if (path_.size() > kMaxParentPath)
      return;  // Too many components, consider this item corrupt and don't index.

    if (indexable_[cur].index_node()) {
      // Already indexed this node (for example, this is a namespace that was already referenced).
      // so we can start inserting the path from this node.
      index_from = indexable_[cur].index_node();
      break;
    }
    path_.push_back(&indexable_[cur]);
    cur = scanner_.GetParentIndex(cur);
  }

  // Add the path to the index (walk in reverse to start from the root).
  for (int path_i = static_cast<int>(path_.size()) - 1; path_i >= 0; path_i--) {
    NamedSymbolRef* named_ref = path_[path_i];

    // Only save the DIE reference for the thing we're attempting to index (the leaf node at
    // path_[0]). Intermediate things like the namespaces and classes along the path don't need DIE
    // references unless Scan() independently decided those need indexing (should_index()). Not only
    // is adding these DIEs unnecessary, it can create unnamed type entries for things like
    // anonymous enums which we don't want.
    if (path_i == 0)
      index_from = index_from->AddChild(named_ref->kind(), named_ref->name(), *named_ref);
    else
      index_from = index_from->AddChild(named_ref->kind(), named_ref->name());
    named_ref->set_index_node(index_from);
  }
}

// NOTE: Changes in this function may require updates in the fast path: AddEntryToIndex().
void UnitIndexer::AddStandaloneEntryToIndex(uint32_t index_me, IndexNode* index_root) {
  // The declaration and its parent DIEs can belong to another unit.

  // Thing to add to the index.
  NamedSymbolRef& named_ref = indexable_[index_me];

  // Compute the name (avoiding GetDieName()) and the DIE to start indexing from.
  std::string name(named_ref.name());
  DwarfDie die;
  if (const auto& declaration = named_ref.declaration()) {
    // A separate declaration supplies the lexical scope.
    if (declaration.unit == unit_.GetUnitIndex()) {
      die = scanner_.GetDieAtIndex(declaration.index);
    } else if (auto unit = unit_.GetBinary()->GetUnitAtIndex(declaration.unit)) {
      die = unit->GetDieAtIndex(declaration.index);
    }
    if (!die)
      return;
    if (name.empty()) {
      // The declaration can fill in the name if the name is not present on the implementation
      // (normally it's not there).
      const char* n = die.short_name();
      name = n ? n : "";
    }
  } else {
    // When there's no declaration, the name will already have been filled in (if present) to the
    // named_ref.
    die = scanner_.GetDieAtIndex(index_me);
  }

  if (name.empty())
    return;  // This item has no name, can't index it.

  named_ref.set_name(std::move(name));

  // When walking the dependency path, the abstract origin (if any) encodes the lexical scope
  // (see has_abstract_origin() declaration above).
  if (DwarfDie origin = die.abstract_origin())
    die = std::move(origin);

  // Stores the reverse path to the node we're inserting. This includes all scopes like namespaces
  // and classes in reverse order, but does not include the thing we're inserting itself.
  // So when insertng std::vector::vector this will be { "vector" (type), "std" (namespace) }.
  struct NameKind {
    NameKind(std::string_view n, IndexNode::Kind k) : name(n), kind(k) {}

    std::string_view name;
    IndexNode::Kind kind = IndexNode::Kind::kNone;
  };
  std::vector<NameKind> path;

  // Walk the path upward saving the path. Don't include the leaf DIE.
  while ((die = die.parent())) {
    if (path.size() > kMaxParentPath)
      return;  // Too deep nesting, give up.

    auto kind = GetKindForDie(die);
    if (kind == IndexNode::Kind::kNone)
      break;  // Hit the top of what we want to index (like the unit).

    const char* n = die.short_name();
    path.emplace_back(n ? n : "", kind);
  }

  // Insert the containing elements (in reverse order to start from the top level and work inwards).
  IndexNode* cur_index = index_root;
  for (const auto& name_kind : Reversed(path))
    cur_index = cur_index->AddChild(name_kind.kind, name_kind.name);

  // Add the leaf item (holding the DIE reference) to the index.
  cur_index->AddChild(named_ref.kind(), named_ref.name(), named_ref);
}

void RecursiveFindExact(const IndexNode* node, const Identifier& input, size_t input_index,
                        std::vector<IndexNode::SymbolRef>* result) {
  if (input_index == input.components().size()) {
    result->insert(result->end(), node->dies().begin(), node->dies().end());
    return;
  }

  // Recursively search each category in this node.
  const std::string name = input.components()[input_index].GetName(false);
  for (auto* map : {&node->namespaces(), &node->types(), &node->functions(), &node->vars()}) {
    auto found = map->find(name);
    if (found != map->end())  // Got a match for this category.
      RecursiveFindExact(&found->second, input, input_index + 1, result);
  }

  // Also implicitly search anonymous namespaces (without advancing the input index).
  auto found = node->namespaces().find("");
  if (found != node->namespaces().end())
    RecursiveFindExact(&found->second, input, input_index, result);
}

}  // namespace

void Index::CreateIndex(DwarfBinary& binary, int32_t dwo_index, bool force_slow_path) {
  // Start the source file work first so it runs on worker threads during DIE indexing.
  SourceFileIndexer source_files(CollectLineTableJobs(binary));

  size_t unit_count = binary.GetNormalUnitCount();
  for (size_t i = 0; i < unit_count; i++) {
    UnitIndex unit_index(false, i);
    if (auto unit_ref_ptr = binary.GetUnitAtIndex(unit_index))
      IndexCompileUnit(*unit_ref_ptr, dwo_index, force_slow_path);
  }

  // Index any DWO units. .dwo files store their units in a separate DWARF section. Normally there
  // will either be many normal units and no DWO units, or no normal units and exactly one DWO unit.
  // But this loops over all of both for generality.
  size_t dwo_unit_count = binary.GetDWOUnitCount();
  for (size_t i = 0; i < dwo_unit_count; i++) {
    UnitIndex unit_index(true, i);
    if (auto unit_ref_ptr = binary.GetUnitAtIndex(unit_index))
      IndexCompileUnit(*unit_ref_ptr, dwo_index, force_slow_path);
  }

  source_files.FinishInto(&files_);
  IndexFileNames();
}

void Index::DumpFileIndex(std::ostream& out) const {
  for (const auto& [filename, file_index_entry] : file_name_index_) {
    const auto& [filepath, compilation_units] = *file_index_entry;
    out << filename << " -> " << filepath << " -> " << compilation_units.size() << " units\n";
  }
}

std::vector<IndexNode::SymbolRef> Index::FindExact(const Identifier& input) const {
  std::vector<IndexNode::SymbolRef> result;
  RecursiveFindExact(&root_, input, 0, &result);
  return result;
}

std::vector<std::string> Index::FindFileMatches(std::string_view name) const {
  std::string_view name_last_comp = ExtractLastFileComponent(name);

  std::vector<std::string> result;

  // Search all files whose last component matches (the input may contain more than one component).
  FileNameIndex::const_iterator iter = file_name_index_.lower_bound(name_last_comp);
  while (iter != file_name_index_.end() && iter->first == name_last_comp) {
    const auto& pair = *iter->second;
    if (PathEndsWith(pair.first, name))
      result.push_back(pair.first);
    ++iter;
  }

  return result;
}

std::vector<std::string> Index::FindFilePrefixes(const std::string& prefix) const {
  std::vector<std::string> result;

  auto found = file_name_index_.lower_bound(prefix);
  while (found != file_name_index_.end() && debug::StringStartsWith(found->first, prefix)) {
    result.push_back(std::string(found->first));
    ++found;
  }
  return result;
}

const std::vector<UnitIndex>* Index::FindFileUnitIndices(const std::string& name) const {
  auto found = files_.find(name);
  if (found == files_.end())
    return nullptr;
  return &found->second;
}

size_t Index::CountSymbolsIndexed() const { return RecursiveCountDies(root_); }

void Index::IndexCompileUnit(const DwarfUnit& unit, int32_t dwo_index, bool force_slow_path) {
  DwarfDie unit_die = unit.GetUnitDie(DwarfDie::Load::kRootOnly);
  if (!unit_die.is_valid())
    return;

  if (unit_die.tag() == DwarfTag::kSkeletonUnit) {
    IndexSkeletonCompileUnit(unit, unit_die);
  } else {
    // Normal compilation unit.
    UnitIndexer indexer(unit, dwo_index);
    indexer.set_force_slow_path(force_slow_path);

    indexer.Scan(&main_functions_, &function_names_);
    indexer.Index(&root_);
  }

  // Evict the unit's DIE table immediately after indexing so transient parse structures do not
  // accumulate across all compilation units during indexing.
  unit.ClearDieCache();
}

// Skeleton units are units in the main binary that reference a .dwo file containing the symbols
// for just that compilation unit. The skeleton unit has the addr_base which defines how addresses
// are relocated from the link-independent .dwo file, as well as the line table.
void Index::IndexSkeletonCompileUnit(const DwarfUnit& unit, DwarfDie unit_die) {
  static constexpr DwarfAttr kAttrs[] = {DwarfAttr::kDwoName, DwarfAttr::kGnuDwoName,
                                         DwarfAttr::kCompDir, DwarfAttr::kAddrBase};
  DwarfDieDecoder unit_decoder(*unit.GetBinary(), kAttrs);
  if (!unit_decoder.Decode(unit_die))
    return;

  std::optional<const char*> dwo_name = unit_decoder.GetCString(DwarfAttr::kDwoName);
  std::optional<const char*> gnu_dwo_name = unit_decoder.GetCString(DwarfAttr::kGnuDwoName);
  std::optional<const char*> comp_dir = unit_decoder.GetCString(DwarfAttr::kCompDir);
  std::optional<uint64_t> addr_base = unit_decoder.GetSectionOffset(DwarfAttr::kAddrBase);
  if ((!dwo_name && !gnu_dwo_name) || !addr_base)
    return;  // Need a name for the .dwo file to work, plus an address.

  SkeletonUnit& skeleton = dwo_refs_.emplace_back();
  skeleton.skeleton_die_offset = unit_die.offset();
  skeleton.dwo_name = dwo_name ? *dwo_name : *gnu_dwo_name;

  skeleton.comp_dir = comp_dir ? *comp_dir : "";
  skeleton.addr_base = *addr_base;

  // The line table of a skeleton unit is in the main binary. CreateIndex() indexes it with the
  // other units.
}

void Index::IndexFileNames() {
  for (FileIndex::const_iterator iter = files_.begin(); iter != files_.end(); ++iter)
    file_name_index_.insert(std::make_pair(ExtractLastFileComponent(iter->first), iter));
}

}  // namespace zxdb
