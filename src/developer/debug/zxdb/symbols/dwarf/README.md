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
- `ReadForm()` reads one attribute from its specification and the unit's form
  parameters. It returns a `FormValue` or an error. `FormValue` stores the
  final form, its class, and its raw value. The form identifies the reference
  base, target string section, or index table. The caller resolves these values
  with unit and section data. `DwarfDieDecoder::AttributeValue` also uses
  `FormValue` to expose values extracted by LLVM.
- `SkipForm()` moves past one value and returns false if the form or its
  bytes are invalid. It needs the form code and the unit's form parameters.
- `UnitContext::Parse()` reads the first DIE of a unit. The context gives the
  table bases, compilation directory, base address, low PC, range list
  reference, and line table offset. `ResolveString()` and `ResolveAddress()`
  resolve forms that use the unit's sections and tables. `ReadAddress()` and
  `ReadLoclistOffset()` take table indices.
- `RangeListReader` reads `.debug_ranges` in DWARF 2 through 4 and
  `.debug_rnglists` in DWARF 5. `ReadFromOffset()` takes a list offset and
  `ReadFromIndex()` takes a `DW_FORM_rnglistx` index. `ReadUnitRanges()`
  reads the root DIE's low and high PCs or its range list.
- `ArangesSet::Parse()` reads one `.debug_aranges` set. It returns the
  ranges, the compilation unit offset, the next set offset, and the
  offsets of any early terminators.
- `DebugNamesReader::Parse()` reads the contributions in `.debug_names`.
  `LookupName()` visits the entries for an exact name. `LookupHash()`
  visits the entries for all names with a given stored hash.
  `VisitNamesWithPrefix()` visits the names that start with the prefix.
  `ReadParent()` reads the parent record for one entry.
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
- `ReadForm()` and `SkipForm()` use the same encoding rules. They reject
  unknown form codes, invalid LEB128 values, missing string terminators,
  and values that extend past the reader limit. On error, the reader can
  remain partway through the value.
- `DW_FORM_addr` uses the address size. `DW_FORM_ref_addr` uses the address
  size in DWARF 2 and the offset size in DWARF 3 and later. Both functions
  accept recognized encodings in any supported unit version.
- Form decoding follows at most eight `DW_FORM_indirect` codes. A direct
  `DW_FORM_implicit_const` gets its value from the abbreviation and uses
  zero DIE bytes. If an indirect form selects `DW_FORM_implicit_const`,
  the decoder reads a signed LEB128 from the DIE as specified in the
  DWARF 5 errata.
- Blocks, inline strings, and 16-byte constants borrow the reader's input.
  Keep the input alive while using these values.
- A `UnitContext` bounds the root DIE to its unit. It keeps the first value
  for each attribute. It resolves indexed forms after it reads the bases.
  Errors in section lookups affect only the values that use those sections.
- A split unit uses its skeleton's address table and base address. For
  DWARF 4 split units, the skeleton can supply GNU address and range bases.
  The GNU range base applies only to the split unit. A DWARF 4 split unit
  uses its own headerless string offsets table. A DWARF 5 split unit uses
  its own string, range, and location list contributions.
- Address indices must fit in the address section. String indices must fit
  in the string offsets contribution. A location list index must fit in the
  offset array, and its target must follow that array in the same
  contribution. Package contribution bounds come from the caller.
- A DWARF 5 range list index must fit in the offset array. The list must
  start after the array and end within the same contribution. A direct
  offset also uses these bounds when the unit identifies its contribution.
  Without a table base, a direct offset in a main unit uses section bounds
  and an index uses the first table.
- A split DWARF 4 unit reads the skeleton's `.debug_ranges` section.
  A split DWARF 5 unit reads its own `.debug_rnglists.dwo` contribution.
  In a package, direct offsets are relative to the start of the contribution.
- Range lists end with a terminator. A bad entry or address overflow causes
  an error for the list. The reader skips empty ranges, reversed ranges, and
  tombstones. It keeps the other ranges in input order.
- An aranges set aligns tuples to the tuple size relative to the set start.
  It accepts versions 2 through 5 and address sizes of 2, 4, and 8 bytes.
  Both the aranges and range list readers require a zero segment selector
  size. An aranges set requires a zero tuple at its end. The reader skips
  earlier zero tuples, empty ranges, and tuples with overflowing end addresses.
- A name index uses the declared contribution length to bound its tables and
  entry pool. The reader keeps the earlier contributions and records the first
  parse error. An invalid entry ends its list. Other lists stay available.
- The name index reads fields in the byte order of the file and uses the
  DWARF32 or DWARF64 offset width. It reads CU and local TU offsets, foreign
  TU signatures, and entry pool offsets from validated table spans.
- A zero bucket count means that the index has no hash table. A zero bucket
  value means that the bucket is empty. Exact lookup scans a hashless index
  by name. Hash lookup uses the stored hashes and skips hashless indexes.
  Exact lookup compares the original spelling after a case-folded hash match.
- The reader decodes entry attributes with `ReadForm()`. A TU index takes
  precedence over a CU index. A single CU is implicit when the entry omits
  its CU index. Parent offsets are relative to the entry pool.
  `DW_FORM_flag_present` means that the parent has no entry in the pool.
- The parsed name indexes have no lazy state. Lookups can share the reader
  across threads while the section bytes stay alive. For relocatable objects
  the caller can supply relocated CU, local TU, and string offsets at parse
  time.
- A parsed context has no lazy state. It borrows the section bytes, including
  the skeleton's address and range sections when applicable. Readers can
  share the context across threads while the section bytes stay alive. The
  reader expects section bytes with ELF relocations already applied.
- The caller can supply an abbreviation table from its cache. The context
  uses the table during parsing. After parsing, the context only depends on
  the section bytes. The binary can clear its caches while a caller holds
  the parsed context.

## Side Effects

Reads change only the reader's cursor and error state.

## Verification

- Build with `cmake --build build --target zxdb_dwarf zxdb_tests`.
- Run `FUCHSIA_ANALYTICS_DISABLED=1 ./build/zxdb_tests --gtest_filter='AbbrevTable.*:ArangesReader.*:DataReader.*:DebugNamesReader.*:DwarfSectionBuilder.*:FormValue.*:Dwarf2To5/FormEncodingTest.*:RangeListReader.*:UnitContext.*:UnitHeader.*:UnitList.*'`.
