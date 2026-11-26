# DWARF reader

## Interface

- `DataReader` reads values from a byte span in the specified byte order.
- `ReadUleb`, `ReadSleb`, `SkipUleb`, and `SkipSleb` read or skip LEB128
  values at a byte pointer. `ReadFixed` reads a fixed-size value at a byte
  pointer.
- `DwarfSections` provides read-only views of the sections and their byte order.
- `ReadUnitHeader()` reads one header. `UnitHeader` gives the unit bounds and
  the parameters for decoding forms.
- `UnitList` stores the headers of one section in offset order and finds the
  header that contains an offset.
- `AbbrevTable::Parse()` reads one abbreviation table at the offset from
  `UnitHeader::abbrev_offset`. `Find()` returns the declaration for a code or
  a null pointer. Each declaration stores its tag, children flag, and attribute
  specifications in their original order. `FixedAttributeSize()` gives the
  total byte size for a unit when all attributes have a fixed size.
- `DwarfSectionBuilder` and `DwarfStringSectionBuilder` write section bytes
  for tests. `DwarfSectionBuilder` writes unit headers and abbreviation
  declarations. It can write raw unit lengths for error tests. The build
  compiles the builders only into `zxdb_tests`.

## Invariants

- `zxdb_dwarf` uses no LLVM headers or libraries.
- The reader treats section bytes as untrusted input. It checks each length by
  subtraction before it adds the length. It never allocates from an input count.
- A reader error is sticky. Later reads return zero or an empty view.
- `DwarfSections` does not own the bytes. The `DwarfBinary` implementation
  keeps the bytes alive.
- DWARF offset zero and type signature zero are valid.
- An unknown DWARF 5 unit type uses the compile unit header layout. The list
  uses the declared length to find the next header.
- A bad header stops the list at that unit. The list keeps the earlier headers
  and the error without logging it. Readers can share a parsed list across
  threads.
- An abbreviation list ends with a zero code. An attribute list ends with a
  zero attribute and a zero form. A bad or incomplete declaration causes the
  parse to return an error for that table.
- Only `DW_CHILDREN_yes` sets the children flag. For repeated codes, the first
  declaration wins. The table keeps unknown nonzero tags, attributes, and
  forms. It reads a signed constant for `DW_FORM_implicit_const` and stores
  `DW_FORM_indirect` as a form.
- The parsed table owns its declarations and is read-only. Readers can share
  the table across threads. Lookup uses a direct index for consecutive codes
  and binary search for other codes. Memory use depends on the declarations
  in the section.

## Side Effects

Reads change only the reader's cursor and error state.

## Verification

- Build with `cmake --build build --target zxdb_dwarf zxdb_tests`.
- Run `FUCHSIA_ANALYTICS_DISABLED=1 ./build/zxdb_tests --gtest_filter='AbbrevTable.*:DataReader.*:DwarfSectionBuilder.*:UnitHeader.*:UnitList.*'`.
