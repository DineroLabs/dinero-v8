# ═══════════════════════════════════════════════════════════════════════════
# Windows MSVC Hermetic Toolchain File
# ═══════════════════════════════════════════════════════════════════════════
# Purpose: Lock down MSVC compiler, Windows SDK, and kill ambient discovery
# Usage:   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
#                -DCMAKE_TOOLCHAIN_FILE=toolchains/windows-msvc-hermetic.cmake
#
# What this file does:
#   1. Enforces MSVC compiler (no clang-cl, MinGW, or other compilers)
#   2. Pins Windows SDK version (no auto-selection)
#   3. Locks CRT linkage (static vs dynamic must be explicit)
#   4. Kills vcpkg auto-integration (no ambient package discovery)
#   5. Disables registry-based package resolution
#   6. Blocks PATH-based include/library discovery
#   7. Enforces Visual Studio generator (no Ninja with MSVC)
#
# Why Windows needs this MOST:
#   - Multiple MSVC versions installed side-by-side
#   - Windows SDK auto-selection is non-deterministic
#   - vcpkg auto-integrates silently
#   - Registry-based discovery is invisible
#   - PATH pollution is worse than macOS Homebrew
#
# Design philosophy: "Windows is radioactive by default"
# Inspired by: Bitcoin Core, LLVM, Chromium Windows build systems
# ═══════════════════════════════════════════════════════════════════════════

cmake_minimum_required(VERSION 3.21)

# Include common hermetic rules
include(${CMAKE_CURRENT_LIST_DIR}/common-hermetic.cmake)

# ─────────────────────────────────────────────────────────────────────────────
# Platform Identity
# ─────────────────────────────────────────────────────────────────────────────

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# ─────────────────────────────────────────────────────────────────────────────
# Compiler Lock: MSVC Only
# ─────────────────────────────────────────────────────────────────────────────
# Why: Prevents clang-cl, MinGW, or other Windows compilers from being used
# Solution: Explicitly require MSVC and set compiler to cl.exe

set(CMAKE_C_COMPILER cl.exe CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_CXX_COMPILER cl.exe CACHE FILEPATH "C++ compiler" FORCE)

# Enforce MSVC (checked after project() call)
if(CMAKE_CXX_COMPILER_ID AND NOT MSVC)
  message(FATAL_ERROR "Hermetic Windows build requires MSVC. Found: ${CMAKE_CXX_COMPILER_ID}")
endif()

message(STATUS "🔒 Hermetic Toolchain: MSVC compiler enforced")

# ─────────────────────────────────────────────────────────────────────────────
# Windows SDK Pinning (CRITICAL)
# ─────────────────────────────────────────────────────────────────────────────
# Why: Multiple SDKs may be installed (10.0.19041.0, 10.0.22000.0, 10.0.22621.0)
#      CMake auto-selects "latest" which differs per machine
# Solution: Pin exact SDK version for deterministic builds
#
# Common Windows SDK versions:
#   10.0.19041.0 - Windows 10 2004
#   10.0.22000.0 - Windows 11 21H2
#   10.0.22621.0 - Windows 11 22H2 (recommended for new projects)

set(CMAKE_SYSTEM_VERSION 10.0.22621.0 CACHE STRING "Windows SDK version" FORCE)

# Prevent Visual Studio from overriding SDK selection
set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION ${CMAKE_SYSTEM_VERSION} CACHE STRING "VS SDK lock" FORCE)

message(STATUS "🔒 Hermetic Toolchain: Windows SDK locked to ${CMAKE_SYSTEM_VERSION}")

# ─────────────────────────────────────────────────────────────────────────────
# CRT Linkage: Explicit Static Runtime (CRITICAL)
# ─────────────────────────────────────────────────────────────────────────────
# Why: MSVC runtime linkage MUST be consistent across all dependencies
#      Mixing /MD (dynamic) and /MT (static) causes linking failures
# Solution: Force static CRT for all build configurations
#
# CRT Options:
#   MultiThreaded           → /MT  (Release static)
#   MultiThreadedDebug      → /MTd (Debug static)
#   MultiThreadedDLL        → /MD  (Release dynamic)
#   MultiThreadedDebugDLL   → /MDd (Debug dynamic)
#
# DineroCoin uses STATIC CRT for:
#   - RocksDB vendored build (static libs)
#   - OpenSSL vendored build (static libs)
#   - GoogleTest vendored build (static libs)
#   - Bitcoin Core compatibility

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "CRT linkage" FORCE)

message(STATUS "🔒 Hermetic Toolchain: CRT linkage set to static (/MT)")

# ─────────────────────────────────────────────────────────────────────────────
# vcpkg Quarantine: Kill Auto-Integration
# ─────────────────────────────────────────────────────────────────────────────
# Why: vcpkg auto-integrates via CMAKE_TOOLCHAIN_FILE environment variable
#      This silently adds packages from C:\vcpkg or user vcpkg installations
# Solution: Explicitly clear vcpkg integration variables

set(CMAKE_TOOLCHAIN_FILE "" CACHE FILEPATH "No vcpkg toolchain" FORCE)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "" CACHE FILEPATH "No vcpkg chainload" FORCE)
set(VCPKG_TARGET_TRIPLET "" CACHE STRING "No vcpkg triplet" FORCE)
set(VCPKG_INSTALLED_DIR "" CACHE PATH "No vcpkg installed dir" FORCE)

