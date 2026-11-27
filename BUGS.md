# Known bugs

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
