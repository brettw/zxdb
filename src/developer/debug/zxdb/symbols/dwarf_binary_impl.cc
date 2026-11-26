// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_binary_impl.h"

#include <lib/syslog/cpp/macros.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_set>

#include <llvm/DebugInfo/DIContext.h>
#include <llvm/DebugInfo/DWARF/DWARFAcceleratorTable.h>
#include <llvm/DebugInfo/DWARF/DWARFCompileUnit.h>
#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/DebugInfo/DWARF/DWARFObject.h>
#include <llvm/DebugInfo/DWARF/DWARFUnit.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>

#include "src/developer/debug/shared/logging/logging.h"
#include "src/developer/debug/zxdb/common/file_util.h"
#include "src/developer/debug/zxdb/symbols/dwarf/aranges_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/range_list_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf_sections_llvm.h"
#include "src/developer/debug/zxdb/symbols/dwarf_unit_impl.h"
#include "src/lib/elflib/elflib.h"

namespace zxdb {

namespace {

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

// This function exists to filter out specific errors when decompressing
// .debug_gdb_scripts sections from compressed debuginfo files which fail in
// LLVM.
void LLVMErrorHandler(llvm::Error error) {
  llvm::handleAllErrors(std::move(error), [](llvm::ErrorInfoBase& info) {
    if (info.message().find("gdb_scripts")) {
      // Suppress errors for decompressing the "debug_gdb_scripts" section in rust binaries.
      return;
    }

    // Otherwise just pass through to the console like the defaut error handler.
    LOGS(Error) << info.message();
  });
}

}  // namespace

// To work around https://github.com/llvm/llvm-project/issues/58641.
class DwarfBinaryImpl::DebugAranges {
 public:
  DebugAranges(llvm::DWARFContext& context, const DwarfSections& sections) {
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
    for (const auto& compile_unit : context.compile_units()) {
      uint64_t offset = compile_unit->getOffset();
      if (parsed_units.insert(offset).second) {
        auto unit = ReadUnitContext(sections, *compile_unit);
        if (unit.has_error()) {
          LOGS(Error) << unit.err().msg();
          continue;
        }
        auto ranges = RangeListReader::ReadUnitRanges(unit.value());
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
    // Check
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

DwarfBinaryImpl::~DwarfBinaryImpl() {}

fxl::WeakPtr<DwarfBinaryImpl> DwarfBinaryImpl::GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

std::string DwarfBinaryImpl::GetName() const { return name_; }

std::string DwarfBinaryImpl::GetBuildID() const { return build_id_; }

std::time_t DwarfBinaryImpl::GetModificationTime() const { return modification_time_; }

const DwarfSymbolFactory* DwarfBinaryImpl::GetSymbolFactory() const {
  return symbol_factory_.get();
}

bool DwarfBinaryImpl::HasBinary() const {
  if (!binary_name_.empty())
    return true;

  if (auto debug = elflib::ElfLib::Create(name_))
    return debug->ProbeHasProgramBits();

  return false;
}

uint64_t GetFirstCodeAddress(const llvm::object::ObjectFile& object) {
  std::optional<uint64_t> first;
  for (const llvm::object::SectionRef& section : object.sections()) {
    // For ELF, a text section is one with SHF_EXECINSTR.
    if (section.isText())
      first = std::min(first.value_or(std::numeric_limits<uint64_t>::max()), section.getAddress());
  }
  return first.value_or(0);
}

Err DwarfBinaryImpl::Load(fxl::WeakPtr<DwarfSymbolFactory::Delegate> delegate,
                          DwarfSymbolFactory::FileType file_type) {
  symbol_factory_ = fxl::MakeRefCounted<DwarfSymbolFactory>(weak_factory_.GetWeakPtr(),
                                                            std::move(delegate), file_type);

  if (auto debug = elflib::ElfLib::Create(name_)) {
    if (debug->ProbeHasProgramBits()) {
      // Found in ".debug" file.
      plt_symbols_ = debug->GetPLTOffsets();
      elf_symbols_ = GetMergedElfSymbols(*debug);
      mapped_length_ = ComputeMappedLength(*debug);
    } else if (auto elf = elflib::ElfLib::Create(binary_name_)) {
      // Found in binary file.
      plt_symbols_ = elf->GetPLTOffsets();
      elf_symbols_ = GetMergedElfSymbols(*elf);
      mapped_length_ = ComputeMappedLength(*elf);
    }
  }

  llvm::Expected<llvm::object::OwningBinary<llvm::object::Binary>> bin_or_err =
      llvm::object::createBinary(name_);
  if (!bin_or_err) {
    auto err_str = llvm::toString(bin_or_err.takeError());
    return Err("Error loading symbols for \"" + name_ + "\": " + err_str);
  }

  modification_time_ = GetFileModificationTime(name_);

  auto binary_pair = bin_or_err->takeBinary();
  binary_buffer_ = std::move(binary_pair.second);
  binary_ = std::move(binary_pair.first);

  first_code_address_ = zxdb::GetFirstCodeAddress(*GetLLVMObjectFile());

  // Overwrite the default Error handler object, but leave everything else default.
  context_ = llvm::DWARFContext::create(*GetLLVMObjectFile(),
                                        llvm::DWARFContext::ProcessDebugRelocations::Process,
                                        nullptr, "", &LLVMErrorHandler);
  sections_ = GetDwarfSections(*context_);
  info_units_ = UnitList::Parse(sections_.info, sections_.endian, UnitSection::kInfo);
  types_units_ = UnitList::Parse(sections_.types, sections_.endian, UnitSection::kTypes);
  info_dwo_units_ = UnitList::Parse(sections_.info_dwo, sections_.endian, UnitSection::kInfo);
  types_dwo_units_ = UnitList::Parse(sections_.types_dwo, sections_.endian, UnitSection::kTypes);

  return Err();
}

llvm::object::ObjectFile* DwarfBinaryImpl::GetLLVMObjectFile() {
  return static_cast<llvm::object::ObjectFile*>(binary_.get());
}

llvm::DWARFContext* DwarfBinaryImpl::GetLLVMContext() const { return context_.get(); }

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

llvm::DWARFUnit* DwarfBinaryImpl::FindLLVMUnit(bool is_dwo, UnitSection section,
                                               uint64_t unit_offset) const {
  const llvm::DWARFUnitVector& units =
      is_dwo ? context_->getDWOUnitsVector() : context_->getNormalUnitsVector();
  if (section == UnitSection::kInfo) {
    // LLVM's offset lookup covers only the info units and can return a containing unit.
    auto* unit = units.getUnitForOffset(unit_offset);
    return unit && unit->getOffset() == unit_offset ? unit : nullptr;
  }

  const auto end = units.end();
  auto found = std::lower_bound(
      units.begin() + units.getNumInfoUnits(), end, unit_offset,
      [](const auto& unit, uint64_t offset) { return unit->getOffset() < offset; });
  return found != end && (*found)->getOffset() == unit_offset ? found->get() : nullptr;
}

fxl::RefPtr<DwarfUnit> DwarfBinaryImpl::GetUnitAtIndex(UnitIndex i) {
  auto entry = GetUnitHeader(i);
  if (!entry)
    return nullptr;
  auto* llvm_unit = FindLLVMUnit(i.is_dwo, entry->section, entry->header.offset);
  if (!llvm_unit)
    return nullptr;

  auto found = unit_map_.find(llvm_unit);
  if (found != unit_map_.end())
    return found->second;

  auto unit = fxl::MakeRefCounted<DwarfUnitImpl>(this, llvm_unit, entry->header);
  unit_map_.emplace(llvm_unit, unit);
  return unit;
}

fxl::RefPtr<DwarfUnit> DwarfBinaryImpl::UnitForRelativeAddress(uint64_t relative_address) {
  if (!debug_aranges_) {
    debug_aranges_ = std::make_unique<DebugAranges>(*context_, sections_);
  }
  auto offset = debug_aranges_->FindAddress(relative_address);
  if (!offset)
    return nullptr;
  auto index = info_units_.FindIndexForOffset(*offset);
  if (!index || info_units_[*index].is_type_unit())
    return nullptr;
  return GetUnitAtIndex(UnitIndex(false, *index));
}

fxl::RefPtr<DwarfUnit> DwarfBinaryImpl::CreateUncachedUnit(llvm::DWARFUnit* llvm_unit) {
  if (!llvm_unit || &llvm_unit->getContext() != context_.get())
    return nullptr;

  bool is_dwo = llvm_unit->isDWOUnit();
  uint64_t offset = llvm_unit->getOffset();
  // Both sections can have a unit at the same offset. Match the LLVM unit in that section.
  for (UnitSection section : {UnitSection::kInfo, UnitSection::kTypes}) {
    const UnitList& list = GetUnitList(is_dwo, section);
    auto index = list.FindIndexForOffset(offset);
    if (index && list[*index].offset == offset &&
        FindLLVMUnit(is_dwo, section, offset) == llvm_unit)
      return fxl::MakeRefCounted<DwarfUnitImpl>(this, llvm_unit, list[*index]);
  }
  return nullptr;
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

llvm::DWARFUnit* DwarfBinaryImpl::GetLLVMUnitForDieRef(DwarfDieRef die_ref, bool is_dwo) const {
  if (!die_ref.is_valid() || !context_)
    return nullptr;

  UnitSection section =
      die_ref.section() == DwarfDieRef::Section::kMain ? UnitSection::kInfo : UnitSection::kTypes;
  const UnitList& list = GetUnitList(is_dwo, section);
  auto index = list.FindIndexForOffset(die_ref.offset());
  return index ? FindLLVMUnit(is_dwo, section, list[*index].offset) : nullptr;
}

llvm::DWARFDie DwarfBinaryImpl::GetLLVMDieAtOffset(DwarfDieRef die_ref) const {
  if (llvm::DWARFUnit* unit = GetLLVMUnitForDieRef(die_ref, false))
    return unit->getDIEForOffset(die_ref.offset());
  if (llvm::DWARFUnit* unit = GetLLVMUnitForDieRef(die_ref, true))
    return unit->getDIEForOffset(die_ref.offset());
  return llvm::DWARFDie();
}

void DwarfBinaryImpl::EnsureSignatureMap() const {
  if (signature_to_die_.has_value())
    return;

  auto& map = signature_to_die_.emplace();
  auto add_units = [&map](const UnitList& units) {
    for (const UnitHeader& unit : units) {
      if (unit.type_signature) {
        map[*unit.type_signature] =
            DwarfDieRef::ForTypeUnit(unit.version, unit.offset + *unit.type_offset);
      }
    }
  };

  add_units(info_units_);
  add_units(types_units_);
  add_units(info_dwo_units_);
  add_units(types_dwo_units_);
}

DwarfDieRef DwarfBinaryImpl::GetDieRefForSignature(uint64_t signature) const {
  EnsureSignatureMap();
  auto found = signature_to_die_->find(signature);
  if (found == signature_to_die_->end())
    return DwarfDieRef();
  return found->second;
}

const llvm::DWARFDebugNames* DwarfBinaryImpl::GetDebugNames() const {
  if (!context_)
    return nullptr;
  const auto& names = context_->getDebugNames();
  if (names.begin() == names.end())
    return nullptr;
  return &names;
}

std::shared_ptr<const LineFileTable> DwarfBinaryImpl::GetLineFileTable(llvm::DWARFUnit* unit,
                                                                       uint64_t file_index) const {
  if (!unit)
    return nullptr;
  FX_DCHECK(&unit->getContext() == context_.get());
  return line_tables_.GetFileTable(sections_, *unit, file_index);
}

LineTable DwarfBinaryImpl::DecodeLineTable(llvm::DWARFUnit* unit) const {
  if (!unit)
    return LineTable();
  FX_DCHECK(&unit->getContext() == context_.get());
  return line_tables_.DecodeLineTable(sections_, *unit, first_code_address_);
}

void DwarfBinaryImpl::ClearLLVMCache() {
  signature_to_die_.reset();
  line_tables_.Clear();

  // Drop cached DwarfUnitImpl wrappers accumulated during indexing, and the line tables they
  // own. Iterate the underlying LLVM unit vectors directly rather than calling GetUnitAtIndex(),
  // which would immediately re-populate unit_map_ for every unit in the binary.
  // Even though Index::IndexCompileUnit() evicts each unit's caches as it finishes,
  // cross-unit DIE references (such as DW_FORM_ref_addr and DW_FORM_ref_sig8) resolved via
  // GetLLVMDieAtOffset() can lazily re-extract DIE arrays in previously indexed units, so a final
  // sweep is still required here.
  unit_map_.clear();
  if (!context_)
    return;

  for (const auto& unit : context_->normal_units()) {
    if (unit) {
      unit->clear();
    }
  }

  for (const auto& unit : context_->dwo_units()) {
    if (unit) {
      unit->clear();
    }
  }
}

}  // namespace zxdb
