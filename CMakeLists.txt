cmake_minimum_required(VERSION 3.24)
project(kestrel VERSION 0.1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Kestrel: low-latency trading engine (Version 1: software engine & platform)
#
# Build presets live in CMakePresets.json:
#   debug / release / bench / asan / ubsan / tsan
#
# Optional networking backends are opt-in and default OFF so that the replay
# build never requires hardware or extra dependencies:
#   KESTREL_ENABLE_AF_XDP  - AF_XDP copy-mode backend (Linux, kernel 5.4+)
#   KESTREL_ENABLE_DPDK    - DPDK backend (requires externally built DPDK)
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(KESTREL_BUILD_TESTS  "Build unit/property/integration/golden tests" ON)
option(KESTREL_BUILD_BENCH  "Build benchmarks and latency harnesses" ON)
option(KESTREL_BUILD_TOOLS  "Build host tools (journal dump, workload gen)" ON)
option(KESTREL_ENABLE_AF_XDP "Build AF_XDP backend" OFF)
option(KESTREL_ENABLE_DPDK   "Build DPDK backend" OFF)
option(KESTREL_ENABLE_LTO    "Enable LTO for Release/bench builds" OFF)
option(KESTREL_WERROR        "Treat warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Global compile flags
# ---------------------------------------------------------------------------
add_library(kestrel_flags INTERFACE)
target_compile_options(kestrel_flags INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
  -fno-omit-frame-pointer)
if(KESTREL_WERROR)
  target_compile_options(kestrel_flags INTERFACE -Werror)
endif()

set(KESTREL_SANITIZE "" CACHE STRING "Sanitizers: asan|ubsan|tsan|'asan,ubsan'")
if(KESTREL_SANITIZE)
  target_compile_options(kestrel_flags INTERFACE
    -fsanitize=${KESTREL_SANITIZE} -fno-sanitize-recover=all)
  target_link_options(kestrel_flags INTERFACE -fsanitize=${KESTREL_SANITIZE})
endif()

find_package(Threads REQUIRED)
target_link_libraries(kestrel_flags INTERFACE Threads::Threads)

# ---------------------------------------------------------------------------
# Core library (header-only hot path; compiled pieces in src/)
# ---------------------------------------------------------------------------
add_library(kestrel_core INTERFACE)
target_include_directories(kestrel_core INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kestrel_core INTERFACE kestrel_flags)

# Compiled engine sources (journal writer/reader, replay backend, simulator) are
# added as a `kestrel_engine` library when Milestone B begins — no empty targets.

# LTO opt-in
if(KESTREL_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo OUTPUT _ipo_msg)
  if(NOT _ipo)
    message(WARNING "LTO requested but not supported: ${_ipo_msg}")
  endif()
endif()

# ---------------------------------------------------------------------------
# Dependencies (pinned, fetched over network, verified at configure time)
# ---------------------------------------------------------------------------
include(FetchContent)

if(KESTREL_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2      # verified: builds under GCC 14.2, C++23
    GIT_SHALLOW    TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

if(KESTREL_BUILD_BENCH)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.0       # verified: builds under GCC 14.2, C++23
    GIT_SHALLOW    TRUE)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(KESTREL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(KESTREL_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(KESTREL_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

message(STATUS "kestrel ${PROJECT_VERSION} | build=${CMAKE_BUILD_TYPE} "
  "sanitize=${KESTREL_SANITIZE} tests=${KESTREL_BUILD_TESTS} "
  "bench=${KESTREL_BUILD_BENCH} afxdp=${KESTREL_ENABLE_AF_XDP} "
  "dpdk=${KESTREL_ENABLE_DPDK}")
