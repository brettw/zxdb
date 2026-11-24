# Finding a breakpoint by function name

A backtrace can show `a::b::f(int)` even when the compiler recorded only `f`
in DWARF. For example, GCC with `-g1` can omit both enclosing namespaces
and keep the linkage name `_ZN1a1b1fEi`. Demangling that name gives zxdb
the missing spelling.

## Which breakpoint spelling should I use?

Use `break a::b::f` for the definition. Each overload gets a breakpoint.
You can omit enclosing namespaces. Include each enclosing class that DWARF
identifies. When zxdb knows only the spelling, both `break b::f` and
`break f` find the recovered name. A leading `::` requires the full path.
For example, `break ::b::f` searches for `b` at global scope.

| Available information | How zxdb finds the function |
| --- | --- |
| DWARF contains the enclosing namespaces and the function | Follows the source declarations. |
| DWARF supplies a linkage name at unit scope | Demangles the name and indexes it as another spelling for the same function. |
| A DWARF name contains the whole path | Splits the name into components. The literal spelling, such as `$(a::b::f)`, still works. |
| Only line tables and inline copies remain | If Clang omits both the enclosing scopes and the linkage name, use the short function name. |
| Only ELF function symbols are available | Uses the defined function's ELF address. zxdb recognizes plain C names, Itanium C++ names, and Rust v0 names. |

Use `$elf(_ZN1a1b1fEi)` to select a raw ELF name. Use `$plt(...)` to stop
at a module's import stub. Ordinary function breakpoints skip those stubs.

Completion escapes names that need a literal component. For example, a Rust
method named `new` can be written as `break S::$(new)`, and a C++ conversion
operator as `break X::$(operator int)`.

## How lookup works

The function-name index groups candidates by their last component.
For `a::Box<b::T>::f`, a search for `f` finds the candidate. A search for
`Box<b::T>::f` also checks the enclosing component. The debugger supplies a parser
from `zxdb_expr` when it constructs the symbol system. The parser keeps the
template argument `b::T` inside `Box<b::T>` and returns the components
to the index.

Each entry refers to a DWARF definition or an ELF function in one module.
Breakpoint resolution uses that reference directly. When DWARF and ELF
match at the same entry address, zxdb uses DWARF for source lines and
prologue skipping. Completion presents each distinct spelling once.
On the first function-name query, zxdb indexes ordinary C++ ELF functions by
base name. It parses their full names when a lookup or completion matches
the base name.

A name alone cannot establish whether `b` is a namespace or a class.
A namespace function and a static member function can both have the
linkage name `_ZN1a1b1fEi`. Function lookup can use the spelling in both
cases. Expressions such as `print &a::b::f` use the source declarations
for name lookup.
