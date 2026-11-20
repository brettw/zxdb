// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/line_program_test_util.h"

#include <lib/syslog/cpp/macros.h>

#include "llvm/ADT/StringMap.h"
#include "llvm/DebugInfo/DWARF/DWARFDataExtractor.h"
#include "llvm/DebugInfo/DWARF/DWARFObject.h"
#include "llvm/Support/MemoryBuffer.h"

namespace zxdb {

namespace {

// DWARF 5 line table content types and forms.
constexpr uint8_t kLnctPath = 1;
constexpr uint8_t kLnctDirectoryIndex = 2;
constexpr uint8_t kLnctMd5 = 5;
constexpr uint8_t kFormData2 = 0x05;
constexpr uint8_t kFormString = 0x08;
constexpr uint8_t kFormData1 = 0x0b;
constexpr uint8_t kFormStrp = 0x0e;
constexpr uint8_t kFormUdata = 0x0f;
constexpr uint8_t kFormData16 = 0x1e;
constexpr uint8_t kFormLineStrp = 0x1f;
constexpr uint8_t kFormStrx1 = 0x25;

// The offset of the first entry of .debug_str_offsets, after its DWARF 5 header.
constexpr uint32_t kStrOffsetsBase = 8;

// Operand counts of the standard opcodes 1 to 12.
constexpr uint8_t kStandardLengths[12] = {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};

void AppendUleb(std::vector<uint8_t>& out, uint64_t value) {
  do {
    uint8_t byte = value & 0x7f;
    value >>= 7;
    if (value)
      byte |= 0x80;
    out.push_back(byte);
  } while (value);
}

void AppendSleb(std::vector<uint8_t>& out, int64_t value) {
  bool more = true;
  while (more) {
    uint8_t byte = value & 0x7f;
    value >>= 7;
    if ((value == 0 && !(byte & 0x40)) || (value == -1 && (byte & 0x40)))
      more = false;
    else
      byte |= 0x80;
    out.push_back(byte);
  }
}

void AppendLittleEndian(std::vector<uint8_t>& out, uint64_t value, size_t size) {
  for (size_t i = 0; i < size; i++)
    out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void AppendCString(std::vector<uint8_t>& out, const std::string& str) {
  out.insert(out.end(), str.begin(), str.end());
  out.push_back(0);
}

void IgnoreError(llvm::Error err) { llvm::consumeError(std::move(err)); }

}  // namespace

std::ostream& operator<<(std::ostream& out, const LineRow& row) {
  out << "{0x" << std::hex << row.address << std::dec << ", file " << row.file << ", line "
      << row.line << ", col " << row.column << ",";
  if (row.is_stmt())
    out << " is_stmt";
  if (row.basic_block())
    out << " basic_block";
  if (row.prologue_end())
    out << " prologue_end";
  if (row.epilogue_begin())
    out << " epilogue_begin";
  if (row.end_sequence())
    out << " end_sequence";
  return out << "}";
}

LineTableBuilder::LineTableBuilder(uint16_t version, Format format)
    : version_(version), format_(format) {}

uint64_t LineTableBuilder::AddDirectory(std::string name) {
  dirs_.push_back(std::move(name));
  return version_ >= 5 ? dirs_.size() - 1 : dirs_.size();
}

uint64_t LineTableBuilder::AddFile(std::string name, uint64_t dir_index) {
  files_.push_back({std::move(name), dir_index});
  return version_ >= 5 ? files_.size() - 1 : files_.size();
}

void LineTableBuilder::SetAddress(uint64_t address, uint8_t size) {
  std::vector<uint8_t> bytes = {2};  // DW_LNE_set_address.
  AppendLittleEndian(bytes, address, size);
  Extended(bytes.size(), bytes);
}

void LineTableBuilder::EndSequence() { Extended(1, {1}); }
void LineTableBuilder::Copy() { program_.push_back(1); }

void LineTableBuilder::AdvancePc(uint64_t operation_advance) {
  program_.push_back(2);
  AppendUleb(program_, operation_advance);
}

void LineTableBuilder::AdvanceLine(int64_t delta) {
  program_.push_back(3);
  AppendSleb(program_, delta);
}

void LineTableBuilder::SetFile(uint64_t file) {
  program_.push_back(4);
  AppendUleb(program_, file);
}

void LineTableBuilder::SetColumn(uint64_t column) {
  program_.push_back(5);
  AppendUleb(program_, column);
}

void LineTableBuilder::NegateStmt() { program_.push_back(6); }
void LineTableBuilder::SetBasicBlock() { program_.push_back(7); }
void LineTableBuilder::ConstAddPc() { program_.push_back(8); }

void LineTableBuilder::FixedAdvancePc(uint16_t delta) {
  program_.push_back(9);
  AppendLittleEndian(program_, delta, 2);
}

void LineTableBuilder::SetPrologueEnd() { program_.push_back(10); }
void LineTableBuilder::SetEpilogueBegin() { program_.push_back(11); }

void LineTableBuilder::SetIsa(uint64_t isa) {
  program_.push_back(12);
  AppendUleb(program_, isa);
}

void LineTableBuilder::SetDiscriminator(uint64_t discriminator) {
  std::vector<uint8_t> bytes = {4};  // DW_LNE_set_discriminator.
  AppendUleb(bytes, discriminator);
  Extended(bytes.size(), bytes);
}

void LineTableBuilder::DefineFile(const std::string& name, uint64_t dir_index) {
  std::vector<uint8_t> bytes = {3};  // DW_LNE_define_file.
  AppendCString(bytes, name);
  AppendUleb(bytes, dir_index);
  AppendUleb(bytes, 0);  // Modification time.
  AppendUleb(bytes, 0);  // Length.
  Extended(bytes.size(), bytes);
}

void LineTableBuilder::Special(uint64_t operation_advance, int64_t line_advance) {
  FX_CHECK(line_advance >= line_base_ && line_advance < line_base_ + line_range_);
  uint64_t opcode = (line_advance - line_base_) + (line_range_ * operation_advance) + opcode_base_;
  FX_CHECK(opcode <= 255);
  SpecialOpcode(static_cast<uint8_t>(opcode));
}

void LineTableBuilder::SpecialOpcode(uint8_t opcode) { program_.push_back(opcode); }

void LineTableBuilder::Extended(uint64_t length, const std::vector<uint8_t>& bytes) {
  program_.push_back(0);
  AppendUleb(program_, length);
  program_.insert(program_.end(), bytes.begin(), bytes.end());
}

void LineTableBuilder::Raw(const std::vector<uint8_t>& bytes) {
  program_.insert(program_.end(), bytes.begin(), bytes.end());
}

void LineTableBuilder::Uleb(uint64_t value) { AppendUleb(program_, value); }

void LineTableBuilder::WriteString(std::vector<uint8_t>& out, StringForm form,
                                   const std::string& str,
                                   LineTableTestSections& sections) const {
  size_t offset_size = format_ == Format::kDwarf64 ? 8 : 4;
  switch (form) {
    case StringForm::kString:
      AppendCString(out, str);
      break;
    case StringForm::kLineStrp:
      AppendLittleEndian(
          out, sections.AddString(sections.line_str_, sections.line_str_offsets_, str),
          offset_size);
      break;
    case StringForm::kStrp:
      AppendLittleEndian(out, sections.AddString(sections.str_, sections.str_offsets_map_, str),
                         offset_size);
      break;
    case StringForm::kStrx1:
      out.push_back(sections.AddStrOffset(str));
      break;
  }
}

std::vector<uint8_t> LineTableBuilder::Build(LineTableTestSections& sections) const {
  const bool dwarf64 = format_ == Format::kDwarf64;
  const size_t offset_size = dwarf64 ? 8 : 4;

  // Everything after the header_length field up to the end of the header.
  std::vector<uint8_t> params;
  params.push_back(min_inst_length_);
  if (version_ >= 4)
    params.push_back(max_ops_per_inst_);
  params.push_back(default_is_stmt_ ? 1 : 0);
  params.push_back(static_cast<uint8_t>(line_base_));
  params.push_back(line_range_);
  params.push_back(opcode_base_);
  for (int opcode = 1; opcode < opcode_base_; opcode++) {
    uint8_t length = opcode <= 12 ? kStandardLengths[opcode - 1] : 0;
    if (auto found = opcode_lengths_.find(opcode); found != opcode_lengths_.end())
      length = found->second;
    params.push_back(length);
  }

  if (version_ < 5) {
    for (const std::string& dir : dirs_)
      AppendCString(params, dir);
    params.push_back(0);
    for (const FileEntry& file : files_) {
      AppendCString(params, file.name);
      AppendUleb(params, file.dir_index);
      AppendUleb(params, 0);  // Modification time.
      AppendUleb(params, 0);  // Length.
    }
    params.push_back(0);
  } else {
    auto form_code = [](StringForm form) {
      switch (form) {
        case StringForm::kString:
          return kFormString;
        case StringForm::kLineStrp:
          return kFormLineStrp;
        case StringForm::kStrp:
          return kFormStrp;
        case StringForm::kStrx1:
          return kFormStrx1;
      }
      return kFormString;
    };
    StringForm dir_form = dir_string_form_.value_or(string_form_);
    uint8_t string_form = form_code(string_form_);

    params.push_back(1);  // Directory entry format count.
    AppendUleb(params, kLnctPath);
    AppendUleb(params, form_code(dir_form));
    AppendUleb(params, dirs_.size());
    for (const std::string& dir : dirs_)
      WriteString(params, dir_form, dir, sections);

    params.push_back(include_md5_ ? 3 : 2);  // File entry format count.
    AppendUleb(params, kLnctPath);
    AppendUleb(params, string_form);
    AppendUleb(params, kLnctDirectoryIndex);
    switch (dir_index_form_) {
      case DirIndexForm::kUdata:
        AppendUleb(params, kFormUdata);
        break;
      case DirIndexForm::kData1:
        AppendUleb(params, kFormData1);
        break;
      case DirIndexForm::kData2:
        AppendUleb(params, kFormData2);
        break;
    }
    if (include_md5_) {
      AppendUleb(params, kLnctMd5);
      AppendUleb(params, kFormData16);
    }
    AppendUleb(params, files_.size());
    for (const FileEntry& file : files_) {
      WriteString(params, string_form_, file.name, sections);
      switch (dir_index_form_) {
        case DirIndexForm::kUdata:
          AppendUleb(params, file.dir_index);
          break;
        case DirIndexForm::kData1:
          AppendLittleEndian(params, file.dir_index, 1);
          break;
        case DirIndexForm::kData2:
          AppendLittleEndian(params, file.dir_index, 2);
          break;
      }
      if (include_md5_) {
        for (int i = 0; i < 16; i++)
          params.push_back(static_cast<uint8_t>(i * 17));
      }
    }
  }

  // Everything after the unit_length field.
  std::vector<uint8_t> body;
  AppendLittleEndian(body, version_, 2);
  if (version_ >= 5) {
    body.push_back(address_size_);
    body.push_back(0);  // Segment selector size.
  }
  AppendLittleEndian(body, params.size() + header_length_adjust_, offset_size);
  body.insert(body.end(), params.begin(), params.end());
  body.insert(body.end(), program_.begin(), program_.end());

  std::vector<uint8_t> result;
  uint64_t unit_length = body.size() + unit_length_adjust_;
  if (raw_unit_length_) {
    AppendLittleEndian(result, *raw_unit_length_, 4);
  } else if (dwarf64) {
    AppendLittleEndian(result, 0xffffffff, 4);
    AppendLittleEndian(result, unit_length, 8);
  } else {
    AppendLittleEndian(result, unit_length, 4);
  }
  result.insert(result.end(), body.begin(), body.end());
  FX_CHECK(truncate_bytes_ <= result.size());
  result.resize(result.size() - truncate_bytes_);
  return result;
}

LineTableTestSections::LineTableTestSections() = default;
LineTableTestSections::~LineTableTestSections() = default;

uint64_t LineTableTestSections::Add(const LineTableBuilder& builder) {
  return AddRaw(builder.Build(*this));
}

uint64_t LineTableTestSections::AddRaw(const std::vector<uint8_t>& bytes) {
  FX_CHECK(!context_);
  uint64_t offset = line_.size();
  line_.insert(line_.end(), bytes.begin(), bytes.end());
  return offset;
}

uint64_t LineTableTestSections::AddString(std::vector<uint8_t>& section,
                                          std::map<std::string, uint64_t>& offsets,
                                          const std::string& str) {
  if (auto found = offsets.find(str); found != offsets.end())
    return found->second;
  uint64_t offset = section.size();
  AppendCString(section, str);
  offsets[str] = offset;
  return offset;
}

uint8_t LineTableTestSections::AddStrOffset(const std::string& str) {
  FX_CHECK(str_offsets_.size() < 256);
  str_offsets_.push_back(str);
  return static_cast<uint8_t>(str_offsets_.size() - 1);
}

void LineTableTestSections::AddCompileUnit(uint16_t version, uint64_t line_offset,
                                           const std::vector<uint64_t>& decl_files) {
  FX_CHECK(!context_);
  std::vector<uint8_t> body;
  AppendLittleEndian(body, version, 2);
  if (version >= 5) {
    body.push_back(1);  // DW_UT_compile.
    body.push_back(8);  // Address size.
    AppendLittleEndian(body, 0, 4);  // Abbreviation offset.
  } else {
    AppendLittleEndian(body, 0, 4);
    body.push_back(8);
  }
  AppendUleb(body, 1);  // The compile unit abbreviation.
  AppendLittleEndian(body, line_offset, 4);
  AppendLittleEndian(body, kStrOffsetsBase, 4);
  for (uint64_t decl_file : decl_files) {
    AppendUleb(body, 2);  // The subprogram abbreviation.
    AppendUleb(body, decl_file);
  }
  body.push_back(0);  // End of the children.

  AppendLittleEndian(info_, body.size(), 4);
  info_.insert(info_.end(), body.begin(), body.end());
}

void LineTableTestSections::Finish(uint8_t address_size) {
  FX_CHECK(!context_);
  address_size_ = address_size;

  auto make_buffer = [](const std::vector<uint8_t>& data, const char* name) {
    return llvm::MemoryBuffer::getMemBufferCopy(
        llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size()), name);
  };
  // Abbreviations for AddCompileUnit().
  std::vector<uint8_t> abbrev = {
      1, 0x11, 1,              // 1: DW_TAG_compile_unit with children.
      0x10, 0x17,              // DW_AT_stmt_list, DW_FORM_sec_offset.
      0x72, 0x17,              // DW_AT_str_offsets_base, DW_FORM_sec_offset.
      0, 0,                    //
      2, 0x2e, 0,              // 2: DW_TAG_subprogram without children.
      0x3a, kFormUdata, 0, 0,  // DW_AT_decl_file.
      0,
  };

