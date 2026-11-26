// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/line_program.h"

#include <string.h>

#include <algorithm>
#include <cinttypes>
#include <limits>

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"
#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_sections.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"
#include "src/developer/debug/zxdb/symbols/line_sequence.h"

// The header parser reads the fields of the header in order (DWARF 5 section 6.2.4). Like LLVM, it
// reads the fixed fields only inside the unit and the directory and file tables only up to the
// end that header_length gives. An error in a fixed field fails the parse. An error in a table
// keeps the entries before the error, as LLVM does.
//
// The line program decoder runs the DWARF line number state machine (DWARF 5 section 6.2) over
// the program bytes. It is a template on a "sink" that receives the emitted rows. The sink declares
// at compile time whether it needs the line, column, and flag registers. The file scan used by the
// indexer doesn't need them, so its copy of the decoder skips their operands.
//
// Rows are emitted by special opcodes, DW_LNS_copy, and DW_LNE_end_sequence. To match LLVM, rows
// aren't emitted while the address register holds the tombstone value for the address size that
// the last DW_LNE_set_address used. Only DW_LNE_set_address changes this state.

namespace zxdb {

namespace {

// Standard opcodes (DWARF 5 section 7.22).
enum : uint8_t {
  kLnsCopy = 1,
  kLnsAdvancePc = 2,
  kLnsAdvanceLine = 3,
  kLnsSetFile = 4,
  kLnsSetColumn = 5,
  kLnsNegateStmt = 6,
  kLnsSetBasicBlock = 7,
  kLnsConstAddPc = 8,
  kLnsFixedAdvancePc = 9,
  kLnsSetPrologueEnd = 10,
  kLnsSetEpilogueBegin = 11,
  kLnsSetIsa = 12,
};

// Extended opcodes.
enum : uint8_t {
  kLneEndSequence = 1,
  kLneSetAddress = 2,
  kLneDefineFile = 3,
};

uint64_t TombstoneForSize(size_t size) {
  return size >= 8 ? ~uint64_t{0} : (uint64_t{1} << (8 * size)) - 1;
}

// Reads the operands of DW_LNE_define_file. Like LLVM, this reads to the end of the unit, not to
// the end of the opcode, and a failed read gives an empty name or a 0 directory.
LineFileEntry ReadDefinedFile(const uint8_t* p, const uint8_t* end) {
  LineFileEntry entry;
  const uint8_t* nul = static_cast<const uint8_t*>(memchr(p, 0, end - p));
  if (!nul) {
    entry.name = std::string_view();
    return entry;
  }
  entry.name = std::string_view(reinterpret_cast<const char*>(p), nul - p);
  p = nul + 1;
  entry.dir_index = ReadUleb(p, end).value_or(0);
  return entry;
}

template <typename Sink>
LineProgramStatus DecodeProgram(const LineProgramHeader& header, Sink& sink) {
  const Endian endian = header.endian;

  const uint8_t default_flags = header.default_is_stmt ? LineRow::kIsStmt : 0;
  const uint8_t opcode_base = header.opcode_base;
  const uint8_t line_range = header.line_range;
  const uint64_t min_inst_length = header.min_inst_length;

  // Opcode 0 always introduces an extended opcode, even when opcode_base is 0.
  const uint8_t first_special = std::max<uint8_t>(opcode_base, 1);

  // Address advance of DW_LNS_const_add_pc. LLVM doesn't advance when line_range is 0.
  const uint64_t const_add_pc_advance =
      line_range ? ((255 - opcode_base) / line_range) * min_inst_length : 0;

  uint64_t address = 0;
  uint32_t line = 1;
  uint32_t file = 1;
  uint16_t column = 0;
  uint8_t flags = default_flags;
  bool tombstoned = false;

  auto emit = [&](uint8_t extra_flags) {
    if (tombstoned)
      return;
    sink.OnRow(address, file, line, column, flags | extra_flags);
    // Clear the registers that apply only to one row. LLVM does this only for emitted rows.
    flags &= LineRow::kIsStmt;
  };

  const uint8_t* p = header.program.data();
  const uint8_t* const end = p + header.program.size();
  while (p < end) {
    uint8_t opcode = *p++;

    if (opcode >= first_special) {
      // Special opcodes are most of a typical program.
      uint8_t adjusted = opcode - opcode_base;
      if (line_range) {
        address += (adjusted / line_range) * min_inst_length;
        if constexpr (Sink::kNeedsDetails)
          line += static_cast<uint32_t>(header.line_base + (adjusted % line_range));
      }
      emit(0);
      continue;
    }

    if (opcode == 0) {
      auto operand = ReadUleb(p, end);
      if (!operand)
        return LineProgramStatus::kCorrupt;
      uint64_t length = *operand;
      if (length == 0)
        continue;
      if (length > static_cast<uint64_t>(end - p))
        return LineProgramStatus::kCorrupt;

      // LLVM always continues after the length given by the opcode, even if the operands of a
      // known opcode have a different size.
      const uint8_t* next = p + length;
      switch (*p) {
        case kLneEndSequence:
          emit(LineRow::kEndSequence);
          address = 0;
          line = 1;
          file = 1;
          column = 0;
          flags = default_flags;
          break;
        case kLneSetAddress: {
          // The operand size comes from the opcode length, not from the header.
          size_t size = length - 1;
          if (size == 1 || size == 2 || size == 4 || size == 8) {
            address = ReadFixed(p + 1, size, endian);
            tombstoned = address == TombstoneForSize(size);
          }
          break;
        }
        case kLneDefineFile:
          sink.OnDefineFile(ReadDefinedFile(p + 1, end));
          break;
        default:
          // DW_LNE_set_discriminator and unknown extended opcodes have no effect on stored rows.
          break;
      }
      p = next;
      continue;
    }

    // Standard opcode. LLVM uses fixed semantics for the opcodes it knows and doesn't check the
    // header's operand counts for them.
    switch (opcode) {
      case kLnsCopy:
        emit(0);
        break;
      case kLnsAdvancePc: {
        auto advance = ReadUleb(p, end);
        if (!advance)
          return LineProgramStatus::kCorrupt;
        address += *advance * min_inst_length;
        break;
      }
      case kLnsAdvanceLine:
        if constexpr (Sink::kNeedsDetails) {
          auto delta = ReadSleb(p, end);
          if (!delta)
            return LineProgramStatus::kCorrupt;
          line += static_cast<uint32_t>(*delta);
        } else {
          if (!SkipSleb(p, end))
            return LineProgramStatus::kCorrupt;
        }
        break;
      case kLnsSetFile: {
        auto value = ReadUleb(p, end);
        if (!value)
          return LineProgramStatus::kCorrupt;
        file = static_cast<uint32_t>(std::min<uint64_t>(*value, UINT32_MAX));
        break;
      }
      case kLnsSetColumn:
        if constexpr (Sink::kNeedsDetails) {
          auto value = ReadUleb(p, end);
          if (!value)
            return LineProgramStatus::kCorrupt;
          column = static_cast<uint16_t>(*value);  // LLVM also truncates to 16 bits.
        } else {
          if (!SkipUleb(p, end))
            return LineProgramStatus::kCorrupt;
        }
        break;
      case kLnsNegateStmt:
        flags ^= LineRow::kIsStmt;
        break;
      case kLnsSetBasicBlock:
        flags |= LineRow::kBasicBlock;
        break;
      case kLnsConstAddPc:
        address += const_add_pc_advance;
        break;
      case kLnsFixedAdvancePc:
        if (end - p < 2)
          return LineProgramStatus::kCorrupt;
        address += ReadFixed(p, 2, endian);
        p += 2;
        break;
      case kLnsSetPrologueEnd:
        flags |= LineRow::kPrologueEnd;
        break;
      case kLnsSetEpilogueBegin:
        flags |= LineRow::kEpilogueBegin;
        break;
      case kLnsSetIsa:
        if (!SkipUleb(p, end))
          return LineProgramStatus::kCorrupt;
        break;
      default:
        // An unknown standard opcode. The header gives its number of ULEB128 operands.
        for (uint8_t i = 0; i < header.standard_opcode_lengths[opcode - 1]; i++) {
          if (!SkipUleb(p, end))
            return LineProgramStatus::kCorrupt;
        }
        break;
    }
  }

  return LineProgramStatus::kOk;
}

class RowSink {
 public:
  static constexpr bool kNeedsDetails = true;

