// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_UNIT_CONTEXT_CACHE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_UNIT_CONTEXT_CACHE_H_

#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>

#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_sections.h"
#include "src/developer/debug/zxdb/symbols/dwarf/unit_context.h"
#include "src/developer/debug/zxdb/symbols/unit_context_source.h"

namespace zxdb {

class DwarfBinaryImpl;

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
  UnitContextResult ParseSplit(const DwarfBinaryImpl& binary, UnitIndex index,
                               const UnitHeader& header, UnitSection kind,
                               UnitContextSource skeleton);
  std::optional<UnitIndex> FindSkeleton(const DwarfBinaryImpl& binary, uint64_t dwo_id);
  std::shared_ptr<const UnitContext> GetSkeletonContext(const DwarfBinaryImpl& binary,
                                                        UnitContextSource skeleton);
  void Clear();

  struct AbbrevSection {
    std::span<const uint8_t> bytes;
    uint64_t offset;
  };

  std::shared_ptr<const AbbrevTable> GetDieAbbrevs(bool is_dwo, AbbrevSection section);
  std::shared_ptr<const ErrOr<AbbrevTable>> GetAbbrevs(bool is_dwo,
                                                       std::span<const uint8_t> section,
                                                       uint64_t offset);

  std::unordered_map<const UnitHeader*, UnitContextResult> normal_units_;

  // An explicit skeleton can come from another binary.
  struct SplitEntry {
    UnitContextSource skeleton;
    UnitContextResult result;
  };
  std::unordered_multimap<const UnitHeader*, SplitEntry> split_units_;

  // Built on first lookup. An empty value marks an ID shared by multiple skeletons.
  std::optional<std::unordered_map<uint64_t, std::optional<UnitIndex>>> skeletons_;

  // Section kind, table offset, and section or package contribution end. Raw DIE table builds
  // share these abbreviations with the owner thread.
  using AbbrevKey = std::tuple<bool, uint64_t, size_t>;
  std::mutex abbrev_mutex_;
  std::map<AbbrevKey, std::shared_ptr<const ErrOr<AbbrevTable>>> abbrevs_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_UNIT_CONTEXT_CACHE_H_
