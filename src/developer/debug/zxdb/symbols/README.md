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
  `CompileUnit`. `DwarfBinaryImpl` queries both normal compilation units
  (`getNormalUnitsVector()`) and split-DWARF DWO units (`getDWOUnitsVector()`).
- When resolving a DWARF type signature whose target `DwarfDieRef` is invalid
  (`!sig_ref.is_valid()`), return an empty `fxl::RefPtr<Symbol>()` instead of
  crashing with `FX_CHECK`.
- In `FindDebugNamesEntries()`, compile unit and DIE unit offsets are validated
  with `has_value()` because section and unit offset 0 is a valid DWARF offset.
  Never use 0 as an invalid sentinel.
- Local type units in `.debug_names` use `.debug_info` offsets. Foreign type
  unit entries return `std::nullopt` until `zxdb` supports split DWARF
  (https://fxbug.dev/583730372).
- All file names from line tables come from `ResolveLineFileName()`, so names
  from line rows, `DW_AT_decl_file`, and the file index compare equal.
- `IsStrippedSequenceStart()` is the only rule for removed code: a sequence
  that starts below `DwarfBinary::GetFirstCodeAddress()`. This matches LLDB.
  `LineTable` drops those sequences, and the file index ignores their files.
  Both use `LineSequenceValidator` to also reject decreasing addresses and
  commit only complete sequences. Equal addresses and zero-length sequences
  are permitted.
- The line table parser and decoder follow the DWARF standard and the rules in
  `line_tables.md`. Their tests check expected values. Don't add tests that
  compare zxdb's results with LLVM's: zxdb is reducing its dependency on LLVM.
- `SourceFileIndexer` workers read only immutable section data. They must not
  call LLVM APIs that change the `DWARFContext` or a `DWARFUnit` (for example
  `getUnitDIE()` or `getLineTableForUnit()`). The thread that owns the context
  collects each unit's `LineStringSections`, including its string offsets
  base for `DW_FORM_strx`, before the workers start.
- `DW_LNE_define_file` can add files that the line table header doesn't have.
  File lookups go through `LineTableCache`, which decodes the program when an
  index is past the header's entries.
- zxdb doesn't use LLVM's line table parser. Line tables with more than one
  operation per instruction (VLIW) aren't supported and fail to parse.
- The line table parser and decoder read untrusted data. They must check each
  length by subtraction before they add it, and their memory use must stay
  in proportion to the input. Don't reserve memory from a count in the data.
  See "Malformed input" in `line_tables.md`.
- Index fallback: when `.debug_names` is absent or empty, or when
  `force_slow_index` is set, `ModuleSymbolsImpl` falls back to the full-DIE
  `Index`.

## Side Effects

Using `zxdb/symbols` produces these disk, cache, and IPC side effects:

- Calling `Get()` on a `LazySymbol` or `UncachedLazySymbol` decodes DWARF DIE
  records from the binary or `.dwo` file via `SymbolFactory` and caches
  `LazySymbol::symbol_`.
- Calling `Symbol::GetFullName()`, `Symbol::GetIdentifier()`, or
  `Collection::GetSpecialType()` lazily computes and caches the formatted name,
  `Identifier`, or `SpecialType` on the `Symbol`.
- `DwarfUnit::GetLineTable()` decodes and caches the unit's line table.
  `DwarfBinary::GetLineFileTable()` usually parses only the line table header.
  Both share the file tables in the binary's `LineTableCache`.
  `DwarfBinary::ClearLLVMCache()` clears the cache.
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
