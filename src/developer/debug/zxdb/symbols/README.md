<!--* freshness: { reviewed: '2026-10-12' } *-->

# Symbol Library (`zxdb/symbols`)

## Overview

`//src/developer/debug/zxdb/symbols` is the DWARF and ELF symbol indexing and
lookup subsystem (Layer 2) for `zxdb`. It decodes DWARF Debug Information
Entries (DIEs), line tables, and ELF symbols into a reference-counted object
graph, and evaluates DWARF location expressions (`DwarfExprEval`).

The [DWARF reader](dwarf/README.md) provides the section views and byte reader.

For lookup behavior with incomplete DWARF or ELF symbols alone, see
[Finding a breakpoint by function name](name_lookup.md).

## Interface

Public headers expose these symbol classes, factories, and evaluators:

- `zxdb::Symbol` (`symbol.h`): Reference-counted base class
  (`fxl::RefCountedThreadSafe<Symbol>`) for DWARF DIE symbols with manual RTTI
  (`As<T>()`). Exposes `GetLazySymbol`, `GetDieRef`, `parent`,
  `GetAssignedName`, `GetFullName`, `GetIdentifier`, `GetCompileUnit`,
  `GetDwarfUnit`, `GetModuleSymbols`, `GetSymbolContext`, and `GetLanguage`.
- `zxdb::Type` (`type.h`, `collection.h`): Base type symbol (`StripCV`,
  `StripCVT`, `is_declaration`, `byte_size`). Subclasses include `BaseType`,
  `ModifiedType`, `Collection` (struct, class, union, and Rust enum or tuple),
  `ArrayType`, `FunctionType`, `Enumeration`, `MemberPtr`, `VariantPart`, and
  `Variant`.
- `zxdb::Value`, `zxdb::Variable`, `zxdb::DataMember`: Named data symbols with
  `type`, `is_external`, `artificial`, `const_value`, and `VariableLocation`
  metadata (`value.h`, `variable.h`, `data_member.h`).
- `zxdb::CodeBlock`, `zxdb::Function`, `zxdb::CompileUnit`: Lexical scopes,
  functions, inlined routines, and compilation units (`code_block.h`,
  `function.h`, `compile_unit.h`). Exposes address range queries (`code_ranges`,
  `GetAbsoluteCodeRanges`, `ContainsAddress`) and scope traversal
  (`GetMostSpecificChild`, `GetContainingFunction`, `GetInlineChain`,
  `GetAmbiguousInlineChain`).
- `zxdb::LazySymbolBase`, `zxdb::LazySymbol`, `zxdb::UncachedLazySymbol`,
  `zxdb::SymbolFactory`, `zxdb::DwarfSymbolFactory`: Lazy DIE reference wrappers
  and factories that decode `DwarfDieRef` entries into `Symbol` objects on
  demand (`lazy_symbol.h`, `symbol_factory.h`, `dwarf_symbol_factory.h`).
  `DwarfSymbolFactory` uses `DwarfDie` to decode an entry. It stores a
  `DwarfDieRef` in each lazy symbol so it can find the entry again.
- `zxdb::SystemSymbols`, `zxdb::TargetSymbols`, `zxdb::ProcessSymbols`,
  `zxdb::ModuleSymbols`, `zxdb::ModuleSymbolsImpl`, `zxdb::LoadedModuleSymbols`:
  Symbol containers from global `BuildIDIndex` caches down to loaded modules
  with `SymbolContext` address translation (`system_symbols.h`,
  `target_symbols.h`, `process_symbols.h`, `module_symbols.h`,
  `loaded_module_symbols.h`).
- `zxdb::DwarfExpr`, `zxdb::DwarfExprEval`, `zxdb::SymbolDataProvider`:
  Asynchronous stack-based DWARF expression evaluator and target memory/register
  provider interface (`dwarf_expr.h`, `dwarf_expr_eval.h`,
  `symbol_data_provider.h`).
- Binary readers, symbol indices, line tables, and scope visitors:
  `zxdb::DwarfBinary`, `zxdb::DwarfUnit`, `zxdb::DwarfDieDecoder`,
  `zxdb::DwarfDieScanner`, `zxdb::ElfSymbol`, `zxdb::Index`, and
  `zxdb::IndexNode`. Also exposes `zxdb::Identifier`, `zxdb::Location`,
  `zxdb::InputLocation`, `zxdb::LineTable`, `zxdb::LineDetails`,
  `zxdb::VariableLocation`, `zxdb::VisitScopes`, and symbol name helpers in
  `symbol_utils.h` (`zxdb::GetSymbolScopePrefix`, `zxdb::NameHasTemplate`,
  `zxdb::StripTemplate`, `zxdb::AddTemplateParameterToName`).

