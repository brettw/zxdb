// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_PROGRAM_TEST_UTIL_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_PROGRAM_TEST_UTIL_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "llvm/ADT/StringMap.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Support/MemoryBuffer.h"
#include "src/developer/debug/zxdb/symbols/dwarf/dwarf_section_builder.h"
#include "src/developer/debug/zxdb/symbols/dwarf_form.h"
#include "src/developer/debug/zxdb/symbols/line_file_table.h"
#include "src/developer/debug/zxdb/symbols/line_program.h"
#include "src/developer/debug/zxdb/symbols/line_row.h"

namespace zxdb {

class LineTableTestSections;

// The unit type of a DWARF 5 compile unit header (DW_UT_compile).
constexpr uint8_t kUnitTypeCompile = 0x01;

// Returns the form as one byte, for raw test data in braced lists. The form must fit in a one-byte
// unsigned LEB128 value.
uint8_t FormByte(DwarfForm form);

// For test failure messages.
std::ostream& operator<<(std::ostream& out, const LineRow& row);

// Computes the result of LineProgram::ScanFiles() from decoded rows, independently of the scan.
// file_count is the size of the completed file table.
std::vector<uint32_t> GetLiveRowFiles(std::span<const LineRow> rows, size_t file_count,
                                      uint64_t first_code_address);

// Writes one DWARF line table for tests. The header defaults match what Clang writes for x86-64.
//
// The program functions append opcodes in order. The header functions can be called at any time
// before LineTableTestSections::Add().
class LineTableBuilder {
 public:
  enum class Format { kDwarf32, kDwarf64 };

  // How DWARF 5 directory and file names are stored. kStrx1 needs a unit with a string offsets
  // table. See LineTableTestSections::AddCompileUnit().
  enum class StringForm { kString, kLineStrp, kStrp, kStrx1 };

  // How DWARF 5 file entries store their directory index.
  enum class DirIndexForm { kUdata, kData1, kData2 };

  explicit LineTableBuilder(uint16_t version, Format format = Format::kDwarf32);

  // Writes big-endian data. Call it before adding opcodes. LineTableTestSections::Finish() must
  // use the same byte order.
  void set_big_endian(bool big_endian);

  // Header parameters.
  void set_address_size(uint8_t s) { address_size_ = s; }  // DWARF 5 header field.
  void set_min_inst_length(uint8_t v) { min_inst_length_ = v; }
  void set_max_ops_per_inst(uint8_t v) { max_ops_per_inst_ = v; }
  void set_default_is_stmt(bool v) { default_is_stmt_ = v; }
  void set_line_base(int8_t v) { line_base_ = v; }
  void set_line_range(uint8_t v) { line_range_ = v; }
  void set_string_form(StringForm f) { string_form_ = f; }
  // Overrides the string form for directory names only.
  void set_dir_string_form(StringForm f) { dir_string_form_ = f; }
  void set_dir_index_form(DirIndexForm f) { dir_index_form_ = f; }
  void set_include_md5(bool v) { include_md5_ = v; }

  // Sets opcode_base. Standard opcodes 1 to 12 get their standard operand counts. Opcodes above 12
  // get 0 operands unless set_standard_opcode_length() changes them.
  void set_opcode_base(uint8_t v) { opcode_base_ = v; }
  void set_standard_opcode_length(uint8_t opcode, uint8_t length) {
    opcode_lengths_[opcode] = length;
  }

  // Adjusts the unit_length and header_length fields from their correct values, for corrupt
  // tables.
  void set_unit_length_adjust(int64_t v) { unit_length_adjust_ = v; }
  void set_header_length_adjust(int64_t v) { header_length_adjust_ = v; }

  // Sets the unit length to a raw 32-bit value, for reserved and invalid values.
  void set_raw_unit_length(uint32_t v) { raw_unit_length_ = v; }

  // Replaces the directory and file tables with these bytes, for invalid tables.
  void set_raw_tables(std::vector<uint8_t> tables) { raw_tables_ = std::move(tables); }

  // Directory and file tables. Directory entry 0 must be the compilation directory in DWARF 5. In
  // DWARF 2 to 4, the first added directory gets index 1 and the compilation directory isn't in
  // the table. The return value is the value that file entries or file registers use.
  uint64_t AddDirectory(std::string name);
  uint64_t AddFile(std::string name, uint64_t dir_index);

  // Program opcodes.
  void SetAddress(uint64_t address, uint8_t size = 8);
  void EndSequence();
  void Copy();
  void AdvancePc(uint64_t operation_advance);
  void AdvanceLine(int64_t delta);
  void SetFile(uint64_t file);
  void SetColumn(uint64_t column);
  void NegateStmt();
  void SetBasicBlock();
  void ConstAddPc();
  void FixedAdvancePc(uint16_t delta);
  void SetPrologueEnd();
  void SetEpilogueBegin();
  void SetIsa(uint64_t isa);
  void SetDiscriminator(uint64_t discriminator);
  void DefineFile(const std::string& name, uint64_t dir_index);

