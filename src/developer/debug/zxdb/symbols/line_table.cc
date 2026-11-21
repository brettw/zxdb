// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/line_table.h"

#include <lib/syslog/cpp/macros.h>

#include <algorithm>

#include "src/developer/debug/shared/largest_less_or_equal.h"
#include "src/developer/debug/zxdb/symbols/line_details.h"
#include "src/developer/debug/zxdb/symbols/line_sequence.h"
#include "src/developer/debug/zxdb/symbols/symbol_context.h"

namespace zxdb {

namespace {

const std::shared_ptr<const LineFileTable>& EmptyFileTable() {
  static const std::shared_ptr<const LineFileTable> empty = std::make_shared<LineFileTable>();
  return empty;
}

// The order of the parameters matters because "line 0" is handled in "greedy" mode only for the
// candidate line. If the caller is asking about an address that matches line 0, we don't want to
// expand that past line boundaries, but we do want to expand other lines across line 0 in greedy
// mode.
bool SameFileLine(const LineRow& reference, const LineRow& candidate,
                  LineTable::LineExtentMode extent_mode) {
  // end_sequence rows don't have files or lines associated with them, they're just markers. The
  // table will report the previous line's file and line as a side-effect of the way it's encoded,
  // so explicitly fail matching for these.
  if (reference.end_sequence() || candidate.end_sequence())
    return false;

  if (extent_mode == LineTable::LineExtentMode::kIncludeLineZero && candidate.line == 0)
    return true;
  return reference.file == candidate.file && reference.line == candidate.line;
}

}  // namespace

LineTable::LineTable() : files_(EmptyFileTable()) {}

LineTable::LineTable(std::shared_ptr<const LineFileTable> files, std::vector<LineRow> rows,
                     uint64_t first_code_address)
    : files_(files ? std::move(files) : EmptyFileTable()) {
  // Find the live sequences in program order.
  std::vector<Sequence> found;
  size_t sequence_begin = 0;
  LineSequenceValidator sequence;
  for (size_t i = 0; i < rows.size(); i++) {
    sequence.AddAddress(rows[i].address);
    if (!rows[i].end_sequence())
      continue;

    uint64_t begin = rows[sequence_begin].address;
    uint64_t end = rows[i].address;
    if (sequence.Finish(first_code_address))
      found.emplace_back(AddressRange(begin, end), sequence_begin, i + 1);
    sequence_begin = i + 1;
  }

  std::stable_sort(found.begin(), found.end(), [](const Sequence& a, const Sequence& b) {
    if (a.addresses.begin() != b.addresses.begin())
      return a.addresses.begin() < b.addresses.begin();
    return a.addresses.end() < b.addresses.end();
  });

  // Compilers usually write sequences in address order. Copy only when the rows need reordering
  // or some rows were dropped.
  bool in_place = true;
  size_t expected_begin = 0;
  for (const Sequence& seq : found) {
    if (seq.row_begin != expected_begin) {
      in_place = false;
      break;
    }
    expected_begin = seq.row_end;
  }
  if (expected_begin != rows.size())
    in_place = false;

  if (in_place) {
    rows_ = std::move(rows);
    sequences_ = std::move(found);
  } else {
    CopySequences(rows, found);
  }

  max_end_.reserve(sequences_.size());
  uint64_t max_end = 0;
  for (const Sequence& seq : sequences_) {
    max_end = std::max(max_end, seq.addresses.end());
    max_end_.push_back(max_end);
  }
}

void LineTable::CopySequences(const std::vector<LineRow>& rows,
                              const std::vector<Sequence>& found) {
  size_t row_count = 0;
  for (const Sequence& seq : found)
    row_count += seq.row_end - seq.row_begin;

  rows_.reserve(row_count);
  sequences_.reserve(found.size());
  for (const Sequence& seq : found) {
    size_t new_begin = rows_.size();
    rows_.insert(rows_.end(), rows.begin() + seq.row_begin, rows.begin() + seq.row_end);
    sequences_.emplace_back(seq.addresses, new_begin, rows_.size());
  }
}

LineTable::SequenceView LineTable::GetSequenceAt(size_t index) const {
  FX_DCHECK(index < sequences_.size());
  return ViewSequence(sequences_[index]);
}

LineTable::SequenceView LineTable::ViewSequence(const Sequence& seq) const {
  return SequenceView(
      std::span<const LineRow>(rows_).subspan(seq.row_begin, seq.row_end - seq.row_begin));
}

LineTable::SequenceView LineTable::GetSequenceForAddress(const SymbolContext& address_context,
                                                         TargetPointer absolute_address) const {
  return GetSequenceForRelativeAddress(address_context.AbsoluteToRelative(absolute_address));
}

LineTable::FoundRow LineTable::GetRowForAddress(const SymbolContext& address_context,
                                                TargetPointer absolute_address,
                                                StatementSelection statement_selection) const {
  SequenceView sequence = GetSequenceForAddress(address_context, absolute_address);
  std::span<const LineRow> seq = sequence.rows();
  if (seq.empty())
    return FoundRow();

  TargetPointer rel_address = address_context.AbsoluteToRelative(absolute_address);
  // LargestLessOrEqual() will return the first item when it compares equal to an item and that's
  // a sequence of exact matches. That's the behavior we want here.
  auto found = debug::LargestLessOrEqual(
      seq.begin(), seq.end(), rel_address,
      [](const LineRow& row, TargetPointer addr) { return row.address < addr; },
      [](const LineRow& row, TargetPointer addr) { return row.address == addr; });

  // The address should not be before the beginning of this sequence (the only end() case for
  // LargestLessOrEqual()). Otherwise GetSequenceForAddress() shouldn't have returned it.
  FX_DCHECK(found != seq.end());

  size_t start_search_index = std::distance(seq.begin(), found);
  size_t symbolizable_index = start_search_index;

  // If the found item has a 0 line entry, then we need to adjust backwards to find a non-zero line
  // entry for symbolization, and adjust forwards for a non-zero line entry with is_stmt set. This
  // is effectively also what gdb does, see https://fxbug.dev/436290252.
  while (seq[symbolizable_index].line == 0 && symbolizable_index > 0) {
    symbolizable_index--;
  }

  size_t next_statement_index = start_search_index;
  if (statement_selection == StatementSelection::kNextStatement) {
    // Skip compiler-generated rows until we find the next is_stmt row. Don't advance to an "end
    // sequence" line because that doesn't represent actual code, just the end of the extent of the
    // sequence. This is going to correspond to where we would place a line-based breakpoint at (or
    // near) absolute_address.
    while (next_statement_index + 1 < seq.size() &&
           (seq[next_statement_index].line == 0 || !seq[next_statement_index].is_stmt()))
      next_statement_index++;
  }

  return FoundRow(sequence, symbolizable_index, next_statement_index);
}

LineDetails LineTable::GetLineDetails(const SymbolContext& symbol_context,
                                      TargetPointer absolute_address, LineExtentMode extent_mode,
                                      const std::string& compilation_dir) const {
  uint64_t relative_address = symbol_context.AbsoluteToRelative(absolute_address);
  SequenceView sequence = GetSequenceForRelativeAddress(relative_address);
  if (sequence.empty())
    return LineDetails();

  // A sequence containing an address has at least one row and the end_sequence row.
  std::span<const LineRow> rows = sequence.rows_with_end_marker();
  FX_DCHECK(rows.size() >= 2);

  // Find the last row at or before the address, excluding the end_sequence row. When the compiler
  // emits several rows at the address of the first instruction of a function, the last one is the
  // one that describes the code.
  auto upper =
      std::upper_bound(rows.begin() + 1, rows.end() - 1, relative_address,
                       [](uint64_t address, const LineRow& row) { return address < row.address; });
  size_t found_row_index = std::distance(rows.begin(), upper) - 1;

  // Adjust the beginning and end ranges to include all matching entries of the same line.
  //
  // Note that this code must not try to hide "line 0" entries (corresponding to compiler-generated
  // code). This function is used by the stepping code which has its own handling for these ranges.
  // Trying to put "line 0" code in with the previous or next entry (what some other code does that
  // tries to hide this from the user) will confuse the stepping code which will always step through
  // these instructions.
  size_t first_row_index = found_row_index;
  while (first_row_index > 0 &&
         SameFileLine(rows[found_row_index], rows[first_row_index - 1], extent_mode)) {
    first_row_index--;
  }
  size_t last_row_index = found_row_index;  // Inclusive.
  while (last_row_index < rows.size() - 1 &&
         SameFileLine(rows[found_row_index], rows[last_row_index + 1], extent_mode)) {
    last_row_index++;
  }

  // Resolve the file name. Skip for "line 0" entries which are compiled-generated code not
  // associated with a line entry, leaving the file name and compilation directory empty. Typically
  // there will be a file if we ask, but that's leftover from the previous row in the table by the
  // state machine and is not relevant.
  std::string file_name;
  std::string result_compilation_dir;
  int line = rows[first_row_index].line_number();
  if (line) {
    if (const std::string* name = GetFileNameForRow(rows[first_row_index]))
      file_name = *name;
    result_compilation_dir = compilation_dir;
  }

  LineDetails result(FileLine(std::move(file_name), std::move(result_compilation_dir), line));

  // Add entries for each row. The last row doesn't count because it is the end_sequence marker
  // that provides the ending address of the previous entry.
  for (size_t i = first_row_index; i <= last_row_index && i < rows.size() - 1; i++) {
    if (rows[i + 1].address < rows[i].address)
      break;  // Going backwards, corrupted so give up.

    LineDetails::LineEntry entry;
    entry.column = rows[i].column;
    entry.range = AddressRange(symbol_context.RelativeToAbsolute(rows[i].address),
                               symbol_context.RelativeToAbsolute(rows[i + 1].address));
    result.entries().push_back(entry);
  }

  return result;
}

LineTable::SequenceView LineTable::GetSequenceForRelativeAddress(
    TargetPointer relative_address) const {
  // Search backward from the last sequence that starts at or before the address. Usually that
  // sequence contains the address or no sequence does. Overlapping and empty sequences can need
  // more steps. The search stops when no earlier sequence ends after the address.
  auto upper = std::upper_bound(
      sequences_.begin(), sequences_.end(), relative_address,
      [](TargetPointer addr, const Sequence& seq) { return addr < seq.addresses.begin(); });
  for (size_t i = std::distance(sequences_.begin(), upper); i > 0; i--) {
    if (max_end_[i - 1] <= relative_address)
      break;
    if (sequences_[i - 1].addresses.InRange(relative_address))
      return ViewSequence(sequences_[i - 1]);
  }
  return SequenceView();
}

}  // namespace zxdb
