// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_TABLE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_TABLE_H_

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "src/developer/debug/zxdb/common/address_range.h"
#include "src/developer/debug/zxdb/symbols/arch.h"
#include "src/developer/debug/zxdb/symbols/line_file_table.h"
#include "src/developer/debug/zxdb/symbols/line_row.h"

namespace zxdb {

class LineDetails;
class SymbolContext;

// The decoded line table of one DWARF unit.
//
// This is plain data with no virtual functions. Tests construct it from rows directly. The
// constructor groups the rows into sequences, drops sequences for stripped code, and sorts the
// sequences by address. The object doesn't change after construction, so it is safe to read from
// more than one thread.
class LineTable {
 public:
  // The line table can contain lines marked with a line number of 0. These entries indicate code
  // that is compiler-generated and not associated with any source line.
  enum SkipMode {
    // Return the exact row entry for the address, even if it's marked compiler-generated.
    kExactMatch,

    // If a line query matches a compiler generated line, advances to the next
    // non-compiler-generated line. This usually makes more sense to the user since compiler
    // generated instructions will normally be associated with some code.
    //
    // This mode can still return compiler generated line entries if there is no "next" row to
    // advance to.
    kSkipCompilerGenerated,
  };

  // A row from the line table, as found by GetRowForAddress(). It contains the sequence of rows
  // with two important indices (which may be equal to each other):
  //
  // symbolizable_index and get_symbolizable() correspond to the best entry in the sequence that
  // is suitable for symbolizing with a valid file and line number.
  //
  // statement_index and get_statement() correspond to the best entry in the row for setting
  // breakpoints.
  //
  // Other entries are likely irrelevant for the requested address but may be inspected anyway.
  struct FoundRow {
    FoundRow() = default;
    FoundRow(std::span<const LineRow> seq, size_t symbolizable_index, size_t statement_index)
        : sequence(seq), symbolizable_index(symbolizable_index), statement_index(statement_index) {}

    bool empty() const { return sequence.empty(); }

    // Returns the entry associated with the corresponding index. Call only when !empty(). If this
    // FoundRow is for finding an address to place a line-based breakpoint, use get_statement().
    // Otherwise, use get_symbolizable().
    const LineRow& get_symbolizable() const { return sequence[symbolizable_index]; }
    const LineRow& get_statement() const { return sequence[statement_index]; }

    // The sequence of rows associated with the address. These will be contiguous addresses. This
    // will be empty if nothing was matched. If nonempty, the last row will always be marked with an
    // end_sequence flag.
    std::span<const LineRow> sequence;

    // Index within the sequence of the found row to use for symbolization of this address. Valid
    // when !empty().
    size_t symbolizable_index = 0;

    // Index within the sequence of the found row to use for setting line-based breakpoints around
    // this address. Valid when !empty().
    size_t statement_index = 0;
  };

  // A block of rows with contiguous addresses, ending with an end_sequence row.
  struct Sequence {
    Sequence() = default;
    Sequence(const AddressRange& a, size_t r_begin, size_t r_end)
        : addresses(a), row_begin(r_begin), row_end(r_end) {}

    AddressRange addresses;

    // Index into rows() of the first row.
    size_t row_begin = 0;

    // Index into rows() of the end_sequence row.
    size_t row_end = 0;
  };

  // An empty line table.
  LineTable();

  // The rows are in DWARF program order. Rows after the last end_sequence row are dropped because
  // they don't form a complete sequence. Sequences that start at a stripped address (see
  // IsStrippedSequenceStart()) are also dropped.
  LineTable(std::shared_ptr<const LineFileTable> files, std::vector<LineRow> rows);

  const LineFileTable& files() const { return *files_; }

  // Returns the file name for the row's file, or null if it has none.
  const std::string* GetFileNameForRow(const LineRow& row) const {
    return files_->GetFileName(row.file);
  }

  // The rows of all live sequences, in address order of the sequences.
  const std::vector<LineRow>& rows() const { return rows_; }

  // Sequences consist of a contiguous range of addresses and are sorted by address.
  const std::vector<Sequence>& sequences() const { return sequences_; }
  size_t GetNumSequences() const { return sequences_.size(); }

  // Returns the rows of the given sequence, excluding the end_sequence row.
  std::span<const LineRow> GetSequenceAt(size_t index) const;

  // Returns the sequence of rows (contiguous addresses ending in an end_sequence row) containing
  // the address. The returned array will be empty if the address was not found. See
  // GetRowForAddress().
  //
  // Watch out: the addresses in the returned rows will all be module-relative.
  std::span<const LineRow> GetRowSequenceForAddress(const SymbolContext& address_context,
                                                    TargetPointer absolute_address) const;

  // Finds the row in the line table that covers the given address. If there is no match, the
  // returned sequence will be empty. When more than one row has the address, this returns the
  // first one.
  //
  // Watch out: the addresses in the returned rows will all be module-relative.
  FoundRow GetRowForAddress(const SymbolContext& address_context, TargetPointer absolute_address,
                            SkipMode skip_mode = kSkipCompilerGenerated) const;

  // Computes the extent of the line containing the given address. In greedy mode, the extent
  // continues across adjacent "line 0" rows.
  //
  // When more than one row has the address, this uses the last one, which is what LLVM does. The
  // compilation_dir is used for the FileLine of the result.
  LineDetails GetLineDetails(const SymbolContext& symbol_context, TargetPointer absolute_address,
                             bool greedy, const std::string& compilation_dir) const;

  // Returns null if no sequence contains the address. Sequences can overlap in corrupt or unusual
  // tables. Then this returns the containing sequence with the latest start address.
  const Sequence* GetSequenceForRelativeAddress(TargetPointer relative_address) const;

 private:
  std::span<const LineRow> GetRowsWithEnd(const Sequence& seq) const;

  // Fills rows_ and sequences_ with copies of the found sequences, in the order given.
  void CopySequences(const std::vector<LineRow>& rows, const std::vector<Sequence>& found);

  std::shared_ptr<const LineFileTable> files_;
  std::vector<LineRow> rows_;
  std::vector<Sequence> sequences_;

  // max_end_[i] is the largest end address of sequences_[0] to sequences_[i]. The address lookup
  // uses it to stop searching backward once no earlier sequence can contain the address.
  std::vector<uint64_t> max_end_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_TABLE_H_
