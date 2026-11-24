# Standalone zxdb

This repository builds the Fuchsia debugger as a standalone Linux x86-64
executable with an integrated local debug agent. It includes the console,
Debug Adapter Protocol (DAP) server, ELF/DWARF symbols, expression evaluation,
symbol downloads, and minidump reading.

## System packages

On Debian 13, install all required packages with:

```sh
sudo apt-get update
sudo apt-get install clang clang-format g++ cmake ninja-build llvm-19-dev \
  libre2-dev libcurl4-openssl-dev libzstd-dev rapidjson-dev \
  libdouble-conversion-dev zlib1g-dev libgtest-dev libgmock-dev
```

## Build and run

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure
./build/zxdb --local
```

CMake selects Clang by default because upstream host syslog uses Clang
builtins. To choose an LLVM installation, pass
`-DLLVM_DIR=/usr/lib/llvm-19/lib/cmake/llvm`. Use a fresh build directory when
changing compilers.

Inside the console, launch a local program with an absolute path:

```text
break main
run /absolute/path/to/program
frame
print some_variable
next
continue
quit
```

Compile the program with debug information (`-g`) for source-level debugging.
Use `--symbol-path=/path/to/binary` if its symbols cannot be found automatically.
Linux ptrace permissions apply to local launches and attachments.

To debug a program that is already running, use `attach <pid>` or
`attach <name>`. A name matches part of the command name or the executable
path. With `attach --exact`, the name must equal one of these or its base name.

The DAP server can use the integrated agent too:

```sh
./build/zxdb --local --enable-debug-adapter --debug-adapter-port=15678
```

Fuchsia analytics are disabled by default. Use `zxdb --analytics=enable` to
opt in or `zxdb --analytics-show` to check the shared Fuchsia preference. To
disable analytics for one run, set `FUCHSIA_ANALYTICS_DISABLED=1`.
Tests use this environment setting.

Build just the executable with `cmake --build build --target zxdb -j 4`.
Configure with `-DBUILD_TESTING=OFF` to omit test executables and fixtures.
Install the executable with `cmake --install build --prefix /your/install/path`.
System shared libraries remain runtime dependencies.

## Tests

The `local_debugging` integration test uses the built-in script runner to
launch a local inferior, stop at a breakpoint, inspect variables, step, and
resume to process exit. It requires permission to trace child processes. Run
it separately with:

```sh
ctest --test-dir build -R '^local_debugging$' --output-on-failure
```

Two upstream tests require separately distributed CIPD fixtures that are not
in this repository. They are excluded from test discovery unless supplied:

- `ModuleSymbols.ResolveLineInputLocation_Await`: configure with
  `-DZXDB_RUST_TEST_BINARY=/path/to/rust_symbol_test`.
- `MinidumpTest.Backtrace`: configure with
  `-DZXDB_SAMPLE_CORE_DIR=/path/to/sample_core` (containing `core/core.dmp`
  and its companion symbol files).

To time symbol indexing with optimized code against the existing debug executable,
build a separate release configuration and enable the optional benchmark:

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=/usr/lib/llvm-19/lib/cmake/llvm \
  -DZXDB_INDEX_BENCHMARK_BINARY="$PWD/build/zxdb"
cmake --build build-release -j 2
FUCHSIA_ANALYTICS_DISABLED=1 ./build-release/zxdb_tests \
  --gtest_filter=Index.BenchmarkIndexing
```

The benchmark reports file-loading and full-DIE indexing times separately in
microseconds. It indexes only the selected binary, without its shared libraries.
The index time includes the source file index, which runs on worker threads.
The benchmark also reports the source file work alone on one thread.
The benchmark is omitted when `ZXDB_INDEX_BENCHMARK_BINARY` is unset. When it is
set, `LineProgramBinary.BenchmarkBinary` also decodes every line table of that
binary and checks that the file scan and the source file index agree with the
rows.
For comparisons, use the same input binary, finish compilation first, and run
both versions on the same CPU with warm filesystem caches. For example:

```sh
FUCHSIA_ANALYTICS_DISABLED=1 taskset -c 0 ./build-release/zxdb_tests \
  --gtest_filter=Index.BenchmarkIndexing --gtest_repeat=10
```

Core selection matters on hybrid CPUs. Keep a baseline test executable and
alternate runs of the baseline and changed versions. Name demangling is deferred
until a symbol's display name is requested, so that cost is outside the loading
measurement. The benchmark also excludes module teardown and later symbol queries.

## Formatting

C and C++ code uses the Fuchsia style in `.clang-format`. Format it with
clang-format 19, because other versions format some code differently. Debian 13
provides it in the `clang-format` package.

```sh
cmake --build build --target format        # Reformat the tracked files.
cmake --build build --target check-format  # Fail if any file needs formatting.
```

`scripts/clang-format` does the same without a build directory. Use it with
`--check` to check only. Code under `third_party/` keeps its upstream style, and
the line table fixtures in `src/developer/debug/zxdb/symbols/test_data/line_table/`
keep their source lines for the tests. A `.clang-format` file in each of these
directories turns formatting off.

## Imported dependencies and source changes

Fuchsia support libraries live in separate directories under `src/lib/`.
Each library has an `UPSTREAM.md` with its original path and revision, and a
copy of its license. Only needed subsets are imported. Third-party cppdap,
Crashpad, and mini_chromium subsets live under `third_party/`, with their own
licenses and provenance.

The debugger remains at `src/developer/debug/`, matching its original include
paths. CMake creates build-directory symlinks for SDK-style syslog and UTF
includes, and for source-root RapidJSON includes. Builds use only this
repository and installed packages; they do not read a neighboring Fuchsia
checkout or fetch dependencies from the network.

CMake is the only build. Its source lists started from the upstream Linux
GN and Bazel selections, and this repository doesn't have GN or Bazel files.
The
`debug::shared` target preserves `alwayslink` for shared and logging with
whole-archive linking. Fuchsia-only launchers, FIDL runtimes, packaging, and
`ffx` integration are outside this host build.

Source adaptations include missing standard-header includes, canonical
library include paths, LLVM API compatibility, and host library compatibility
for range formatting and RapidJSON keys. Tests also exposed two runtime
portability issues: filesystem timestamps now use the system clock epoch,
and callback result storage retains stable addresses during synchronous
completion. A regression test covers the callback case. LLVM 19's ScopedPrinter header needs
the DWARF reader and its test helper compiled as C++20; the remaining build
uses C++23.
