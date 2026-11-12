cmake_minimum_required(VERSION 3.24)

# Fuchsia sources use Clang builtins. Honor explicit compiler selections, but
# choose Clang for a fresh build rather than CMake's usual GCC default.
if(NOT DEFINED CMAKE_CXX_COMPILER AND "$ENV{CXX}" STREQUAL "")
  find_program(CMAKE_CXX_COMPILER NAMES clang++ REQUIRED)
endif()
project(zxdb LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The standalone build currently supports Linux only")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Upstream host syslog requires Clang. Configure with -DCMAKE_CXX_COMPILER=clang++")
endif()

set(CMAKE_CXX_STANDARD 20)
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

add_subdirectory(src/lib)
add_subdirectory(src/developer/debug/shared)