  RowSink(std::vector<LineRow>& rows, std::vector<LineFileEntry>& defined_files)
      : rows_(rows), defined_files_(defined_files) {}

  void OnRow(uint64_t address, uint32_t file, uint32_t line, uint16_t column, uint8_t flags) {
    rows_.emplace_back(address, file, line, column, flags);
  }

  void OnDefineFile(LineFileEntry entry) { defined_files_.push_back(entry); }

 private:
  std::vector<LineRow>& rows_;
  std::vector<LineFileEntry>& defined_files_;
};

// Collects the files of live sequences. A sequence is the group of rows up to and including an
// end_sequence row, as LineTable groups them. The files of a sequence are pending until its end
// row shows whether it is live.
//
// The result matches the rows that a full decode gives, resolved with the completed file table. A
// row can reference a file index before the DW_LNE_define_file that adds it. Such an index is kept
// until the end of the program, when the size of the table is known.
class FileScanSink {
 public:
  static constexpr bool kNeedsDetails = false;

  FileScanSink(const LineProgramHeader& header, uint64_t first_code_address,
               std::vector<LineFileEntry>& defined_files)
      : version_(header.version),
        first_code_address_(first_code_address),
        file_entries_(header.files.size()),
        state_(LineFileEntries(header).size(), kUnseen),
        defined_files_(defined_files) {}

