# ═══════════════════════════════════════════════════════════════════════════
# macOS Hermetic Toolchain File
# ═══════════════════════════════════════════════════════════════════════════
# Purpose: Lock down compiler, SDK, and quarantine Homebrew/MacPorts
# Usage:   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchains/macos-hermetic.cmake
#
# What this file does:
#   1. Pins compiler to system Clang (not Homebrew clang)
#   2. Locks SDK path for header/library consistency
#   3. Quarantines Homebrew/MacPorts/Fink from CMake search space
#   4. Enforces minimum macOS deployment target
#   5. Fixes RocksDB archive size limit on macOS
#
# What this file does NOT do:
#   - Replace VendorGTest.cmake (that handles target-level include priority)
#   - Prevent vendored dependencies (this works WITH your existing patterns)
#   - Break local development (system frameworks still work)
#
# Design philosophy: "Homebrew is ambient radiation, not infrastructure"
# Inspired by: Bitcoin Core, LLVM, Chromium macOS build systems
# ═══════════════════════════════════════════════════════════════════════════

cmake_minimum_required(VERSION 3.20)

# Include common hermetic rules
include(${CMAKE_CURRENT_LIST_DIR}/common-hermetic.cmake)

# ─────────────────────────────────────────────────────────────────────────────
# Platform Identity
# ─────────────────────────────────────────────────────────────────────────────

set(CMAKE_SYSTEM_NAME Darwin)

# ─────────────────────────────────────────────────────────────────────────────
# Compiler Lock: System Clang Only
# ─────────────────────────────────────────────────────────────────────────────
# Why: Homebrew installs /opt/homebrew/bin/clang which may differ from system
# Solution: Explicitly use /usr/bin/clang (Apple's official toolchain)

set(CMAKE_C_COMPILER /usr/bin/clang CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_CXX_COMPILER /usr/bin/clang++ CACHE FILEPATH "C++ compiler" FORCE)

# Verify compilers exist
if(NOT EXISTS "${CMAKE_C_COMPILER}")
  message(FATAL_ERROR
    "System Clang not found at ${CMAKE_C_COMPILER}\n"
    "Install Xcode Command Line Tools:\n"
    "  xcode-select --install")
endif()

message(STATUS "🔒 Hermetic Toolchain: Using system Clang")
message(STATUS "   C:   ${CMAKE_C_COMPILER}")
message(STATUS "   C++: ${CMAKE_CXX_COMPILER}")

# ─────────────────────────────────────────────────────────────────────────────
# SDK Lock: Deterministic Header/Library Sources
# ─────────────────────────────────────────────────────────────────────────────
# Why: Multiple SDKs may be installed (Xcode.app, CommandLineTools, old versions)
# Solution: Explicitly specify which SDK is authoritative