  // Appends the special opcode for the given advances. The advances must be encodable.
  void Special(uint64_t operation_advance, int64_t line_advance);
  void SpecialOpcode(uint8_t opcode);

  // Appends an extended opcode with the given length field and the bytes that follow it (the
  // sub-opcode and the operands).
  void Extended(uint64_t length, const std::vector<uint8_t>& bytes);

  void Raw(const std::vector<uint8_t>& bytes);
  void Uleb(uint64_t value);

  // Removes bytes from the end of the program, after the unit length is computed. This makes a
  // table whose unit length is larger than the data.
  void set_truncate_bytes(size_t n) { truncate_bytes_ = n; }

 private:
  friend class LineTableTestSections;

  struct FileEntry {
    std::string name;
    uint64_t dir_index;
  };

  std::vector<uint8_t> Build(LineTableTestSections& sections) const;
  void WriteString(std::vector<uint8_t>& out, StringForm form, const std::string& str,
                   LineTableTestSections& sections) const;

  // Appends a fixed-size value in the byte order of this builder.
  void Put(std::vector<uint8_t>& out, uint64_t value, size_t size) const;

  uint16_t version_;
  Format format_;
  bool big_endian_ = false;
  uint8_t address_size_ = 8;
  uint8_t min_inst_length_ = 1;
  uint8_t max_ops_per_inst_ = 1;
  bool default_is_stmt_ = true;
  int8_t line_base_ = -5;
  uint8_t line_range_ = 14;
  uint8_t opcode_base_ = 13;
  std::map<uint8_t, uint8_t> opcode_lengths_;
  StringForm string_form_ = StringForm::kLineStrp;
  std::optional<StringForm> dir_string_form_;
  DirIndexForm dir_index_form_ = DirIndexForm::kUdata;
  bool include_md5_ = false;
  int64_t unit_length_adjust_ = 0;
  int64_t header_length_adjust_ = 0;
  std::optional<uint32_t> raw_unit_length_;
  std::optional<std::vector<uint8_t>> raw_tables_;
  size_t truncate_bytes_ = 0;

  std::vector<std::string> dirs_;
  std::vector<FileEntry> files_;
  std::vector<uint8_t> program_;
};

// In-memory .debug_line, .debug_line_str, and .debug_str sections with an LLVM DWARFContext.
// Strings are merged like a linker does, so equal strings have one offset.
class LineTableTestSections {
 public:
  LineTableTestSections();
  ~LineTableTestSections();

  // Appends a table and returns its section offset.
  uint64_t Add(const LineTableBuilder& builder);

  // Appends raw bytes to the line section.
  uint64_t AddRaw(const std::vector<uint8_t>& bytes);

  size_t line_size() const { return line_.size(); }

  // Adds a compile unit to .debug_info. Its DW_AT_stmt_list is the given line table offset. A null
  // offset makes a unit without a line table. The unit has DW_AT_str_offsets_base for the kStrx1
  // strings. It has one DW_TAG_subprogram child for each value in decl_files, with that
  // DW_AT_decl_file.
  void AddCompileUnit(uint16_t version, std::optional<uint64_t> line_offset,
                      const std::vector<uint64_t>& decl_files);

  // Creates the context. Call once, after all tables are added.
  void Finish(uint8_t address_size = 8, bool little_endian = true);

  const llvm::DWARFContext& context() const { return *context_; }
  llvm::DWARFContext& mutable_context() { return *context_; }

  // Returns the unit in the order AddCompileUnit() added it.
  llvm::DWARFUnit* unit(size_t index) { return context_->getUnitAtIndex(index); }
  const llvm::DWARFSection& line_section() const;

  // Parses the table without a unit, so DW_FORM_strx names don't resolve.
  ErrOr<LineProgram> Parse(uint64_t offset) const;

 private:
  friend class LineTableBuilder;

  uint8_t AddStrOffset(const std::string& str);

  std::vector<uint8_t> line_;
  DwarfStringSectionBuilder line_str_;
  DwarfStringSectionBuilder str_;
  std::vector<uint8_t> info_;
  std::vector<std::string> str_offsets_;  // Strings for kStrx1, by index.
  bool little_endian_ = true;

  // The context references these buffers, so they must outlive it.
  llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> buffers_;
  std::unique_ptr<llvm::DWARFContext> context_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_PROGRAM_TEST_UTIL_H_
