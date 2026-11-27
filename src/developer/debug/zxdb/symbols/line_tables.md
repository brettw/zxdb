# Line tables

zxdb parses DWARF line tables with its own code: the header, the directory and
file tables, and the line number program. This file describes the design, the known
differences from LLVM, and the edge cases that the tests must cover. Keep the
edge case list current: add a test and an entry for each new edge case.

## Design

zxdb parses each line table header and decodes the line program that follows
it. The parser gets the section bytes and byte order from `DwarfSections`.
`UnitContext` reads the line table offset and the string offsets contribution.
For a unit in a package, the native package index supplies the line
contribution. The binary checks the relative offset against the contribution
size and passes the parser an absolute offset in `.debug_line.dwo`.
DWO units use the `.dwo` line, string, and string offsets sections. All units
use the main `.debug_line_str` section.

| Component | File | Purpose |
|-----------|------|---------|
| `LineRow` | `line_row.h` | 24-byte row: address, line, file, column, flags. |
| `LineSequenceValidator`, `IsStrippedSequenceStart()` | `line_sequence.h` | Shared validation of sequence order and removed code. |
| `LineFileTable`, `ResolveLineFileName()` | `line_file_table.*` | Resolved, normalized file names, indexed by the raw file register value. |
| `LineProgram`, `LineFileEntries` | `line_program.*` | Immutable parsed program, explicit decode results, and views of effective file entries. |
| Section adapter | `dwarf_sections_llvm.*` | Creates section views from the object loader. |
| Line program source | `DwarfBinary::GetLineProgramSource()` | Collects the unit's line table offset and string sections on the binary's owning thread. |
| `LineTable` | `line_table.*` | Concrete sequences and rows, address lookup, `GetLineDetails()`. |
| `LineTableCache` | `line_table_cache.*` | One file table for each line table of a binary, shared by `DW_AT_decl_file` lookups and decoded tables. |
| `SourceFileIndexer`, `LineFilePathResolver`, `CollectLineTableJobs()` | `source_file_indexer.*` | Parallel file indexing with a path cache. |
| `DwarfBinary::GetLineFileTable()`, `DwarfBinaryImpl::DecodeLineTable()` | `dwarf_binary*.h` | Access to the binary's `LineTableCache`. |

Key decisions:

- **One decoder, two sinks.** `DecodeProgram<Sink>()` in `line_program.cc` is
  a template. The row sink builds `LineRow`s. The file scan sink skips the
  line and column operands and builds no rows. Both use the same row emission,
  tombstone, and reset code.
- **The decoder follows LLVM's rules** of `DWARFDebugLine::LineTable::parse()`:
  - Rows are emitted by special opcodes, `DW_LNS_copy`, and
    `DW_LNE_end_sequence`. All registers reset after `end_sequence`.
  - The basic_block, prologue_end, and epilogue_begin flags clear only after
    an emitted row.
  - `DW_LNE_set_address` takes its operand size from the opcode length. Sizes
    other than 1, 2, 4, and 8 are ignored.
  - Rows aren't emitted while the address equals the tombstone for the operand
    size. Only `set_address` changes this state.
  - After an extended opcode, decoding always continues at the offset that the
    opcode length gives.
  - Known standard opcodes have fixed operands. Unknown standard opcodes skip
    the number of ULEB128 operands that the header gives.
  - Opcode 0 always starts an extended opcode, and opcodes at or above
    `opcode_base` are special opcodes.
- **The header parser follows `Prologue::parse()`:**
  - DWARF 2 to 5, DWARF32 and DWARF64, and both byte orders.
  - The fixed fields are read only inside the unit. An error there fails the
    parse.
  - The directory and file tables are read only up to the end that
    `header_length` gives. An error in a table keeps the entries before it
    and doesn't fail the parse.
  - A DWARF 5 entry format must have `DW_LNCT_path`. The forms are the ones
    that `DWARFFormValue::extractValue()` accepts, including
    `DW_FORM_indirect`. An MD5 that isn't 16 bytes is a table error.
  - A directory index has the value that `getAsUnsignedConstant()` gives. The
    constant forms and `DW_FORM_flag` give their value, and
    `DW_FORM_flag_present` gives 1. `DW_FORM_data16` gives 16, its block size.
    The reference forms and `DW_FORM_sdata` aren't unsigned constants, so they
    give 0.
  - Names are views into the section data. A name that can't be decoded (for
    example an offset past the end of a string section) is different from an
    empty name.
- **File names match `getFileNameByIndex()`.** `ResolveLineFileName()` uses
  the same directory choice. `IsPathAbsoluteInAnyStyle()` and
  `AppendPathName()` give the same results as the `llvm::sys::path` functions
  that LLVM uses. An absolute name in POSIX or Windows style ignores the
  directory.
- **Immutable program and explicit results.** `DecodeRows()` and `ScanFiles()`
  are const operations. Each returns its output, decode status, and files
  added by `DW_LNE_define_file`. The header does not change. Call
  `program.files(result.defined_files)` to view the effective file entries,
  or `program.header_files()` to view only the header entries. `LineFileEntries`
  joins these ranges without copying them. The program and the result must
  outlive the view. Section data must outlive the program and its results.
