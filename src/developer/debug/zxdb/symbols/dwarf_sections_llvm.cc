// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_sections_llvm.h"

#include <iterator>

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFObject.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf_binary_impl.h"

namespace zxdb {

namespace {

using SectionVisitor = llvm::function_ref<void(const llvm::DWARFSection&)>;
using VisitSections = void (llvm::DWARFObject::*)(SectionVisitor) const;

std::span<const uint8_t> GetSingleSection(const llvm::DWARFObject& object,
                                          VisitSections visit_sections) {
  std::span<const uint8_t> result;
  size_t count = 0;
  (object.*visit_sections)([&](const llvm::DWARFSection& section) {
    if (++count == 1)
      result = AsBytes(section.Data);
  });
  return count == 1 ? result : std::span<const uint8_t>();
}

const llvm::DWARFUnitIndex::Entry* GetPackageEntry(llvm::DWARFContext& context,
                                                   const UnitHeader& header) {
  const auto& index = header.is_type_unit() ? context.getTUIndex() : context.getCUIndex();
  auto signature = header.is_type_unit() ? header.type_signature : header.dwo_id;
  if (signature && index) {
    if (auto* entry = index.getFromHash(*signature))
      return entry;
  }
  return index.getFromOffset(header.offset);
}

UnitSection GetUnitSection(const llvm::DWARFUnit& unit) {
  return unit.isTypeUnit() && unit.getVersion() < 5 ? UnitSection::kTypes : UnitSection::kInfo;
}

struct UnitSections {
  UnitHeader header;
  std::span<const uint8_t> abbrev;
  std::optional<UnitContext::PackageContributions> package;
};

ErrOr<UnitSections> GetSplitUnitSections(const DwarfSections& sections, llvm::DWARFContext& context,
                                         UnitHeader header) {
  UnitSections result{.header = header, .abbrev = sections.abbrev_dwo, .package = std::nullopt};
  if (const auto* entry = GetPackageEntry(context, header)) {
    auto contribution =
        [entry](llvm::DWARFSectionKind section) -> std::optional<UnitContext::Contribution> {
      if (const auto* part = entry->getContribution(section))
        return UnitContext::Contribution{part->getOffset(), part->getLength()};
      return std::nullopt;
    };
    // The package index gives the bounds of this unit's abbreviation contribution.
    auto abbrev = contribution(llvm::DW_SECT_ABBREV);
    if (!abbrev || abbrev->offset > result.abbrev.size() ||
        abbrev->size > result.abbrev.size() - abbrev->offset) {
      return Err("Invalid DWARF package abbreviation contribution.");
    }
    result.header.abbrev_offset = abbrev->offset;
    result.abbrev = result.abbrev.first(abbrev->offset + abbrev->size);
    result.package = UnitContext::PackageContributions{
        contribution(llvm::DW_SECT_STR_OFFSETS),
        contribution(llvm::DW_SECT_LOCLISTS),
        contribution(llvm::DW_SECT_RNGLISTS),
    };
  }
  return result;
}

UnitContextResult ShareContext(ErrOr<UnitContext> context) {
  if (!context)
    return context.err();
  return std::make_shared<const UnitContext>(context.take_value());
}

}  // namespace

DwarfSections GetDwarfSections(const llvm::DWARFObject& object) {
  return {
      .info = GetSingleSection(object, &llvm::DWARFObject::forEachInfoSections),
      .abbrev = AsBytes(object.getAbbrevSection()),
      .str = AsBytes(object.getStrSection()),
      .str_offsets = AsBytes(object.getStrOffsetsSection().Data),
      .line = AsBytes(object.getLineSection().Data),
      .line_str = AsBytes(object.getLineStrSection()),
      .addr = AsBytes(object.getAddrSection().Data),
      .ranges = AsBytes(object.getRangesSection().Data),
      .rnglists = AsBytes(object.getRnglistsSection().Data),
      .loc = AsBytes(object.getLocSection().Data),
      .loclists = AsBytes(object.getLoclistsSection().Data),
      .aranges = AsBytes(object.getArangesSection()),
      .names = AsBytes(object.getNamesSection().Data),
      .types = GetSingleSection(object, &llvm::DWARFObject::forEachTypesSections),
      .info_dwo = GetSingleSection(object, &llvm::DWARFObject::forEachInfoDWOSections),
      .abbrev_dwo = AsBytes(object.getAbbrevDWOSection()),
      .str_dwo = AsBytes(object.getStrDWOSection()),
      .str_offsets_dwo = AsBytes(object.getStrOffsetsDWOSection().Data),
      .line_dwo = AsBytes(object.getLineDWOSection().Data),
      .loc_dwo = AsBytes(object.getLocDWOSection().Data),
      .loclists_dwo = AsBytes(object.getLoclistsDWOSection().Data),
      .rnglists_dwo = AsBytes(object.getRnglistsDWOSection().Data),
      .types_dwo = GetSingleSection(object, &llvm::DWARFObject::forEachTypesDWOSections),
      .endian = object.isLittleEndian() ? Endian::kLittle : Endian::kBig,
  };
}

DwarfSections GetDwarfSections(const llvm::DWARFContext& context) {
  return GetDwarfSections(context.getDWARFObj());
}

void UnitContextCache::Clear() {
  normal_units_ = decltype(normal_units_)();
  split_units_ = decltype(split_units_)();
  std::lock_guard lock(abbrev_mutex_);
  abbrevs_.clear();
}

std::optional<UnitContextCache::AbbrevSection> UnitContextCache::GetPackageAbbrev(
    const DwarfSections& sections, llvm::DWARFContext& context, const UnitHeader& header) {
  auto data = GetSplitUnitSections(sections, context, header);
  if (!data)
    return std::nullopt;
  return AbbrevSection{data.value().abbrev, data.value().header.abbrev_offset};
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
  auto* unit = binary.GetLLVMUnit(index);
  if (!unit)
    return Err("Missing split DWARF unit metadata.");
  auto* linked = skeleton.binary ? nullptr : unit->getLinkedUnit();
  // A foreign implicit skeleton has no known owner to keep it alive in this cache.
  bool cacheable = !linked || &linked->getContext() == binary.GetLLVMContext();
  if (cacheable) {
    auto [begin, end] = split_units_.equal_range(&header);
    for (auto it = begin; it != end; ++it) {
      if (it->second.skeleton == skeleton && it->second.linked == linked)
        return it->second.result;
    }
  }

  auto result = ParseSplit(binary, header, kind, skeleton, *unit, linked);
  if (cacheable)
    split_units_.emplace(&header, SplitEntry{skeleton, linked, result});
  return result;
}

UnitContextResult UnitContextCache::ParseSplit(const DwarfBinaryImpl& binary,
                                               const UnitHeader& header, UnitSection kind,
                                               UnitContextSource skeleton, llvm::DWARFUnit& unit,
                                               llvm::DWARFUnit* linked) {
  auto sections = binary.GetSections();
  auto data = GetSplitUnitSections(sections, unit.getContext(), header);
  if (!data)
    return data.err();
  sections.abbrev_dwo = data.value().abbrev;
  auto* address_unit = linked;
  if (!skeleton.binary && !address_unit) {
    // LLVM uses the sole main info unit as the address table when there is no link.
    auto units = unit.getContext().info_section_units();
    if (units.begin() != units.end() && std::next(units.begin()) == units.end())
      address_unit = units.begin()->get();
  }
  auto address = GetAddressContext(binary, skeleton, address_unit);
  UnitContext::Options options{.is_dwo = true, .package = data.value().package};
  if (skeleton.binary || linked)
    options.skeleton = address.get();
  else
    options.address_table = address.get();
  auto abbrevs = GetAbbrevs(true, sections.abbrev_dwo, data.value().header.abbrev_offset);
  return ShareContext(
      UnitContext::Parse(sections, data.value().header, kind, options, abbrevs.get()));
}

std::shared_ptr<const UnitContext> UnitContextCache::GetAddressContext(
    const DwarfBinaryImpl& binary, UnitContextSource provider, llvm::DWARFUnit* implicit) {
  if (!provider.binary) {
    if (!implicit || implicit->isDWOUnit())
      return {};
    UnitSection kind = GetUnitSection(*implicit);
    uint64_t offset = implicit->getOffset();
    if (&implicit->getContext() != binary.GetLLVMContext()) {
      // The foreign unit has no known binary to cache its context.
      auto sections = GetDwarfSections(implicit->getContext());
      auto info = kind == UnitSection::kInfo ? sections.info : sections.types;
      auto header = ReadUnitHeader(info, offset, sections.endian, kind);
      if (!header)
        return {};
      return ShareContext(UnitContext::Parse(sections, header.value(), kind)).take_value_or_empty();
    }
    auto index = binary.FindUnitForRef(
        kind == UnitSection::kInfo ? DwarfDieRef::Main(offset) : DwarfDieRef::Type(offset), false);
    if (!index)
      return {};
    provider = {&binary, *index};
  }

  if (provider.index.is_dwo)
    return {};
  if (provider.binary != &binary)
    return provider.binary->GetUnitContext(provider.index).take_value_or_empty();
  // This binary already holds its owner-thread cache guard.
  auto entry = binary.GetUnitHeader(provider.index);
  return entry
             ? GetNormal(binary.GetSections(), entry->header, entry->section).take_value_or_empty()
             : nullptr;
}

}  // namespace zxdb
