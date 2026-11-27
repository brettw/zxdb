// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_binary_impl.h"

#include <lib/fit/defer.h>
#include <lib/syslog/cpp/macros.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <unordered_set>

#include "src/developer/debug/shared/logging/logging.h"
#include "src/developer/debug/zxdb/common/checked_math.h"
#include "src/developer/debug/zxdb/common/file_util.h"
#include "src/developer/debug/zxdb/symbols/dwarf/aranges_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/range_list_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf_unit_impl.h"
#include "src/lib/elflib/elflib.h"

namespace zxdb {

namespace {

// Some system ELF headers don't define the compact relocation section type.
constexpr uint32_t kCompactRelocationSection = 0x40000014;

#ifndef NDEBUG
auto CheckUnitContextCacheAccess(std::atomic_flag& active) {
  FX_DCHECK(!active.test_and_set(std::memory_order_acquire))
      << "Concurrent unit context cache access";
  return fit::defer([&active] { active.clear(std::memory_order_release); });
}
#endif

uint64_t ComputeMappedLength(elflib::ElfLib& elf) {
  uint64_t max = 0;
  for (const Elf64_Phdr& header : elf.GetSegmentHeaders()) {
    // Only check segments that are loaded. Some segments contain things like DWARF symbols that
    // won't be loaded. Here we only want the size in-memory to resolve addresses in the program's
    // address space.
    if (header.p_type == PT_LOAD)
      max = std::max(max, header.p_vaddr + header.p_memsz);
  }
  return max;
}

DwarfBinary::ElfTableSymbol MakeElfTableSymbol(const Elf64_Sym& sym) {
  DwarfBinary::ElfTableSymbol result;
  switch (ELF64_ST_TYPE(sym.st_info)) {
    case STT_OBJECT:
      result.type = DwarfBinary::ElfTableSymbol::Type::kObject;
      break;
    case STT_FUNC:
      result.type = DwarfBinary::ElfTableSymbol::Type::kFunction;
      break;
    case STT_TLS:
      result.type = DwarfBinary::ElfTableSymbol::Type::kTls;
      break;
    default:
      break;
  }
  result.is_defined = sym.st_shndx != SHN_UNDEF;
  result.value = sym.st_value;
  result.size = sym.st_size;
  return result;
}

// Merges the "symbols" and "dynamic symbols" into a single map. Returns an empty map if the symbols
// couldn't be loaded.
//
// The ".dynsym" table is normally described as containing a subset of the information (just the
// global symbols) in the ".symtab" section. But in a stripped binary, there will be only a
// ".dynsym" section. To handle all the cases, this merges both tables. If a name is the same, this
// assumes the symbols are the same. The dynamic one will be used in the case of duplicates.
std::map<std::string, DwarfBinary::ElfTableSymbol> GetMergedElfSymbols(elflib::ElfLib& elf) {
  std::map<std::string, DwarfBinary::ElfTableSymbol> result;

  // A later symbol replaces an earlier symbol with the same name, in a table and between the
  // tables. Thus the dynamic symbols, which are visited last, take precedence.
  auto add_symbol = [&result](std::string name, const Elf64_Sym& symbol) {
    result.insert_or_assign(std::move(name), MakeElfTableSymbol(symbol));
  };
  elf.VisitAllSymbols(add_symbol);
  elf.VisitAllDynamicSymbols(add_symbol);

  return result;
}

// GNU compression changes the prefix but preserves the DWARF section kind.
std::optional<std::string_view> DwarfSectionKind(std::string_view name) {
  if (name.starts_with(".debug_"))
    return name.substr(std::string_view(".debug_").size());
  if (name.starts_with(".zdebug_"))
    return name.substr(std::string_view(".zdebug_").size());
  return std::nullopt;
}

// Reject repeated DWARF sections and find the lowest executable section address. Separate debug
// files preserve section addresses and flags.
ErrOr<uint64_t> InspectElfSections(elflib::ElfLib& elf, const std::string& path) {
  std::optional<uint64_t> first;
  std::unordered_set<std::string_view> dwarf_sections;
  const auto& headers = elf.GetSectionHeaders();
  for (size_t i = 0; i < headers.size(); ++i) {
    const auto& section = headers[i];
    if (section.sh_flags & SHF_EXECINSTR)
      first = std::min(first.value_or(std::numeric_limits<uint64_t>::max()), section.sh_addr);

    auto kind = DwarfSectionKind(elf.GetSectionName(i));
    if (kind && !dwarf_sections.insert(*kind).second) {
      return Err(
          "The object \"%s\" has more than one .debug_%s section. "
          "This usually means a relocatable (.o) file. Link it first. "
          "zxdb reads linked executables, shared libraries, and their separate debug files.",
          path.c_str(), std::string(*kind).c_str());
    }
  }

  if (elf.GetEhdr()->e_type == ET_REL) {
    // Split DWARF files can be ET_REL. Reject sections that would need debug relocations.
    for (const auto& section : headers) {
      if (section.sh_size && (section.sh_type == SHT_REL || section.sh_type == SHT_RELA ||
                              section.sh_type == kCompactRelocationSection)) {
        auto target = elf.GetSectionName(section.sh_info);
        if (DwarfSectionKind(target)) {
          return Err("The object \"%s\" has relocations for %s. Link it first.", path.c_str(),
                     std::string(target).c_str());
        }
      }
    }
  }
  return first.value_or(0);
}

DwarfSections ReadDwarfSections(elflib::ElfLib& elf, const std::string& path) {
  auto read = [&elf, &path](std::string_view name) -> std::span<const uint8_t> {
    auto index = elf.FindSection(name);
    if (!index)
      return {};
    auto section = elf.ReadSection(*index);
    if (section.is_error()) {
      LOGS(Error) << path << ": " << name << ": " << section.error_value();
      return {};
    }
    return section.value();
  };

  // As in LLDB's ObjectFileELF, only sections with SHF_COMPRESSED are decompressed. Legacy GNU
  // .zdebug_* names still participate in the duplicate check but provide no DWARF bytes.
  return {
      .info = read(".debug_info"),
      .abbrev = read(".debug_abbrev"),
      .str = read(".debug_str"),
      .str_offsets = read(".debug_str_offsets"),
      .line = read(".debug_line"),
      .line_str = read(".debug_line_str"),
      .addr = read(".debug_addr"),
      .ranges = read(".debug_ranges"),
      .rnglists = read(".debug_rnglists"),
      .loc = read(".debug_loc"),
      .loclists = read(".debug_loclists"),
      .aranges = read(".debug_aranges"),
      .names = read(".debug_names"),
      .types = read(".debug_types"),
      .info_dwo = read(".debug_info.dwo"),
      .abbrev_dwo = read(".debug_abbrev.dwo"),
      .str_dwo = read(".debug_str.dwo"),
      .str_offsets_dwo = read(".debug_str_offsets.dwo"),
      .line_dwo = read(".debug_line.dwo"),
      .loclists_dwo = read(".debug_loclists.dwo"),
      .rnglists_dwo = read(".debug_rnglists.dwo"),
      .types_dwo = read(".debug_types.dwo"),
      .cu_index = read(".debug_cu_index"),
      .tu_index = read(".debug_tu_index"),
  };
}

}  // namespace

// Maps code addresses to compilation units using .debug_aranges and root DIE ranges.
// Omit ranges that start at zero because linkers can leave these behind for discarded code.
class DwarfBinaryImpl::DebugAranges {
 public:
  explicit DebugAranges(const DwarfBinaryImpl& binary) {
    const auto& sections = binary.GetSections();
    std::unordered_set<uint64_t> parsed_units;

    // Extract aranges from .debug_aranges section first.
    for (uint64_t aranges_offset = 0; aranges_offset < sections.aranges.size();) {
      auto decoded = ArangesSet::Parse(sections.aranges, aranges_offset, sections.endian);
      if (decoded.has_error()) {
        LOGS(Error) << decoded.err().msg();
        break;
      }
      const ArangesSet& set = decoded.value();
      for (uint64_t offset : set.premature_terminators()) {
        LOGS(Warn) << Err("DWARF address range set at offset 0x%" PRIx64
                          " has a premature terminator at offset 0x%" PRIx64,
                          aranges_offset, offset)
                          .msg();
      }
      for (const auto& range : set.ranges()) {
        if (range.begin() != 0)
          ranges_.push_back({range.begin(), range.end(), set.unit_offset()});
      }
      parsed_units.insert(set.unit_offset());
      aranges_offset = set.next_offset();
    }

    // Generate aranges from compile units because .debug_aranges could be incomplete.
    // But we can skip those units that we have parsed.
    for (uint32_t i = 0; i < binary.info_units_.size(); ++i) {
      const auto& header = binary.info_units_[i];
      if (header.is_type_unit())
        continue;
      uint64_t offset = header.offset;
      if (parsed_units.insert(offset).second) {
        auto unit = binary.GetUnitContext(UnitIndex(false, i));
        if (unit.has_error()) {
          LOGS(Error) << unit.err().msg();
          continue;
        }
        auto ranges = RangeListReader::ReadUnitRanges(*unit.value());
        if (ranges.has_error()) {
          LOGS(Error) << ranges.err().msg();
          continue;
        }
        for (const auto& range : ranges.value()) {
          if (range.begin() != 0)
            ranges_.push_back({range.begin(), range.end(), offset});
        }
      }
    }

    std::sort(ranges_.begin(), ranges_.end(),
              [](const Range& first, const Range& second) { return first.begin < second.begin; });
    // Clip overlapping ranges so the lookup can search by end address.
    Range* prev = nullptr;
    for (Range& range : ranges_) {
      if (prev && prev->end > range.begin) {
        // This could happen when the linker resolves the same symbol in different CUs to the same
        // address in the final executable.
        prev->end = range.begin;
      }
      prev = &range;
    }
  }

