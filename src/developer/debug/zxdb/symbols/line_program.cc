// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/line_program.h"

#include <string.h>

#include <algorithm>
#include <cinttypes>

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDataExtractor.h"
#include "llvm/DebugInfo/DWARF/DWARFSection.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "src/developer/debug/zxdb/symbols/line_file_table.h"

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

bool ReadUleb(const uint8_t*& p, const uint8_t* end, uint64_t* out) {
  if (p < end && !(*p & 0x80)) {
    *out = *p++;
    return true;
  }

  uint64_t result = 0;
  unsigned shift = 0;
  while (p < end) {
    uint8_t byte = *p++;
    if (shift < 64)
      result |= static_cast<uint64_t>(byte & 0x7f) << shift;
    shift += 7;
    if (!(byte & 0x80)) {
      *out = result;
      return true;
    }
  }
  return false;
}

bool ReadSleb(const uint8_t*& p, const uint8_t* end, int64_t* out) {
  uint64_t result = 0;
  unsigned shift = 0;
  while (p < end) {
    uint8_t byte = *p++;
    if (shift < 64)
      result |= static_cast<uint64_t>(byte & 0x7f) << shift;
    shift += 7;
    if (!(byte & 0x80)) {
      if (shift < 64 && (byte & 0x40))
        result |= ~uint64_t{0} << shift;
      *out = static_cast<int64_t>(result);
      return true;
    }
  }
  return false;
}

bool SkipLeb(const uint8_t*& p, const uint8_t* end) {
  while (p < end) {
    if (!(*p++ & 0x80))
      return true;
  }
  return false;
}

uint64_t ReadLittleEndian(const uint8_t* p, size_t size) {
  uint64_t result = 0;
  for (size_t i = 0; i < size; i++)
    result |= static_cast<uint64_t>(p[i]) << (8 * i);
  return result;
}

uint64_t TombstoneForSize(size_t size) {
  return size >= 8 ? ~uint64_t{0} : (uint64_t{1} << (8 * size)) - 1;
}

template <typename Sink>
LineProgramStatus DecodeProgram(const LineProgramHeader& header, Sink& sink) {
  // Multiple operations per instruction (VLIW) and big-endian data aren't supported.
  if (!header.little_endian || header.max_ops_per_inst > 1)
    return LineProgramStatus::kUnsupported;

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
      uint64_t length;
      if (!ReadUleb(p, end, &length))
        return LineProgramStatus::kCorrupt;
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
            address = ReadLittleEndian(p + 1, size);
            tombstoned = address == TombstoneForSize(size);
          }
          break;
        }
        case kLneDefineFile:
          // This adds to the file table. Let LLVM handle it.
          return LineProgramStatus::kUnsupported;
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
        uint64_t advance;
        if (!ReadUleb(p, end, &advance))
          return LineProgramStatus::kCorrupt;
        address += advance * min_inst_length;
        break;
      }
      case kLnsAdvanceLine:
        if constexpr (Sink::kNeedsDetails) {
          int64_t delta;
          if (!ReadSleb(p, end, &delta))
            return LineProgramStatus::kCorrupt;
          line += static_cast<uint32_t>(delta);
        } else {
          if (!SkipLeb(p, end))
            return LineProgramStatus::kCorrupt;
        }
        break;
      case kLnsSetFile: {
        uint64_t value;
        if (!ReadUleb(p, end, &value))
          return LineProgramStatus::kCorrupt;
        file = static_cast<uint32_t>(std::min<uint64_t>(value, UINT32_MAX));
        break;
      }
      case kLnsSetColumn:
        if constexpr (Sink::kNeedsDetails) {
          uint64_t value;
          if (!ReadUleb(p, end, &value))
            return LineProgramStatus::kCorrupt;
          column = static_cast<uint16_t>(value);  // LLVM also truncates to 16 bits.
        } else {
          if (!SkipLeb(p, end))
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
        address += ReadLittleEndian(p, 2);
        p += 2;
        break;
      case kLnsSetPrologueEnd:
        flags |= LineRow::kPrologueEnd;
        break;
      case kLnsSetEpilogueBegin:
        flags |= LineRow::kEpilogueBegin;
        break;
      case kLnsSetIsa:
        if (!SkipLeb(p, end))
          return LineProgramStatus::kCorrupt;
        break;
      default:
        // An unknown standard opcode. The header gives its number of ULEB128 operands.
        for (uint8_t i = 0; i < header.standard_opcode_lengths[opcode - 1]; i++) {
          if (!SkipLeb(p, end))
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

  explicit RowSink(std::vector<LineRow>* rows) : rows_(rows) {}

  void OnRow(uint64_t address, uint32_t file, uint32_t line, uint16_t column, uint8_t flags) {
    rows_->emplace_back(address, file, line, column, flags);
  }

 private:
  std::vector<LineRow>* rows_;
};

// Collects the files of live sequences. A sequence is the group of rows up to and including an
// end_sequence row, as LineTable groups them. The files of a sequence are pending until its end
// row shows whether it is live.
class FileScanSink {
 public:
  static constexpr bool kNeedsDetails = false;

  explicit FileScanSink(size_t file_count) : state_(file_count, kUnseen) {}

  void OnRow(uint64_t address, uint32_t file, uint32_t, uint16_t, uint8_t flags) {
    if (!in_sequence_) {
      in_sequence_ = true;
      sequence_start_ = address;
      last_file_ = kNoFile;
    }

    // Most rows have the same file as the previous row, so check that first.
    if (file != last_file_) {
      last_file_ = file;
      if (file < state_.size() && state_[file] == kUnseen) {
        state_[file] = kPending;
        pending_.push_back(file);
      }
    }

    if (flags & LineRow::kEndSequence) {
      uint8_t new_state = IsStrippedSequenceStart(sequence_start_) ? kUnseen : kLive;
      for (uint32_t pending_file : pending_)
        state_[pending_file] = new_state;
      pending_.clear();
      in_sequence_ = false;
    }
  }

  void TakeFiles(std::vector<uint32_t>* files) {
    files->clear();
    for (size_t i = 0; i < state_.size(); i++) {
      if (state_[i] == kLive)
        files->push_back(static_cast<uint32_t>(i));
    }
  }

 private:
  enum : uint8_t { kUnseen, kPending, kLive };
  static constexpr uint32_t kNoFile = UINT32_MAX;

  std::vector<uint8_t> state_;
  std::vector<uint32_t> pending_;
  bool in_sequence_ = false;
  uint64_t sequence_start_ = 0;
  uint32_t last_file_ = kNoFile;
};

void IgnoreLLVMError(llvm::Error err) { llvm::consumeError(std::move(err)); }

}  // namespace

