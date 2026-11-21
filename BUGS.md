# Known bugs

## LLVM DIE extraction reads out of bounds after a skipped value

Status: open. zxdb doesn't work around this bug. A later change will fix DIE
extraction.

Found: 2026-11-20, by a differential fuzzer that compared the zxdb line table
parser with LLVM 19.

### Problem

`llvm::DWARFFormValue::skipValue()` doesn't check the size of a value against
the data. It adds the size to the offset and returns `true`, so the offset can
then be past the end of the data. This applies to these forms:

- The block forms `DW_FORM_block`, `DW_FORM_block1`, `DW_FORM_block2`, and
  `DW_FORM_block4`. The length comes from the data, so it can be very large.
- The fixed-size forms, for example `DW_FORM_data8`. On a 4-byte buffer,
  `skipValue(DW_FORM_data8)` returns `true` with the offset at 8.

`llvm::DWARFDebugInfoEntry::extractFast()` also adds fixed attribute sizes to
the offset without a check.

The next read from that offset goes out of bounds.
`llvm::DataExtractor::getULEB128()` has only an `assert` that the offset is in
the data, and release builds don't have the assert. It reads from the wild
address even when the caller gives an `llvm::Error`. For a small overrun, the
read gets data from memory after the section and doesn't crash. ASan doesn't
report it, because `libLLVM.so` isn't instrumented. For example, LLVM 19 read
26 directories from a DWARF 5 line table that had one byte left for them.

LLVM 19 (`/usr/lib/llvm-19`, which zxdb links) has this bug. LLVM main has the
same code (`llvm/lib/DebugInfo/DWARF/DWARFFormValue.cpp` and
`llvm/lib/Support/DataExtractor.cpp`, checked at a3e5f59d69).

`DWARFFormValue::extractValue()` doesn't have this bug. Its reads check the
size, and it reads blocks with `getBytes()`.

### Effect on zxdb

A corrupt `.debug_info` can make zxdb read out of bounds and crash. These
paths skip attribute values with `skipValue()`:

- `llvm::DWARFDebugInfoEntry::extractFast()`, when LLVM extracts the DIEs of a
  unit. zxdb calls this through `DWARFUnit` APIs, for example `getUnitDIE()`
  and DIE iteration.
- `zxdb::DwarfDieDecoder::DecodeInternal()`
  (`src/developer/debug/zxdb/symbols/dwarf_die_decoder.cc`), for each
  attribute that it doesn't decode.

A DIE that has a block attribute with a length past the end of the unit, or a
fixed-size attribute that ends past it, followed by an attribute that reads a
LEB128 value, causes the bad read.

The line table parser doesn't have this bug. zxdb doesn't use LLVM to parse
line tables, and its parser checks each length (see "Malformed input" in
`src/developer/debug/zxdb/symbols/line_tables.md`).

### Verification

The crash was confirmed with ASan on `skipValue()` and `getULEB128()`
directly, and the fixed-size case with `skipValue()` directly. A differential
fuzzer found both through `llvm::DWARFDebugLine::Prologue::parse()`, which uses
`skipValue()` for unknown content types. They weren't confirmed with a full
`.debug_info` section. The effect on the DIE paths comes from reading the code.

```cpp
// A DW_FORM_block whose ULEB128 length (0x7fffffff) is past the 8-byte data.
const char bytes[] = {'\xff', '\xff', '\xff', '\xff', '\x07', 0, 0, 0};
llvm::DataExtractor data(llvm::StringRef(bytes, sizeof(bytes)), true, 8);
llvm::dwarf::FormParams params{5, 8, llvm::dwarf::DWARF32};
uint64_t offset = 0;
llvm::DWARFFormValue::skipValue(llvm::dwarf::DW_FORM_block, data, &offset, params);  // true
llvm::Error err = llvm::Error::success();
data.getULEB128(&offset, &err);  // ASan: SEGV in DataExtractor::getULEB128().
```

### Possible fix

- In zxdb, check the offset against the end of the unit after each
  `skipValue()` call in `DwarfDieDecoder`, and stop the DIE when it is past
  the end.
- In LLVM, make `skipValue()` return `false` when a value is past the end of
  the data, check the fixed sizes in `extractFast()`, and make `getLEB128()`
  fail when the offset is past the end. Report this upstream.

## LLVM DW_FORM_strx offset can wrap in DIEs

Status: open. zxdb doesn't work around this bug.

`llvm::DWARFUnit::getStringOffsetSectionItem()` computes
`getStringOffsetsBase() + Index * ItemSize` and then checks
`Offset + ItemSize` against the section size. A `DW_AT_str_offsets_base` near
2^64 makes the sum wrap, so the check passes. The read that follows checks
its bounds again, so it doesn't read out of bounds. But the wrapped offset can
select a wrong entry, so a DIE name can be wrong.

The zxdb line table parser checks the index before it computes the offset
(see `IndexedStringBounds` in `line_program_unittest.cc`). DIEs still use the
LLVM code. This comes from reading the code. It wasn't confirmed with a test.

## Line table MD5 checksums aren't used

Status: open. Found: 2026-11-20, by an audit of the DWARF 4 and 5 line table
features against LLDB.

### Problem

A DWARF 5 file entry can have a `DW_LNCT_MD5` field, which is the MD5 of the
source file when it was compiled. Clang writes it by default. The zxdb parser
reads the field and checks that it is a 16-byte block, but it doesn't keep the
value. `LineFileEntry` has only the name and the directory index.

### Effect on zxdb

zxdb can't tell when the source file on disk isn't the file that the binary was
built from. It shows the file and lines as if they match. When the file has
changed, the listing and the source of a breakpoint location can show the wrong
code.

### LLDB behavior

LLDB keeps the checksum with each support file (`ParseSupportFilesFromPrologue()`
in `SymbolFileDWARF.cpp`, `SupportFile` in `SupportFile.h`). When it shows a
source file, `SourceManager` computes the MD5 of the file on disk. If the line
table has a checksum and the two differ, LLDB shows a warning once for the file:
"source file checksum mismatch between line table (...) and file on disk
(...)". The `source cache dump` command shows both checksums for each cached
file.

### Possible fix

- Keep the 16 bytes of `DW_LNCT_MD5` in `LineFileEntry` and in
  `LineFileTable`, for each file index.
- When the source file provider loads a file, compute its MD5 and compare it
  with the checksum of the line table. Show a warning once for each file if
  they differ, like LLDB.
- The file scan and the source file index don't need the checksum, so keep it
  out of the scan path.
