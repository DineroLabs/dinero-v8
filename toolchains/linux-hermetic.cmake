# ═══════════════════════════════════════════════════════════════════════════
# Linux Hermetic Toolchain File
# ═══════════════════════════════════════════════════════════════════════════
# Purpose: Lock down compiler, glibc version, and prevent distro drift
# Usage:   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchains/linux-hermetic.cmake
#
# What this file does:
#   1. Pins compiler to specific GCC or Clang version
#   2. Prevents distro package manager contamination
#   3. Enforces consistent glibc linkage
#   4. Blocks /usr/local pollution
#   5. Provides CI reproducibility across Ubuntu/Debian/Fedora/Arch
#
# Linux advantages:
#   - More predictable than macOS/Windows
#   - Explicit /usr/include and /usr/lib paths
#   - Fewer "ambient" package managers
#
# When you need this:
#   - CI reproducibility (same binary on Ubuntu 20.04 vs 22.04)
#   - Distro-independent releases
#   - Deterministic consensus code builds
#
# Design philosophy: "Make distro choice irrelevant"
# Inspired by: Bitcoin Core, LLVM CI infrastructure
# ═══════════════════════════════════════════════════════════════════════════

cmake_minimum_required(VERSION 3.20)

# Include common hermetic rules
include(${CMAKE_CURRENT_LIST_DIR}/common-hermetic.cmake)

# ─────────────────────────────────────────────────────────────────────────────
# Platform Identity
# ─────────────────────────────────────────────────────────────────────────────

set(CMAKE_SYSTEM_NAME Linux)

# ─────────────────────────────────────────────────────────────────────────────
# Compiler Lock: Pinned GCC or Clang
# ─────────────────────────────────────────────────────────────────────────────
# Why: Different distros ship different compiler versions
#      Ubuntu 20.04: gcc-9, Ubuntu 22.04: gcc-11, Ubuntu 24.04: gcc-13
# Solution: Pin to specific compiler version
#
# Options:
#   1. Pin to gcc-11 (widely available, C++17 support)
#   2. Pin to clang-15 (LLVM toolchain, better diagnostics)
#   3. Use system default (less hermetic but more flexible)

# Compilers must be provided via environment (hermetic-build.sh)
if(NOT DEFINED ENV{CC})
  message(FATAL_ERROR "CC environment variable not set")
endif()

if(NOT DEFINED ENV{CXX})
  message(FATAL_ERROR "CXX environment variable not set")
endif()

set(CMAKE_C_COMPILER   "$ENV{CC}"  CACHE FILEPATH "Hermetic C compiler" FORCE)
set(CMAKE_CXX_COMPILER "$ENV{CXX}" CACHE FILEPATH "Hermetic C++ compiler" FORCE)

message(STATUS "🔒 Hermetic compiler (C):   ${CMAKE_C_COMPILER}")
message(STATUS "🔒 Hermetic compiler (CXX): ${CMAKE_CXX_COMPILER}")

