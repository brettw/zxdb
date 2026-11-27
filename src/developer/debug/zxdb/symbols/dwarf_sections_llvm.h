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
#include "src/developer/debug/zxdb/symbols/unit_index.h"

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

// Identifies an explicit address table provider. Its binary must outlive a split binary that
// caches views of the provider's sections. A null binary selects the default provider.
struct UnitContextSource {
  const DwarfBinary* binary = nullptr;
  UnitIndex index;
  bool operator==(const UnitContextSource&) const = default;
};

using UnitContextResult = ErrOr<std::shared_ptr<const UnitContext>>;

// Caches results for one binary. Unit context access stays on the owner thread.
class UnitContextCache {
 private:
  friend class DwarfBinaryImpl;
  friend class UnitContextCacheTest;
  UnitContextResult GetNormal(const DwarfSections& sections, const UnitHeader& header,
                              UnitSection kind);
  UnitContextResult GetSplit(const DwarfBinaryImpl& binary, UnitIndex index,
                             const UnitHeader& header, UnitSection kind,
                             UnitContextSource skeleton);
  UnitContextResult ParseSplit(const DwarfBinaryImpl& binary, const UnitHeader& header,
                               UnitSection kind, UnitContextSource skeleton, llvm::DWARFUnit& unit,
                               llvm::DWARFUnit* linked);
  std::shared_ptr<const UnitContext> GetAddressContext(const DwarfBinaryImpl& binary,
                                                       UnitContextSource provider,
                                                       llvm::DWARFUnit* implicit);
  void Clear();

  struct AbbrevSection {
    std::span<const uint8_t> bytes;
    uint64_t offset;
  };

  static std::optional<AbbrevSection> GetPackageAbbrev(const DwarfSections& sections,
                                                       llvm::DWARFContext& context,
                                                       const UnitHeader& header);
  std::shared_ptr<const AbbrevTable> GetDieAbbrevs(bool is_dwo, AbbrevSection section);
  std::shared_ptr<const ErrOr<AbbrevTable>> GetAbbrevs(bool is_dwo,
                                                       std::span<const uint8_t> section,
                                                       uint64_t offset);

  std::unordered_map<const UnitHeader*, UnitContextResult> normal_units_;

  // An explicit skeleton can come from another binary. An implicit LLVM link can change.
  struct SplitEntry {
    UnitContextSource skeleton;
    const llvm::DWARFUnit* linked;
    UnitContextResult result;
  };
  std::unordered_multimap<const UnitHeader*, SplitEntry> split_units_;

  // Section kind, table offset, and section or package contribution end. Raw DIE table builds
  // share these abbreviations with the owner thread.
  using AbbrevKey = std::tuple<bool, uint64_t, size_t>;
  std::mutex abbrev_mutex_;
  std::map<AbbrevKey, std::shared_ptr<const ErrOr<AbbrevTable>>> abbrevs_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_SECTIONS_LLVM_H_
