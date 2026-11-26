// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_PROGRAM_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_PROGRAM_H_

#include <stdint.h>

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "src/developer/debug/zxdb/common/err_or.h"
#include "src/developer/debug/zxdb/symbols/line_row.h"

namespace zxdb {

struct DwarfSections;

// The zxdb line table parser.
//
// zxdb parses the header of each line table, including the directory and file tables, and decodes
// the line number program that follows it. Neither part uses LLVM's line table parser, so worker
// threads can parse line tables from section data alone. The decoder doesn't build intermediate
// structures, so the indexer can find the referenced files of a unit much faster than a full LLVM
// parse.
//
// The parser follows llvm::DWARFDebugLine::Prologue::parse() and the decoder follows the row
// emission rules of llvm::DWARFDebugLine::LineTable::parse(). Line tables with more than one
// operation per instruction (VLIW) aren't supported, and their headers fail to parse.

// Content types of the entry formats of DWARF 5 directory and file tables (DWARF 5 section 7.22).
// The parser reads kLnctPath, kLnctDirectoryIndex, and kLnctMd5, and skips the others.
enum : uint16_t {
  kLnctPath = 0x01,
  kLnctDirectoryIndex = 0x02,
  kLnctTimestamp = 0x03,
  kLnctSize = 0x04,
  kLnctMd5 = 0x05,

  // LLVM extension for the source text of a file.
  kLnctLlvmSource = 0x2001,
};

// The string sections that the names in a line table can reference. Strings are views into these
// sections. Collect them on the thread that owns the DWARFContext. After that they don't change,
// so any thread can use them. See line_program_llvm.h for the LLVM adapter.
struct LineStringSections {
  std::string_view line_str;  // .debug_line_str, for DW_FORM_line_strp.
  std::string_view str;       // The unit's string section, for DW_FORM_strp and DW_FORM_strx.

  // The unit's string offsets table, for DW_FORM_strx. Unset if the unit has none.
  std::string_view str_offsets;
  std::optional<uint64_t> str_offsets_base;
  uint8_t str_offset_size = 4;  // 4 for DWARF32 and 8 for DWARF64 contributions.
};

// Gives the main line string and string sections for a parse without a unit.
LineStringSections GetLineStringSections(const DwarfSections& sections);

// One entry of a file name table. The name views section data. A name that can't be decoded is
// nullopt, which isn't the same as an empty name.
struct LineFileEntry {
  std::optional<std::string_view> name;
  uint64_t dir_index = 0;
};

// A parsed line table header.
struct LineProgramHeader {
  uint16_t version = 0;
  bool little_endian = true;

  uint8_t min_inst_length = 1;
  bool default_is_stmt = true;
  int8_t line_base = 0;
  uint8_t line_range = 0;
  uint8_t opcode_base = 0;

  // Indexed by (opcode - 1). Entries at or above opcode_base - 1 are unused.
  std::array<uint8_t, 256> standard_opcode_lengths{};

  // The tables in header order. In DWARF 5, directory 0 and file 0 are the first entries. In
  // DWARF 2 to 4, indices start at 1, so directory index d is include_dirs[d - 1] and file index f
  // is files[f - 1]. A directory that can't be decoded is nullopt.
  std::vector<std::optional<std::string_view>> include_dirs;
  std::vector<LineFileEntry> files;

  // The opcodes that follow the header, up to the end of the unit.
  std::span<const uint8_t> program;
};

// A view of the effective file entries: the header's entries followed by files defined by the
// program. This avoids copying the header tables for each decode. The header, defined entries,
// and section data must outlive the view.
class LineFileEntries {
 public:
  explicit LineFileEntries(const LineProgramHeader& header,
                           std::span<const LineFileEntry> defined_files = {})
      : header_(header), defined_files_(defined_files) {}

  // The range of raw file register values. In DWARF 2 to 4, index 0 has no file.
  size_t size() const;
  // Returns null for an out-of-range index or the unused DWARF 2 to 4 index 0.
  const LineFileEntry* GetFileEntry(uint64_t file_index) const;

  // Returns the directory of the entry that relative names use, or nullopt if the entry has no
  // directory or the directory can't be decoded. For DWARF 5, directory 0 is the compilation
  // directory, which relative names don't include.
  std::optional<std::string_view> GetFileDirectory(const LineFileEntry& entry) const;

 private:
  const LineProgramHeader& header_;
  std::span<const LineFileEntry> defined_files_;
};

// The bounds of a line table in its section, from the unit length at its offset.
struct LineTableExtent {
  uint64_t begin = 0;  // The offset after the unit length.

  // The end of the unit. A unit length past the end of the section gives the section end, like in
  // LLVM.
  uint64_t end = 0;

  uint8_t offset_size = 4;  // 4 for DWARF32 and 8 for DWARF64.
};

// Reads the unit length of the line table at the offset of the line section.
ErrOr<LineTableExtent> ReadLineTableExtent(std::string_view line_section, uint64_t offset,
                                           bool little_endian);

enum class LineProgramStatus {
  kOk,

  // An operand is truncated or malformed. The result keeps the output and file definitions
  // decoded before the error.
  kCorrupt,
};

// One line table in a .debug_line section, with its parsed header.
//
// This class uses only section data, not LLVM objects. Worker threads can use it while other
// threads use the same DWARFContext. See SourceFileIndexer.
class LineProgram {
 public:
  // Parses the header of the line table at the given section offset. The section data must outlive
  // this object.
  static ErrOr<LineProgram> Parse(std::string_view line_section, uint64_t offset,
                                  bool little_endian, const LineStringSections& strings);

  struct DecodeResult {
    LineProgramStatus status = LineProgramStatus::kOk;
    std::vector<LineRow> rows;
    std::vector<LineFileEntry> defined_files;
  };

  struct FileScanResult {
    LineProgramStatus status = LineProgramStatus::kOk;
    std::vector<uint32_t> file_indices;
    std::vector<LineFileEntry> defined_files;
  };

  // The parsed header never changes. Program-defined files belong to the decode result.
  const LineProgramHeader& header() const { return header_; }

  LineFileEntries header_files() const { return LineFileEntries(header_); }

  // Views the effective file entries for a decode result. The program and the result must
  // outlive the returned view. Header entries are borrowed, never copied.
  LineFileEntries files(std::span<const LineFileEntry> defined_files) const {
    return LineFileEntries(header_, defined_files);
  }

  // Rows are in program order, including end markers and incomplete trailing sequences. A corrupt
  // program keeps the rows and file definitions before the error. Names still view section data.
  [[nodiscard]] DecodeResult DecodeRows() const;

  // Finds sorted, unique file indices in complete, live sequences. The scan uses the same
  // sequence-validity rule as LineTable and includes program-defined files, even when a row refers
  // to one before its definition. A corrupt program keeps results from before the error.
  [[nodiscard]] FileScanResult ScanFiles(uint64_t first_code_address) const;

 private:
  LineProgram() = default;
  LineProgramHeader header_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_PROGRAM_H_