  std::optional<uint64_t> FindAddress(uint64_t address) const {
    auto it = std::partition_point(ranges_.begin(), ranges_.end(),
                                   [address](const Range& range) { return range.end <= address; });
    // it is the first range that has end > address.
    if (it != ranges_.end() && it->begin <= address) {
      return it->cu_offset;
    }
    return std::nullopt;
  }

 private:
  struct Range {
    uint64_t begin;
    uint64_t end;
    uint64_t cu_offset;
  };
  std::vector<Range> ranges_;
};

DwarfBinaryImpl::DwarfBinaryImpl(const std::string& name, const std::string& binary_name,
                                 const std::string& build_id)
    : name_(name), binary_name_(binary_name), build_id_(build_id), weak_factory_(this) {}

DwarfBinaryImpl::DwarfBinaryImpl(DwarfSections sections)
    : sections_(sections), weak_factory_(this) {
  InitUnitLists();
}

DwarfBinaryImpl::~DwarfBinaryImpl() {}

fxl::WeakPtr<DwarfBinaryImpl> DwarfBinaryImpl::GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

std::string DwarfBinaryImpl::GetName() const { return name_; }

std::string DwarfBinaryImpl::GetBuildID() const { return build_id_; }

std::time_t DwarfBinaryImpl::GetModificationTime() const { return modification_time_; }

const DwarfSymbolFactory* DwarfBinaryImpl::GetSymbolFactory() const {
  return symbol_factory_.get();
}

bool DwarfBinaryImpl::HasBinary() const {
  return !binary_name_.empty() || (object_ && object_->ProbeHasProgramBits());
}

Err DwarfBinaryImpl::Load(fxl::WeakPtr<DwarfSymbolFactory::Delegate> delegate,
                          DwarfSymbolFactory::FileType file_type) {
  symbol_factory_ = fxl::MakeRefCounted<DwarfSymbolFactory>(weak_factory_.GetWeakPtr(),
                                                            std::move(delegate), file_type);

  auto object = elflib::ElfLib::CreateMapped(name_);
  if (object.is_error())
    return Err("Error loading symbols for \"" + name_ + "\": " + object.error_value());
  auto elf = std::move(object).value();
  auto first_code_address = InspectElfSections(*elf, name_);
  if (first_code_address.has_error())
    return first_code_address.err();

  auto load_program_tables = [this](elflib::ElfLib& program) {
    plt_symbols_ = program.GetPLTOffsets();
    elf_symbols_ = GetMergedElfSymbols(program);
    mapped_length_ = ComputeMappedLength(program);
  };
  if (elf->ProbeHasProgramBits() || binary_name_ == name_) {
    load_program_tables(*elf);
  } else if (!binary_name_.empty()) {
    auto binary = elflib::ElfLib::CreateMapped(binary_name_);
    if (binary.is_ok())
      load_program_tables(*binary.value());
  }

  modification_time_ = GetFileModificationTime(name_);
  first_code_address_ = first_code_address.value();
  object_ = std::move(elf);
  sections_ = ReadDwarfSections(*object_, name_);
  debug_names_ = DebugNamesReader::Parse(sections_);
  InitUnitLists();
  return Err();
}

void DwarfBinaryImpl::InitUnitLists() {
  info_units_ = UnitList::Parse(sections_.info, sections_.endian, UnitSection::kInfo);
  types_units_ = UnitList::Parse(sections_.types, sections_.endian, UnitSection::kTypes);
  info_dwo_units_ = UnitList::Parse(sections_.info_dwo, sections_.endian, UnitSection::kInfo);
  types_dwo_units_ = UnitList::Parse(sections_.types_dwo, sections_.endian, UnitSection::kTypes);
  die_tables_[false].resize(GetNormalUnitCount());
  die_tables_[true].resize(GetDWOUnitCount());

  if (sections_.cu_index.empty() && sections_.tu_index.empty())
    return;
  auto cu_index =
      PackageIndex::Parse(sections_.cu_index, sections_.endian, PackageIndex::Kind::kCu);
  auto tu_index =
      PackageIndex::Parse(sections_.tu_index, sections_.endian, PackageIndex::Kind::kTu);

  package_units_.reserve(GetDWOUnitCount());
  for (UnitSection section : {UnitSection::kInfo, UnitSection::kTypes}) {
    for (const auto& header : GetUnitList(true, section)) {
      package_units_.push_back(
          ReadPackageUnit(header.is_type_unit() ? tu_index : cu_index, header, section));
    }
  }
}

DwarfBinaryImpl::PackageUnitResult DwarfBinaryImpl::ReadPackageUnit(
    const ErrOr<PackageIndex>& package_index, const UnitHeader& header, UnitSection kind) const {
  if (!package_index)
    return package_index.err();
  const auto& index = package_index.value();
  auto abbrev_bytes = sections_.abbrev_dwo;
  using Section = PackageIndex::Section;
  auto id = header.is_type_unit() ? header.type_signature : header.dwo_id;
  auto row = id ? index.FindByHash(*id) : std::nullopt;
  if (!row)
    row = index.FindByOffset(header.offset);
  if (!row)
    return std::optional<PackageUnit>();

  auto unit =
      index.GetContribution(*row, kind == UnitSection::kInfo ? Section::kInfo : Section::kTypes);
  // Check that the row describes this unit before using any of its other contributions.
  if (!unit || unit->offset != header.offset ||
      unit->size != header.next_unit_offset() - header.offset) {
    return Err("Inconsistent DWARF package unit contribution.");
  }
  auto abbrev = index.GetContribution(*row, Section::kAbbrev);
  // The header's abbreviation offset is relative to the contribution (DWARF 5 section 7.3.5.3).
  // LLVM 19 and LLDB reject a nonzero offset.
  if (!abbrev || header.abbrev_offset >= abbrev->size || abbrev->Slice(abbrev_bytes).empty())
    return Err("Invalid DWARF package abbreviation contribution.");

  return std::optional(PackageUnit{
      .abbrev = {abbrev_bytes.first(abbrev->offset + abbrev->size),
                 abbrev->offset + header.abbrev_offset},
      .contributions = {index.GetContribution(*row, Section::kStrOffsets),
                        index.GetContribution(*row, Section::kLoclists),
                        index.GetContribution(*row, Section::kRnglists)},
      .line = index.GetContribution(*row, Section::kLine),
  });
}

const DwarfSections& DwarfBinaryImpl::GetSections() const { return sections_; }

uint64_t DwarfBinaryImpl::GetMappedLength() const { return mapped_length_; }

const std::map<std::string, DwarfBinary::ElfTableSymbol>& DwarfBinaryImpl::GetELFSymbols() const {
  return elf_symbols_;
}

const std::map<std::string, uint64_t> DwarfBinaryImpl::GetPLTSymbols() const {
  return plt_symbols_;
}

uint32_t DwarfBinaryImpl::GetNormalUnitCount() const {
  return info_units_.size() + types_units_.size();
}

uint32_t DwarfBinaryImpl::GetDWOUnitCount() const {
  return info_dwo_units_.size() + types_dwo_units_.size();
}

const UnitList& DwarfBinaryImpl::GetUnitList(bool is_dwo, UnitSection section) const {
  if (section == UnitSection::kInfo)
    return is_dwo ? info_dwo_units_ : info_units_;
  return is_dwo ? types_dwo_units_ : types_units_;
}

std::optional<DwarfBinaryImpl::IndexedUnit> DwarfBinaryImpl::GetUnitHeader(UnitIndex index) const {
  // Match LLVM's section order for both normal and DWO units.
  const UnitList& info = GetUnitList(index.is_dwo, UnitSection::kInfo);
  if (index.index < info.size())
    return IndexedUnit{info[index.index], UnitSection::kInfo};

  const UnitList& types = GetUnitList(index.is_dwo, UnitSection::kTypes);
  size_t types_index = index.index - info.size();
  if (types_index < types.size())
    return IndexedUnit{types[types_index], UnitSection::kTypes};
  return std::nullopt;
}

fxl::RefPtr<DwarfUnit> DwarfBinaryImpl::GetUnitAtIndex(UnitIndex i) {
  auto entry = GetUnitHeader(i);
  if (!entry)
    return nullptr;

  auto found = unit_map_.find(i);
  if (found != unit_map_.end())
    return found->second;

  auto unit = fxl::MakeRefCounted<DwarfUnitImpl>(this, i, entry->section, entry->header);
  unit_map_.emplace(i, unit);
  return unit;
}

fxl::RefPtr<DwarfUnit> DwarfBinaryImpl::UnitForRelativeAddress(uint64_t relative_address) {
  if (!debug_aranges_) {
    debug_aranges_ = std::make_unique<DebugAranges>(*this);
  }
  auto offset = debug_aranges_->FindAddress(relative_address);
  if (!offset)
    return nullptr;
  auto index = info_units_.FindIndexForOffset(*offset);
  if (!index || info_units_[*index].is_type_unit())
    return nullptr;
  return GetUnitAtIndex(UnitIndex(false, *index));
}

fxl::RefPtr<DwarfUnit> DwarfBinaryImpl::CreateUncachedUnit(const DwarfDie& die) {
  if (!die.belongs_to(*this))
    return nullptr;
  UnitIndex index = die.unit_index();
  auto entry = GetUnitHeader(index);
  return fxl::MakeRefCounted<DwarfUnitImpl>(this, index, entry->section, entry->header);
}

std::optional<uint64_t> DwarfBinaryImpl::GetDebugAddrEntry(uint64_t addr_base,
                                                           uint64_t index) const {
  // DW_AT_addr_base gives the byte offset of the first entry after the header. The index counts
  // entries from that offset. This lookup assumes 8-byte addresses.
  constexpr uint64_t kEntrySize = 8;
  constexpr uint64_t kMaxOffset = std::numeric_limits<uint64_t>::max();
  if (index > kMaxOffset / kEntrySize)
    return std::nullopt;
  uint64_t entry_offset = index * kEntrySize;
  if (addr_base > kMaxOffset - entry_offset)
    return std::nullopt;

  const DwarfSections& sections = GetSections();
  DataReader reader(sections.addr, sections.endian);
  reader.Seek(addr_base + entry_offset);
  uint64_t result = reader.Address(kEntrySize);
  if (!reader.ok())
    return std::nullopt;
  return result;
}

std::optional<UnitIndex> DwarfBinaryImpl::FindUnitForRef(DwarfDieRef die_ref, bool is_dwo) const {
  if (!die_ref)
    return std::nullopt;
  UnitSection section =
      die_ref.section() == DwarfDieRef::Section::kMain ? UnitSection::kInfo : UnitSection::kTypes;
  auto index = GetUnitList(is_dwo, section).FindIndexForOffset(die_ref.offset());
  if (!index)
    return std::nullopt;
  return UnitIndex(is_dwo, section == UnitSection::kInfo
                               ? *index
                               : GetUnitList(is_dwo, UnitSection::kInfo).size() + *index);
}

std::shared_ptr<const DwarfDie::UnitTable> DwarfBinaryImpl::GetDieTable(UnitIndex index,
                                                                        DwarfDie::Load load) const {
  auto& entries = die_tables_[index.is_dwo];
  if (index.index >= entries.size())
    return nullptr;

  std::unique_lock lock(die_cache_mutex_);
  auto& table = entries[index.index];
  if (table && (load == DwarfDie::Load::kRootOnly || table->load == DwarfDie::Load::kAllDies))
    return table;

  const auto unit = GetUnitHeader(index);
  std::optional<UnitContextCache::AbbrevSection> abbrev_section = UnitContextCache::AbbrevSection{
      index.is_dwo ? sections_.abbrev_dwo : sections_.abbrev, unit->header.abbrev_offset};
  if (index.is_dwo && !package_units_.empty()) {
    const auto& package = package_units_[index.index];
    if (!package)
      abbrev_section.reset();
    else if (package.value())
      abbrev_section = package.value()->abbrev;
  }
  auto abbrevs =
      abbrev_section ? unit_contexts_.GetDieAbbrevs(index.is_dwo, *abbrev_section) : nullptr;

  auto bytes = unit->section == UnitSection::kInfo
                   ? (index.is_dwo ? sections_.info_dwo : sections_.info)
                   : (index.is_dwo ? sections_.types_dwo : sections_.types);
  auto parsed = DieTable::Parse(bytes, unit->header, sections_.endian, std::move(abbrevs), load);
  auto snapshot = std::make_shared<const DwarfDie::UnitTable>(
      this, index, unit->section, std::move(parsed), sections_.endian, load);
  table = snapshot;
  lock.unlock();
  if (snapshot->error().has_error())
    LOGS(Error) << snapshot->error().msg();
  return snapshot;
}

DwarfDie DwarfBinaryImpl::GetDieAtIndex(UnitIndex index, uint64_t die_index,
                                        DwarfDie::Load load) const {
  auto table = GetDieTable(index, load);
  return table && table->Get(die_index) ? DwarfDie(std::move(table), die_index) : DwarfDie();
}

DwarfDie DwarfBinaryImpl::GetDieAtOffset(UnitIndex index, uint64_t die_offset) const {
  auto table = GetDieTable(index, DwarfDie::Load::kAllDies);
  if (table) {
    if (auto die_index = table->Find(die_offset))
      return DwarfDie(std::move(table), *die_index);
  }
  return {};
}

void DwarfBinaryImpl::ClearDieCache(UnitIndex index) const {
  std::lock_guard lock(die_cache_mutex_);
  die_tables_[index.is_dwo][index.index].reset();
}

DwarfDie DwarfBinaryImpl::GetDieAtOffset(DwarfDieRef die_ref) const {
  if (auto unit = FindUnitForRef(die_ref, false))
    return GetDieAtOffset(*unit, die_ref.offset());
  return GetDieAtOffset(die_ref, true);
}

DwarfDie DwarfBinaryImpl::GetDieAtOffset(DwarfDieRef die_ref, bool is_dwo) const {
  if (auto unit = FindUnitForRef(die_ref, is_dwo))
    return GetDieAtOffset(*unit, die_ref.offset());
  return {};
}

DwarfDie DwarfBinaryImpl::GetUnitDieForRef(DwarfDieRef die_ref, bool is_dwo) const {
  auto unit = FindUnitForRef(die_ref, is_dwo);
  return unit ? GetDieAtIndex(*unit, 0, DwarfDie::Load::kRootOnly) : DwarfDie();
}

void DwarfBinaryImpl::EnsureSignatureMap() const {
  if (signature_to_die_)
    return;

  auto& maps = signature_to_die_.emplace();
  auto add_units = [](SignatureMap& map, const UnitList& units, UnitSection section) {
    for (const UnitHeader& unit : units) {
      if (unit.type_signature) {
        uint64_t offset = unit.offset + *unit.type_offset;
        map[*unit.type_signature] =
            section == UnitSection::kInfo ? DwarfDieRef::Main(offset) : DwarfDieRef::Type(offset);
      }
    }
  };

  add_units(maps[false], info_units_, UnitSection::kInfo);
  add_units(maps[false], types_units_, UnitSection::kTypes);
  add_units(maps[true], info_dwo_units_, UnitSection::kInfo);
  add_units(maps[true], types_dwo_units_, UnitSection::kTypes);
}

DwarfDieRef DwarfBinaryImpl::FindSignature(uint64_t signature, bool is_dwo) const {
  std::lock_guard lock(die_cache_mutex_);
  EnsureSignatureMap();
  const auto& map = (*signature_to_die_)[is_dwo];
  auto found = map.find(signature);
  return found == map.end() ? DwarfDieRef() : found->second;
}

DwarfDieRef DwarfBinaryImpl::GetDieRefForSignature(uint64_t signature) const {
  if (DwarfDieRef ref = FindSignature(signature, true))
    return ref;
  return FindSignature(signature, false);
}

DwarfDie DwarfBinaryImpl::GetDieForSignature(uint64_t signature, bool is_dwo) const {
  if (DwarfDieRef ref = FindSignature(signature, is_dwo))
    return GetDieAtOffset(ref, is_dwo);
  return GetDieAtOffset(FindSignature(signature, !is_dwo), !is_dwo);
}

const DebugNamesReader* DwarfBinaryImpl::GetDebugNames() const {
  return debug_names_ && debug_names_->index_count() != 0 ? &*debug_names_ : nullptr;
}

UnitContextResult DwarfBinaryImpl::GetUnitContext(UnitIndex index,
                                                  UnitContextSource skeleton) const {
#ifndef NDEBUG
  // Same-binary skeleton lookups stay inside UnitContextCache. A different binary has its own
  // guard. Release the guard on return so a DWO index worker can hand the binary to the module.
  auto access = CheckUnitContextCacheAccess(unit_context_cache_active_);
#endif
  auto unit = GetUnitHeader(index);
  if (!unit)
    return Err("Unknown DWARF unit.");
  if (index.is_dwo)
    return unit_contexts_.GetSplit(*this, index, unit->header, unit->section, skeleton);
  return unit_contexts_.GetNormal(sections_, unit->header, unit->section);
}

std::optional<LineProgramSource> DwarfBinaryImpl::GetLineProgramSource(UnitIndex index) const {
  auto context = GetUnitContext(index);
  if (!context || !context.value()->stmt_list())
    return std::nullopt;
  uint64_t offset = *context.value()->stmt_list();
  auto line = index.is_dwo ? sections_.line_dwo : sections_.line;
  if (index.is_dwo && !package_units_.empty()) {
    const auto& entry = package_units_[index.index];
    if (!entry)
      return std::nullopt;
    if (const auto& package = entry.value()) {
      const auto& contribution = package->line;
      if (!contribution || offset >= contribution->size)
        return std::nullopt;
      auto absolute = CheckedAdd(contribution->offset, offset);
      if (!absolute)
        return std::nullopt;
      offset = *absolute;
    }
  }
  if (offset >= line.size())
    return std::nullopt;

  LineStringSections strings = GetLineStringSections(sections_);
  if (index.is_dwo)
    strings.str = AsStringView(sections_.str_dwo);
  if (auto contribution = context.value()->string_offsets()) {
    auto bytes = index.is_dwo ? sections_.str_offsets_dwo : sections_.str_offsets;
    strings.str_offsets = AsStringView(bytes.first(contribution->end));
    strings.str_offsets_base = contribution->base;
    strings.str_offset_size = contribution->entry_size;
  }
  return LineProgramSource{AsStringView(line), offset, sections_.endian, strings};
}

std::shared_ptr<const LineFileTable> DwarfBinaryImpl::GetLineFileTable(UnitIndex unit,
                                                                       uint64_t file_index) const {
  auto source = GetLineProgramSource(unit);
  return source ? line_tables_.GetFileTable(*source, file_index) : nullptr;
}

LineTable DwarfBinaryImpl::DecodeLineTable(UnitIndex unit) const {
  auto source = GetLineProgramSource(unit);
  return source ? line_tables_.DecodeLineTable(*source, first_code_address_) : LineTable();
}

void DwarfBinaryImpl::ClearDwarfCaches() {
#ifndef NDEBUG
  auto access = CheckUnitContextCacheAccess(unit_context_cache_active_);
#endif
  line_tables_.Clear();
  unit_map_.clear();

  // A cross-unit reference can reload a previously indexed unit. Release those tables too.
  std::lock_guard lock(die_cache_mutex_);
  for (auto& entries : die_tables_) {
    for (auto& entry : entries)
      entry.reset();
  }
  signature_to_die_.reset();
  unit_contexts_.Clear();
}

}  // namespace zxdb
