# Standalone zxdb

This repository extracts the Fuchsia debugger into a standalone project.

zxdb loads linked executables, shared libraries, and their separate debug files,
including `.dwo` and `.dwp`. It does not support bare relocatable object files
(`.o` files or `ld -r` output). An object with more than one section of the same
DWARF kind fails to load with an error that names the section.

## Imported Fuchsia libraries

- Place support libraries imported from Fuchsia under `src/lib/`.
- Keep each library in its own named subdirectory, such as `src/lib/fxl/`,
  `src/lib/fit/`, or `src/lib/unwinder/`, regardless of its original location
  in the Fuchsia tree.
- Preserve library boundaries. Do not combine unrelated libraries into a
  shared directory or copy their implementations into debugger subsystems.
- Import only the files and dependencies needed by the standalone debugger.
- Preserve upstream copyright notices and license files. Record the upstream
  source path and revision for each imported library in its directory.
- Keep imported libraries buildable from this repository without requiring a
  neighboring Fuchsia checkout.

## Symbol handling references

For questions about symbol handling, such as DWARF, ELF symbols, line
tables, and unwind information, refer to the LLVM and LLDB source code.

- LLVM and LLDB use different symbol libraries. The LLVM `DebugInfo`
  libraries (`llvm/lib/DebugInfo/DWARF`) are parsers for tools such as
  `llvm-dwarfdump` and the symbolizer. LLDB has its own DWARF parser and
  symbol model (`lldb/source/Plugins/SymbolFile/DWARF` and
  `lldb/source/Symbol`). LLDB uses some LLVM parsers, for example the line
  table parser.
- zxdb parses DWARF and ELF itself. Use the LLVM DWARF code as a second
  reference for how producers encode data.
- For the correct behavior of a debugger, the LLDB code is usually the better
  reference. Examples are how to find the code for a source line, how to
  select a row or a scope, and how to continue after malformed data.
- The local agents file can give the path to an LLVM checkout on the
  computer.

## Subsystem guidance

Read applicable nested `AGENTS.md` files before changing subsystem code.
