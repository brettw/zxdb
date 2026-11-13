# Standalone zxdb

This repository builds the Fuchsia debugger as a standalone Linux x86-64
executable with an integrated local debug agent. It includes the console,
Debug Adapter Protocol (DAP) server, ELF/DWARF symbols, expression evaluation,
symbol downloads, and minidump reading.

## System packages

The build uses CMake 3.24 or newer, Ninja, Clang, a C++23 standard library,
LLVM development files, RE2, curl, zstd, RapidJSON, double-conversion, and zlib.
GoogleTest and GoogleMock are used by the host tests. Production headers also
use GoogleTest's `gtest_prod.h`.

On Debian 13, install all required packages with:

```sh
sudo apt-get update
sudo apt-get install clang g++ cmake ninja-build llvm-19-dev \
  libre2-dev libcurl4-openssl-dev libzstd-dev rapidjson-dev \
  libdouble-conversion-dev zlib1g-dev libgtest-dev libgmock-dev
```

The port is tested with Clang 19, LLVM 19, and libstdc++ 14. Older distribution
releases may need newer compiler and standard library packages. LLVM and zxdb
must use compatible C++ standard libraries.

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

The DAP server can use the integrated agent too:

```sh
./build/zxdb --local --enable-debug-adapter --debug-adapter-port=15678
```

The upstream analytics implementation is retained. To disable analytics for a
run without changing saved preferences, set `FUCHSIA_ANALYTICS_DISABLED=1`.
Tests use this environment setting.

Build just the executable with `cmake --build build --target zxdb -j 4`.
Configure with `-DBUILD_TESTING=OFF` to omit test executables and fixtures.
Install the executable with `cmake --install build --prefix /your/install/path`.
System shared libraries remain runtime dependencies.

## Tests

The build includes shared, IPC, debugger frontend, DAP, and Linux agent host
unit tests. CMake builds the C++ symbol fixture and copies checked-in ELF,
split-DWARF, symbol-index, and minidump fixtures into the build directory.

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

CMake source lists follow the upstream Linux GN/Bazel selections. The
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
uses C++23. Upstream GN and Bazel files are unchanged.

## Linux debugging behavior

The local agent supports native x86-64 Linux programs. It seizes each thread
before using ptrace, including all existing threads at attach. Launched programs
wait behind a pipe until attachment is complete. Pause uses `PTRACE_INTERRUPT`;
it does not inject SIGSTOP. Exit notifications follow final kernel reaping.

Ordinary fork and clone-created processes are followed alongside their parent.
A vfork child runs with the parent's software breakpoints temporarily removed
and the parent threads stopped. The child becomes a separate debugged process
at exec. Non-thread `CLONE_VM` processes are unsupported: both tasks remain
stopped with an error, and detach restores the parent's instructions before
releasing them. There is no configurable follow-parent/follow-child policy.

SIGCHLD, SIGCONT, and SIGWINCH pass through automatically. Other delivered
signals stop for inspection; use `continue --forward` to deliver the signal,
or `continue` to suppress it. Linux signal numbers, codes, fault addresses,
and sender PIDs are available in exception information. Group stops preserve
job-control behavior until SIGCONT. General registers can be read and written;
floating-point/vector registers and hardware breakpoints/watchpoints remain
unsupported. Hardware slot counts are zero.

Launch preserves the caller's working directory, environment, signal mask, and
inherited SIGPIPE disposition. Supply an executable path; there is no PATH
search. Stdin is `/dev/null`, stdout and stderr use pipes, and there is no
inferior PTY or terminal job-control integration. Linux minidump export and
Fuchsia component/limbo operations are unsupported. Existing-process filters
can find processes; observing future unrelated processes is not implemented.
These are platform support limits, not pending audit fixes.

Normal quit closes the agent connection, restores software breakpoints, and
detaches before reaping the agent. A failed restoration leaves the process
stopped and reports an error. If teardown cannot restore a process, it kills
that process instead of releasing patched instructions. Unexpected tracer death
kills traced tasks through `PTRACE_O_EXITKILL`. Use `--debug-mode` for both
frontend and local-agent logs.

IPC version 79 adds Linux signal metadata and an exec module-reset flag. Older
protocol versions retain their wire layouts; full Linux exec reset requires a
version 79 frontend. Module lookup uses the inferior's mapped loader or root
namespace and falls back to mapped ELF images when loader symbols are absent.