# Block vcpkg from environment variables
if(DEFINED ENV{VCPKG_ROOT})
  message(STATUS "🔒 Hermetic Toolchain: Ignoring VCPKG_ROOT=$ENV{VCPKG_ROOT}")
  unset(ENV{VCPKG_ROOT})
endif()

message(STATUS "🔒 Hermetic Toolchain: vcpkg auto-integration KILLED")

# ─────────────────────────────────────────────────────────────────────────────
# Registry Discovery Quarantine
# ─────────────────────────────────────────────────────────────────────────────
# Why: Windows uses registry for package discovery (Qt, Python, etc.)
#      This is non-deterministic and machine-specific
# Solution: Block all registry-based package discovery (already in common-hermetic.cmake)

message(STATUS "🔒 Hermetic Toolchain: Windows registry discovery blocked")

# ─────────────────────────────────────────────────────────────────────────────
# PATH Quarantine: Block System Environment
# ─────────────────────────────────────────────────────────────────────────────
# Why: PATH often contains:
#      - C:\msys64\mingw64\bin (MinGW headers/libs)
#      - C:\Program Files\Git\usr\bin (Unix-like tools)
#      - User-installed Python, Perl, etc.
# Solution: CMake ignores PATH for find_program() (already in common-hermetic.cmake)

message(STATUS "🔒 Hermetic Toolchain: PATH-based discovery blocked")

# ─────────────────────────────────────────────────────────────────────────────
# Find Root Path: Vendored Dependencies Only
# ─────────────────────────────────────────────────────────────────────────────
# CMAKE_FIND_ROOT_PATH defines the ONLY locations CMake searches
# This is the nuclear option - everything else is invisible

if(CMAKE_SOURCE_DIR)
  set(CMAKE_FIND_ROOT_PATH
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/vendor
    CACHE PATH "Root search paths" FORCE
  )

  # NEVER search outside CMAKE_FIND_ROOT_PATH
  set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER CACHE STRING "Program search mode" FORCE)
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY CACHE STRING "Library search mode" FORCE)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY CACHE STRING "Include search mode" FORCE)
  set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY CACHE STRING "Package search mode" FORCE)

  message(STATUS "🔒 Hermetic Toolchain: Search restricted to CMAKE_FIND_ROOT_PATH")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Generator Enforcement: Visual Studio Only
# ─────────────────────────────────────────────────────────────────────────────
# Why: Ninja with MSVC requires manual environment setup (vcvarsall.bat)
#      Visual Studio generator handles MSVC environment automatically
# Solution: Require Visual Studio generator for reproducibility

if(CMAKE_GENERATOR)
  if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    message(FATAL_ERROR
      "Hermetic MSVC builds require Visual Studio generator.\n"
      "Current generator: ${CMAKE_GENERATOR}\n"
      "Use: cmake -G \"Visual Studio 17 2022\" -A x64 ...")
  endif()
  message(STATUS "🔒 Hermetic Toolchain: Generator: ${CMAKE_GENERATOR}")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# MSVC-Specific Build Flags
# ─────────────────────────────────────────────────────────────────────────────

# Warning level (W3 = production, W4 = strict)
add_compile_options(/W3)

# Parallel compilation (use all CPU cores)
add_compile_options(/MP)

# Exception handling model (sync C++ exceptions)
add_compile_options(/EHsc)

# Disable specific MSVC warnings
add_compile_options(
  /wd4996  # 'function': was declared deprecated
  /wd4267  # conversion from 'size_t' to 'type', possible loss of data
  /wd4244  # conversion from 'type1' to 'type2', possible loss of data
)

# Security features
add_compile_options(
  /GS      # Buffer security checks
  /guard:cf # Control Flow Guard
)

# Release optimizations
add_compile_options($<$<CONFIG:Release>:/O2>)  # Maximum optimization
add_compile_options($<$<CONFIG:Release>:/Ob2>) # Inline expansion
add_compile_options($<$<CONFIG:Release>:/GL>)  # Whole program optimization

# Linker flags for release
add_link_options($<$<CONFIG:Release>:/LTCG>)        # Link-time code generation
add_link_options($<$<CONFIG:Release>:/OPT:REF>)     # Remove unused functions
add_link_options($<$<CONFIG:Release>:/OPT:ICF>)     # Identical COMDAT folding

message(STATUS "🔒 Hermetic Toolchain: MSVC build flags configured")

# ─────────────────────────────────────────────────────────────────────────────
# RocksDB Compatibility: Static CRT Enforcement
# ─────────────────────────────────────────────────────────────────────────────
# RocksDB CMake uses WITH_WINDOWS_UTF8_FILENAMES and other Windows-specific options

set(WITH_WINDOWS_UTF8_FILENAMES ON CACHE BOOL "RocksDB UTF-8 filenames" FORCE)

# ─────────────────────────────────────────────────────────────────────────────
# Status Summary
# ─────────────────────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════════")
message(STATUS "Windows MSVC Hermetic Toolchain: ACTIVE")
message(STATUS "═══════════════════════════════════════════════════════════")
message(STATUS "Compiler:      MSVC (cl.exe)")
message(STATUS "Windows SDK:   ${CMAKE_SYSTEM_VERSION}")
message(STATUS "CRT Linkage:   Static (/MT)")
message(STATUS "vcpkg:         KILLED")
message(STATUS "Registry:      BLOCKED")
message(STATUS "PATH:          IGNORED")
message(STATUS "Generator:     ${CMAKE_GENERATOR}")
message(STATUS "Build:         Reproducible, deterministic, auditable")
message(STATUS "═══════════════════════════════════════════════════════════")
message(STATUS "")