- `DwarfDie` holds an entry in an immutable `DieTable`. Use it to read
  the tag, walk the tree, or iterate over the attribute specifications.
  The handle keeps its table alive through cache eviction. Keep the binary
  alive while reading the encoded attributes. Use `DwarfDie::ref()` to
  store the section and offset for later lookup. Resolve names and decode
  attributes on the binary's owning thread because those operations can use
  the unit context cache.
  Name lookup follows the specification, abstract origin, and type signature
  in that order. A Clang inline instance can reach its name through an
  abstract origin that points to a separate specification.
- `DwarfUnit::GetUnitDie()` returns the root and loads its children. Use
  `kRootOnly` when you need only the root. A root-only handle keeps its
  original view after a full load. Later root lookups reuse a full table
  when it is cached. `GetDieAtIndex()` and `GetDieAtOffset()` return handles
  in that unit. To find a DIE across units, pass its `DwarfDieRef` to
  `DwarfBinary::GetDieAtOffset()`.
- `DwarfUnit::GetDies()` walks one full DIE table snapshot in section
  order, including null entries. `DwarfDie::unit_dies()` walks the snapshot
  that a handle already owns. Each iterator reuses its entry handle as it
  advances. Copy the handle to keep an entry past the current iteration
  or through cache eviction.
- `DwarfUnit::FunctionForRelativeAddress()` uses the unit's
  `FunctionAddressMap` to find a function or inline instance.
- The index scans each unit in table order. It uses parent indices to track
  scope and stores `DwarfDieRef` values so it can release the table after
  indexing.
- `DwarfDieDecoder::Decode()` takes a `DwarfDie` and reads attributes within
  that entry's bounds. `GetValue()` returns an `AttributeValue` with a zxdb
  `FormValue` and the form parameters and byte order of its unit. The value
  borrows its source DIE from the decoder until the next decode or until
  the decoder is destroyed.

- Line tables: `zxdb::LineTable` (`line_table.h`) is concrete, immutable data
  made of `zxdb::LineRow` rows (`line_row.h`) and a `zxdb::LineFileTable`
  (`line_file_table.h`). Tests construct it directly with
  `line_table_test_support.h`. `zxdb::LineProgram` (`line_program.h`) parses
  a line table header and decodes its program without LLVM's line table
  parser. Its header remains immutable. Decode and scan results carry status,
  output, and program-defined files. `LineFileEntries` views the effective
  file entries without copying the header. `line_program_llvm.h` contains
  the adapter functions that must run on the LLVM context thread.
  `LineTable::SequenceView` separates ordinary rows from the end marker.
  `StatementSelection` and `LineExtentMode` name the lookup policies.
  `zxdb::SourceFileIndexer` (`source_file_indexer.h`) finds the source files
  of each unit for the `Index`. See `line_tables.md` for the design and the
  required test coverage.

## Invariants

Callers and implementations must preserve these invariants:

- zxdb loads linked executables, shared libraries, and their separate debug
  files, including `.dwo` and `.dwp`. It does not support bare relocatable
  object files (`.o` files or `ld -r` output). If an object has more than one
  section of the same DWARF kind, loading fails with an error that names it.
- `Symbol` objects use `fxl::RefCountedThreadSafe<Symbol>` and do not globally
  cache or deduplicate instances by DIE offset. Never compare `Symbol*` pointers
  to test semantic symbol equality.
- Downward edges in the symbol graph (parent to child) must use cached
  `LazySymbol` instances. Upward edges (such as `Symbol::parent()` and
  `Function::containing_block()`) must use `UncachedLazySymbol` to prevent
  reference-counting cycles and memory leaks.
- `SymbolFactory::CreateSymbol()`, `LazySymbol::Get()`, and
  `UncachedLazySymbol::Get()` never return `nullptr`. On failure or unloaded
  modules, they return an empty `Symbol` with `DwarfTag::kNone`.
  `UncachedLazySymbol::Get()` constructs a new `fxl::RefPtr<Symbol>` on each
  call. Store the `fxl::RefPtr<Symbol>` in a local variable before you call
  `Symbol::As<Derived>()`. When you walk `Symbol::parent()` chains, verify
  `is_valid()` or `tag() != DwarfTag::kNone`.
