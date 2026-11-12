cmake_minimum_required(VERSION 3.24)

# Fuchsia sources use Clang builtins. Honor explicit compiler selections, but
# choose Clang for a fresh build rather than CMake's usual GCC default.
if(NOT DEFINED CMAKE_CXX_COMPILER AND "$ENV{CXX}" STREQUAL "")
  find_program(CMAKE_CXX_COMPILER NAMES clang++ REQUIRED)
endif()
project(zxdb LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The standalone build currently supports Linux only")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Upstream host syslog requires Clang. Configure with -DCMAKE_CXX_COMPILER=clang++")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(re2 CONFIG QUIET)
if(NOT TARGET re2::re2)
  # Some distributions ship RE2 without its upstream CMake package.
  find_path(RE2_INCLUDE_DIR re2/re2.h REQUIRED)
  find_library(RE2_LIBRARY NAMES re2 REQUIRED)
  add_library(re2::re2 UNKNOWN IMPORTED)
  set_target_properties(re2::re2 PROPERTIES
    IMPORTED_LOCATION "${RE2_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${RE2_INCLUDE_DIR}"
  )
endif()
include(CTest)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "The integrated Linux debug agent currently requires x86-64")
endif()
find_package(LLVM REQUIRED CONFIG)
if(LLVM_VERSION_MAJOR LESS 19)
  message(FATAL_ERROR "LLVM 19 or newer is required")
endif()
add_library(llvm_host INTERFACE)
target_include_directories(llvm_host SYSTEM INTERFACE ${LLVM_INCLUDE_DIRS})
target_link_libraries(llvm_host INTERFACE LLVM)
target_compile_options(llvm_host INTERFACE -fno-rtti)
find_package(CURL REQUIRED)
find_library(ZSTD_LIBRARY NAMES zstd REQUIRED)
add_library(zstd UNKNOWN IMPORTED)
set_target_properties(zstd PROPERTIES IMPORTED_LOCATION "${ZSTD_LIBRARY}")
find_path(RAPIDJSON_INCLUDE_DIR rapidjson/document.h REQUIRED)
add_library(rapidjson INTERFACE)
target_include_directories(rapidjson SYSTEM INTERFACE "${RAPIDJSON_INCLUDE_DIR}")
target_compile_definitions(rapidjson INTERFACE RAPIDJSON_HAS_STDSTRING=1)
find_library(DOUBLE_CONVERSION_LIBRARY NAMES double-conversion REQUIRED)
add_library(double_conversion UNKNOWN IMPORTED)
set_target_properties(double_conversion PROPERTIES IMPORTED_LOCATION "${DOUBLE_CONVERSION_LIBRARY}")
# Upstream uses gtest_prod.h in production headers; tests remain optional.
find_path(GTEST_INCLUDE_DIR gtest/gtest_prod.h REQUIRED)
add_library(gtest_headers INTERFACE)
target_include_directories(gtest_headers SYSTEM INTERFACE "${GTEST_INCLUDE_DIR}")

# Preserve source-root RapidJSON includes in imported analytics code.
file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/include/third_party/rapidjson/include")
file(CREATE_LINK "${RAPIDJSON_INCLUDE_DIR}/rapidjson"
  "${PROJECT_BINARY_DIR}/include/third_party/rapidjson/include/rapidjson" SYMBOLIC)
target_include_directories(rapidjson INTERFACE "${PROJECT_BINARY_DIR}/include")

add_subdirectory(src/lib)
add_subdirectory(src/developer/debug/shared)
add_subdirectory(third_party/cppdap)
find_package(ZLIB REQUIRED)
add_subdirectory(third_party/mini_chromium/src)
add_subdirectory(third_party/crashpad/src)
add_subdirectory(src/developer/debug/ipc)
add_subdirectory(src/developer/debug/debug_agent)
add_subdirectory(src/developer/debug/zxdb)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
