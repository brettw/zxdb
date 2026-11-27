# Upstream source

Source: Fuchsia `src/lib/elflib`

Revision: `d6be97150582a56d5b731e686ad507dd17686b00`

Only files needed by the standalone host build are imported. See `LICENSE`.

## Local changes

- `elflib.h` uses the system `<elf.h>` instead of `llvm/BinaryFormat/ELF.h`.
  The ELF names are global, not in the `elflib` namespace. The library doesn't
  link LLVM.
- `elflib.h` defines `ELFCOMPRESS_ZSTD` if the system header doesn't. glibc
  2.36 and earlier don't define it.
- `ElfLib::VisitAllSymbols()` and `ElfLib::VisitAllDynamicSymbols()` give the
  symbols without a map. `GetAllSymbols()` and `GetAllDynamicSymbols()` use
  them.
- `ElfLib::CreateMapped()` owns a read-only file mapping. Like the other
  constructors, it accepts ELF32 and ELF64 in native byte order.
- Section header loading supports extended section numbering. The debug and
  program bits probes use the extended count when present. The file and
  callback accessors cache only successful reads.
- `FindSection()` and `GetSectionName()` look up ELF section names and indices.
  `ReadSection()` borrows uncompressed bytes and owns decompressed bytes for
  the lifetime of the reader. The standalone library links zlib and zstd for
  ELF sections with `SHF_COMPRESSED`.
