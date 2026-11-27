// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_SECTIONS_LLVM_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_SECTIONS_LLVM_H_

#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>

#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_sections.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_context.h"

namespace llvm {
class DWARFContext;
class DWARFObject;
class DWARFUnit;
}  // namespace llvm

namespace zxdb {

class DwarfBinary;
class DwarfBinaryImpl;

// Converts LLVM objects to section views for the byte readers.
DwarfSections GetDwarfSections(const llvm::DWARFObject& object);
DwarfSections GetDwarfSections(const llvm::DWARFContext& context);

// LLVM gives normal units the main sections and DWO units the corresponding .dwo sections.
std::span<const uint8_t> GetUnitLineSection(const DwarfSections& sections,
                                            const llvm::DWARFUnit& unit);
std::span<const uint8_t> GetUnitStrSection(const DwarfSections& sections,
                                           const llvm::DWARFUnit& unit);
std::span<const uint8_t> GetUnitStrOffsetsSection(const DwarfSections& sections,
                                                  const llvm::DWARFUnit& unit);

// Identifies a skeleton and, when available, its binary. The skeleton binary must outlive the
// split binary that caches a view of its section data.
struct UnitContextSource {
  llvm::DWARFUnit* unit = nullptr;
  const DwarfBinary* binary = nullptr;
};

using UnitContextResult = ErrOr<std::shared_ptr<const UnitContext>>;

// Parses the requested unit from these sections without caching it. Call on the LLVM context
// thread and keep the section bytes alive while using the result.
UnitContextResult ReadUnitContextUncached(const DwarfSections& sections, llvm::DWARFUnit& unit,
                                          UnitContextSource skeleton = {});

// Uses the binary's cache if it owns the unit. Otherwise parses from fallback_sections or from
// the unit's LLVM context if the caller supplies no sections.
UnitContextResult GetUnitContext(llvm::DWARFUnit& unit, const DwarfBinary* binary,
                                 UnitContextSource skeleton = {},
                                 const DwarfSections* fallback_sections = nullptr);

// Caches results for one binary. Unit context access stays on the owner thread.
class UnitContextCache {
 private:
  friend class DwarfBinaryImpl;
  friend class UnitContextCacheTest;
  friend UnitContextResult ReadUnitContextUncached(const DwarfSections&, llvm::DWARFUnit&,
                                                   UnitContextSource);

  UnitContextResult GetOrParse(const DwarfSections& sections, llvm::DWARFUnit& unit,
                               UnitContextSource skeleton);
  void Clear();

  struct Entry {
    llvm::DWARFUnit* skeleton;
    UnitContextResult result;
  };

  struct AbbrevSection {
    std::span<const uint8_t> bytes;
    uint64_t offset;
  };

  static std::optional<AbbrevSection> GetPackageAbbrev(const DwarfSections& sections,
                                                       llvm::DWARFContext& context,
                                                       const UnitHeader& header);
  static UnitContextResult Parse(const DwarfSections& sections, llvm::DWARFUnit& unit,
                                 UnitContextSource skeleton, UnitContextCache* cache);
  std::shared_ptr<const AbbrevTable> GetDieAbbrevs(bool is_dwo, AbbrevSection section);
  std::shared_ptr<const ErrOr<AbbrevTable>> GetAbbrevs(bool is_dwo,
                                                       std::span<const uint8_t> section,
                                                       uint64_t offset);

  // A split unit can be read with a skeleton or before its skeleton is known.
  std::unordered_multimap<const llvm::DWARFUnit*, Entry> units_;

  // Section kind, table offset, and section or package contribution end. Raw DIE table builds
  // share these abbreviations with the owner thread.
  using AbbrevKey = std::tuple<bool, uint64_t, size_t>;
  std::mutex abbrev_mutex_;
  std::map<AbbrevKey, std::shared_ptr<const ErrOr<AbbrevTable>>> abbrevs_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_SECTIONS_LLVM_H_
