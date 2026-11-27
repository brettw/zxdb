// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/unit_context_cache.h"

#include "src/developer/debug/zxdb/symbols/dwarf_binary_impl.h"

namespace zxdb {

namespace {

UnitContextResult ShareContext(ErrOr<UnitContext> context) {
  if (!context)
    return context.err();
  return std::make_shared<const UnitContext>(context.take_value());
}

}  // namespace

void UnitContextCache::Clear() {
  normal_units_ = decltype(normal_units_)();
  split_units_ = decltype(split_units_)();
  skeletons_.reset();
  std::lock_guard lock(abbrev_mutex_);
  abbrevs_.clear();
}

std::shared_ptr<const AbbrevTable> UnitContextCache::GetDieAbbrevs(bool is_dwo,
                                                                   AbbrevSection section) {
  auto abbrevs = GetAbbrevs(is_dwo, section.bytes, section.offset);
  if (!*abbrevs)
    return {};
  const auto* table = &abbrevs->value();
  return std::shared_ptr<const AbbrevTable>(std::move(abbrevs), table);
}

std::shared_ptr<const ErrOr<AbbrevTable>> UnitContextCache::GetAbbrevs(
    bool is_dwo, std::span<const uint8_t> section, uint64_t offset) {
  AbbrevKey key(is_dwo, offset, section.size());
  std::lock_guard lock(abbrev_mutex_);
  auto found = abbrevs_.find(key);
  if (found != abbrevs_.end())
    return found->second;
  return abbrevs_
      .emplace(key, std::make_shared<const ErrOr<AbbrevTable>>(AbbrevTable::Parse(section, offset)))
      .first->second;
}

UnitContextResult UnitContextCache::GetNormal(const DwarfSections& sections,
                                              const UnitHeader& header, UnitSection kind) {
  auto found = normal_units_.find(&header);
  if (found != normal_units_.end())
    return found->second;
  auto abbrevs = GetAbbrevs(false, sections.abbrev, header.abbrev_offset);
  auto result = ShareContext(UnitContext::Parse(sections, header, kind, {}, abbrevs.get()));
  return normal_units_.emplace(&header, std::move(result)).first->second;
}

UnitContextResult UnitContextCache::GetSplit(const DwarfBinaryImpl& binary, UnitIndex index,
                                             const UnitHeader& header, UnitSection kind,
                                             UnitContextSource skeleton) {
  auto [begin, end] = split_units_.equal_range(&header);
  for (auto it = begin; it != end; ++it) {
    if (it->second.skeleton == skeleton)
      return it->second.result;
  }

  auto result = ParseSplit(binary, index, header, kind, skeleton);
  return split_units_.emplace(&header, SplitEntry{skeleton, std::move(result)})->second.result;
}

UnitContextResult UnitContextCache::ParseSplit(const DwarfBinaryImpl& binary, UnitIndex index,
                                               const UnitHeader& header, UnitSection kind,
                                               UnitContextSource skeleton) {
  const auto& sections = binary.GetSections();
  AbbrevSection abbrev{sections.abbrev_dwo, header.abbrev_offset};
  UnitContext::Options options{.is_dwo = true};
  if (!binary.package_units_.empty()) {
    const auto& package = binary.package_units_[index.index];
    if (!package)
      return package.err();
    if (package.value()) {
      abbrev = package.value()->abbrev;
      options.package = package.value()->contributions;
    }
  }
  auto abbrevs = GetAbbrevs(true, abbrev.bytes, abbrev.offset);
  auto parse = [&](const UnitContext* provider) {
    options.skeleton = provider;
    return UnitContext::Parse(sections, header, kind, options, abbrevs.get());
  };

  if (skeleton.binary || binary.info_units_.size() == 0 || header.is_type_unit()) {
    auto address = GetSkeletonContext(binary, skeleton);
    return ShareContext(parse(address.get()));
  }

  auto id = header.dwo_id;
  std::optional<ErrOr<UnitContext>> unlinked;
  if (header.version < 5) {
    unlinked.emplace(parse(nullptr));
    id = *unlinked ? unlinked->value().dwo_id() : std::nullopt;
  }
  auto match = id ? FindSkeleton(binary, *id) : std::nullopt;
  auto address = match ? GetSkeletonContext(binary, {&binary, *match}) : nullptr;
  if (!address && unlinked)
    return ShareContext(std::move(*unlinked));
  return ShareContext(parse(address.get()));
}

std::optional<UnitIndex> UnitContextCache::FindSkeleton(const DwarfBinaryImpl& binary,
                                                        uint64_t dwo_id) {
  if (!skeletons_) {
    auto& map = skeletons_.emplace();
    for (uint32_t i = 0; i < binary.info_units_.size(); ++i) {
      const auto& header = binary.info_units_[i];
      std::optional<uint64_t> id;
      if (header.unit_type == kUnitTypeSkeleton) {
        id = header.dwo_id;
      } else if (header.version < 5 && header.unit_type == kUnitTypeCompile) {
        auto context = GetNormal(binary.GetSections(), header, UnitSection::kInfo);
        if (context)
          id = context.value()->dwo_id();
      }
      if (!id)
        continue;
      auto [entry, inserted] = map.try_emplace(*id, UnitIndex(false, i));
      if (!inserted)
        entry->second.reset();
    }
  }

  // Like LLDB, link only a unique matching DWO ID. LLVM 19 can use the only main
  // info unit even when its ID differs.
  auto found = skeletons_->find(dwo_id);
  return found == skeletons_->end() ? std::nullopt : found->second;
}

std::shared_ptr<const UnitContext> UnitContextCache::GetSkeletonContext(
    const DwarfBinaryImpl& binary, UnitContextSource skeleton) {
  if (!skeleton.binary || skeleton.index.is_dwo)
    return {};
  if (skeleton.binary != &binary)
    return skeleton.binary->GetUnitContext(skeleton.index).take_value_or_empty();

  // This binary already holds its owner-thread cache guard.
  auto unit = binary.GetUnitHeader(skeleton.index);
  return unit ? GetNormal(binary.GetSections(), unit->header, unit->section).take_value_or_empty()
              : nullptr;
}

}  // namespace zxdb
