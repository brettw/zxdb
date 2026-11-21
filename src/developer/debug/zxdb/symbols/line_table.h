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
  // Selects only the breakpoint candidate. Symbolization always searches backward over line 0.
  enum class StatementSelection {
    kKeepMatchedRow,
    // Advance to a nonzero is_stmt row, or the last nonterminal row if none follows.
    kNextStatement,
  };

  enum class LineExtentMode {
    kSameLine,
    kIncludeLineZero,
  };

  // A borrowed view of a complete sequence. All addresses are module-relative. A default view
  // means no sequence was found. A marker-only sequence is present, but rows() is empty.
  class SequenceView {
   public:
    SequenceView() = default;
    bool empty() const { return rows_.empty(); }
    std::span<const LineRow> rows() const {
      return empty() ? rows_ : rows_.first(rows_.size() - 1);
    }
    std::span<const LineRow> rows_with_end_marker() const { return rows_; }
    AddressRange addresses() const {
      return empty() ? AddressRange() : AddressRange(rows_.front().address, end_address());
    }
    uint64_t end_address() const { return empty() ? 0 : rows_.back().address; }

   private:
    friend class LineTable;
    explicit SequenceView(std::span<const LineRow> rows) : rows_(rows) {}
    std::span<const LineRow> rows_;
  };

  // Candidate rows from GetRowForAddress(). Both indices refer to sequence.rows(). Symbolization
  // searches backward over line 0; statement selection follows StatementSelection. Either candidate
  // can have line 0 or an unresolved file. Check these before constructing FileLine.
  struct FoundRow {
    FoundRow() = default;
    FoundRow(SequenceView seq, size_t symbolizable_index, size_t statement_index)
        : sequence(seq), symbolizable_index(symbolizable_index), statement_index(statement_index) {}

    bool empty() const { return sequence.empty(); }

    // Returns the entry associated with the corresponding index. Call only when !empty(). If this
    // FoundRow is for finding an address to place a line-based breakpoint, use get_statement().
    // Otherwise, use get_symbolizable().
    const LineRow& get_symbolizable() const { return sequence.rows()[symbolizable_index]; }
    const LineRow& get_statement() const { return sequence.rows()[statement_index]; }

    // The containing sequence. It borrows this table and is empty when no address was matched.
    SequenceView sequence;

    // Index within the sequence of the found row to use for symbolization of this address. Valid
    // when !empty().
    size_t symbolizable_index = 0;

    // Index within the sequence of the found row to use for setting line-based breakpoints around
    // this address. Valid when !empty().
    size_t statement_index = 0;
  };

  // An empty line table.
  LineTable();

  // The rows are in DWARF program order. Rows after the last end_sequence row are dropped because
  // they don't form a complete sequence. Sequences that start at a stripped address are also
  // dropped. That is an address below first_code_address, the lowest address of a code section of
  // the binary. Sequences with decreasing addresses are discarded. See LineSequenceValidator.
  LineTable(std::shared_ptr<const LineFileTable> files, std::vector<LineRow> rows,
            uint64_t first_code_address);

  const LineFileTable& files() const { return *files_; }

  // Returns the file name for the row's file, or null if it has none.
  const std::string* GetFileNameForRow(const LineRow& row) const {
    return files_->GetFileName(row.file);
  }

  // The rows of all live sequences, in address order of the sequences.
  std::span<const LineRow> rows_with_end_markers() const { return rows_; }

  // Sequences consist of a contiguous range of addresses and are sorted by address.
  size_t GetNumSequences() const { return sequences_.size(); }

  // Returns a view valid for the lifetime of this table.
  SequenceView GetSequenceAt(size_t index) const;

  // Returns an empty view when no sequence covers the address. All view addresses remain
  // module-relative. Overlapping sequences select the one with the latest start address.
  SequenceView GetSequenceForAddress(const SymbolContext& address_context,
                                     TargetPointer absolute_address) const;
  SequenceView GetSequenceForRelativeAddress(TargetPointer relative_address) const;

  // Finds the row in the line table that covers the given address. If there is no match, the
  // returned sequence will be empty. When more than one row has the address, this returns the
  // first one before applying symbolization and statement selection.
  //
  // Watch out: the addresses in the returned rows will all be module-relative.
  FoundRow GetRowForAddress(
      const SymbolContext& address_context, TargetPointer absolute_address,
      StatementSelection statement_selection = StatementSelection::kNextStatement) const;

  // Computes the extent of the line containing the address. kIncludeLineZero extends across
  // adjacent line 0 rows. A query on line 0 itself does not absorb adjacent source lines.
  //
  // When more than one row has the address, this uses the last one, which is what LLVM does. The
  // compilation_dir is used for the FileLine of the result.
  LineDetails GetLineDetails(const SymbolContext& symbol_context, TargetPointer absolute_address,
                             LineExtentMode extent_mode, const std::string& compilation_dir) const;

 private:
  // Storage offsets are private. row_end is the exclusive end, including the terminal marker.
  struct Sequence {
    Sequence() = default;
    Sequence(const AddressRange& a, size_t r_begin, size_t r_end)
        : addresses(a), row_begin(r_begin), row_end(r_end) {}

    AddressRange addresses;

    // Index of the first row in rows_.
    size_t row_begin = 0;

    // Exclusive end of the sequence in rows_.
    size_t row_end = 0;
  };

  SequenceView ViewSequence(const Sequence& sequence) const;

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