LineProgramStatus DecodeLineProgramRows(const LineProgramHeader& header,
                                        std::vector<LineRow>* rows) {
  rows->clear();
  RowSink sink(rows);
  return DecodeProgram(header, sink);
}

LineProgramStatus ScanLineProgramFiles(const LineProgramHeader& header, size_t file_count,
                                       std::vector<uint32_t>* files) {
  FileScanSink sink(file_count);
  LineProgramStatus status = DecodeProgram(header, sink);
  sink.TakeFiles(files);
  return status;
}

std::vector<uint32_t> GetLiveRowFiles(std::span<const LineRow> rows, size_t file_count) {
  std::vector<bool> live(file_count);

  size_t sequence_begin = 0;
  for (size_t i = 0; i < rows.size(); i++) {
    if (!rows[i].end_sequence())
      continue;
    if (!IsStrippedSequenceStart(rows[sequence_begin].address)) {
      for (size_t row = sequence_begin; row <= i; row++) {
        if (rows[row].file < file_count)
          live[rows[row].file] = true;
      }
    }
    sequence_begin = i + 1;
  }

  std::vector<uint32_t> result;
  for (size_t i = 0; i < file_count; i++) {
    if (live[i])
      result.push_back(static_cast<uint32_t>(i));
  }
  return result;
}

std::vector<LineRow> LineRowsFromLLVM(const llvm::DWARFDebugLine::LineTable& table) {
  std::vector<LineRow> result;
  result.reserve(table.Rows.size());
  for (const auto& row : table.Rows) {
    uint8_t flags = 0;
    if (row.IsStmt)
      flags |= LineRow::kIsStmt;
    if (row.EndSequence)
      flags |= LineRow::kEndSequence;
    if (row.PrologueEnd)
      flags |= LineRow::kPrologueEnd;
    if (row.EpilogueBegin)
      flags |= LineRow::kEpilogueBegin;
    if (row.BasicBlock)
      flags |= LineRow::kBasicBlock;
    result.emplace_back(row.Address.Address, row.File, row.Line, row.Column, flags);
  }
  return result;
}