- `Symbol`, `CodeBlock`, and `ModuleSymbols` store module-relative addresses.
  Pass a `SymbolContext` bound to a loaded module's base address to convert to
  or from absolute process addresses.
- `TargetSymbols::ResolveInputLocation` rejects address-based locations
  (`InputLocation::Type::kAddress`) because address resolution requires a
  running process via `ProcessSymbols`.
- Use `Symbol::As<Derived>()` instead of C++ `dynamic_cast` because the build
  disables compiler RTTI.
- DWARF unit offsets and type signatures of `0` are valid DWARF values
  (`DwarfDieRef`, `UnitSymbolFactory`, `DwarfSymbolFactory`). Never use `0` as
  an invalid sentinel. Use `std::optional<uint64_t>` and guard accessors with
  `DCHECK(is_valid())`.
- `DwarfSymbolFactory` handles `DwarfTag::kTypeUnit` alongside
  `DwarfTag::kCompileUnit` so child DIEs can reach `llvm::DWARFUnit` via
  `CompileUnit`.
- `DwarfBinaryImpl` reads unit headers from `DwarfSections`. Its unit lists
  put info units before types units for both normal and DWO sections. DIE
  lookups use the ELF section and unit offset. Raw DIE lookups return handles
  directly. The index requests a cached unit wrapper when it needs to walk
  the scope of a declaration in another unit.
- When a type signature has no target DIE, `DwarfSymbolFactory` returns an
  empty `Symbol`.
- A unit-relative DIE reference must point into the unit's DIE data. A
  section-relative reference uses the source's info section. Type signature
  lookups prefer the source's normal or split section set. If that section set
  has no matching signature, the lookup searches the other set in the binary.
- `DwarfBinaryImpl` parses `.debug_names` at load time. The reader
  uses immutable section bytes, parsed abbreviations, and any offset
  relocations that LLVM supplies for the name index tables.