  // A defined file can be referenced after this, so the state needs a slot for it.
  void OnDefineFile(LineFileEntry entry) {
    file_entries_++;
    // DWARF 2 to 4 file indices start at 1.
    state_.resize(version_ >= 5 ? file_entries_ : file_entries_ + 1, kUnseen);
    // The last file can be the new index, which the next row must record in its slot.
    last_file_ = kNoFile;
    defined_files_.push_back(entry);
  }

  void OnRow(uint64_t address, uint32_t file, uint32_t, uint16_t, uint8_t flags) {
    sequence_.AddAddress(address);

    // Most rows have the same file as the previous row, so check that first.
    if (file != last_file_) {
      last_file_ = file;
      if (file >= state_.size()) {
        pending_.push_back(file);  // Not in the table yet. See EndSequence().
      } else if (state_[file] == kUnseen) {
        state_[file] = kPending;
        pending_.push_back(file);
      }
    }

    if (flags & LineRow::kEndSequence)
      EndSequence();
  }

  std::vector<uint32_t> TakeFiles() {
    for (uint32_t file : late_files_) {
      if (file < state_.size())
        state_[file] = kLive;
    }

    std::vector<uint32_t> files;
    for (size_t i = 0; i < state_.size(); i++) {
      if (state_[i] == kLive)
        files.push_back(static_cast<uint32_t>(i));
    }
    return files;
  }

 private:
  enum : uint8_t { kUnseen, kPending, kLive };
  static constexpr uint32_t kNoFile = UINT32_MAX;

  void EndSequence() {
    bool live = sequence_.Finish(first_code_address_);
    for (uint32_t file : pending_) {
      if (file >= state_.size()) {
        // A later DW_LNE_define_file can still add this index.
        if (live)
          late_files_.push_back(file);
      } else if (live) {
        state_[file] = kLive;
      } else if (state_[file] == kPending) {
        state_[file] = kUnseen;
      }
    }
    pending_.clear();
    last_file_ = kNoFile;
  }

  uint16_t version_;
  uint64_t first_code_address_;
  size_t file_entries_;
  std::vector<uint8_t> state_;
  std::vector<LineFileEntry>& defined_files_;
  std::vector<uint32_t> pending_;
  std::vector<uint32_t> late_files_;  // Live references outside the table when they were seen.
  LineSequenceValidator sequence_;
  uint32_t last_file_ = kNoFile;
};

// Returns the null-terminated string at the offset of the section, or nullopt if there is none.
std::optional<std::string_view> StringAt(std::string_view section, uint64_t offset) {
  if (offset >= section.size())
    return std::nullopt;
  const void* nul = memchr(section.data() + offset, 0, section.size() - offset);
  if (!nul)
    return std::nullopt;
  return std::string_view(section.data() + offset,
                          static_cast<const char*>(nul) - (section.data() + offset));
}

std::optional<std::string_view> IndexedString(const LineStringSections& strings, uint64_t index,
                                              Endian endian) {
  if (!strings.str_offsets_base)
    return std::nullopt;
  // Check the index against the number of entries, so the entry offset can't overflow.
  uint64_t base = *strings.str_offsets_base;
  uint64_t size = strings.str_offset_size;
  if (size == 0 || base > strings.str_offsets.size() ||
      index >= (strings.str_offsets.size() - base) / size)
    return std::nullopt;
  uint64_t entry = base + index * size;
  uint64_t offset =
      ReadFixed(reinterpret_cast<const uint8_t*>(strings.str_offsets.data()) + entry, size, endian);
  return StringAt(strings.str, offset);
}

// The value of one attribute of a DWARF 5 directory or file entry.
struct FormValue {
  // The string of a string form. Nullopt for other forms and for strings that can't be decoded.
  std::optional<std::string_view> string;