// static
ErrOr<LineProgram> LineProgram::Parse(const llvm::DWARFContext& context,
                                      const llvm::DWARFSection& section, uint64_t offset,
                                      uint8_t address_size, const llvm::DWARFUnit* unit) {
  llvm::DWARFDataExtractor data(context.getDWARFObj(), section, context.isLittleEndian(),
                                address_size);
  if (!data.isValidOffset(offset))
    return Err("Line table offset 0x%" PRIx64 " is outside the section.", offset);

  LineProgram result;
  result.context_ = &context;
  result.section_ = &section;
  result.unit_ = unit;
  result.offset_ = offset;
  result.address_size_ = address_size;

  uint64_t cur = offset;
  if (llvm::Error err = result.prologue_.parse(data, &cur, IgnoreLLVMError, context, unit))
    return Err("Can't parse line table header: " + llvm::toString(std::move(err)));

  const llvm::DWARFDebugLine::Prologue& prologue = result.prologue_;

  // The unit length can be larger than the section. LLVM uses the rest of the section then.
  uint64_t program_end = offset + prologue.TotalLength + prologue.sizeofTotalLength();
  program_end = std::min<uint64_t>(program_end, section.Data.size());
  uint64_t program_begin = offset + prologue.getLength();
  if (program_begin > program_end)
    return Err("Line table header at 0x%" PRIx64 " is longer than its unit.", offset);

  LineProgramHeader& header = result.header_;
  header.min_inst_length = prologue.MinInstLength;
  // DWARF 2 and 3 have no maximum_operations_per_instruction field and LLVM reports 0.
  header.max_ops_per_inst = std::max<uint8_t>(prologue.MaxOpsPerInst, 1);
  header.default_is_stmt = prologue.DefaultIsStmt;
  header.line_base = prologue.LineBase;
  header.line_range = prologue.LineRange;
  header.opcode_base = prologue.OpcodeBase;
  header.little_endian = context.isLittleEndian();
  size_t length_count =
      std::min(prologue.StandardOpcodeLengths.size(), header.standard_opcode_lengths.size());
  std::copy_n(prologue.StandardOpcodeLengths.begin(), length_count,
              header.standard_opcode_lengths.begin());
  header.program = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(section.Data.data()) + program_begin,
      program_end - program_begin);

  return result;
}

// static
std::optional<uint64_t> LineProgram::GetUnitLineTableOffset(llvm::DWARFUnit& unit) {
  llvm::DWARFDie unit_die = unit.getUnitDIE();
  if (!unit_die)
    return std::nullopt;
  std::optional<uint64_t> stmt_list =
      llvm::dwarf::toSectionOffset(unit_die.find(llvm::dwarf::DW_AT_stmt_list));
  if (!stmt_list)
    return std::nullopt;
  return *stmt_list + unit.getLineTableOffset();
}

// static
ErrOr<LineProgram> LineProgram::ParseForUnit(llvm::DWARFUnit& unit) {
  std::optional<uint64_t> offset = GetUnitLineTableOffset(unit);
  if (!offset)
    return Err("The unit has no line table.");
  return ParseForUnit(unit, *offset);
}

// static
ErrOr<LineProgram> LineProgram::ParseForUnit(llvm::DWARFUnit& unit, uint64_t offset) {
  return Parse(unit.getContext(), unit.getLineSection(), offset, unit.getAddressByteSize(), &unit);
}

size_t LineProgram::GetFileCount() const { return GetLineFileCount(prologue_); }

LineProgramStatus LineProgram::DecodeRows(std::vector<LineRow>* rows) {
  LineProgramStatus status = DecodeLineProgramRows(header_, rows);
  if (status == LineProgramStatus::kUnsupported)
    return ParseWithLLVM(rows);
  return status;
}

LineProgramStatus LineProgram::ScanFiles(std::vector<uint32_t>* files) {
  LineProgramStatus status = ScanLineProgramFiles(header_, GetFileCount(), files);
  if (status == LineProgramStatus::kUnsupported) {
    std::vector<LineRow> rows;
    status = ParseWithLLVM(&rows);
    *files = GetLiveRowFiles(rows, GetFileCount());
  }
  return status;
}

LineProgramStatus LineProgram::ParseWithLLVM(std::vector<LineRow>* rows) {
  llvm::DWARFDataExtractor data(context_->getDWARFObj(), *section_, context_->isLittleEndian(),
                                address_size_);
  llvm::DWARFDebugLine::LineTable table;
  uint64_t cur = offset_;
  llvm::Error err = table.parse(data, &cur, *context_, unit_, IgnoreLLVMError);

  *rows = LineRowsFromLLVM(table);
  prologue_ = table.Prologue;

  if (err) {
    llvm::consumeError(std::move(err));
    return LineProgramStatus::kCorrupt;
  }
  return LineProgramStatus::kOk;
}

}  // namespace zxdb
