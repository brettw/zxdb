// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_FUNCTION_NAME_INDEX_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_FUNCTION_NAME_INDEX_H_

#include <lib/fit/function.h>

#include <map>
#include <string_view>
#include <variant>

#include "src/developer/debug/zxdb/symbols/identifier.h"
#include "src/developer/debug/zxdb/symbols/index_node.h"

namespace zxdb {

struct ElfSymbolRecord;

// Alternate spellings for functions whose scope is present in the name itself.
class FunctionNameIndex {
 public:
  using Ref = std::variant<IndexNode::SymbolRef, const ElfSymbolRecord*>;

  FunctionNameIndex(const IndexNode& root, FunctionNameParser function_name_parser)
      : root_(root), function_name_parser_(function_name_parser) {}

  void Add(Identifier name, Ref ref);
  void AddLinkageName(const char* name, Ref ref, std::string_view indexed_name = {});
  void AddQualifiedName(const std::string& name, Ref ref);
  // Merges the DWARF entries of a split unit's index. ELF records are added to the module's own
  // index later, so other never has any.
  void MergeFrom(FunctionNameIndex& other) { entries_.merge(other.entries_); }

  // A relative query can omit enclosing scopes. Prefix matching applies to the leaf only.
  // Return false from the visitor to stop.
  void Visit(const Identifier& query, bool prefix,
             fit::function<bool(const Identifier&, const Ref&)> visitor) const;

 private:
  // Adds demangler spellings and leaves the source spelling in id.
  void AddRustAliases(Identifier& id, Ref ref);

  struct Entry {
    Identifier name;
    Ref ref;
  };
  const IndexNode& root_;
  FunctionNameParser function_name_parser_;
  std::multimap<std::string, Entry> entries_;
  // ELF records live in the module. Only matching C++ base names need the full parser.
  std::multimap<std::string, const ElfSymbolRecord*, std::less<>> elf_by_base_name_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_FUNCTION_NAME_INDEX_H_