- `FindDebugNamesEntries()` checks that both the unit and DIE offsets
  are present and that their sum fits. Offset zero is valid. Local type units
  use `.debug_info` offsets. The reader returns foreign type signatures and
  their related CU offsets. The symbol lookup skips those entries until
  `DebugNamesEntry` can identify the owning DWO (https://fxbug.dev/583730372).
- Exact and prefix name lookups omit a variable when its indexed parent
  identifies it as function-local. If the parent record cannot be read,
  normal DIE resolution can still check the entry.
- All file names from line tables come from `ResolveLineFileName()`, so names
  from line rows, `DW_AT_decl_file`, and the file index compare equal.
- The DIE attribute, line table, location list, range list, aranges, and
  name index parsers read section bytes from `DwarfSections`.
  `dwarf_sections_llvm.cc` maps the LLVM sections to these views. The byte
  readers expect any required ELF relocations to be applied to the bytes.
  `UnitContext` reads root DIE metadata for string offsets, indexed addresses,
  location lists, range lists, compilation directories, and line table
  offsets. LLVM supplies split package contributions and skeleton links.
- Each binary caches immutable unit contexts by LLVM unit identity. A split
  unit also uses its skeleton identity when it has one. The split unit gets
  the skeleton context from the skeleton binary's cache when possible. The
  skeleton binary keeps its section bytes alive for the split binary.
- The binary shares parsed abbreviation tables by section kind, offset, and
  contribution end. The unit context and DIE table use the same abbreviations.
  Each DIE table holds the abbreviations it needs through cache eviction.
- LLVM objects, unit wrappers, unit contexts, and line tables belong to the
  binary's owning thread. Each DWO index worker owns its binary until it
  passes the binary to the module.
- Raw DIE and signature lookups can run from other threads. They use immutable
  unit lists and package abbreviation bounds computed at load time. One lock
  protects DIE table builds, eviction, and the signature maps. A table build
  can acquire the abbreviation cache lock while holding the DIE cache lock.
  Readers can share snapshots while the binary keeps the section bytes alive.
- The address-to-unit map uses `.debug_aranges` first. It uses root DIE ranges
  for units without a parsed set and clips overlaps at the start of the later
  range. The map and the symbol factory omit ranges that start at address
  zero because the linker can use zero for removed code.
- The function address map reads code ranges from concrete function and
  inline DIEs in their own units. Separate split units resolve indexed
  addresses through the skeleton unit. Later entries in DIE order win
  where ranges overlap, so a nested inline instance covers its caller.
  This also chooses the later DIE for identical ranges, as LLVM 19 does.
  LLVM 19's range split assumes nested ranges. The map also preserves the
  portions of unrelated crossing ranges outside their overlap.
- `IsStrippedSequenceStart()` is the only rule for removed code: a sequence
  that starts below `DwarfBinary::GetFirstCodeAddress()`. This matches LLDB.
  `LineTable` drops those sequences, and the file index ignores their files.
  Both use `LineSequenceValidator` to also reject decreasing addresses and
  commit only complete sequences. Equal addresses and zero-length sequences
  are permitted.
- The line table parser and decoder follow the DWARF standard and the rules in
  `line_tables.md`. Their tests check expected values. Don't add tests that
  compare zxdb's results with LLVM's: zxdb is reducing its dependency on LLVM.
- `SourceFileIndexer` workers read only immutable section data. Before
  the thread that owns the `DWARFContext` starts the workers, it
  selects each unit's line and string sections from `DwarfSections` and
  collects its `LineStringSections`. This includes the string offsets base,
  entry size, and contribution bounds from `UnitContext` for `DW_FORM_strx`.
- `DW_LNE_define_file` can add files that the line table header doesn't have.
  File lookups go through `LineTableCache`, which decodes the program when an
  index is past the header's entries.
- zxdb doesn't use LLVM's line table parser. Line tables with more than one
  operation per instruction (VLIW) aren't supported and fail to parse.
- The line table parser and decoder read untrusted data. They must check each
  length by subtraction before they add it, and their memory use must stay
  in proportion to the input. Don't reserve memory from a count in the data.
  See "Malformed input" in `line_tables.md`.
- When `.debug_names` has no readable contribution or `force_slow_index`
  is set, `ModuleSymbolsImpl` uses the full-DIE `Index`.

## Side Effects

Using `zxdb/symbols` produces these disk, cache, and IPC side effects:

- Calling `Get()` on a `LazySymbol` or `UncachedLazySymbol` decodes DWARF DIE
  records from the binary or `.dwo` file via `SymbolFactory` and caches
  `LazySymbol::symbol_`.
- Calling `Symbol::GetFullName()`, `Symbol::GetIdentifier()`, or
  `Collection::GetSpecialType()` lazily computes and caches the formatted name,
  `Identifier`, or `SpecialType` on the `Symbol`.
- The first function address lookup scans the unit's DIE table and decodes
  concrete function ranges. It caches the map even when there are no
  functions. The map stores DIE offsets and stays valid after DIE table
  eviction.
- `DwarfUnit::GetLineTable()` decodes and caches the unit's line table.
  `DwarfBinary::GetLineFileTable()` usually parses only the line table header.
  Both share the file tables in the binary's `LineTableCache`.
- `DwarfBinary::ClearDwarfCaches()` clears cached unit wrappers, file
  tables, unit contexts, DIE tables, abbreviations, and type signature maps.
  Previously returned contexts and DIE handles keep their snapshots while
  the section bytes stay alive.
- `Index::CreateIndex()` starts worker threads for the source file index when
  the binary has 256 KB or more of line tables: one, plus one for each 2 MB,
  up to six.
- `ElfSymbolRecord::GetUnmangledName()` demangles and caches the name on demand.
  Loading a module builds the raw ELF name and address indices. The first
  function-name lookup or completion indexes the source spellings of defined
  ELF functions.
- `SystemSymbols::GetModule()` loads ELF and DWARF files from disk via
  `BuildIDIndex` and builds the symbol `Index`, or calls
  `RequestDownloadFunction` when local symbols are missing.
- `ProcessSymbols::SetModules()` and `RetryLoadBuildID()` load and unload module
  symbols, update `TargetSymbols`, and invoke `ProcessSymbols::Notifications`
  callbacks.
- `DwarfExprEval::Eval()` and `SymbolDataProvider` methods issue asynchronous
  register and memory reads and writes against the target process.

## Verification

Run these commands to build and test `zxdb/symbols`:

- Build: `cmake --build build --target zxdb_symbols`
- Test: `cmake --build build --target zxdb_tests && ./build/zxdb_tests`