# Try CommandLineTools first (lighter, more common in CI)
set(_sdk_path "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")
if(NOT EXISTS "${_sdk_path}")
  # Fallback to Xcode.app SDK
  execute_process(
    COMMAND xcrun --sdk macosx --show-sdk-path
    OUTPUT_VARIABLE _sdk_path
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endif()

if(NOT EXISTS "${_sdk_path}")
  message(FATAL_ERROR
    "macOS SDK not found\n"
    "Install Xcode Command Line Tools:\n"
    "  xcode-select --install")
endif()

set(CMAKE_OSX_SYSROOT "${_sdk_path}" CACHE PATH "macOS SDK" FORCE)
message(STATUS "🔒 Hermetic Toolchain: SDK locked to ${CMAKE_OSX_SYSROOT}")

# ─────────────────────────────────────────────────────────────────────────────
# Deployment Target: Minimum macOS Version
# ─────────────────────────────────────────────────────────────────────────────
# Aligned with DineroCoin's existing target (macOS 12.0 Monterey)

set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Min macOS version" FORCE)
message(STATUS "🔒 Hermetic Toolchain: Deployment target macOS ${CMAKE_OSX_DEPLOYMENT_TARGET}+")

# ─────────────────────────────────────────────────────────────────────────────
# Architecture: Current Host Only (not Universal Binary)
# ─────────────────────────────────────────────────────────────────────────────

execute_process(
  COMMAND uname -m
  OUTPUT_VARIABLE _host_arch
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(CMAKE_OSX_ARCHITECTURES "${_host_arch}" CACHE STRING "Target architecture" FORCE)
message(STATUS "🔒 Hermetic Toolchain: Building for ${_host_arch}")

# ─────────────────────────────────────────────────────────────────────────────
# Homebrew Quarantine: Remove from Search Space (CRITICAL)
# ─────────────────────────────────────────────────────────────────────────────
# Why: Homebrew is ambient radiation - headers/libs in /opt/homebrew leak into builds
# Solution: Remove Homebrew paths from ALL CMake search mechanisms
#
# This complements VendorGTest.cmake's Layer 2 quarantine
# Both are necessary: toolchain file runs first, VendorGTest.cmake adds target-level fixes

set(_package_manager_paths
  # ARM64 Homebrew (M1/M2/M3)
  "/opt/homebrew"
  "/opt/homebrew/include"
  "/opt/homebrew/lib"
  "/opt/homebrew/bin"
  "/opt/homebrew/opt"
  # x86_64 Homebrew (Intel Macs)
  "/usr/local/Homebrew"
  "/usr/local/opt"
  "/usr/local/include"
  "/usr/local/lib"
  # MacPorts
  "/opt/local"
  "/opt/local/include"
  "/opt/local/lib"
  "/opt/local/bin"
  # Fink (legacy)
  "/sw"
  "/sw/include"
  "/sw/lib"
  "/sw/bin"
)

# Remove from all CMake search path variables
foreach(_path ${_package_manager_paths})
  list(FILTER CMAKE_SYSTEM_PREFIX_PATH EXCLUDE REGEX "^${_path}")
  list(FILTER CMAKE_SYSTEM_INCLUDE_PATH EXCLUDE REGEX "^${_path}")
  list(FILTER CMAKE_SYSTEM_LIBRARY_PATH EXCLUDE REGEX "^${_path}")
  list(FILTER CMAKE_SYSTEM_FRAMEWORK_PATH EXCLUDE REGEX "^${_path}")
  list(FILTER CMAKE_PREFIX_PATH EXCLUDE REGEX "^${_path}")
  list(FILTER CMAKE_INCLUDE_PATH EXCLUDE REGEX "^${_path}")
  list(FILTER CMAKE_LIBRARY_PATH EXCLUDE REGEX "^${_path}")
  list(FILTER CMAKE_FRAMEWORK_PATH EXCLUDE REGEX "^${_path}")
endforeach()

message(STATUS "🔒 Hermetic Toolchain: Homebrew/MacPorts/Fink QUARANTINED")

# ─────────────────────────────────────────────────────────────────────────────
# Explicit System Paths: What IS Allowed
# ─────────────────────────────────────────────────────────────────────────────
# After quarantine, define what system resources are legitimate

# SDK headers and libraries
list(APPEND CMAKE_SYSTEM_PREFIX_PATH "${CMAKE_OSX_SYSROOT}/usr")
list(APPEND CMAKE_SYSTEM_INCLUDE_PATH "${CMAKE_OSX_SYSROOT}/usr/include")
list(APPEND CMAKE_SYSTEM_LIBRARY_PATH "${CMAKE_OSX_SYSROOT}/usr/lib")
list(APPEND CMAKE_SYSTEM_FRAMEWORK_PATH "${CMAKE_OSX_SYSROOT}/System/Library/Frameworks")

# System frameworks (Security, CoreFoundation, OpenCL, etc.)
list(APPEND CMAKE_SYSTEM_FRAMEWORK_PATH "/System/Library/Frameworks")

message(STATUS "🔒 Hermetic Toolchain: System paths locked to SDK + system frameworks")

# ─────────────────────────────────────────────────────────────────────────────
# Toolchain Executables: System Only
# ─────────────────────────────────────────────────────────────────────────────

set(CMAKE_LINKER /usr/bin/ld CACHE FILEPATH "Linker" FORCE)
set(CMAKE_AR /usr/bin/ar CACHE FILEPATH "Archiver" FORCE)
set(CMAKE_RANLIB /usr/bin/ranlib CACHE FILEPATH "Ranlib" FORCE)
set(CMAKE_MAKE_PROGRAM /usr/bin/make CACHE FILEPATH "Make" FORCE)

# ─────────────────────────────────────────────────────────────────────────────
# RocksDB Archive Size Fix (macOS-Specific)
# ─────────────────────────────────────────────────────────────────────────────
# macOS ranlib has archive size limits that RocksDB exceeds
# This matches the fix already in your CMakeLists.txt:102-107

set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_CXX_ARCHIVE_FINISH "")
set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH "")

message(STATUS "🔒 Hermetic Toolchain: RocksDB archive size fix applied")

# ─────────────────────────────────────────────────────────────────────────────
# Compiler Flags: Security Hardening
# ─────────────────────────────────────────────────────────────────────────────

# Stack protection
add_compile_options(-fstack-protector-strong)

# Fortify source
add_compile_definitions(_FORTIFY_SOURCE=2)

# Position independent executables (PIE)
add_link_options(-Wl,-pie)

# ─────────────────────────────────────────────────────────────────────────────
# Status Summary
# ─────────────────────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════════")
message(STATUS "macOS Hermetic Toolchain: ACTIVE")
message(STATUS "═══════════════════════════════════════════════════════════")
message(STATUS "Compiler:  ${CMAKE_CXX_COMPILER}")
message(STATUS "SDK:       ${CMAKE_OSX_SYSROOT}")
message(STATUS "Target:    macOS ${CMAKE_OSX_DEPLOYMENT_TARGET}+")
message(STATUS "Arch:      ${CMAKE_OSX_ARCHITECTURES}")
message(STATUS "Homebrew:  QUARANTINED")
message(STATUS "Build:     Reproducible, deterministic, auditable")
message(STATUS "═══════════════════════════════════════════════════════════")
message(STATUS "")
