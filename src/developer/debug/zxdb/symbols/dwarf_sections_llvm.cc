// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_sections_llvm.h"

#include <iterator>

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFObject.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf_binary.h"

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

const llvm::DWARFUnitIndex::Entry* GetPackageEntry(llvm::DWARFContext* context,
                                                   const UnitHeader& header, bool is_dwo) {
  if (!context || !is_dwo)
    return nullptr;

  const auto& index = header.is_type_unit() ? context->getTUIndex() : context->getCUIndex();
  auto signature = header.is_type_unit() ? header.type_signature : header.dwo_id;
  if (signature && index) {
    if (auto* entry = index.getFromHash(*signature))
      return entry;
  }
  return index.getFromOffset(header.offset);
}

UnitContextSource GetLinkedSkeleton(llvm::DWARFUnit& unit, UnitContextSource skeleton) {
  if (!unit.isDWOUnit())
    return {};
  return skeleton.unit ? skeleton : UnitContextSource{unit.getLinkedUnit()};
}

UnitSection GetUnitSection(const llvm::DWARFUnit& unit) {
  return unit.isTypeUnit() && unit.getVersion() < 5 ? UnitSection::kTypes : UnitSection::kInfo;
}

std::span<const uint8_t> GetInfoSection(const DwarfSections& sections, bool is_dwo,
                                        UnitSection kind) {
  if (kind == UnitSection::kTypes)
    return is_dwo ? sections.types_dwo : sections.types;
  return is_dwo ? sections.info_dwo : sections.info;
}

struct UnitSections {
  UnitHeader header;
  std::span<const uint8_t> abbrev;
  std::optional<UnitContext::PackageContributions> package;
};

ErrOr<UnitSections> GetUnitSections(const DwarfSections& sections, llvm::DWARFContext* context,
                                    UnitHeader header, bool is_dwo) {
  UnitSections result{.header = header,
                      .abbrev = is_dwo ? sections.abbrev_dwo : sections.abbrev,
                      .package = std::nullopt};
  if (const auto* entry = GetPackageEntry(context, header, is_dwo)) {
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

std::span<const uint8_t> GetUnitLineSection(const DwarfSections& sections,
                                            const llvm::DWARFUnit& unit) {
  return unit.isDWOUnit() ? sections.line_dwo : sections.line;
}

std::span<const uint8_t> GetUnitStrSection(const DwarfSections& sections,
                                           const llvm::DWARFUnit& unit) {
  return unit.isDWOUnit() ? sections.str_dwo : sections.str;
}

std::span<const uint8_t> GetUnitStrOffsetsSection(const DwarfSections& sections,
                                                  const llvm::DWARFUnit& unit) {
  return unit.isDWOUnit() ? sections.str_offsets_dwo : sections.str_offsets;
}

UnitContextResult ReadUnitContextUncached(const DwarfSections& sections, llvm::DWARFUnit& unit,
                                          UnitContextSource skeleton) {
  return UnitContextCache::Parse(sections, unit, GetLinkedSkeleton(unit, skeleton), nullptr);
}

UnitContextResult GetUnitContext(llvm::DWARFUnit& unit, const DwarfBinary* binary,
                                 UnitContextSource skeleton,
                                 const DwarfSections* fallback_sections) {
  if (binary && binary->GetLLVMContext() == &unit.getContext())
    return binary->GetUnitContext(unit, skeleton);
  if (fallback_sections)
    return ReadUnitContextUncached(*fallback_sections, unit, skeleton);
  return ReadUnitContextUncached(GetDwarfSections(unit.getContext()), unit, skeleton);
}

UnitContextResult UnitContextCache::GetOrParse(const DwarfSections& sections, llvm::DWARFUnit& unit,
                                               UnitContextSource skeleton) {
  skeleton = GetLinkedSkeleton(unit, skeleton);
  // A foreign skeleton without a known owner can disappear while this cache still exists.
  if (skeleton.unit && &skeleton.unit->getContext() != &unit.getContext() &&
      (!skeleton.binary || skeleton.binary->GetLLVMContext() != &skeleton.unit->getContext())) {
    return Parse(sections, unit, skeleton, this);
  }

  auto [begin, end] = units_.equal_range(&unit);
  for (auto it = begin; it != end; ++it) {
    const auto& entry = it->second;
    if (entry.skeleton == skeleton.unit)
      return entry.result;
  }

  // Section data is immutable. Keep parse errors too, so a bad unit costs one parse.
  auto result = Parse(sections, unit, skeleton, this);
  units_.emplace(&unit, Entry{skeleton.unit, result});
  return result;
}

void UnitContextCache::Clear() {
  units_ = decltype(units_)();
  std::lock_guard lock(abbrev_mutex_);
  abbrevs_.clear();
}

std::optional<UnitContextCache::AbbrevSection> UnitContextCache::GetPackageAbbrev(
    const DwarfSections& sections, llvm::DWARFContext& context, const UnitHeader& header) {
  auto data = GetUnitSections(sections, &context, header, true);
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

UnitContextResult UnitContextCache::Parse(const DwarfSections& sections, llvm::DWARFUnit& unit,
                                          UnitContextSource skeleton, UnitContextCache* cache) {
  bool is_dwo = unit.isDWOUnit();
  UnitSection kind = GetUnitSection(unit);
  auto header = ReadUnitHeader(GetInfoSection(sections, is_dwo, kind), unit.getOffset(),
                               sections.endian, kind);
  if (!header)
    return header.err();
  auto data = GetUnitSections(sections, &unit.getContext(), header.value(), is_dwo);
  if (!data)
    return data.err();

  DwarfSections unit_sections = sections;
  if (is_dwo)
    unit_sections.abbrev_dwo = data.value().abbrev;
  UnitContext::Options options;
  options.is_dwo = is_dwo;
  options.package = data.value().package;

  std::shared_ptr<const UnitContext> address_context;
  if (is_dwo) {
    auto* address_unit = skeleton.unit;
    if (!address_unit) {
      // LLVM's address lookup uses the sole main info unit when there is no explicit link.
      auto main_units = unit.getContext().info_section_units();
      if (main_units.begin() != main_units.end() &&
          std::next(main_units.begin()) == main_units.end()) {
        address_unit = main_units.begin()->get();
      }
    }
    if (address_unit && !address_unit->isDWOUnit()) {
      if (&address_unit->getContext() == &unit.getContext()) {
        address_context = (cache ? cache->GetOrParse(sections, *address_unit, {})
                                 : ReadUnitContextUncached(sections, *address_unit))
                              .take_value_or_empty();
      } else {
        address_context = GetUnitContext(*address_unit, skeleton.binary).take_value_or_empty();
      }
      if (skeleton.unit)
        options.skeleton = address_context.get();
      else
        options.address_table = address_context.get();
    }
  }

  auto abbrevs =
      cache ? cache->GetAbbrevs(is_dwo, data.value().abbrev, data.value().header.abbrev_offset)
            : nullptr;
  auto context =
      UnitContext::Parse(unit_sections, data.value().header, kind, options, abbrevs.get());
  if (context.has_error())
    return context.err();
  return std::make_shared<const UnitContext>(context.take_value());
}

}  // namespace zxdb
