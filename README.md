# Standalone zxdb

This repository extracts the Fuchsia debugger into a standalone project.
The first supported target is the Linux `shared` library and its host tests.

## Build on Linux

Use CMake 3.24 or newer, Ninja, Clang with C++20 standard library headers,
RE2 development files, and GoogleTest development files for tests.
On Debian or Ubuntu, install these with:

```sh
sudo apt-get install clang g++ cmake ninja-build libre2-dev libgtest-dev
```

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake selects Clang by default. The upstream host syslog implementation uses
Clang builtins. An explicit compiler can be selected with
`-DCMAKE_CXX_COMPILER=clang++`. Use a fresh build directory when changing
compilers.

Build just the library with `cmake --build build --target debug_shared`.
Configure with `-DBUILD_TESTING=OFF` to omit GoogleTest and test targets.
CMake consumers can link to `debug::shared`; its dependencies and public
include directories are propagated by the target.

## Imported dependencies

Fuchsia support libraries live in separate directories under `src/lib/`.
Each library has an `UPSTREAM.md` with its original path and revision, and a
copy of the upstream license. Only the needed library subsets are imported;
their C++ source files retain their upstream contents.

The debugger remains at `src/developer/debug/`, matching its original include
paths. CMake creates build-directory symlinks for SDK-style `lib/syslog/...`
and `lib/utf-utils/...` includes. Builds use only this repository and installed
system dependencies, without reading the original Fuchsia checkout or
fetching dependencies from the network.

The `debug_shared` source list follows the Linux `shared` target in the
upstream `BUILD.bazel`. The `debug::shared` target also preserves the upstream
`alwayslink` behavior for shared and logging with whole-archive linking.
Host syslog replaces the Fuchsia logging backend by
source selection. Zircon imports provide host-compatible types and macros;
the build does not link Zircon system calls or FIDL runtimes. The upstream
containers dependency is omitted because these sources do not use it.

The debugger source changes needed for this build are missing standard-header
includes in `file_line_function.h`, `register_value.h`, `serialization.h`, and
`mock_memory.cc`. These make declarations available with the host standard
library without relying on indirect includes.
