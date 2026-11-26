// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_debug_names.h"

#include <optional>
#include <set>

#include "src/developer/debug/zxdb/common/checked_math.h"
#include "src/developer/debug/zxdb/symbols/dwarf/debug_names_reader.h"

namespace zxdb {

namespace {

// Skipping function-local variables avoids extracting compilation units during module-wide
// variable lookups. If the parent can't be read, keep the entry for normal DIE resolution.
bool IsLocalVariableEntry(const DebugNamesReader& names, const DebugNamesReader::Entry& entry) {
  if (entry.tag != DwarfTag::kVariable || !entry.parent_offset)
    return false;

  auto parent = names.ReadParent(entry);
  if (parent.has_error() || !parent.value())
    return false;

  DwarfTag tag = parent.value()->tag;
  return tag == DwarfTag::kSubprogram || tag == DwarfTag::kInlinedSubroutine ||
         tag == DwarfTag::kLexicalBlock;
}

std::optional<DwarfDieRef> ResolveEntryDieRef(const DebugNamesReader& names,
                                              const DebugNamesReader::Entry& entry) {
  if (!entry.die_unit_offset || IsLocalVariableEntry(names, entry))
    return std::nullopt;

  auto unit_offset = entry.cu_offset ? entry.cu_offset : entry.local_tu_offset;
  if (unit_offset) {
    if (auto offset = CheckedAdd(*unit_offset, *entry.die_unit_offset))
      return DwarfDieRef::Main(*offset);
  }

  // TODO(https://fxbug.dev/583730372): Resolve foreign TUs when DebugNamesEntry can identify the
  // owning DWO. LLDB's GetForeignTypeUnit uses the related CU to select the DWO.
  return std::nullopt;
}

}  // namespace

bool IsDwarf32DebugNamesTable(std::span<const uint8_t> section, uint64_t offset) {
  // A 64-bit table starts with the escape value 0xffffffff in place of its 32-bit unit length. The
  // escape value is the same in both byte orders.
  if (offset > section.size() || section.size() - offset < 4)
    return false;
  for (size_t i = 0; i < 4; i++) {
    if (section[offset + i] != 0xff)
      return true;
  }
  return false;
}

std::vector<DebugNamesEntry> FindDebugNamesEntries(const DebugNamesReader& debug_names,
                                                   std::string_view name) {
  std::vector<DebugNamesEntry> results;
  std::set<DwarfDieRef> seen;
  debug_names.LookupName(name, [&](std::string_view, const DebugNamesReader::Entry& entry) {
    auto ref = ResolveEntryDieRef(debug_names, entry);
    if (ref && seen.insert(*ref).second)
      results.push_back({.die_ref = *ref, .tag = entry.tag});
  });
  return results;
}

std::vector<DebugNamesEntry> FindDebugNamesPrefixEntries(const DebugNamesReader& debug_names,
                                                         std::string_view prefix) {
  std::vector<DebugNamesEntry> results;
  std::set<DwarfDieRef> seen;
  std::set<std::string_view> seen_names;
  debug_names.VisitNamesWithPrefix(prefix, [&](std::string_view name) {
    // A spelling can occur in more than one index. Look it up once across all indexes.
    if (!seen_names.insert(name).second)
      return;
    for (const auto& entry : FindDebugNamesEntries(debug_names, name)) {
      if (seen.insert(entry.die_ref).second)
        results.push_back(entry);
    }
  });
  return results;
}

}  // namespace zxdb
