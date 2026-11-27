// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/function_name_index.h"

#include <algorithm>
#include <cstdlib>
#include <memory>

#include "llvm/Demangle/Demangle.h"
#include "src/developer/debug/zxdb/symbols/elf_symbol_record.h"

namespace zxdb {
namespace {

bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

bool IsIdentifierChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || IsAsciiDigit(c) || c == '_';
}

bool IsUnqualifiedName(std::string_view name) {
  if (name.starts_with('~'))
    name.remove_prefix(1);
  return !name.empty() && !IsAsciiDigit(name.front()) &&
         std::ranges::all_of(name, IsIdentifierChar);
}

using DemangledName = std::unique_ptr<char, decltype(&std::free)>;

std::optional<Identifier> ParseCppName(const llvm::ItaniumPartialDemangler& demangler,
                                       FunctionNameParser function_name_parser) {
  DemangledName full(demangler.getFunctionName(nullptr, nullptr), std::free);
  if (!full)
    return std::nullopt;
  std::string name(full.get());
  // The demangler decorates names such as foo[abi:cxx11] with their linkage-only ABI tags.
  size_t pos = 0;
  while ((pos = name.find("[abi:", pos)) != std::string::npos) {
    auto end = name.find(']', pos);
    if (end == std::string::npos)
      break;
    name.erase(pos, end + 1 - pos);
  }
  return function_name_parser(name);
}

bool MatchesScope(const IndexNode& root, const Identifier& name, const Identifier& query) {
  const auto& path = name.components();
  const auto& parts = query.components();
  if (path.size() < parts.size() ||
      !std::equal(parts.rbegin() + 1, parts.rend(), path.rbegin() + 1))
    return false;
  size_t omitted = path.size() - parts.size();
  if (omitted && query.qualification() == IdentifierQualification::kGlobal)
    return false;

  // Known class and function scopes must be written explicitly. When DWARF omits the
  // enclosing declarations, a linkage name alone cannot distinguish a class from a namespace.
  const IndexNode* node = &root;
  for (size_t i = 0; i < omitted; ++i) {
    const auto& component = path[i].name();
    auto ns = node->namespaces().find(component);
    if (ns == node->namespaces().end())
      return !node->types().contains(component) && !node->functions().contains(component);
    node = &ns->second;
  }
  return true;
}

}  // namespace

void FunctionNameIndex::AddLinkageName(const char* name, Ref ref, std::string_view indexed_name) {
  std::string_view linkage(name);
  if (!linkage.starts_with("_Z") && !linkage.starts_with("_R")) {
    if (linkage != indexed_name && IsUnqualifiedName(linkage))
      Add(Identifier(IdentifierQualification::kGlobal, IdentifierComponent(name)), ref);
    return;
  }
  if (!function_name_parser_)
    return;

  std::optional<Identifier> id;
  if (linkage.starts_with("_Z")) {
    llvm::ItaniumPartialDemangler demangler;
    if (demangler.partialDemangle(name) || !demangler.isFunction())
      return;
    if (auto elf = std::get_if<const ElfSymbolRecord*>(&ref)) {
      DemangledName base(demangler.getFunctionBaseName(nullptr, nullptr), std::free);
      if (base && IsUnqualifiedName(base.get())) {
        elf_by_base_name_.emplace(base.get(), *elf);
        return;
      }
    }
    id = ParseCppName(demangler, function_name_parser_);
  } else {
    DemangledName demangled(llvm::rustDemangle(linkage), std::free);
    if (demangled)
      id = function_name_parser_(demangled.get());
    if (id && !id->components().empty())
      AddRustAliases(*id, ref);
  }
  if (id && !(id->components().size() == 1 && id->components().front().name() == indexed_name))
    Add(std::move(*id), ref);
}

void FunctionNameIndex::AddRustAliases(Identifier& id, Ref ref) {
  // Rust writes generic functions as path::function::<args>. Keep the exact spelling and
  // also index the function name so a breakpoint can select all instantiations.
  auto& parts = id.components();
  if (parts.size() >= 2 && parts.back().name().starts_with('<')) {
    Add(id, ref);
    parts.pop_back();
  }
  // An inherent method is printed as <crate::Type>::method.
  const std::string& impl = parts.front().name();
  if (parts.size() >= 2 && impl.starts_with('<') && impl.ends_with('>') && !impl.contains(" as ")) {
    if (auto type = function_name_parser_(impl.substr(1, impl.size() - 2))) {
      Add(id, ref);
      type->components().insert(type->components().end(), parts.begin() + 1, parts.end());
      id = std::move(*type);
    }
  }
}

void FunctionNameIndex::AddQualifiedName(const std::string& name, Ref ref) {
  if (!function_name_parser_)
    return;
  if (auto id = function_name_parser_(name))
    Add(std::move(*id), ref);
}

void FunctionNameIndex::Add(Identifier name, Ref ref) {
  if (name.components().empty())
    return;
  std::string leaf = name.components().back().name();
  entries_.emplace(std::move(leaf), Entry{std::move(name), ref});
}

void FunctionNameIndex::Visit(const Identifier& query, bool prefix,
                              fit::function<bool(const Identifier&, const Ref&)> visitor) const {
  const auto& parts = query.components();
  if (parts.empty() || parts.back().special() != SpecialIdentifier::kNone)
    return;
  const std::string& leaf = parts.back().name();
  for (auto it = entries_.lower_bound(leaf); it != entries_.end(); ++it) {
    if (prefix ? !it->first.starts_with(leaf) : it->first != leaf)
      break;
    if (MatchesScope(root_, it->second.name, query) && !visitor(it->second.name, it->second.ref))
      return;
  }

  // LLVM's base name omits template arguments. Compare the full leaf after parsing a candidate.
  std::string_view base(leaf);
  base = base.substr(0, base.find('<'));
  for (auto it = elf_by_base_name_.lower_bound(base); it != elf_by_base_name_.end(); ++it) {
    if (prefix ? !it->first.starts_with(base) : it->first != base)
      break;
    llvm::ItaniumPartialDemangler demangler;
    if (demangler.partialDemangle(it->second->linkage_name.c_str()))
      continue;
    auto id = ParseCppName(demangler, function_name_parser_);
    if (!id || id->components().empty())
      continue;
    const auto& candidate = id->components().back().name();
    if ((prefix ? candidate.starts_with(leaf) : candidate == leaf) &&
        MatchesScope(root_, *id, query) && !visitor(*id, it->second))
      return;
  }
}

}  // namespace zxdb