  // The value that llvm::DWARFFormValue::getAsUnsignedConstant() gives for a constant or flag
  // form. LLVM has no value for the other forms, so they give 0.
  uint64_t constant = 0;

  // The size of a block form, for DW_LNCT_MD5. 0 for the other forms.
  uint64_t block_size = 0;
};

// Reads a value of the form. Fails if the reader fails or the form is unknown. The forms are the
// ones that llvm::DWARFFormValue::extractValue() accepts.
ErrOr<FormValue> ReadForm(DataReader& reader, uint64_t form, size_t offset_size,
                          uint8_t address_size, const LineStringSections& strings, Endian endian) {
  // DW_FORM_indirect gives the actual form in the data. LLVM follows any number of them.
  while (form == static_cast<uint64_t>(DwarfForm::kIndirect) && reader.ok())
    form = reader.Uleb();
  if (!reader.ok())
    return Err("The form value is truncated.");

  // A value outside the range of DwarfForm is an unknown form. Checking it before the cast keeps a
  // large value from becoming a known form.
  if (form > static_cast<uint64_t>(std::numeric_limits<int>::max()))
    return Err("Unknown form 0x%" PRIx64 ".", form);

  FormValue value;
  switch (static_cast<DwarfForm>(form)) {
    case DwarfForm::kImplicitConst:
      // The value is in an abbreviation, which line tables don't have. LLVM reads nothing and
      // uses 0.
      break;
    case DwarfForm::kString:
      value.string = reader.CString();
      break;
    case DwarfForm::kLineStrp:
      value.string = StringAt(strings.line_str, reader.Fixed(offset_size));
      break;
    case DwarfForm::kStrp:
      value.string = StringAt(strings.str, reader.Fixed(offset_size));
      break;
    case DwarfForm::kStrx:
    case DwarfForm::kGnuStrIndex:
      value.string = IndexedString(strings, reader.Uleb(), endian);
      break;
    case DwarfForm::kStrx1:
    case DwarfForm::kStrx2:
    case DwarfForm::kStrx3:
    case DwarfForm::kStrx4:
      value.string = IndexedString(
          strings, reader.Fixed(form - static_cast<uint64_t>(DwarfForm::kStrx1) + 1), endian);
      break;
    case DwarfForm::kStrpSup:
    case DwarfForm::kGnuStrpAlt:
      // Strings in a supplementary file can't be decoded.
      reader.Skip(offset_size);
      break;
    case DwarfForm::kData1:
    case DwarfForm::kFlag:
      value.constant = reader.Fixed(1);
      break;
    case DwarfForm::kData2:
      value.constant = reader.Fixed(2);
      break;
    case DwarfForm::kData4:
      value.constant = reader.Fixed(4);
      break;
    case DwarfForm::kData8:
      value.constant = reader.Fixed(8);
      break;
    case DwarfForm::kUdata:
      value.constant = reader.Uleb();
      break;
    case DwarfForm::kFlagPresent:
      value.constant = 1;
      break;
    case DwarfForm::kData16:
      // LLVM treats this as a constant and as a 16-byte block. The constant is the block size.
      reader.Skip(16);
      value.constant = 16;
      value.block_size = 16;
      break;
    case DwarfForm::kBlock1:
      value.block_size = reader.Fixed(1);
      reader.Skip(value.block_size);
      break;
    case DwarfForm::kBlock2:
      value.block_size = reader.Fixed(2);
      reader.Skip(value.block_size);
      break;
    case DwarfForm::kBlock4:
      value.block_size = reader.Fixed(4);
      reader.Skip(value.block_size);
      break;
    case DwarfForm::kBlock:
    case DwarfForm::kExprloc:
      value.block_size = reader.Uleb();
      reader.Skip(value.block_size);
      break;
    // The forms below have no constant, string, or block value in LLVM. This includes the
    // reference forms, which aren't constants even though their encodings are.
    case DwarfForm::kRef1:
      reader.Skip(1);
      break;
    case DwarfForm::kRef2:
      reader.Skip(2);
      break;
    case DwarfForm::kRef4:
    case DwarfForm::kRefSup4:
      reader.Skip(4);
      break;
    case DwarfForm::kRef8:
    case DwarfForm::kRefSig8:
    case DwarfForm::kRefSup8:
      reader.Skip(8);
      break;
    case DwarfForm::kSdata:
      reader.SkipSleb();
      break;
    case DwarfForm::kRefUdata:
    case DwarfForm::kAddrx:
    case DwarfForm::kLoclistx:
    case DwarfForm::kRnglistx:
    case DwarfForm::kGnuAddrIndex:
      reader.SkipUleb();
      break;
    case DwarfForm::kAddr:
      reader.Skip(address_size);
      break;
    case DwarfForm::kSecOffset:
    case DwarfForm::kRefAddr:
    case DwarfForm::kGnuRefAlt:
      reader.Skip(offset_size);
      break;
    case DwarfForm::kAddrx1:
    case DwarfForm::kAddrx2:
    case DwarfForm::kAddrx3:
    case DwarfForm::kAddrx4:
      reader.Skip(form - static_cast<uint64_t>(DwarfForm::kAddrx1) + 1);
      break;
    case DwarfForm::kLlvmAddrxOffset:
      reader.SkipUleb();
      reader.Skip(4);
      break;
    default:
      return Err("Unknown form 0x%" PRIx64 ".", form);
  }
  if (!reader.ok())
    return Err("The form value is truncated.");
  return value;
}

struct EntryFormat {
  uint64_t content_type;
  uint64_t form;
};

// Reads the entry format of a DWARF 5 directory or file table. Fails if it has no path.
ErrOr<std::vector<EntryFormat>> ReadEntryFormat(DataReader& reader) {
  std::vector<EntryFormat> format;
  uint64_t count = reader.Fixed(1);
  bool has_path = false;
  for (uint64_t i = 0; i < count && reader.ok(); i++) {
    EntryFormat& entry = format.emplace_back();
    entry.content_type = reader.Uleb();
    entry.form = reader.Uleb();
    if (entry.content_type == kLnctPath)
      has_path = true;
  }
  if (!reader.ok())
    return Err("The entry format is truncated.");
  if (!has_path)
    return Err("The entry format has no path.");
  return format;
}

// Checks the count of a table whose entries read no data. Such an entry format has only forms like
// DW_FORM_implicit_const. Every entry is the same and has no name, and the count can be very large
// for a small table. LLVM makes every entry. The parser makes them too, so the indices of later
// files are the same, but it fails the header when the count is larger than the size of the
// header. Then the entries use memory in proportion to the input.
Err CheckEmptyEntryCount(uint64_t count, uint64_t header_length) {
  if (count > header_length)
    return Err("has a table of empty entries with a count larger than the header.");
  return Err();
}

// Reads the DWARF 5 directory and file tables. Stops at the first error, keeping the entries
// before it. Fails only for the count that CheckEmptyEntryCount() rejects.
Err ReadV5Tables(DataReader& reader, size_t offset_size, uint8_t address_size,
                 uint64_t header_length, const LineStringSections& strings,
                 LineProgramHeader* header) {
  ErrOr<std::vector<EntryFormat>> dir_format = ReadEntryFormat(reader);
  if (dir_format.has_error())
    return Err();
  uint64_t dir_count = reader.Uleb();
  for (uint64_t i = 0; i < dir_count && reader.ok(); i++) {
    uint64_t entry_begin = reader.offset();
    std::optional<std::string_view> dir;
    for (const EntryFormat& format : dir_format.value()) {
      ErrOr<FormValue> value =
          ReadForm(reader, format.form, offset_size, address_size, strings, header->endian);
      if (value.has_error())
        return Err();
      if (format.content_type == kLnctPath)
        dir = value.value().string;
    }
    if (i == 0 && reader.offset() == entry_begin) {
      if (Err err = CheckEmptyEntryCount(dir_count, header_length); err.has_error())
        return err;
      // Entries that read no data are identical. Keep every slot for directory indices.
      header->include_dirs.assign(dir_count, dir);
      break;
    }
    header->include_dirs.push_back(dir);
  }

  ErrOr<std::vector<EntryFormat>> file_format = ReadEntryFormat(reader);
  if (file_format.has_error())
    return Err();
  uint64_t file_count = reader.Uleb();
  for (uint64_t i = 0; i < file_count && reader.ok(); i++) {
    uint64_t entry_begin = reader.offset();
    LineFileEntry file;
    for (const EntryFormat& format : file_format.value()) {
      ErrOr<FormValue> value =
          ReadForm(reader, format.form, offset_size, address_size, strings, header->endian);
      if (value.has_error())
        return Err();
      switch (format.content_type) {
        case kLnctPath:
          file.name = value.value().string;
          break;
        case kLnctDirectoryIndex:
          file.dir_index = value.value().constant;
          break;
        case kLnctMd5:
          // LLVM rejects an MD5 that isn't a 16-byte block.
          if (value.value().block_size != 16)
            return Err();
          break;
        default:
          break;
      }
    }
    if (i == 0 && reader.offset() == entry_begin) {
      if (Err err = CheckEmptyEntryCount(file_count, header_length); err.has_error())
        return err;
      // Keep every slot so files added by DW_LNE_define_file retain their indices.
      header->files.assign(file_count, file);
      break;
    }
    header->files.push_back(file);
  }
  return Err();
}

// Reads the DWARF 2 to 4 directory and file tables. Each table ends with an empty string. Stops at
// the first error, keeping the entries before it.
void ReadV2Tables(DataReader& reader, LineProgramHeader* header) {
  for (;;) {
    std::string_view dir = reader.CString();
    if (!reader.ok() || dir.empty())
      break;
    header->include_dirs.push_back(dir);
  }
  if (!reader.ok())
    return;

  for (;;) {
    std::string_view name = reader.CString();
    if (!reader.ok() || name.empty())
      return;
    LineFileEntry file;
    file.name = name;
    file.dir_index = reader.Uleb();
    reader.SkipUleb();  // Modification time.
    reader.SkipUleb();  // Length.
    if (!reader.ok())
      return;
    header->files.push_back(file);
  }
}

ErrOr<LineProgramHeader> ParseLineProgramHeader(std::string_view line_section, uint64_t offset,
                                                Endian endian, const LineStringSections& strings) {
  ErrOr<LineTableExtent> extent = ReadLineTableExtent(line_section, offset, endian);
  if (extent.has_error())
    return extent.err();
  uint64_t unit_end = extent.value().end;
  size_t offset_size = extent.value().offset_size;

  LineProgramHeader header;
  header.endian = endian;
  DataReader reader(AsBytes(line_section), endian);
  reader.set_limit(unit_end);
  reader.Seek(extent.value().begin);

  header.version = static_cast<uint16_t>(reader.Fixed(2));
  if (reader.ok() && (header.version < 2 || header.version > 5))
    return Err("Line table at 0x%" PRIx64 " has unsupported version %u.", offset, header.version);
  uint8_t address_size = 8;
  if (header.version >= 5) {
    address_size = static_cast<uint8_t>(reader.Fixed(1));
    reader.Fixed(1);  // Segment selector size.
  }
  uint64_t header_length = reader.Fixed(offset_size);
  if (!reader.ok())
    return Err("Line table header at 0x%" PRIx64 " is truncated.", offset);

  if (header_length > unit_end - reader.offset())
    return Err("Line table header at 0x%" PRIx64 " is longer than its unit.", offset);
  uint64_t program_begin = reader.offset() + header_length;
  reader.set_limit(program_begin);

  header.min_inst_length = static_cast<uint8_t>(reader.Fixed(1));
  if (header.version >= 4) {
    // DWARF 2 and 3 have no field. Like LLVM, treat 0 as 1.
    uint8_t max_ops_per_inst = static_cast<uint8_t>(reader.Fixed(1));
    if (reader.ok() && max_ops_per_inst > 1) {
      return Err("Line table at 0x%" PRIx64
                 " has more than one operation per instruction, which isn't supported.",
                 offset);
    }
  }
  header.default_is_stmt = reader.Fixed(1) != 0;
  header.line_base = static_cast<int8_t>(reader.Fixed(1));
  header.line_range = static_cast<uint8_t>(reader.Fixed(1));
  header.opcode_base = static_cast<uint8_t>(reader.Fixed(1));
  for (int opcode = 1; opcode < header.opcode_base; opcode++)
    header.standard_opcode_lengths[opcode - 1] = static_cast<uint8_t>(reader.Fixed(1));
  if (!reader.ok())
    return Err("Line table header at 0x%" PRIx64 " is truncated.", offset);

  if (header.version >= 5) {
    if (Err err = ReadV5Tables(reader, offset_size, address_size, header_length, strings, &header);
        err.has_error())
      return Err("Line table at 0x%" PRIx64 " %s", offset, err.msg().c_str());
  } else {
    ReadV2Tables(reader, &header);
  }

  header.program = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(line_section.data()) + program_begin,
      unit_end - program_begin);
  return header;
}

}  // namespace

