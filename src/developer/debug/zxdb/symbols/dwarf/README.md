# DWARF reader

## Interface

- `DataReader` reads values from a byte span in the specified byte order.
- `ReadUleb`, `ReadSleb`, `SkipUleb`, and `SkipSleb` read or skip LEB128
  values at a byte pointer. `ReadFixed` reads a fixed-size value at a byte
  pointer.
- `DwarfSections` provides read-only views of the sections and their byte order.
- `DwarfSectionBuilder` and `DwarfStringSectionBuilder` write section bytes
  for tests. The build compiles them only into `zxdb_tests`.

## Invariants

- `zxdb_dwarf` uses no LLVM headers or libraries.
- The reader treats section bytes as untrusted input. It checks each length by
  subtraction before it adds the length. It never allocates from an input count.
- A reader error is sticky. Later reads return zero or an empty view.
- `DwarfSections` does not own the bytes. The `DwarfBinary` implementation
  keeps the bytes alive.
- DWARF offset zero is valid.

## Side Effects

Reads change only the reader's cursor and error state.

## Verification

- Build with `cmake --build build --target zxdb_dwarf zxdb_tests`.
- Run `FUCHSIA_ANALYTICS_DISABLED=1 ./build/zxdb_tests --gtest_filter='DataReader.*:DwarfSectionBuilder.*'`.
