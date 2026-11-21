---
name: editing-zxdb
description: >
  Workflows for editing, building, testing, and manually checking code under
  src/developer/debug (zxdb, debug_agent, debug_ipc, and shared) with the CMake
  build.
---

# Editing & Testing `zxdb` and `debug_agent` (`src/developer/debug`)

Use this skill when you edit, build, test, or manually check features across
`zxdb`, `debug_agent`, `ipc`, or `shared`. CMake is the only build. Run the
commands from the repository root.

## Configure and Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
```

Add new library sources to the subsystem's `CMakeLists.txt` and new tests to
`src/developer/debug/zxdb/Tests.cmake`.

## Host Unit Tests (`zxdb`, `ipc`, `shared`)

Run the unified host unit test binary. Set `FUCHSIA_ANALYTICS_DISABLED=1` so
the tests don't use saved analytics preferences:

```bash
FUCHSIA_ANALYTICS_DISABLED=1 ./build/zxdb_tests
```

To run specific GoogleTest suites or cases, pass `--gtest_filter`:

```bash
FUCHSIA_ANALYTICS_DISABLED=1 ./build/zxdb_tests --gtest_filter="DwarfSymbolFactory.*"
```

Two tests need optional fixtures and fail without them:
`ModuleSymbols.ResolveLineInputLocation_Await` and `MinidumpTest.Backtrace`.
CTest excludes them. See the root `README.md`.

## Agent Unit Tests (`debug_agent`)

```bash
./build/src/developer/debug/zxdb/debug_agent_tests
```

## Local Integration Tests

CTest runs all unit tests and the local debugging tests in `tests/`. The local
tests launch inferiors, so they need permission to trace child processes:

```bash
ctest --test-dir build --output-on-failure
ctest --test-dir build -R '^local_debugging$' --output-on-failure
```

## Indexing Benchmark

To measure symbol indexing, use a release build. See the root `README.md`
for the method:

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DZXDB_INDEX_BENCHMARK_BINARY="$PWD/build/zxdb"
cmake --build build-release --target zxdb_tests
FUCHSIA_ANALYTICS_DISABLED=1 ./build-release/zxdb_tests \
  --gtest_filter=Index.BenchmarkIndexing
```

## Manual Feature Verification

Run the debugger with the integrated local agent and launch a program that has
debug information:

```bash
./build/zxdb --local
```

To check DAP features, start the DAP server with the local agent:

```bash
./build/zxdb --local --enable-debug-adapter --debug-adapter-port=15678
```
