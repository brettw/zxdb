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
  // Relocatable objects can have multiple COMDAT sections of the same type. The byte reader
  // doesn't support these objects.
  return count == 1 ? result : std::span<const uint8_t>();
}

const llvm::DWARFUnitIndex::Entry* GetPackageEntry(llvm::DWARFUnit& unit,
                                                   const UnitHeader& header) {
  if (!unit.isDWOUnit())
    return nullptr;

  // Match LLVM's split-unit lookup by signature, then by section offset.
  const auto& index =
      unit.isTypeUnit() ? unit.getContext().getTUIndex() : unit.getContext().getCUIndex();
  auto signature = unit.isTypeUnit() ? header.type_signature : header.dwo_id;
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
  abbrevs_.clear();
}

UnitContextResult UnitContextCache::Parse(const DwarfSections& sections, llvm::DWARFUnit& unit,
                                          UnitContextSource skeleton, UnitContextCache* cache) {
  auto info = AsBytes(unit.getInfoSection().Data);
  bool is_dwo = unit.isDWOUnit();
  UnitSection kind =
      unit.isTypeUnit() && unit.getVersion() < 5 ? UnitSection::kTypes : UnitSection::kInfo;
  auto header = ReadUnitHeader(info, unit.getOffset(), sections.endian, kind);
  if (header.has_error())
    return header.err();

  // Use the section of this LLVM unit even when the object has several sections of this kind.
  DwarfSections unit_sections = sections;
  if (kind == UnitSection::kTypes)
    (is_dwo ? unit_sections.types_dwo : unit_sections.types) = info;
  else
    (is_dwo ? unit_sections.info_dwo : unit_sections.info) = info;

  UnitContext::Options options;
  options.is_dwo = is_dwo;
  if (const auto* entry = GetPackageEntry(unit, header.value())) {
    auto contribution =
        [entry](llvm::DWARFSectionKind section) -> std::optional<UnitContext::Contribution> {
      if (const auto* part = entry->getContribution(section))
        return UnitContext::Contribution{part->getOffset(), part->getLength()};
      return std::nullopt;
    };
    // The package index gives the bounds of this unit's abbreviation contribution.
    auto abbrev = contribution(llvm::DW_SECT_ABBREV);
    if (!abbrev || abbrev->offset > sections.abbrev_dwo.size() ||
        abbrev->size > sections.abbrev_dwo.size() - abbrev->offset) {
      return Err("Invalid DWARF package abbreviation contribution.");
    }
    header.value().abbrev_offset = abbrev->offset;
    unit_sections.abbrev_dwo = sections.abbrev_dwo.first(abbrev->offset + abbrev->size);
    options.package = UnitContext::PackageContributions{
        contribution(llvm::DW_SECT_STR_OFFSETS),
        contribution(llvm::DW_SECT_LOCLISTS),
        contribution(llvm::DW_SECT_RNGLISTS),
    };
  }

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

  UnitContext::AbbrevLookup lookup_abbrevs;
  if (cache) {
    lookup_abbrevs = [cache, is_dwo](std::span<const uint8_t> section,
                                     uint64_t offset) -> const ErrOr<AbbrevTable>& {
      AbbrevKey key(is_dwo, offset, section.size());
      auto found = cache->abbrevs_.find(key);
      if (found != cache->abbrevs_.end())
        return found->second;
      return cache->abbrevs_.emplace(key, AbbrevTable::Parse(section, offset)).first->second;
    };
  }
  auto context = UnitContext::Parse(unit_sections, header.value(), kind, options, lookup_abbrevs);
  if (context.has_error())
    return context.err();
  return std::make_shared<const UnitContext>(context.take_value());
}

}  // namespace zxdb
