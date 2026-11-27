// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/debug_names_reader.h"

#include <inttypes.h>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_sections.h"
#include "src/developer/debug/zxdb/symbols/dwarf/form_value.h"

namespace zxdb {

namespace {

// DWARF 5 section 7.19.
enum IndexAttribute {
  kCompileUnit = 1,
  kTypeUnit = 2,
  kDieOffset = 3,
  kParent = 4,
  kTypeHash = 5,
};

constexpr size_t kHashWidth = 4;
constexpr size_t kSignatureWidth = 8;

struct FoldRange {
  uint32_t first;
  uint32_t last;
  uint32_t stride;
  int32_t delta;
};

#include "src/developer/debug/zxdb/symbols/dwarf/debug_names_case_fold.inc"

uint32_t FoldCodePoint(uint32_t code) {
  // DWARF adds the Turkish dotted and dotless I to Unicode simple case folding.
  if (code == 0x130 || code == 0x131)
    return 'i';
  auto range = std::ranges::lower_bound(kFoldRanges, code, {}, &FoldRange::last);
  if (range != std::end(kFoldRanges) && code >= range->first &&
      (code - range->first) % range->stride == 0)
    return code + range->delta;
  return code;
}

std::optional<uint32_t> NameHash(std::string_view name) {
  // LLVM's caseFoldingDjbHash folds UTF-8 for the hash and compares the original bytes.
  uint32_t hash = 5381;
  while (!name.empty()) {
    uint8_t first = name.front();
    name.remove_prefix(1);
    if (first < 0x80) {
      hash = hash * 33 + (first >= 'A' && first <= 'Z' ? first + ('a' - 'A') : first);
      continue;
    }

    // Read one UTF-8 code point. For invalid UTF-8, the caller can still compare raw names
    // with a linear scan.
    unsigned width = std::countl_one(first);
    constexpr uint32_t kMinCodePoint[] = {0, 0, 0x80, 0x800, 0x10000};
    if (width < 2 || width > 4 || width - 1 > name.size())
      return std::nullopt;
    uint32_t code = first & (0x7f >> width);
    for (unsigned i = 1; i < width; ++i) {
      uint8_t next = name.front();
      name.remove_prefix(1);
      if ((next & 0xc0) != 0x80)
        return std::nullopt;
      code = (code << 6) | (next & 0x3f);
    }
    if (code < kMinCodePoint[width] || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
      return std::nullopt;

    code = FoldCodePoint(code);
    if (code < 0x80) {
      hash = hash * 33 + code;
      continue;
    }
    // Hash the UTF-8 bytes of the folded code point. Folding can change its byte width.
    constexpr uint8_t kLeadingByte[] = {0, 0, 0xc0, 0xe0, 0xf0};
    width = code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
    unsigned shift = (width - 1) * 6;
    hash = hash * 33 + (kLeadingByte[width] | (code >> shift));
    while (shift != 0) {
      shift -= 6;
      hash = hash * 33 + (0x80 | ((code >> shift) & 0x3f));
    }
  }
  return hash;
}

std::optional<uint64_t> UnsignedValue(const FormValue& value) {
  if (auto* number = std::get_if<uint64_t>(&value.value))
    return *number;
  if (auto* number = std::get_if<int64_t>(&value.value); number && *number >= 0)
    return static_cast<uint64_t>(*number);
  return std::nullopt;
}

std::optional<uint64_t> UnitIndex(const FormValue& value) {
  // LLVM 19's getAsUnsignedConstant accepts flags for unit indexes and excludes DW_FORM_sdata.
  if (value.form_class == FormValue::Class::kFlag)
    return std::get<uint64_t>(value.value);
  if (value.form_class == FormValue::Class::kConstant && value.form != DwarfForm::kSdata)
    return UnsignedValue(value);
  return std::nullopt;
}

}  // namespace

DebugNamesReader DebugNamesReader::Parse(const DwarfSections& sections) {
  DebugNamesReader result(sections.str, sections.endian);
  DataReader reader(sections.names, sections.endian);
  while (reader.remaining() != 0) {
    auto index = result.ReadIndex(reader);
    if (index.has_error()) {
      // LLVM's DWARFDebugNames::extract keeps the earlier indexes on a contribution error.
      result.error_ = index.err();
      break;
    }
    result.indexes_.push_back(index.take_value());
  }
  return result;
}

ErrOr<DebugNamesReader::NameIndex> DebugNamesReader::ReadIndex(DataReader& section) const {
  uint64_t start = section.offset();
  auto error = [start](const char* reason) {
    return Err("DWARF name index at offset 0x%" PRIx64 ": %s", start, reason);
  };

  auto length = section.InitialLength();
  if (!section.ok() || length.length > section.remaining())
    return error("invalid or truncated contribution length");
  DataReader reader(section.Bytes(length.length), endian_);
  NameIndex index;
  index.forms.version = reader.Fixed(2);
  index.forms.offset_size = length.offset_size;
  // Match LLVM's Header::extract. Ignore the reserved padding and tolerate other versions
  // with the same header layout. The header has no address size for DW_FORM_addr.
  reader.Skip(2);
  uint64_t cu_count = reader.Fixed(4);
  uint64_t local_tu_count = reader.Fixed(4);
  uint64_t foreign_tu_count = reader.Fixed(4);
  uint64_t bucket_count = reader.Fixed(4);
  uint64_t name_count = reader.Fixed(4);
  uint64_t abbrev_size = reader.Fixed(4);
  uint64_t augmentation_size = reader.Fixed(4);
  if (!reader.ok())
    return error("truncated header");

  // LLVM rounds the augmentation size up to four bytes and accepts unknown augmentations.
  uint64_t augmentation_padding = (4 - augmentation_size % 4) % 4;
  reader.Skip(augmentation_size);
  reader.Skip(augmentation_padding);
  if (!reader.ok())
    return error("truncated augmentation");

  struct Table {
    std::span<const uint8_t>& bytes;
    uint64_t count;
    size_t width;
    const char* error;
  };
  // The DWARF offset width applies to CU, local TU, string, and entry offsets. Hashes, bucket
  // slots, and foreign type signatures have fixed widths in both formats.
  size_t offset_size = length.offset_size;
  for (const Table& table : {
           Table{index.cus, cu_count, offset_size, "truncated compilation unit list"},
           Table{index.local_tus, local_tu_count, offset_size, "truncated local type unit list"},
           Table{index.foreign_tus, foreign_tu_count, kSignatureWidth,
                 "truncated foreign type unit list"},
           Table{index.buckets, bucket_count, kHashWidth, "truncated bucket table"},
           Table{index.hashes, bucket_count ? name_count : 0, kHashWidth, "truncated hash table"},
           Table{index.strings, name_count, offset_size, "truncated string offset table"},
           Table{index.entries, name_count, offset_size, "truncated entry offset table"},
       }) {
    if (table.count > reader.remaining() / table.width)
      return error(table.error);
    table.bytes = reader.Bytes(table.count * table.width);
  }

  DataReader abbrevs(reader.Bytes(abbrev_size), endian_);
  if (!reader.ok())
    return error("truncated abbreviations");
  Err abbrev_error = ReadAbbreviations(abbrevs, index);
  if (abbrev_error.has_error())
    return error(abbrev_error.msg().c_str());
  index.pool = reader.Bytes(reader.remaining());

  return index;
}

Err DebugNamesReader::ReadAbbreviations(DataReader& reader, NameIndex& index) {
  for (;;) {
    uint64_t code = reader.Uleb();
    if (!reader.ok())
      return Err("invalid abbreviation code or missing abbreviation table terminator");
    if (code == 0)
      return Err();  // The declared abbreviation size can include padding after this terminator.

    uint64_t tag = reader.Uleb();
    if (!reader.ok() || tag > std::numeric_limits<int>::max())
      return Err("invalid abbreviation tag");
    Abbreviation abbreviation{static_cast<DwarfTag>(tag), {}};
    for (;;) {
      AbbrevTable::AttributeSpec spec{reader.Uleb(), reader.Uleb(), std::nullopt};
      if (!reader.ok())
        return Err("invalid abbreviation attribute");
      if (spec.attribute == 0 && spec.form == 0)
        break;
      if (spec.attribute == 0 || spec.form == 0)
        return Err("abbreviation attribute and form must both be nonzero");

      // Use the DWARF form's abbreviation operand when present. LLVM 19's name index
      // abbreviation reader reads only the attribute/form pair.
      if (spec.form == static_cast<uint64_t>(DwarfForm::kImplicitConst)) {
        spec.implicit_const = reader.Sleb();
        if (!reader.ok())
          return Err("invalid implicit constant in abbreviation");
      }
      abbreviation.attributes.push_back(spec);
    }
    // Like LLVM's name index, reject duplicate codes. Keep full codes to avoid narrowing.
    if (!index.abbreviations.emplace(code, std::move(abbreviation)).second)
      return Err("duplicate abbreviation code");
  }
}

std::optional<uint64_t> DebugNamesReader::ReadTableOffset(const NameIndex& index,
                                                          std::span<const uint8_t> table,
                                                          uint64_t element) const {
  size_t width = index.forms.offset_size;
  if (element >= table.size() / width)
    return std::nullopt;
  return ReadFixed(table.data() + element * width, width, endian_);
}

std::span<const uint8_t> DebugNamesReader::StringBytes(const NameIndex& index, uint64_t row) const {
  auto offset = ReadTableOffset(index, index.strings, row);
  if (!offset || *offset >= strings_.size())
    return {};
  return strings_.subspan(*offset);
}

ErrOr<std::optional<DebugNamesReader::Entry>> DebugNamesReader::ReadEntry(
    size_t index_number, DataReader& reader) const {
  const auto& index = indexes_[index_number];
  uint64_t code = reader.Uleb();
  if (!reader.ok())
    return Err("DWARF name index entry list is truncated or has no terminator.");
  if (code == 0)
    return std::optional<Entry>();

  auto abbreviation = index.abbreviations.find(code);
  if (abbreviation == index.abbreviations.end())
    return Err("Unknown DWARF name index abbreviation code.");

  // Match LLVM's Entry::lookup by taking the first value for each attribute. Decode vendor
  // attributes as well so the cursor reaches the next entry.
  std::array<std::optional<FormValue>, kTypeHash + 1> attributes;
  for (const auto& spec : abbreviation->second.attributes) {
    auto value = ReadForm(spec, index.forms, reader);
    if (value.has_error())
      return value.err();
    if (spec.attribute < attributes.size() && !attributes[spec.attribute])
      attributes[spec.attribute] = value.take_value();
  }

  Entry entry;
  entry.index_ = index_number;
  entry.tag = abbreviation->second.tag;
  if (const auto& die = attributes[kDieOffset];
      die && die->form_class == FormValue::Class::kReference) {
    entry.die_unit_offset = UnsignedValue(*die);
  }
  if (const auto& hash = attributes[kTypeHash]; hash && hash->form == DwarfForm::kData8)
    entry.type_hash = UnsignedValue(*hash);
  if (const auto& parent = attributes[kParent]) {
    entry.has_parent_information = true;
    // LLVM and LLDB treat flag_present as an unindexed parent. An actual offset, including
    // zero, is relative to the entry pool (DWARF 5 errata for table 6.1).
    if (parent->form != DwarfForm::kFlagPresent)
      entry.parent_offset = UnsignedValue(*parent);
  }

  const auto& cu = attributes[kCompileUnit];
  std::optional<uint64_t> cu_row;
  if (cu)
    cu_row = UnitIndex(*cu);
  else if (index.cus.size() == index.forms.offset_size)
    cu_row = 0;  // A single CU is implicit, including the related CU of a type unit.
  if (cu_row)
    entry.related_cu_offset = ReadTableOffset(index, index.cus, *cu_row);

  // LLVM's getCUOffset and LLDB's GetNonSkeletonUnit give the TU precedence over the CU.
  // A bad TU index cannot redirect the entry to the related CU.
  if (const auto& tu = attributes[kTypeUnit]) {
    if (auto tu_row = UnitIndex(*tu)) {
      uint64_t local_count = index.local_tus.size() / index.forms.offset_size;
      if (*tu_row < local_count) {
        entry.local_tu_offset = ReadTableOffset(index, index.local_tus, *tu_row);
      } else if (uint64_t foreign_row = *tu_row - local_count;
                 foreign_row < index.foreign_tus.size() / kSignatureWidth) {
        entry.foreign_tu_signature = ReadFixed(
            index.foreign_tus.data() + foreign_row * kSignatureWidth, kSignatureWidth, endian_);
      }
    }
  } else {
    entry.cu_offset = entry.related_cu_offset;
  }
  return std::optional<Entry>(entry);
}

ErrOr<std::optional<DebugNamesReader::Entry>> DebugNamesReader::ReadParent(
    const Entry& entry) const {
  if (!entry.parent_offset)
    return std::optional<Entry>();
  if (entry.index_ >= indexes_.size() || *entry.parent_offset >= indexes_[entry.index_].pool.size())
    return Err("DWARF name index parent offset is outside the entry pool.");

  // Read one record. The caller can use a parent tag even if that record has no DIE offset.
  DataReader reader(indexes_[entry.index_].pool, endian_);
  reader.Seek(*entry.parent_offset);
  auto parent = ReadEntry(entry.index_, reader);
  if (parent.ok() && !parent.value())
    return Err("DWARF name index parent points to an entry list terminator.");
  return parent;
}

void DebugNamesReader::VisitEntries(size_t index_number, uint64_t row, std::string_view name,
                                    const EntryVisitor& visit) const {
  const auto& index = indexes_[index_number];
  auto offset = ReadTableOffset(index, index.entries, row);
  if (!offset || *offset >= index.pool.size())
    return;

  DataReader reader(index.pool, endian_);
  reader.Seek(*offset);
  // Match DWARFDebugNames::ValueIterator and LLDB's lookups. Stop this list on an error
  // and keep the entries already read. Other lists and indexes can still be read.
  for (;;) {
    auto entry = ReadEntry(index_number, reader);
    if (entry.has_error() || !entry.value())
      return;
    visit(name, *entry.value());
  }
}

void DebugNamesReader::LookupName(std::string_view name, const EntryVisitor& visit) const {
  if (!name.contains('\0'))
    Lookup(name, NameHash(name), visit);
}

void DebugNamesReader::LookupHash(uint32_t hash, const EntryVisitor& visit) const {
  Lookup(std::nullopt, hash, visit);
}

void DebugNamesReader::Lookup(std::optional<std::string_view> name, std::optional<uint32_t> hash,
                              const EntryVisitor& visit) const {
  for (size_t index_number = 0; index_number < indexes_.size(); ++index_number) {
    const auto& index = indexes_[index_number];
    uint64_t name_count = index.strings.size() / index.forms.offset_size;
    uint64_t bucket_count = index.buckets.size() / kHashWidth;
    bool use_hash = bucket_count != 0 && hash.has_value();
    uint64_t row = 0;
    uint64_t bucket = 0;
    if (use_hash) {
      bucket = *hash % bucket_count;
      uint64_t first = ReadFixed(index.buckets.data() + bucket * kHashWidth, kHashWidth, endian_);
      // DWARF numbers hash rows from one. Bucket value zero represents an empty bucket.
      if (first == 0 || first > name_count)
        continue;
      row = first - 1;
    } else if (!name) {
      continue;  // A hash-only lookup has no key to compare in a hashless index.
    }

    for (; row < name_count; ++row) {
      if (use_hash) {
        uint64_t stored = ReadFixed(index.hashes.data() + row * kHashWidth, kHashWidth, endian_);
        if (stored % bucket_count != bucket)
          break;
        if (stored != *hash)
          continue;
      }
      auto bytes = StringBytes(index, row);
      if (name) {
        // Like LLVM's sameNameAs, limit the work to the query length. Match the original
        // spelling after a folded hash match. A collision can lead to several names.
        if (name->size() < bytes.size() && bytes[name->size()] == 0) {
          auto spelling = AsStringView(bytes.first(name->size()));
          if (spelling == *name) {
            VisitEntries(index_number, row, spelling, visit);
            break;  // Each contribution has one list per name.
          }
        }
      } else {
        DataReader strings(bytes, endian_);
        auto spelling = strings.CString();
        if (strings.ok())
          VisitEntries(index_number, row, spelling, visit);
      }
    }
  }
}

void DebugNamesReader::VisitNamesWithPrefix(std::string_view prefix,
                                            const NameVisitor& visit) const {
  if (prefix.empty() || prefix.contains('\0'))
    return;

  for (const auto& index : indexes_) {
    uint64_t name_count = index.strings.size() / index.forms.offset_size;
    for (uint64_t row = 0; row < name_count; ++row) {
      auto bytes = StringBytes(index, row);
      // Check the prefix before looking for the terminator. A prefix without a NUL cannot
      // match across the end of a string. This fast path works for both DWARF offset sizes.
      if (prefix.size() >= bytes.size() || bytes.front() != static_cast<uint8_t>(prefix.front()) ||
          bytes[prefix.size() - 1] != static_cast<uint8_t>(prefix.back()) ||
          AsStringView(bytes.first(prefix.size())) != prefix) {
        continue;
      }
      DataReader strings(bytes, endian_);
      auto name = strings.CString();
      if (strings.ok())
        visit(name);
    }
  }
}

}  // namespace zxdb