- **Malformed input.** A bad symbol file must not crash zxdb or use much
  memory. See [Malformed input](#malformed-input).
- **No VLIW support.** A header with `maximum_operations_per_instruction`
  above 1 fails to parse. Only VLIW architectures use more than one
  operation per instruction, and zxdb doesn't support them. Like LLVM, the
  parser treats 0 as 1.
- **Decode errors.** A truncated or malformed operand returns `kCorrupt`.
  This includes LEB128 values that exceed 64 bits. The rows before the error
  and file definitions are kept in the result. `LineTable` keeps only complete,
  valid sequences. Callers can use partial output without discarding the status.
- **Removed code matches LLDB.** A sequence that starts below the lowest
  address of a code section of the binary is for code that the linker
  removed (`IsStrippedSequenceStart()`). LLDB uses the same rule
  (`ConvertLineTable()` in `SymbolFileDWARF.cpp`). Linkers give removed code
  the address 0, or 0 plus its offset in the removed section, so this also
  finds removed code at small addresses that aren't 0.
  `DwarfBinary::GetFirstCodeAddress()` gives the lowest address of the
  sections that have `SHF_EXECINSTR`. A separate debug file keeps these
  addresses and flags, so the result is the same as for the binary. A binary
  without code sections, such as a `.dwo` file, gives 0, and then no sequence
  is removed code. The decoder doesn't emit rows at a tombstone address, so
  the rule doesn't check tombstones. `LineTable` and the file scan both get the
  first code address, so they agree.
- **One sequence-validity rule.** `LineSequenceValidator` checks addresses
  as rows arrive. Both `LineTable` and the file scan discard a sequence if
  any address decreases or the first address is stripped. They commit only
  at `end_sequence`, so incomplete trailing sequences are also discarded.
  Equal addresses and zero-length sequences remain valid. This rule is
  different from LLDB (see "Known differences from LLDB").
- **`LineTable` is plain data.** The constructor groups rows into sequences
  at `end_sequence` rows and validates them. It then sorts the
  sequences by address. The rows are copied only when the order changes. The
  object doesn't change after construction.
- **Sequence views have one contract.** Index and address queries return
  `SequenceView`. `rows()` excludes the terminal marker;
  `rows_with_end_marker()` includes it. `end_address()` gives the exclusive
  address boundary. A default view is absent. A view containing only a marker
  is present but has no ordinary rows. Views borrow the table's storage.
- **Lookup policies are explicit.** `StatementSelection` controls the
  breakpoint candidate returned by `GetRowForAddress()`. Symbolization always
  searches backward over line 0, independently of statement selection.
  `kNextStatement` falls back to the last ordinary row when no statement
  follows. `LineExtentMode` controls whether `GetLineDetails()` extends across
  adjacent line 0 rows. Duplicate-address lookup still uses the first row
  for `GetRowForAddress()` and the last row for `GetLineDetails()`.
- **Address lookup works for overlapping sequences.** Empty sequences stay in
  the table, because `GetAllLineTableMatchesInUnit()` uses all sequences, but
  the lookup never returns them. The lookup searches backward from the last
  sequence that starts at or before the address. It stops when the largest end
  address of the earlier sequences is at or before the address. For
  non-overlapping tables this is one step. If more than one sequence contains
  the address, the lookup returns the one with the latest start.
- **One file table for each line table.** `LineTableCache` in each
  `DwarfBinaryImpl` gives `DW_AT_decl_file` lookups and decoded line tables the
  same `LineFileTable`. `DW_LNE_define_file` in the program can add files that
  the header doesn't have. The cache keeps the header's table until a caller
  asks for an index past its entries. Then it decodes the program once, and the
  cached snapshot has all files found before decoding ended. Older snapshots
  remain valid. A failed or corrupt decode is not retried for file lookups.
- **Function lookup moved to the caller.** `GetAllLineTableMatchesInUnit()`
  takes a `FunctionForAddress` callback. Production code passes
  `DwarfUnit::FunctionForRelativeAddress()`. Tests use `MockFunctionMap`.
- **The index contains only files with live code.** A file is in the index
  for a unit only if a row of a live sequence references it. Live sequences
  are complete, ordered, and not stripped. This matches what line breakpoints can use.
  A unit is listed at most once for each file, even when two file entries
  have the same name.
- **Parallel indexing.** `Index::CreateIndex()` first collects the line table
  location and size of each unit on its own thread (`CollectLineTableJobs()`).
  It then starts `SourceFileIndexer`, indexes the DIEs, and merges the file
  results in unit order.
- **Thread count from the amount of data.** For most binaries the work runs
  during DIE indexing, which takes much longer, so the thread count doesn't
  change the index time. A split-DWARF main binary has only skeleton units,
  so the index waits for the line tables, and more workers make it faster.
  Below 256 KB of line tables (about 1 ms of work), the work runs on the
  calling thread. Above that there is one worker, plus one for each 2 MB, up
  to six. See [Measurements](#measurements).
- **Thread safety.** zxdb creates its `DWARFContext` with
  `ThreadSafe = false`. Workers read immutable section views prepared by
  the owning thread. The line parser reads those views.
- **String forms that need the unit.** `DW_FORM_strx` and similar forms read
  the unit's string offsets table. `CollectLineTableJobs()` records the base,
  the entry size, and the string sections in `LineStringSections` on the
  owning thread. The string offsets view ends at the contribution boundary.
  Workers read the entries from this view.
- **Path cache.** `LineFilePathResolver` caches normalized paths by the
  addresses of the directory and file name strings. In DWARF 5 the linker
  merges `.debug_line_str`, so a path is computed once for each thread. A
  directory that is absent or can't be decoded has a null address in the key,
  because LLVM uses an empty directory for both. Every name goes through the
  cache.

### TSan check

Do a one-time TSan run when you change the threading of the source file index.
It isn't a checked-in test. Build with `-fsanitize=thread`. Run these tests,
with `ZXDB_INDEX_BENCHMARK_BINARY` set:

- `Index`, `ModuleSymbols`, `LineProgram*`, `SourceFileIndexer*`,
  `LineFilePath*`, `LineTable*`, `FindLine`, and `DwarfDieDecoder`.
- `SourceFileIndexer*` and the fission `ModuleSymbols` tests with
  `--gtest_repeat=200`.

There must be no reports. The system `libLLVM` isn't built with TSan, but
workers don't call LLVM, so the run covers all the worker code.

## Known differences from LLVM

The tests check the zxdb behavior for each of these. They don't run LLVM.

| Input | LLVM | zxdb | Test |
|-------|------|------|------|
| Special opcode 8 when `opcode_base` <= 8 | Decodes it as `DW_LNS_const_add_pc` | Decodes it as the DWARF standard says | `SpecialOpcodeEightWithLowOpcodeBase` |
| File register above 65535 | Truncates to 16 bits | Keeps 32 bits | `LargeFileIndex` |
| Truncated program | Returns an error and keeps the partial rows in its cache | Returns `kCorrupt` and keeps rows before the error | `TruncatedOpcodes` |
| Extended opcode longer than the rest of the unit | Runs the sub-opcode, then stops | Stops before the opcode | `TruncatedOpcodes` |
| Address in more than one sequence | Returns the first sequence by end address, which misses some overlaps | Returns the containing sequence with the latest start | `LineTable.OverlappingSequences` |
| Header longer than the data | Parses partial tables and no rows | Fails the parse | `TruncatedHeaders`, `ParseErrors` |
| More than one operation per instruction (VLIW) | Parses the rows (experimental) | Fails the parse | `MaxOpsPerInstAboveOneFails` |
| Unknown form in a DWARF 5 entry format | LLVM 19 crashes in `DWARFFormValue::extractValue()` | Keeps the entries before it | `Dwarf5TableFormats` |
| Directory index with a reference form or `DW_FORM_sdata` | Dereferences an empty `std::optional` (undefined behavior) | Uses 0 | `Dwarf5TableFormats` |
| DWARF 5 entries whose forms read no data, with a count larger than the header | Makes every entry, so a 36-byte table can make millions | Fails the parse | `EmptyEntries` |
| Extended opcode length that doesn't fit in 64 bits | Continues at the length and decodes its bytes as opcodes | Returns `kCorrupt` | `LebOverflow` |
| `DW_FORM_strx` index whose entry offset overflows 64 bits | Wraps and reads a wrong entry | No name | `IndexedStringBounds` |
| SLEB128 value with extra bytes past bit 63, for a negative value with a zero bit | LLVM 19 shifts the extra bytes by 64 or more (undefined behavior). On x86, they set the zero bits. LLVM main has the fix. | Ignores the extra bytes, like LLVM main | `LebOverflow` |
| DWARF 5 directory entry with an error in a field after the path, for example a field past the end of the header | Keeps the path as a directory | Drops the entry. The file table after it is lost in both, so the file names are the same. | `TableEntryPastHeaderLength` |

## Known differences from LLDB

LLDB is the reference for debugger behavior. These differences are
intentional. LLDB gets its sequences from the LLVM line table parser, which
keeps a sequence only if its end address is after its start address. LLDB then
drops the sequences that start below the first code address.

| Input | LLDB | zxdb | Test |
|-------|------|------|------|
| Sequence with an address that is less than the address of the row before it | Keeps the sequence. Its address lookup does a binary search on the rows, so the result for those rows can be wrong. | Drops the sequence from `LineTable` and from the file scan. A row order that isn't increasing is corrupt data, and LLVM's verifier reports it as an error. | `RejectsDecreasingSequences` |
| Sequence with zero length (the end address is the same as the start address) | Drops the sequence | Keeps the sequence. No address is in it, but `GetAllLineTableMatchesInUnit()` can find its lines. | `EmptySequenceInsideLiveSequence`, `LineTable.SequenceViews` |

## Malformed input

A symbol file can be corrupt, and zxdb loads symbol files that it didn't
build. The parser and the decoder read only inside the unit and check each
length before they use it. They must not crash, read out of bounds, or use
memory out of proportion to the input.

Measures against overflow and large allocations:

- Lengths are checked by subtraction before addition. This applies to the unit
  length, the header length, opcode lengths, block sizes, and string offsets.
- A `DW_FORM_strx` index is checked against the number of entries in the
  string offsets table before the parser computes the entry offset. LLVM
  computes `base + index * size` first, which can wrap to a valid entry.
- LEB128 values that don't fit in 64 bits are errors, as in LLVM. They don't
  wrap. In the program, they return `kCorrupt`. In the header, they end the
  table, like other errors in a table.
- A DWARF 5 entry format can read no data, for example when its only form is
  `DW_FORM_implicit_const`. Then the count could make up to 2^64 entries
  without reading any data. Such entries have no name. The parser makes them,
  like LLVM, so that the files that `DW_LNE_define_file` adds keep their
  indices. But it fails the header when the count is larger than
  `header_length`. After validating the first entry and the count, the parser
  copies that entry into every slot without decoding its forms again. Every
  other entry, row, and defined file reads at least one byte. So the tables,
  the rows, and the file scan state are limited by the size of the section.
- The file scan doesn't allocate state for a file index past the table. Such
  indices are kept in lists whose size is limited by the number of rows.
- The parser doesn't reserve memory from a count in the data.
- The line register is a `uint32_t` that wraps, like in LLVM, so a corrupt
  table can have lines above `INT_MAX`. `FileLine` uses `int` and requires
  that line 0 has no file. `LineRow::line_number()` gives 0 for such lines,
  and every conversion of a row to a `FileLine` uses it. Before this, a debug
  build failed a `FileLine` check in `GetLineDetails()`. The rows keep the raw
  value, so they still equal LLVM's rows.

LLVM 19 has the `DW_FORM_strx`, empty-entry, and SLEB128 defects. zxdb's line
table parser handles these cases in the native reader. LLVM 19's
`DWARFFormValue::skipValue()` can also accept an attribute length that runs
past the available data. The native DIE reader bounds each form read to the
unit or entry.

### Follow-up work

- Check in a libFuzzer target. A temporary one ran on 2026-11-20, with ASan
  and UBSan, for 20 minutes in 6 processes: 1.7 M inputs, seeded with the
  tables of the fixtures and `build/zxdb`. It covered the header parser, the
  decoder, the file scan, file names, `LineTable` queries, `LineTableCache`
  with an LLVM unit, and `SourceFileIndexer` with and without a worker. It
  also checked that the paths that must agree do. It found the `FileLine`
  failure for lines above `INT_MAX`, and nothing after the fix.

  A second set of runs on 2026-11-20 found no zxdb crash and no zxdb
  error:
  - The same fuzzer, with two more header checks, for 10 minutes in 5
    processes: 536,000 inputs. Bytes after the header must not change the
    header, and each name must be in the header or a string section.
  - A differential fuzzer against LLVM 19, in three runs for 25 minutes in
    total: 11.8 M inputs. It compared the header fields, the table sizes, the
    program extent, the file names, and the rows. In the last run, which
    sorted each difference by cause (1.7 M inputs), the differences were the
    known ones in the table above and directory counts where LLVM read past
    the data after accepting an oversized form. It excludes forms that make
    LLVM 19 crash.
  - A deterministic sweep of 20 real tables: 301,000 cases. It set each of
    the first 48 header bytes to every value, set the next bytes to boundary
    values and single-bit changes, and cut the section at every length up to
    the end of the header.

  These runs found the SLEB128, DWARF 5 directory, and form length defects.
- Report the LLVM defects upstream: the expansion of zero-byte entries, the
  decoding of an oversized extended opcode length as opcodes, the
  overflow of the string offset index, the empty `std::optional` dereference
  for non-constant directory indices, the crash on an unknown form, and the
  value sizes that `skipValue()` doesn't check. LLVM main fixed the
  SLEB128 shift.

## Test coverage

The tests must run fast. They build line tables in memory with
`LineTableBuilder` and `LineTableTestSections` (`line_program_test_util.*`).
`LineTableTestSections` owns the bytes. A `DwarfBinaryImpl` can read the
units directly from its section views. Tests of real binaries use the
debugger's object loader.

Each test checks expected rows and file names. The expected values come from
the DWARF standard and the rules in this document. The tests don't compare
with LLVM, because zxdb is reducing its dependency on LLVM. Don't add tests
that do. Every decoder test also checks that the file scan agrees with the
decoded rows (`DecodeAndCheck()`).

### Edge cases that the tests must cover

**Header and unit** (`line_program_unittest.cc`)

- DWARF 2, 3, 4, and 5, each in DWARF32 and DWARF64 (`LineProgramVersionTest`).
- Unsupported versions 0, 1, and 6, a reserved unit length, a header longer
  than its unit, a truncated header, and an offset outside the section
  (`ParseErrors`).
- A header cut at every byte, for DWARF 2 to 5 in DWARF32 and DWARF64. When
  LLVM rejects the header, zxdb rejects it too (`TruncatedHeaders`).
- A unit length longer than the section and shorter than the program
  (`UnitLengthLongerThanSection`, `UnitLengthShorterThanProgram`).
- DWARF64 unit and header lengths near 2^64, which must not overflow, in both
  byte orders (`LineTableExtent`).
- A header length that is too long or too short (`HeaderLengthMismatch`).
- A table entry that crosses the end of the header, in DWARF 4 and 5. Both
  parsers drop it. Also a DWARF 5 directory whose path is before the end and
  whose next field is after it. LLVM keeps the path, zxdb drops the entry, and
  the file names are the same (`TableEntryPastHeaderLength`).
- A DWARF 5 header address size of 0, 3, 4, or 8
  (`UnsupportedHeaderAddressSize`).
- `maximum_operations_per_instruction` of 0, which is 1, and 2, 4, and 255,
  which fail to parse (`MaxOpsPerInstZero`, `MaxOpsPerInstAboveOneFails`).
- More than one table in a section, including a table at offset 0
  (`MultipleTables`).
- An empty program (`EmptyProgram`).
- Big-endian and little-endian tables for DWARF 2, 4, and 5, with 8-byte and
  4-byte addresses, `DW_LNS_fixed_advance_pc`, and a 4-byte tombstone
  (`LineProgramEndianTest`).

**Opcodes**

- Each standard opcode, the flags that clear after a row, a multi-byte
  ULEB128, a negative line advance, and the 16-bit column truncation
  (`StandardOpcodes`).
- All special opcodes for each of these header settings
  (`LineProgramSpecialTest`):
  - Clang's and GCC's line_base and line_range.
  - `opcode_base` of 1, 10, 13, and 20.
  - `min_inst_length` of 0, 1, 2, and 4.
  - `line_range` of 0.
- `DW_LNS_const_add_pc` and `DW_LNS_advance_pc` scaled by `min_inst_length`,
  and `DW_LNS_fixed_advance_pc` not scaled (`ConstAddPc`).
- `opcode_base` below 13, and unknown standard opcodes with operands
  (`OpcodeBaseBelowStandard`, `UnknownStandardOpcodes`).
- `DW_LNE_set_discriminator`, unknown and zero-length extended opcodes, and
  extended opcodes that are longer or shorter than their operands
  (`ExtendedOpcodes`).
- `DW_LNE_define_file`, which the decoder reports and `LineProgram` adds to
  the file table once, also after a second decode (`DefineFile`). A file
  defined in a table without header files, and an opcode length shorter than
  the operands (`DefineFileEdgeCases`). `DW_AT_decl_file` can reference the
  defined file (`DwarfDieDecoder.GetFileDefinedByProgram`).
- The file scan with `DW_LNE_define_file` (`DefineFileScan`):
  - A row that references a file before the opcode that defines it, with and
    without later rows for that file, and in a stripped sequence.
  - Two scans, and a decode before a scan, give the same files. An index past
    the defined files isn't counted.
- A truncated operand for each opcode that has one (`TruncatedOpcodes`).
- LEB128 operands that don't fit in 64 bits, for each opcode that has one,
  and for a DWARF 4 file entry. The signed first-overflow value, `INT64_MAX + 1`,
  is separate from unsigned overflow at 2^64. The largest values that fit,
  and values with extra bytes that only extend them, decode (`LebOverflow`).
  A negative SLEB128 value with a zero bit and extra bytes past bit 63
  decodes to its value.

**Addresses and sequences**

- `set_address` operands of 1, 2, 4, and 8 bytes, a mismatch with the header
  address size, and unsupported sizes 3 and 0x108 (`SetAddressSizes`).
- Tombstones for 2-, 4-, and 8-byte addresses. The suppression continues
  across `end_sequence` (`Tombstones`).
- Sequences at address 0 (`StrippedSequences`).
- A sequence at a small address that isn't 0 but is below the first code
  address is stripped. A sequence at the first code address is live. With a
  first code address of 0, no sequence is stripped
  (`StrippedSequencesBelowFirstCode`). The indexer workers use the first code
  address of each job (`SourceFileIndexer.StrippedBelowFirstCode`).
- Registers reset after `end_sequence`, with `default_is_stmt` false
  (`RegistersResetAfterEndSequence`).
- A last sequence without `end_sequence` (`ScanFiles`, `LineTable.Sequences`).
- A reversed end or an internal address decrease rejects the sequence in both
  scanning and lookup. A valid later sequence survives (`RejectsDecreasingSequences`).
- A corrupt operand after a complete sequence preserves its rows and file
  definitions in both result types (`PartialResultsKeepDefinedFiles`).

**File references**

- DWARF 2 to 4 inline names:
  - A file in the compilation directory.
  - Relative and absolute directories.
  - An absolute file name with a directory.
  - `..` and `.` normalization.
  - An invalid directory index.
  - File 0 and an index past the end (`FileNamesDwarf4`).
- DWARF 5, each combination of these (`LineProgramFileNameTest`):
  - `DW_FORM_string`, `DW_FORM_line_strp`, and `DW_FORM_strp` names.
  - `DW_FORM_udata`, `DW_FORM_data1`, and `DW_FORM_data2` directory indices.
  - With and without MD5.
  - DWARF32 and DWARF64.
  - Directory 0 isn't in relative names, and file 0 is valid.
- More than 128 files, which needs multi-byte counts and indices
  (`ManyFilesDwarf5`).
- DWARF 5 entry formats (`Dwarf5TableFormats`):
  - No path in the directory or the file format.
  - `DW_FORM_indirect`, also twice in a row.
  - An unknown form after a complete entry, and an MD5 that isn't 16 bytes.
    The entries before them stay.
  - Timestamp, size, `DW_LNCT_LLVM_source`, and unknown content types, with
    `DW_FORM_data4`, `DW_FORM_udata`, `DW_FORM_block`, and `DW_FORM_data8`.
  - `DW_FORM_implicit_const` directory indices.
  - Directory indices with `DW_FORM_flag`, `DW_FORM_flag_present`, and
    `DW_FORM_data16`, which LLVM treats as constants, and with
    `DW_FORM_ref1` and `DW_FORM_sdata`, which it doesn't.
  - A `DW_FORM_strx` index and a `DW_FORM_sdata` value that don't fit in 64
    bits end the table. The signed value uses an ignored vendor field because
    LLVM dereferences an empty optional for signed size or directory fields.
- DWARF 5 entry formats whose forms read no data (`EmptyEntries`):
  - Zero, one, or two empty files, followed by a file that `DW_LNE_define_file`
    adds at the next index.
  - Zero, one, or three empty directories, followed by a file table that is read.
  - A zero-byte file entry with a `DW_FORM_flag_present` directory index.
    Every copied entry keeps the value 1.
  - Counts of 2^40 for directories and for files, which fail the parse.
  - Names that can't be decoded: `DW_FORM_strp_sup` and a `DW_FORM_line_strp`
    offset past the end of the section.
- `DW_FORM_strx` indices with 4-byte and 8-byte string offsets and a base of
  0. An index past the table, and an index whose entry offset overflows, have
  no name (`IndexedStringBounds`).
- Indexed strings in both byte orders, with DWARF32 and DWARF64 units
  (`IndexedStringByteOrder`). The six forms are `DW_FORM_strx`, `strx1`,
  `strx2`, `strx3`, `strx4`, and `GNU_str_index`. Nonzero offsets 4 and 13
  resolve to the expected names. The test also checks the string-offset base
  and width captured for workers.
- File register values above 65535 (`LargeFileIndex`).
- File scan rules (`ScanFiles`, `StrippedSequences`):
  - A file that is set but has no row isn't referenced.
  - A file that only an `end_sequence` row has is referenced.
  - A file index outside the table is ignored.
  - Files of stripped and incomplete sequences aren't referenced.
  - A file in both a stripped and a live sequence is referenced.

**Path cache and parallel indexing** (`source_file_indexer_unittest.cc`)

- Merged strings hit the cache (`CachesMergedStrings`).
- The resolver gives the same name as `LineFileTable` for every entry:
  absolute names, empty names, an invalid directory index, a directory that
  can't be decoded, a name that can't be decoded, and an index past the end
  (`MatchesLineFileTable`).
- The same name in different directories (`SameNameDifferentDirectory`).
- Inline strings in DWARF 4 and 5 (`InlineStrings`).
- With and without threads (`SourceFileIndexerTest`):
  - Jobs in reverse unit order merge in job order.
  - A corrupt table doesn't stop the other units.
  - A unit with duplicate file names is listed once.
  - A table at section offset 0 works.
- No jobs, an existing index, and destruction without `FinishInto()`.
- The default thread count: small tables run on the calling thread, and large
  ones start between two and six workers (`DefaultThreadCount`).
- A unit without `DW_AT_stmt_list` gets no job, and the other units are
  indexed (`UnitWithoutLineTable`).
- `DW_FORM_strx1` names resolve on the workers from the string sections that
  the owning thread records, with and without workers, also when the unit's
  DIEs are freed while the workers run (`SourceFileIndexerStrxTest`).

**File table cache** (`line_table_cache_unittest.cc`)

- The header's table is used until an index past it is requested. Then the
  program is decoded once, and the complete table is cached and shared with
  the decoded line table (`HeaderOnlyUntilNeeded`, `DecodeFirst`).
- Without `DW_LNE_define_file`, decoding keeps the header's table, and
  `Clear()` drops it (`SharesHeaderTableWithoutDefineFile`).
- A line table offset outside the section (`BadLineTable`), and a unit
  without `DW_AT_stmt_list`, for which `GetFile()` finds no file
  (`UnitWithoutLineTable`).
- `DwarfDieDecoder::GetFile()` with a header file, a file that
  `DW_LNE_define_file` adds, an index that no table has
  (`GetFileDefinedByProgram`), and a `DW_FORM_strx1` name (`GetFileStrx`).

**LineTable queries** (`line_table_unittest.cc`, `find_line_unittest.cc`)

- Indexed, relative-address, and absolute-address queries expose the same rows
  and terminal boundary. Marker-only sequences are distinct from absent views
  (`SequenceViews`).
- Sequence sorting, row reordering, stripped, reversed, and incomplete
  sequences (`Sequences`, `SequencesInOrderKeepRows`).
- An empty sequence at the start address of a nonempty one, and inside a
  nonempty one (`EmptySequenceAtSameAddress`,
  `EmptySequenceInsideLiveSequence`).
- Nested and overlapping sequences, and addresses at sequence ends
  (`OverlappingSequences`).
- File lookup for files 0, in range, and out of range (`FileNames`).
- `GetLineDetails()`:
  - Same-line extension, and greedy mode across line 0.
  - A line 0 query.
  - Duplicate addresses use the last row.
  - Sequence boundaries.
  - Addresses outside the table.
  - A missing file name (`GetLineDetails`, `GetLineDetailsMissingFile`).
  - Lines that don't fit in an `int`, which are line 0
    (`GetLineDetailsLineOutsideIntRange`).
- The existing `GetRowForAddress`, line 0, end-sequence, and prologue tests,
  converted from `MockLineTable`.
- The DWARF 4 and 5 package tests in `dwarf_binary_impl_unittest.cc` resolve
  the same relative line offset to different file tables in two contributions.
  They also reject an offset at the contribution's end.

**Real binaries** (`line_program_binary_unittest.cc`)

- For every unit, the table decodes without an error, each sequence ends with
  an `end_sequence` row, and addresses don't decrease inside a sequence. Each
  file that live rows reference has a name. The file scan equals the live
  files of the rows (`DecodesTestBinaries`). `SourceFileIndexer`, which
  resolves names on worker threads, must give the same unit list for each
  file as the decoded rows and names, with and without workers. The binaries
  are:
  - The built and checked-in symbol test libraries.
  - The GCC split-DWARF `fission` binary and its `.dwo` files.
  - The line table fixtures that `Tests.cmake` builds from
    `symbols/test_data/line_table/`:
    - `clang_dwarf4` (`-O0`).
    - `clang_dwarf5_gc` (`-O2`, BFD, `--gc-sections`).
    - `clang_dwarf64` (`-gdwarf64`).
    - `clang_dwarf4_lld_tombstone` (LLD, all-ones tombstones). Optional.
    - `gcc_dwarf4_gc` and `gcc_dwarf5`. Optional.
    - `clang_split.dwo` (`-gsplit-dwarf -fdebug-types-section`). Its type
      unit references the `.debug_line.dwo` table (`DwoTypeUnitLineTable`).
- Each fixture has live rows inside the line range of each function, in the
  right file, and no live row is past the end of its file. This includes the
  file and lines that a `#line` directive gives (`FixtureSourceLines`).
- The stripped function's file isn't in the index, and the live files are.
  This is checked for the address-0 and the tombstone forms (`StrippedCode`).
- `BenchmarkBinary` runs the checks of `DecodesTestBinaries` on the benchmark
  binary when `ZXDB_INDEX_BENCHMARK_BINARY` is set.

### Comparison with LLVM's tests

LLVM's tests are `llvm/unittests/DebugInfo/DWARF/DWARFDebugLineTest.cpp` and
the `llvm-dwarfdump` lit tests (`llvm/test/tools/llvm-dwarfdump/X86/debug_line*`,
`debug-line-*`, and `tombstone.s`).

| LLVM test | zxdb coverage |
|-----------|---------------|
| `GetOrParseLineTableValidTable` (v2 to v5, DWARF32 and DWARF64) | `LineProgramVersionTest.Basic` |
| `GetOrParseLineTableAtInvalidOffset`, `...AfterData` | `ParseErrors` (offset outside the section) |
| `PrologueGetLength` | Indirect: every test starts the program at `getLength()`. `HeaderLengthMismatch` |
| `ErrorForReservedLength`, `ErrorForUnsupportedVersion` | `ParseErrors` |
| `ErrorForInvalidV5IncludeDirTable` | `Dwarf5TableFormats` |
| `ErrorForTooLargePrologueLength`, `ErrorForTooShortPrologueLength` | `HeaderLengthMismatch`, `ParseErrors` |
| `ErrorForExtendedOpcodeLengthSmallerThanExpected`, `...LargerThanExpected` | `ExtendedOpcodes` |
| `ErrorForUnitLengthTooLarge` | `UnitLengthLongerThanSection` |
| `ErrorForMismatchedAddressSize`, `...UnsetInitialAddress` | `SetAddressSizes` |
| `ErrorForUnsupportedAddressSizeInSetAddressLength`, `ErrorForAddressSizeGreaterThanByteSize` | `SetAddressSizes` |
| `ErrorForUnsupportedAddressSizeDefinedInHeader` | `UnsupportedHeaderAddressSize` |
| `CallbackUsedForUnterminatedSequence` | `ScanFiles`, `LineTable.Sequences` |
| `OpIndexAdvance`, `OpIndexReset` (VLIW) | Not applicable. zxdb doesn't support VLIW line tables (`MaxOpsPerInstAboveOneFails`). |
| `MaxOpsZeroDwarf3`, `MaxOpsZeroDwarf4` | `LineProgramVersionTest` (v3), `MaxOpsPerInstZero` |
| `LineRangeProblemsReportedCorrectly` | `LineProgramSpecialTest` (line_range 0) |
| `MinInstLengthProblemsReportedCorrectly` | `LineProgramSpecialTest` (min_inst_length 0) |
| `ErrorForTruncatedPrologue` (one case per field) | `TruncatedHeaders` cuts the header at every byte. |
| `ErrorForTruncatedExtendedOpcode`, `ErrorForTruncatedStandardOpcode` | `TruncatedOpcodes` |
| `Parser*` (the `SectionParser` that walks all tables) | Not applicable. zxdb parses tables by unit offset. `MultipleTables` covers more than one table. |
| `VerboseOutput`, `PrintPathsProperly` (dump output) | Not applicable. The file name tests cover path composition. |
| `ClearLineValidTable` | Not applicable. `Index.ClearDwarfCachesEvictsUnitsAndLineTables` covers zxdb's cache. |
| lit `debug-line-dw-lns-copy.s`, `debug-line-dw-lne-end-sequence.s` | `StandardOpcodes`, `ExtendedOpcodes` |
| lit `debug_line_many_files_v5.s` | `ManyFilesDwarf5` |
| lit `debug_line_dwarf64_large_table.s` | `LineProgramVersionTest` DWARF64, `clang_dwarf64` fixture |
| lit `debug_line_short_prologue*.s`, `debug_line_invalid.test` | `HeaderLengthMismatch`, `ParseErrors`, `TruncatedOpcodes` |
| lit `tombstone.s` | `Tombstones`, `clang_dwarf4_lld_tombstone` fixture |
| lit `dwarfdump-line-dwo.s` | `DwoTypeUnitLineTable` and the `clang_split.dwo` fixture in `DecodesTestBinaries` |
| lit `verify_*.yaml` (the verifier) | Not applicable. |

LLVM's tests don't cover these. The zxdb tests do:

- Real output of Clang and GCC, the BFD and LLD linkers, DWARF 4, DWARF 5,
  and DWARF64.
- The file scan rules for stripped and incomplete sequences.
- The path cache, parallel indexing, and merge order.
- `LineTable` sequence handling and `GetLineDetails()`.

## Measurements

The release build of `Index.BenchmarkIndexing` indexes the debug `build/zxdb`
binary: 501 CUs, 10.8 MB of `.debug_line`, and 1.92 M rows. The repository
`README.md` describes the method.

Median of 11 runs for each version, with old and new runs alternating, on
2026-11-19. The baseline is commit `41983d0`. Absolute times on this machine
are about twice the times in that commit's message, so compare the relative
change.

| Configuration | Before | After | Change |
|---------------|--------|-------|--------|
| Index, pinned to one core (`taskset -c 0`) | 1181 ms | 1020 ms | -161 ms (-14%) |
| Index, all cores | 1204 ms | 1006 ms | -198 ms (-16%) |
| Source file work alone, one thread | n/a | 35 to 43 ms | |

The source file work makes the same 1,112 file names. On one core, its 35 to
43 ms is part of the index time. With all cores, it runs during DIE indexing.

### Split DWARF

A debug build of zxdb with `-gsplit-dwarf` has 541 `.dwo` files. Its main
binary has 24 KB of `.debug_info` (skeleton units only) and 11 MB of
`.debug_line`. The benchmark indexes only the main binary, which is the
serial part of indexing a split-DWARF program. Median of 11 alternating runs
on 2026-11-20:

| Index of the main binary | Time |
|--------------------------|------|
| Before (`41983d0`) | 165.1 ms |
| After, two workers | 29.6 ms |
| After, default (six workers for 11 MB) | 20.2 ms |

### Thread count

The source file work alone, median of 9 runs on 2026-11-20:

| Workers | 0 | 1 | 2 | 4 | 6 |
|---------|---|---|---|---|---|
| Split-DWARF zxdb | 42.2 ms | 39.1 ms | 23.5 ms | 18.5 ms | 16.2 ms |
| Debug zxdb | 35.8 ms | 42.2 ms | 23.6 ms | 17.4 ms | 15.7 ms |

For the debug zxdb binary, the work overlaps DIE indexing, so the index time
doesn't depend on the thread count. The median index time of 11 alternating
runs was 986.5 ms with two workers and 991.5 ms with six, which is within the
noise. For the split-DWARF binary, six workers save about 9 ms over two. A
review measured 32 very small tables: 55 µs on the calling thread and 393 µs
with six workers. The default runs such tables on the calling thread.

### Header parse

Before zxdb had its own header parser, LLVM's `Prologue::parse()` took about
3 ms of the 36 to 42 ms of single-thread source file work. On 2026-11-20, in
one process with alternating runs, parsing all 501 headers of the debug zxdb
binary took 1.6 ms (median 2.7 ms) with the zxdb parser and 2.6 ms (median
3.9 ms) with `Prologue::parse()`. The header parse runs on the workers, so the
index time doesn't change measurably. The reasons for the zxdb parser are
thread safety and `DW_FORM_strx` names on the workers.

The checks in [Malformed input](#malformed-input) and the file scan changes
for `DW_LNE_define_file` don't change the time measurably. On 2026-11-20, the
source file work alone on one thread had a median of 36.2 ms before them and
36.1 ms after, from 30 alternating runs each. With the LEB128 overflow checks
added, a second comparison of 40 alternating runs each gave 37.8 ms before and
36.4 ms after. Single runs vary by more than 5 ms on this machine.

The release build of `LineProgramBinary.BenchmarkBinary` decodes all 1.92 M
rows of the benchmark binary.

### Interface and sequence validation

On 2026-11-20, the interface refactor was measured with release builds of
`Index.BenchmarkIndexing`. Both versions read the same debug zxdb binary
(SHA-256 `9879c59b829451cb6bb4143b30c8db52b0c27e36d72a3ddf2b5aae671d42e104`).
Each comparison alternated the old and new executable. No builds ran during
the measurements. Both versions indexed 1,118 files.

| Work | CPU affinity | Runs per version | Before, median | After, median |
|------|--------------|------------------|----------------|---------------|
| Source file work, default six workers | Normal | 7 | 34.07 ms | 34.68 ms |
| Source file work, calling thread | CPU 3 | 5 | 70.85 ms | 72.38 ms |
| Full indexing | CPU 3 | 5 | 1,812.76 ms | 1,791.77 ms |

Normal-affinity full indexing varied from 1.76 to 2.14 seconds before and
1.67 to 2.28 seconds after. The fixed-CPU comparison reduces that scheduling
variation. These runs show about a 2% change in source file work and no clear
material regression. The new scan also validates every sequence's address
order. It still constructs no rows and copies no header file entries.

## Possible improvements

- **Smaller rows.** Store each address as a `uint32_t` offset from its
  sequence start, which makes a row 16 bytes. Do this only if the size of the
  query-time table matters.
