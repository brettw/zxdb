// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_PROGRAM_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_PROGRAM_H_

#include <stdint.h>

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "llvm/DebugInfo/DWARF/DWARFDebugLine.h"
#include "src/developer/debug/zxdb/common/err_or.h"
#include "src/developer/debug/zxdb/symbols/line_row.h"

namespace llvm {
class DWARFContext;
class DWARFUnit;
struct DWARFSection;
}  // namespace llvm

namespace zxdb {

// The zxdb line program decoder.
//
// LLVM parses the header of each line table, which includes the file name table. zxdb decodes the
// line number program that follows the header. The decoder doesn't build intermediate structures,
// so the indexer can find the referenced files of a unit much faster than a full LLVM parse.
//
// The decoder follows the row emission rules of llvm::DWARFDebugLine::LineTable::parse(). When it
// finds a construct that it doesn't support, it reports kUnsupported and the callers parse the
// table with LLVM instead.

// The parameters of a line program, copied from its header.
struct LineProgramHeader {
  uint8_t min_inst_length = 1;
  uint8_t max_ops_per_inst = 1;
  bool default_is_stmt = true;
  int8_t line_base = 0;
  uint8_t line_range = 0;
  uint8_t opcode_base = 0;
  bool little_endian = true;

  // Indexed by (opcode - 1). Entries at or above opcode_base - 1 are unused.
  std::array<uint8_t, 256> standard_opcode_lengths{};

  // The opcodes that follow the header, up to the end of the unit.
  std::span<const uint8_t> program;
};

enum class LineProgramStatus {
  kOk,

  // The program uses a construct that the zxdb decoder doesn't handle. Parse it with LLVM.
  kUnsupported,

  // The program is truncated. The output has the rows that were decoded before the error.
  kCorrupt,
};

// Decodes the rows of a line program. The rows are in program order, with the same contents as
// llvm::DWARFDebugLine::LineTable::Rows.
LineProgramStatus DecodeLineProgramRows(const LineProgramHeader& header,
                                        std::vector<LineRow>* rows);

// Finds the file register values that rows of live sequences reference. A live sequence ends with
// an end_sequence row and its start address isn't a stripped address (see
// IsStrippedSequenceStart()). Values at or above file_count are ignored. The output is sorted.
LineProgramStatus ScanLineProgramFiles(const LineProgramHeader& header, size_t file_count,
                                       std::vector<uint32_t>* files);

// Computes the result of ScanLineProgramFiles() from decoded rows.
std::vector<uint32_t> GetLiveRowFiles(std::span<const LineRow> rows, size_t file_count);

// Converts rows from an LLVM-parsed line table.
std::vector<LineRow> LineRowsFromLLVM(const llvm::DWARFDebugLine::LineTable& table);

// One line table in a .debug_line section, with its parsed header.
//
// This class reads only immutable section data. Worker threads can use it while other threads use
// the same DWARFContext, provided that the unit argument to Parse() is null. See
// SourceFileIndexer.
class LineProgram {
 public:
  // Parses the header of the line table at the given section offset. The context and section must
  // outlive this object. The unit is used to decode string forms that need the unit's string
  // offsets table and can be null.
  static ErrOr<LineProgram> Parse(const llvm::DWARFContext& context,
                                  const llvm::DWARFSection& section, uint64_t offset,
                                  uint8_t address_size, const llvm::DWARFUnit* unit);

  // Returns the effective line table offset of the unit, or nullopt if the unit has no line
  // table. This matches the offset that DWARFContext::getLineTableForUnit() uses.
  //
  // This reads the unit DIE, which can change the state of the LLVM unit. Call it only on the
  // thread that owns the context.
  static std::optional<uint64_t> GetUnitLineTableOffset(llvm::DWARFUnit& unit);

  // Parses the line table header of a unit. Call it only on the thread that owns the context. The
  // second version takes the offset from GetUnitLineTableOffset().
  static ErrOr<LineProgram> ParseForUnit(llvm::DWARFUnit& unit);
  static ErrOr<LineProgram> ParseForUnit(llvm::DWARFUnit& unit, uint64_t offset);

  const llvm::DWARFDebugLine::Prologue& prologue() const { return prologue_; }
  const LineProgramHeader& header() const { return header_; }

  // Number of entries in the file table, indexed by the raw file register value.
  size_t GetFileCount() const;

  // Decode the program. When the zxdb decoder doesn't support the program, these use LLVM to parse
  // it. A DW_LNE_define_file opcode adds entries to the file table, so after a fallback,
  // prologue() returns the prologue from the LLVM parse.
  LineProgramStatus DecodeRows(std::vector<LineRow>* rows);
  LineProgramStatus ScanFiles(std::vector<uint32_t>* files);

 private:
  LineProgram() = default;

  // Parses the whole table with LLVM. Replaces prologue_ with the result.
  LineProgramStatus ParseWithLLVM(std::vector<LineRow>* rows);

  const llvm::DWARFContext* context_ = nullptr;
  const llvm::DWARFSection* section_ = nullptr;
  const llvm::DWARFUnit* unit_ = nullptr;
  uint64_t offset_ = 0;
  uint8_t address_size_ = 0;

  llvm::DWARFDebugLine::Prologue prologue_;
  LineProgramHeader header_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_PROGRAM_H_