  // A DWARF 5 string offsets table for the kStrx1 strings.
  std::vector<uint8_t> str_offsets;
  AppendLittleEndian(str_offsets, 4 + 4 * str_offsets_.size(), 4);
  AppendLittleEndian(str_offsets, 5, 2);  // Version.
  AppendLittleEndian(str_offsets, 0, 2);  // Padding.
  for (const std::string& str : str_offsets_)
    AppendLittleEndian(str_offsets, AddString(str_, str_offsets_map_, str), 4);

  buffers_["debug_line"] = make_buffer(line_, "debug_line");
  buffers_["debug_info"] = make_buffer(info_, "debug_info");
  buffers_["debug_abbrev"] = make_buffer(abbrev, "debug_abbrev");
  buffers_["debug_str_offsets"] = make_buffer(str_offsets, "debug_str_offsets");
  buffers_["debug_line_str"] = make_buffer(line_str_, "debug_line_str");
  buffers_["debug_str"] = make_buffer(str_, "debug_str");
  context_ = llvm::DWARFContext::create(buffers_, address_size, true, IgnoreError, IgnoreError);
}

const llvm::DWARFSection& LineTableTestSections::line_section() const {
  return context_->getDWARFObj().getLineSection();
}

ErrOr<LineProgram> LineTableTestSections::Parse(uint64_t offset) const {
  return LineProgram::Parse(*context_, line_section(), offset, address_size_, nullptr);
}

LineTableTestSections::LLVMResult LineTableTestSections::ParseWithLLVM(uint64_t offset) const {
  llvm::DWARFDataExtractor data(context_->getDWARFObj(), line_section(), true, address_size_);
  llvm::DWARFDebugLine::LineTable table;
  uint64_t cur = offset;
  llvm::Error err = table.parse(data, &cur, *context_, nullptr, IgnoreError);

  LLVMResult result;
  result.ok = !err;
  llvm::consumeError(std::move(err));
  result.rows = LineRowsFromLLVM(table);
  result.prologue = table.Prologue;
  return result;
}

}  // namespace zxdb