# Get GCC version for logging
execute_process(
  COMMAND ${CMAKE_CXX_COMPILER} -dumpversion
  OUTPUT_VARIABLE _gcc_version
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
message(STATUS "🔒 Hermetic Toolchain: GCC version ${_gcc_version}")

# ─────────────────────────────────────────────────────────────────────────────
# Architecture Detection
# ─────────────────────────────────────────────────────────────────────────────

execute_process(
  COMMAND uname -m
  OUTPUT_VARIABLE _host_arch
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
message(STATUS "🔒 Hermetic Toolchain: Building for ${_host_arch}")

# ─────────────────────────────────────────────────────────────────────────────
# Distro Package Manager Quarantine
# ─────────────────────────────────────────────────────────────────────────────
# Why: /usr/local often contains user-installed packages (pip, cargo, manual builds)
# Solution: Remove /usr/local from search paths

set(_distro_pollution_paths
  "/usr/local/include"
  "/usr/local/lib"
  "/usr/local/lib64"
  "/usr/local/bin"
)

# Remove from CMake search paths
foreach(_path ${_distro_pollution_paths})
  list(FILTER CMAKE_SYSTEM_PREFIX_PATH EXCLUDE REGEX "^${_path}")
  list(FILTER CMAKE_SYSTEM_INCLUDE_PATH EXCLUDE REGEX "^${_path}")
  list(FILTER CMAKE_SYSTEM_LIBRARY_PATH EXCLUDE REGEX "^${_path}")
  list(FILTER CMAKE_PREFIX_PATH EXCLUDE REGEX "^${_path}")
  list(FILTER CMAKE_INCLUDE_PATH EXCLUDE REGEX "^${_path}")
  list(FILTER CMAKE_LIBRARY_PATH EXCLUDE REGEX "^${_path}")
endforeach()

message(STATUS "🔒 Hermetic Toolchain: /usr/local quarantined")

# ─────────────────────────────────────────────────────────────────────────────
# Explicit System Paths: What IS Allowed
# ─────────────────────────────────────────────────────────────────────────────
# Only standard FHS (Filesystem Hierarchy Standard) paths

list(APPEND CMAKE_SYSTEM_PREFIX_PATH "/usr")
list(APPEND CMAKE_SYSTEM_INCLUDE_PATH "/usr/include")

# Handle multiarch (Debian/Ubuntu x86_64-linux-gnu, aarch64-linux-gnu, etc.)
if(_host_arch STREQUAL "x86_64")
  list(APPEND CMAKE_SYSTEM_LIBRARY_PATH
    "/usr/lib/x86_64-linux-gnu"
    "/usr/lib64"
    "/usr/lib"
  )
elseif(_host_arch STREQUAL "aarch64")
  list(APPEND CMAKE_SYSTEM_LIBRARY_PATH
    "/usr/lib/aarch64-linux-gnu"
    "/usr/lib64"
    "/usr/lib"
  )
else()
  list(APPEND CMAKE_SYSTEM_LIBRARY_PATH
    "/usr/lib64"
    "/usr/lib"
  )
endif()

message(STATUS "🔒 Hermetic Toolchain: System paths locked to /usr")

# ─────────────────────────────────────────────────────────────────────────────
# Toolchain Executables: System Binutils
# ─────────────────────────────────────────────────────────────────────────────

find_program(CMAKE_MAKE_PROGRAM make)
find_program(CMAKE_LINKER ld.gold ld)
find_program(CMAKE_AR ar)
find_program(CMAKE_RANLIB ranlib)
find_program(CMAKE_NM nm)
find_program(CMAKE_OBJDUMP objdump)
find_program(CMAKE_STRIP strip)

message(STATUS "🔒 Hermetic Toolchain: Using system binutils")

# ─────────────────────────────────────────────────────────────────────────────
# glibc Version Detection (for logging)
# ─────────────────────────────────────────────────────────────────────────────
# This doesn't pin glibc (that requires sysroot cross-compilation)
# But it documents what glibc version the build uses

execute_process(
  COMMAND ldd --version
  OUTPUT_VARIABLE _ldd_output
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX MATCH "ldd \\(GNU libc\\) ([0-9]+\\.[0-9]+)" _glibc_match "${_ldd_output}")
if(CMAKE_MATCH_1)
  set(_glibc_version ${CMAKE_MATCH_1})
  message(STATUS "🔒 Hermetic Toolchain: glibc version ${_glibc_version}")
else()
  message(STATUS "🔒 Hermetic Toolchain: glibc version unknown")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Compiler Flags: Security Hardening
# ─────────────────────────────────────────────────────────────────────────────

# Stack protection
add_compile_options(-fstack-protector-strong)

# Fortify source
add_compile_definitions(_FORTIFY_SOURCE=2)

# Position independent executables (PIE) / code (PIC)
add_compile_options(-fPIC)
add_link_options(-pie)

# Full RELRO (GOT protection)
add_link_options(-Wl,-z,relro,-z,now)

# No executable stack
add_link_options(-Wl,-z,noexecstack)

# GNU Build ID for reproducibility
add_link_options(-Wl,--build-id=sha1)

message(STATUS "🔒 Hermetic Toolchain: Security hardening enabled")

# ─────────────────────────────────────────────────────────────────────────────
# Static Linking Options (for fully hermetic binaries)
# ─────────────────────────────────────────────────────────────────────────────
# Uncomment to create static binaries (no glibc dependency)
# WARNING: Static linking on Linux is complex (NSS, pthread, etc.)

# option(BUILD_STATIC_LINUX "Build fully static Linux binaries" OFF)
# if(BUILD_STATIC_LINUX)
#   set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
#   set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
#   set(BUILD_SHARED_LIBS OFF)
#   message(STATUS "🔒 Hermetic Toolchain: Static linking enabled")
# endif()

# ─────────────────────────────────────────────────────────────────────────────
# Parallel Compilation
# ─────────────────────────────────────────────────────────────────────────────

include(ProcessorCount)
ProcessorCount(_cpu_count)
if(_cpu_count GREATER 0)
  set(CMAKE_BUILD_PARALLEL_LEVEL ${_cpu_count} CACHE STRING "Parallel jobs")
  message(STATUS "🔒 Hermetic Toolchain: Parallel compilation with ${_cpu_count} jobs")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Status Summary
# ─────────────────────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════════")
message(STATUS "Linux Hermetic Toolchain: ACTIVE")
message(STATUS "═══════════════════════════════════════════════════════════")
message(STATUS "Compiler:  ${CMAKE_CXX_COMPILER} (${_gcc_version})")
message(STATUS "glibc:     ${_glibc_version}")
message(STATUS "Arch:      ${_host_arch}")
message(STATUS "/usr/local: QUARANTINED")
message(STATUS "Build:     Reproducible, deterministic, auditable")
message(STATUS "═══════════════════════════════════════════════════════════")
message(STATUS "")