LineStringSections GetLineStringSections(const DwarfSections& sections) {
  LineStringSections result;
  result.line_str = AsStringView(sections.line_str);
  result.str = AsStringView(sections.str);
  return result;
}

size_t LineFileEntries::size() const {
  size_t count = header_.files.size() + defined_files_.size();
  return header_.version >= 5 || count == 0 ? count : count + 1;
}

const LineFileEntry* LineFileEntries::GetFileEntry(uint64_t file_index) const {
  if (header_.version < 5) {
    if (file_index == 0)
      return nullptr;
    file_index--;
  }
  if (file_index < header_.files.size())
    return &header_.files[file_index];
  file_index -= header_.files.size();
  return file_index < defined_files_.size() ? &defined_files_[file_index] : nullptr;
}

std::optional<std::string_view> LineFileEntries::GetFileDirectory(
    const LineFileEntry& entry) const {
  uint64_t dir = entry.dir_index;
  const auto& dirs = header_.include_dirs;
  if (header_.version >= 5)
    return dir != 0 && dir < dirs.size() ? dirs[dir] : std::nullopt;
  return dir > 0 && dir <= dirs.size() ? dirs[dir - 1] : std::nullopt;
}

ErrOr<LineTableExtent> ReadLineTableExtent(std::string_view line_section, uint64_t offset,
                                           Endian endian) {
  if (offset >= line_section.size())
    return Err("Line table offset 0x%" PRIx64 " is outside the section.", offset);

  LineTableExtent extent;
  DataReader reader(AsBytes(line_section), endian);
  reader.Seek(offset);
  uint64_t unit_length = reader.Fixed(4);
  if (unit_length == 0xffffffff) {
    unit_length = reader.Fixed(8);
    extent.offset_size = 8;
  } else if (unit_length >= 0xfffffff0) {
    return Err("Line table at 0x%" PRIx64 " has a reserved unit length.", offset);
  }
  if (!reader.ok())
    return Err("Line table at 0x%" PRIx64 " is truncated.", offset);

  // A unit length larger than the section means the rest of the section, like in LLVM.
  extent.begin = reader.offset();
  extent.end = unit_length < line_section.size() - extent.begin ? extent.begin + unit_length
                                                                : line_section.size();
  return extent;
}

// static
ErrOr<LineProgram> LineProgram::Parse(std::string_view line_section, uint64_t offset, Endian endian,
                                      const LineStringSections& strings) {
  ErrOr<LineProgramHeader> header = ParseLineProgramHeader(line_section, offset, endian, strings);
  if (header.has_error())
    return header.err();

  LineProgram result;
  result.header_ = header.take_value();
  return result;
}

LineProgram::DecodeResult LineProgram::DecodeRows() const {
  DecodeResult result;
  RowSink sink(result.rows, result.defined_files);
  result.status = DecodeProgram(header_, sink);
  return result;
}

LineProgram::FileScanResult LineProgram::ScanFiles(uint64_t first_code_address) const {
  FileScanResult result;
  FileScanSink sink(header_, first_code_address, result.defined_files);
  result.status = DecodeProgram(header_, sink);
  result.file_indices = sink.TakeFiles();
  return result;
}

}  // namespace zxdb
