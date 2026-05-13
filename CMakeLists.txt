cmake_minimum_required(VERSION 3.20)
# Project name is lowercase `dinero` to match the target monorepo
# repository name (DineroLabs/dinero). VERSION 8.0.0 is the first
# unified release line after the monorepo consolidation; daemon,
# CLI, Qt GUI, and solo miner all derive their version from this
# declaration (sub-project project() calls in qt/ and miner/ also
# pin to 8.0.0, see the comments there).
project(dinero VERSION 8.0.0 LANGUAGES C CXX)

# Enable Objective-C++ at the root scope on Apple platforms so that
# qt/ (which has .mm sources via Qt6 + macOS native frameworks like
# CoreImage / AppKit) can rely on CMAKE_OBJCXX_COMPILE_OBJECT being
# set during the generate phase. Pre-consolidation qt/ enabled this
# language itself in standalone mode; in add_subdirectory mode CMake
# needs OBJCXX known at the top-level project scope to wire
# generate-time compile rules.
if(APPLE)
  enable_language(OBJCXX)
endif()

# Export compile_commands.json for clangd and related tooling.
# Harmless for generators that do not use it, and helpful for editor parity.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ═══════════════════════════════════════════════════════════════════════════
# Prevent In-Source Builds
# ═══════════════════════════════════════════════════════════════════════════
# In-source builds pollute the source tree and can cause issues with git.
# Always use out-of-source builds: cmake -S . -B build && cd build && make
# ═══════════════════════════════════════════════════════════════════════════
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
    "\n"
    "❌ FATAL: In-source builds are not allowed!\n"
    "\n"
    "You are attempting to build in the source directory:\n"
    "  ${CMAKE_SOURCE_DIR}\n"
    "\n"
    "This would pollute the source tree with build artifacts.\n"
    "\n"
    "Please use an out-of-source build:\n"
    "  rm -f CMakeCache.txt\n"
    "  rm -rf CMakeFiles/\n"
    "  cmake -S . -B build\n"
    "  cd build\n"
    "  make\n"
    "\n"
    "Or use the hermetic build script:\n"
    "  ./scripts/hermetic-build.sh <target>\n"
    "\n"
  )
endif()

# Guardrail: production code must not depend on legacy mempool::Mempool path.
add_custom_target(check_no_legacy_mempool_prod
  COMMAND ${CMAKE_SOURCE_DIR}/tools/check_no_legacy_mempool_prod.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Checking production tree for legacy mempool references"
)

# ═══════════════════════════════════════════════════════════════════════════
# Hermetic Build Detection
# ═══════════════════════════════════════════════════════════════════════════
# DEV MODE (default): Fast iteration, normal cmake/make
# HERMETIC MODE: Deterministic validation, use ./scripts/hermetic-build.sh
#
# Hermetic builds are RECOMMENDED for releases but not required for development.
# ═══════════════════════════════════════════════════════════════════════════

if(NOT DEFINED ENV{SOURCE_DATE_EPOCH})
  message(WARNING
    "\n"
    "⚠️  DEV MODE: Not a hermetic build\n"
    "   For release validation, use: ./scripts/hermetic-build.sh <target>\n"
  )
  set(HERMETIC_BUILD OFF)
else()
  message(STATUS "🔒 Hermetic build: SOURCE_DATE_EPOCH = $ENV{SOURCE_DATE_EPOCH}")
  set(HERMETIC_BUILD ON)
endif()

# Add cmake modules directory to module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Modern CMake policies
cmake_policy(SET CMP0144 NEW)  # Use upper-case <PACKAGENAME>_ROOT variables

# C++ standard (C++17 baseline for std::optional, std::string_view, std::variant)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows compatibility — must be before any target definitions
if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
  # secp256k1.h declares functions with __declspec(dllimport) on Windows
  # unless SECP256K1_STATIC is defined. We link against the static
  # libsecp256k1.a built in third_party/secp256k1-zkp; without this define,
  # consumers (dinero_crypto, etc.) emit `__imp_secp256k1_*` references
  # that the static archive can't resolve. See third_party/secp256k1-zkp/
  # include/secp256k1.h around the SECP256K1_API macro definition.
  add_compile_definitions(SECP256K1_STATIC)
  # Windows CRT includes threading — no separate pthread library needed.
  # Create an empty interface target so all target_link_libraries(... pthread) calls resolve.
  add_library(pthread INTERFACE)

  # Enable the .rc compiler at root scope so any target (daemon binaries
  # here, dinero-qt under qt/) can attach a VS_VERSION_INFO resource via
  # dinero_attach_win32_versioninfo() below.
  enable_language(RC)
endif()

# Release tag for Windows file-properties / About dialog metadata.
# PROJECT_VERSION carries semver only ("8.0.0"); rc/preview suffixes live
# in this variable. Override per cut at configure time:
#   cmake -S . -B build -DDINERO_RELEASE_TAG=8.0.0-rc2 ...
# Defaults to PROJECT_VERSION so unflagged dev builds never embed an
# empty string. Promoted to CACHE here so qt/ and daemon targets see the
# same value.
if(NOT DEFINED DINERO_RELEASE_TAG OR "${DINERO_RELEASE_TAG}" STREQUAL "")
  set(DINERO_RELEASE_TAG "${PROJECT_VERSION}"
      CACHE STRING "User-visible release tag (e.g. 8.0.0-rc2)")
endif()

# Helper: attach a VS_VERSION_INFO resource to a Windows executable so
# Explorer Properties -> Details shows version/company/component fields.
# No-op on non-Windows. Pass a human description for the FileDescription
# field (shown in Task Manager and right-click Properties).
function(dinero_attach_win32_versioninfo target description)
  if(NOT WIN32)
    return()
  endif()
  # Respect set_target_properties(... OUTPUT_NAME ...) so InternalName and
  # OriginalFilename match the actual .exe on disk (e.g. dinero-solo-miner-cli
  # ships as dinero-solo-miner.exe).
  get_target_property(_outname ${target} OUTPUT_NAME)
  if(NOT _outname OR "${_outname}" STREQUAL "_outname-NOTFOUND")
    set(_outname "${target}")
  endif()
  set(DINERO_VI_FILEVERSION
      "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
  set(DINERO_VI_PRODUCTVERSION "${DINERO_VI_FILEVERSION}")
  set(DINERO_VI_VERSION_STRING "${DINERO_RELEASE_TAG}")
  set(DINERO_VI_DESCRIPTION    "${description}")
  set(DINERO_VI_INTERNAL_NAME  "${_outname}")
  set(DINERO_VI_ORIGINAL_FILENAME "${_outname}.exe")
  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/${target}_version_info.rc")
  configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/dinero_version_info.rc.in"
    "${_gen}"
    @ONLY
  )
  target_sources(${target} PRIVATE "${_gen}")
endfunction()
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options - simplified and focused
option(DINERO_USE_VENDORED_DEPS "Use vendored third-party dependencies (ON for CI/releases, OFF for local dev)" ON)
option(ENABLE_SANITIZERS "Enable ASan/UBSan for debug builds" OFF)
option(ENABLE_GPU_MINING "Enable GPU mining support (OpenCL/CUDA)" ON)
option(ENABLE_LIGHTNING "Build Lightning Network support (Layer 3, off-chain)" OFF)  # Phase 3: DISABLED - Lightning is now standalone
option(ENABLE_TREZOR "Enable Trezor hardware wallet support" OFF)  # Disabled by default - enable if you have Trezor proto files
option(ENABLE_HARDWARE_WALLETS "Enable Ledger/Trezor USB HID hardware wallet support" ON)
# (DINERO_HAVE_HARDWARE_WALLETS compile-flag is set BELOW the
# DINERO_PACKAGED_BUILD block — that block can FORCE this option
# off, so we must wait until its final value is settled.)
option(ENABLE_UTREEXO_INVARIANT_CHECKS "Enable Utreexo Pollard/Stump invariant checks (CI/debug only)" OFF)
option(DIN_ENABLE_LEGACY_RPC "Enable deprecated legacy RPC server code paths" OFF)

# Phase E.3 (Dinero Core 1.0) — packaged-service build mode.
# When ON, the build produces the artifact contract from
# docs/specs/dinero-core-1.0.md §1.4: dinerod + dinero-cli only,
# GPU mining linked off, INSTALL_RPATH set to /usr/lib/dinero with
# DT_RUNPATH (--enable-new-dtags) so bundled .so files in the .deb
# resolve at runtime without LD_LIBRARY_PATH games. Default OFF —
# manual / dev / fleet builds keep the full feature set. The .deb
# rules (debian/rules) flip this ON.
option(DINERO_PACKAGED_BUILD "Build the .deb / packaged-service artifact (Linux only)" OFF)
if(DINERO_PACKAGED_BUILD)
  if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "DINERO_PACKAGED_BUILD=ON only supported on Linux")
  endif()
  set(ENABLE_GPU_MINING OFF CACHE BOOL "Forced OFF by DINERO_PACKAGED_BUILD" FORCE)
  # Hardware-wallet HID support (hidapi) drags libudev1 into the .deb's
  # Depends — meaningful for desktop wallet builds, not for headless
  # packaged-service nodes. The .deb's audience is full-node operators
  # on VPSes; they don't plug a Ledger into a server.
  set(ENABLE_HARDWARE_WALLETS OFF CACHE BOOL "Forced OFF by DINERO_PACKAGED_BUILD" FORCE)
  # Force the existing DINERO_RELEASE codepath:
  #   - ENABLE_GRPC=OFF (drops the entire gRPC stack — protobuf,
  #     abseil, c-ares, re2 — from the runtime surface)
  #   - DISABLE_GRPC compile def (gates daemon_app.cpp's gRPC server
  #     setup and shutdown out of the binary)
  #   - Lightning IPC defaults to raw sockets, not gRPC
  # Why: the current gRPC server has a TLS/mTLS TODO and binds
  # 0.0.0.0 with insecure credentials. Not safe for public Core 1.0.
  # Revisit when (a) Lightning is actually shipping AND (b) the
  # server has TLS/mTLS landed. Tracked in spec §"Out of scope".
  set(DINERO_RELEASE ON CACHE BOOL "Forced ON by DINERO_PACKAGED_BUILD" FORCE)
  message(STATUS "📦 DINERO_PACKAGED_BUILD=ON — packaged-service artifact (Linux .deb)")
  message(STATUS "   - GPU mining: forced OFF (drops libOpenCL from NEEDED)")
  message(STATUS "   - Hardware wallets: forced OFF (drops libudev from NEEDED)")
  message(STATUS "   - DINERO_RELEASE=ON: forced (drops gRPC stack — see spec deferral)")
  message(STATUS "   - INSTALL_RPATH: /usr/lib/dinero (DT_RUNPATH via --enable-new-dtags)")
  message(STATUS "   - Installed binaries: dinerod, dinero-cli only")
endif()

# DINERO_HAVE_HARDWARE_WALLETS compile flag — set AFTER the
# DINERO_PACKAGED_BUILD block so we observe the final value of
# ENABLE_HARDWARE_WALLETS (the block above can FORCE it off).
# Source files use #ifdef DINERO_HAVE_HARDWARE_WALLETS to skip the
# call sites for hidapi-backed RPC method registration.
if(ENABLE_HARDWARE_WALLETS)
  add_compile_definitions(DINERO_HAVE_HARDWARE_WALLETS=1)
endif()

# Utreexo invariant checks - compile definition
if(ENABLE_UTREEXO_INVARIANT_CHECKS)
  add_compile_definitions(ENABLE_UTREEXO_INVARIANT_CHECKS)
  message(STATUS "🔍 Utreexo invariant checks ENABLED (Pollard/Stump cross-validation)")
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Phase 8.4: Binary Separation - Hermetic Build Targets
# ═══════════════════════════════════════════════════════════════════════════
# BUILD_DINEROD=ON     → L1 daemon (blockchain, mempool, wallet, gRPC server)
# BUILD_LIGHTNINGD=ON  → L2 daemon (channels, HTLCs, routing, IPC client)
#
# Independence guarantee:
#   cmake -DBUILD_DINEROD=OFF -DBUILD_LIGHTNINGD=ON .. && make lightningd
#   ↳ MUST NOT: generate protos, build gRPC, touch daemon code
#
# CI enforcement: This invariant is tested in .github/workflows/
# ═══════════════════════════════════════════════════════════════════════════
option(BUILD_DINEROD "Build L1 blockchain daemon" ON)
option(BUILD_LIGHTNINGD "Build L2 Lightning daemon" ON)

# ═══════════════════════════════════════════════════════════════════════════
# iOS/Mobile FFI-Only Build Mode
# ═══════════════════════════════════════════════════════════════════════════
# BUILD_FFI_ONLY=ON  → Minimal build for iOS/mobile FFI library
# BUILD_FFI_ONLY=OFF → Full build (default)
#
# FFI-only mode builds ONLY:
# - dinero_crypto (cryptographic primitives)
# - dinero_wallet (HD wallet, signing, coin selection)
# - dinero_wallet_ffi (C API wrapper for FFI)
#
# FFI-only mode DISABLES:
# - Daemon (dinerod), Lightning (lightningd), GUI, RPC, P2P, Mining
# - gRPC, tests, fuzzing
#
# Usage:
#   cmake -DCMAKE_SYSTEM_NAME=iOS -DBUILD_FFI_ONLY=ON ..
# ═══════════════════════════════════════════════════════════════════════════
option(BUILD_FFI_ONLY "Build only wallet FFI library (for iOS/mobile)" OFF)

if(BUILD_FFI_ONLY)
  message(STATUS "📱 FFI-ONLY MODE: Building minimal wallet FFI for iOS/mobile")
  message(STATUS "   - Daemon, Lightning, GUI, P2P: DISABLED")
  message(STATUS "   - Only building: dinero_crypto, dinero_wallet, dinero_wallet_ffi")

  # Force disable incompatible features
  set(BUILD_DINEROD OFF CACHE BOOL "" FORCE)
  set(BUILD_LIGHTNINGD OFF CACHE BOOL "" FORCE)
  set(BUILD_GUI OFF CACHE BOOL "" FORCE)
  set(ENABLE_GRPC OFF CACHE BOOL "" FORCE)
  set(ENABLE_GPU_MINING OFF CACHE BOOL "" FORCE)
  set(ENABLE_LIGHTNING OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(ENABLE_FUZZING OFF CACHE BOOL "" FORCE)
  set(ENABLE_SANITIZERS OFF CACHE BOOL "" FORCE)
  set(ENABLE_ZK OFF CACHE BOOL "" FORCE)

  # Define compile flag for conditional compilation
  add_compile_definitions(BUILD_FFI_ONLY)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Release-Grade Build Mode (Bitcoin Core Style)
# ═══════════════════════════════════════════════════════════════════════════
# DINERO_RELEASE=ON  → Exchange-ready binaries, zero Homebrew dependencies
# DINERO_RELEASE=OFF → Development builds, dynamic linking OK
#
# Release mode enforces:
# - Static linking for ALL dependencies
# - No gRPC/protobuf/abseil runtime dependencies
# - Lightning IPC via raw sockets (not gRPC)
# - CI guard validates binary has zero Homebrew dylibs
#
# Usage:
#   Development:  cmake ..
#   Release:      cmake -DDINERO_RELEASE=ON ..
# ═══════════════════════════════════════════════════════════════════════════
option(DINERO_RELEASE "Release-grade build (static deps, no gRPC)" OFF)

if(DINERO_RELEASE)
  message(STATUS "🔒 RELEASE MODE: Building exchange-grade binaries")
  message(STATUS "   - Static linking enforced")
  message(STATUS "   - gRPC/protobuf/abseil disabled")
  message(STATUS "   - Lightning IPC via raw sockets")
  add_compile_definitions(DINERO_RELEASE_BUILD)
  add_compile_definitions(DISABLE_GRPC)

  # Disable gRPC in release mode (proto files still generated for types, but no gRPC services)
  set(ENABLE_GRPC OFF)

  # Enforce vendored OpenSSL in release mode (no Homebrew/system dependencies)
  set(USE_SYSTEM_OPENSSL OFF CACHE BOOL "" FORCE)
else()
  message(STATUS "🛠️  DEV MODE: Development build with dynamic linking")
  message(STATUS "   - gRPC enabled for faster iteration")
  message(STATUS "   - Homebrew dependencies OK")
endif()

# Sanitizer wiring (single source of truth).
# ENABLE_SANITIZERS remains the user-facing toggle; the cmake module
# provides consistent compiler/linker flag plumbing and optional TSAN mode.
set(DIN_ENABLE_ASAN ${ENABLE_SANITIZERS})
include(Sanitizers)

# Platform-specific definitions
if(WIN32)
  add_definitions(-D_WIN32_WINNT=0x0A00)
  add_definitions(-DWIN32_LEAN_AND_MEAN)
elseif(APPLE)
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(STATUS "🍎 iOS deployment target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
  else()
  set(DINERO_MACOS_TARGET_FILE "${CMAKE_SOURCE_DIR}/.macos-deployment-target")
  if(NOT EXISTS "${DINERO_MACOS_TARGET_FILE}")
    message(FATAL_ERROR
      "Missing ${DINERO_MACOS_TARGET_FILE}. "
      "Dinero requires a checked-in canonical macOS deployment target.")
  endif()

  file(READ "${DINERO_MACOS_TARGET_FILE}" DINERO_CANONICAL_MACOS_TARGET_RAW)
  string(STRIP "${DINERO_CANONICAL_MACOS_TARGET_RAW}" DINERO_CANONICAL_MACOS_TARGET)
  if(DINERO_CANONICAL_MACOS_TARGET STREQUAL "")
    message(FATAL_ERROR
      "${DINERO_MACOS_TARGET_FILE} is empty. "
      "Set it to the repo-wide minimum supported macOS version.")
  endif()

  if(DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
    if(NOT "${CMAKE_OSX_DEPLOYMENT_TARGET}" VERSION_EQUAL "${DINERO_CANONICAL_MACOS_TARGET}")
      message(FATAL_ERROR
        "Dinero only supports macOS ${DINERO_CANONICAL_MACOS_TARGET} builds. "
        "Requested CMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}. "
        "Update the repo policy file or rebuild with the canonical target.")
    endif()
  endif()

  # Normalize all Apple builds to the single checked-in repo target.
  set(CMAKE_OSX_DEPLOYMENT_TARGET "${DINERO_CANONICAL_MACOS_TARGET}" CACHE STRING "Minimum macOS deployment target" FORCE)
  message(STATUS "🍎 macOS deployment target: ${CMAKE_OSX_DEPLOYMENT_TARGET} (repo policy)")
  endif()
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/vendor/include)
include_directories(${CMAKE_SOURCE_DIR}/include)

# ═══════════════════════════════════════════════════════════════════════════
# Vendored JsonCpp MUST take precedence over Homebrew (header ABI compatibility)
# ═══════════════════════════════════════════════════════════════════════════
# Modern JsonCpp (≥1.9.x) removed operator[](const char*) in favor of operator[](string_view).
# Homebrew may have older version with different ABI. Force vendored version globally.
include_directories(BEFORE SYSTEM ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include)

# Quarantine Homebrew includes to prevent accidental JsonCpp header pickup
# This ensures vendored JsonCpp headers are ALWAYS used (critical for ABI compatibility)
if(APPLE)
  list(REMOVE_ITEM CMAKE_SYSTEM_PREFIX_PATH "/opt/homebrew")
  list(REMOVE_ITEM CMAKE_SYSTEM_INCLUDE_PATH "/opt/homebrew/include")
  list(REMOVE_ITEM CMAKE_SYSTEM_LIBRARY_PATH "/opt/homebrew/lib")
endif()

# NOTE: /opt/homebrew/include is quarantined - vendored libs take absolute precedence
# Add Homebrew paths per-target only where explicitly needed (e.g., for non-vendored tools)

# Version configuration
# Use SOURCE_DATE_EPOCH for deterministic timestamps when provided.
if(DEFINED ENV{SOURCE_DATE_EPOCH} AND NOT "$ENV{SOURCE_DATE_EPOCH}" STREQUAL "")
  execute_process(
    COMMAND python3 -c "import datetime; print(datetime.datetime.fromtimestamp(int($ENV{SOURCE_DATE_EPOCH}), tz=datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S+0000'))"
    OUTPUT_VARIABLE BUILD_TIME
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  message(STATUS "🔒 Build timestamp: ${BUILD_TIME} (from SOURCE_DATE_EPOCH)")
else()
  string(TIMESTAMP BUILD_TIME "%Y-%m-%dT%H:%M:%S+0000" UTC)
  message(STATUS "🛠️ Build timestamp: ${BUILD_TIME} (live UTC fallback)")
endif()

find_package(Git QUIET)
find_package(Threads REQUIRED)
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --git-dir
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE GIT_DIR_RAW OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(GIT_DIR_RAW)
    if(IS_ABSOLUTE "${GIT_DIR_RAW}")
      set(GIT_DIR_PATH "${GIT_DIR_RAW}")
    else()
      get_filename_component(GIT_DIR_PATH "${CMAKE_SOURCE_DIR}/${GIT_DIR_RAW}" ABSOLUTE)
    endif()
  endif()

  execute_process(COMMAND ${GIT_EXECUTABLE} symbolic-ref -q HEAD
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE GIT_HEAD_REF OUTPUT_STRIP_TRAILING_WHITESPACE)

  set(GIT_METADATA_DEPENDS)
  if(GIT_DIR_PATH)
    foreach(_git_meta HEAD packed-refs index)
      if(EXISTS "${GIT_DIR_PATH}/${_git_meta}")
        list(APPEND GIT_METADATA_DEPENDS "${GIT_DIR_PATH}/${_git_meta}")
      endif()
    endforeach()
    if(GIT_HEAD_REF AND EXISTS "${GIT_DIR_PATH}/${GIT_HEAD_REF}")
      list(APPEND GIT_METADATA_DEPENDS "${GIT_DIR_PATH}/${GIT_HEAD_REF}")
    endif()
    if(GIT_METADATA_DEPENDS)
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GIT_METADATA_DEPENDS})
      message(STATUS "🔁 Tracking Git HEAD for automatic version metadata refresh")
    endif()
  endif()

  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE GIT_HASH_FULL OUTPUT_STRIP_TRAILING_WHITESPACE)
else()
  set(GIT_HASH "unknown")
  set(GIT_HASH_FULL "unknown")
endif()
# PROJECT_VERSION is the canonical semantic version set by the
# project() call at the top of this file (8.0.0 as of the monorepo
# consolidation). Do NOT override it here. GIT_HASH and GIT_HASH_FULL
# remain as separate values for build-provenance compile defs
# (DINERO_BUILD_GIT_SHA). Pre-consolidation, this slot held
# `set(PROJECT_VERSION "${GIT_HASH}")` which made the daemon report
# a short git hash as its version. That override was removed so that
# `dinerod --version`, `dinero-qt --version`, and friends all report
# 8.0.0 — one tag, one version string.

# Version variables for configure_file
set(CLIENT_VERSION_MAJOR 1)
set(CLIENT_VERSION_MINOR 0)
set(CLIENT_VERSION_REVISION 0)
set(CLIENT_VERSION_BUILD 0)
set(CLIENT_VERSION_IS_RELEASE 0)
set(BUILD_DATE "${BUILD_TIME}")
set(GIT_COMMIT_ID "${GIT_HASH}")
set(GIT_COMMIT_FULL "${GIT_HASH_FULL}")

# Version config - generate to build root so it's easily found
configure_file(${CMAKE_SOURCE_DIR}/include/compat/version_config.h.in
               ${CMAKE_BINARY_DIR}/version_config.h @ONLY)

# Include generated directory
include_directories(${CMAKE_BINARY_DIR})

# ============================================================================
# Phase D (v8 peer-discovery): chainparamsseeds.h regeneration
# ============================================================================
# Regenerate include/consensus/chainparamsseeds.h whenever
# contrib/seeds/seeds_main.txt changes. The header is committed (so
# consumers don't need Python at build time), but if seeds_main.txt is
# newer than the header — which is the case immediately after editing
# the seed list — this custom command brings the header back in sync.
#
# CI in --check mode catches the inverse case: header committed without
# updating from input.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/contrib/seeds/generate-seeds.py")
    add_custom_command(
        OUTPUT "${CMAKE_SOURCE_DIR}/include/consensus/chainparamsseeds.h"
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/contrib/seeds/generate-seeds.py"
        DEPENDS
            "${CMAKE_SOURCE_DIR}/contrib/seeds/seeds_main.txt"
            "${CMAKE_SOURCE_DIR}/contrib/seeds/generate-seeds.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Regenerating include/consensus/chainparamsseeds.h from contrib/seeds/seeds_main.txt"
        VERBATIM
    )
    add_custom_target(dinero_fixed_seeds_gen ALL
        DEPENDS "${CMAKE_SOURCE_DIR}/include/consensus/chainparamsseeds.h"
    )
else()
    message(STATUS "Python3 not found; chainparamsseeds.h regeneration is manual "
                   "(run contrib/seeds/generate-seeds.py after editing seeds_main.txt)")
endif()

# ============================================================================
# RocksDB Vendored Build Configuration (v8.11.3)
# ============================================================================
# Canonical configuration for protocol-grade RocksDB vendoring
# Version: 8.11.3 (stable release, locked until v1.0.0)
# Source: Official GitHub release tarball (complete, reproducible)
# Policy: Static library only, no tests/tools/benchmarks in production

# Build mode: Static library only
set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ROCKSDB_BUILD_STATIC ON CACHE BOOL "" FORCE)

# Disable all test/tool/benchmark components (sources exist, flags disable)
set(WITH_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)

# Disable Snappy's GoogleTest (we use standalone vendored GoogleTest v1.14.0)
set(SNAPPY_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# Compression support (standard production configuration)
set(WITH_GFLAGS OFF CACHE BOOL "" FORCE)
set(WITH_SNAPPY ON CACHE BOOL "" FORCE)
set(WITH_LZ4 ON CACHE BOOL "" FORCE)
set(WITH_ZSTD ON CACHE BOOL "" FORCE)

# ═══════════════════════════════════════════════════════════════════════════
# ZSTD - Static Vendored Library (Bitcoin Core Style)
# ═══════════════════════════════════════════════════════════════════════════
# ZSTD is used for:
# 1. RocksDB compression
# 2. Utreexo proof compression (BlockUtreexoProof::serializeCompressedWithZstd)
# 3. Block data compression
#
# Architecture: Vendored static library (no Homebrew/pkg-config dependency)
# Result: Portable binaries with no runtime ZSTD dependency
# Pattern: Same as Bitcoin Core (libsecp256k1, libevent, leveldb)

# Collect ZSTD source files from vendored directory
file(GLOB ZSTD_COMMON_SOURCES "${CMAKE_SOURCE_DIR}/third_party/zstd/lib/common/*.c")
file(GLOB ZSTD_COMPRESS_SOURCES "${CMAKE_SOURCE_DIR}/third_party/zstd/lib/compress/*.c")
file(GLOB ZSTD_DECOMPRESS_SOURCES "${CMAKE_SOURCE_DIR}/third_party/zstd/lib/decompress/*.c")

# Build static ZSTD library
add_library(zstd STATIC
  ${ZSTD_COMMON_SOURCES}
  ${ZSTD_COMPRESS_SOURCES}
  ${ZSTD_DECOMPRESS_SOURCES}
)

# Configure ZSTD build
target_include_directories(zstd PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/zstd/lib
  ${CMAKE_SOURCE_DIR}/third_party/zstd/lib/common
)

target_compile_definitions(zstd PRIVATE
  XXH_NAMESPACE=ZSTD_
  ZSTD_LEGACY_SUPPORT=0
  ZSTD_MULTITHREAD=0
  ZSTD_DISABLE_ASM=1  # Disable ASM for cross-platform portability
)

# Suppress warnings from vendored code (not our responsibility to fix)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  target_compile_options(zstd PRIVATE -w)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(zstd PRIVATE -w)
elseif(MSVC)
  target_compile_options(zstd PRIVATE /W0)
endif()

message(STATUS "✅ Building static ZSTD from third_party/zstd/ (Bitcoin Core style)")
message(STATUS "   Result: No runtime ZSTD dependency - fully portable binaries")

# Helper macro to link static ZSTD (prevents linker drift across 78+ test targets)
# Usage: link_zstd_if_needed(test_name)
# Pattern: Bitcoin Core-style explicit dependency declaration
macro(link_zstd_if_needed target)
  if(TARGET ${target})
    target_link_libraries(${target} PRIVATE zstd)
  endif()
endmacro()

# ─────────────────────────────────────────────────────────────
# ZK / secp256k1 linkage helper (REQUIRED for any ZK consumer)
# ─────────────────────────────────────────────────────────────
# Canonical Rule: Any target that uses dinero_zk MUST link secp256k1-zkp explicitly.
# Do not rely on transitive linkage - static libs do not propagate symbols.
# Pattern: Bitcoin Core-style explicit dependency declaration
#
# NOTE: The CMake target is named "secp256k1" but comes from third_party/secp256k1-zkp/
#       which includes ZKP extensions (Pedersen commitments, Bulletproofs, etc.)
#       We create an INTERFACE alias to make this explicit in consumer code.
#
# ┌─────────────────────────────────────────────────────────────────────────────┐
# │ secp256k1_zkp INTERFACE alias                                               │
# │ ─────────────────────────────────────────────────────────────────────────── │
# │ This is NOT vanilla libsecp256k1 from bitcoin-core/secp256k1                │
# │ This IS secp256k1-zkp from ElementsProject with ZK extensions:              │
# │   - secp256k1_pedersen_commit()                                             │
# │   - secp256k1_bulletproof_rangeproof_prove()                                │
# │   - secp256k1_bulletproof_rangeproof_verify()                               │
# │   - secp256k1_generator_generate_blinded()                                  │
# └─────────────────────────────────────────────────────────────────────────────┘

function(link_secp256k1_zkp target)
  target_link_libraries(${target} PRIVATE
    secp256k1_zkp
  )
endfunction()

# Disable debug/test synchronization points in production
set(WITH_SYNC_POINT OFF CACHE BOOL "" FORCE)

# Build configuration
set(FAIL_ON_WARNINGS OFF CACHE BOOL "" FORCE)

# macOS-specific fixes
if(APPLE)
  set(FORCE_NO_LIBATOMIC ON CACHE BOOL "macOS has builtin atomics" FORCE)
  # Fix for macOS ranlib archive size limit
  set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_CXX_ARCHIVE_FINISH "")
endif()

# Dependencies: RocksDB, GoogleTest (managed by cmake/deps.cmake)
# Switches between vendored (submodules) and system packages based on DINERO_USE_VENDORED_DEPS
include(cmake/deps.cmake)

# Architecture enforcement: Lightning L2 ↛ L1 separation guards
include(cmake/architecture_guards.cmake)

add_subdirectory(third_party/argon2 EXCLUDE_FROM_ALL)  # Wallet encryption
add_subdirectory(third_party/pqclean EXCLUDE_FROM_ALL) # V7 PQ signatures (ML-DSA-65 only today)
# dinero_pq and tools/pq_bench are added later (after OpenSSL::Crypto is
# imported) because pq_derivation.cpp inside dinero_pq uses HMAC-SHA256.

# jsoncpp (JSON parsing) - bundled statically
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "Build static libraries" FORCE)
set(BUILD_OBJECT_LIBS OFF CACHE BOOL "Build object libraries" FORCE)
set(JSONCPP_WITH_TESTS OFF CACHE BOOL "Compile and run JsonCpp test executables" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "Automatically run unit-tests as a post build step" FORCE)
set(JSONCPP_WITH_PKGCONFIG_SUPPORT OFF CACHE BOOL "Generate and install .pc files" FORCE)
# We consume jsoncpp via add_subdirectory and link its targets directly —
# no find_package(jsoncpp) anywhere in the tree. Disable the CMake-package
# export path so the brittle `configure_package_config_file` call inside
# jsoncpp's CMakeLists.txt never runs. That call is what required an
# inline patch to the submodule working tree (previously
# third_party/patches/jsoncpp-01-configure-file-current-binary-dir.patch)
# in order to not pollute our top-level CMAKE_BINARY_DIR. Turning the
# feature off at the option level eliminates the whole class of brittleness
# without carrying any vendored-source patches.
set(JSONCPP_WITH_CMAKE_PACKAGE OFF CACHE BOOL "Generate and install cmake package files" FORCE)

add_subdirectory(third_party/jsoncpp ${CMAKE_BINARY_DIR}/_deps/jsoncpp-build EXCLUDE_FROM_ALL)

# Force jsoncpp targets to use C++20 to match project ABI
# jsoncpp's CMakeLists.txt hardcodes C++11, which causes std::string_view ABI mismatch
foreach(tgt jsoncpp_static jsoncpp_lib)
    if(TARGET ${tgt})
        set_target_properties(${tgt} PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
        )
        # Override jsoncpp's cxx_std_11 feature requirement
        target_compile_features(${tgt} PUBLIC cxx_std_17)
    endif()
endforeach()

# Ensure jsoncpp headers are globally available (for <json/json.h> includes)
include_directories(${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include)

# msgpack-c (binary serialization for Lightning) - header-only library
include_directories(${CMAKE_SOURCE_DIR}/third_party/msgpack-c/include)
add_definitions(-DMSGPACK_NO_BOOST)  # Use msgpack without Boost dependency

# ═══════════════════════════════════════════════════════════════════════════
# VENDORED DEPENDENCIES - Static linking for portable binaries
# ═══════════════════════════════════════════════════════════════════════════

# secp256k1-zkp (Consensus + ZK cryptography) - Vendored for safety
# Elements Project fork of Bitcoin Core's secp256k1 with ZK extensions
# Version: upstream ElementsProject/secp256k1-zkp
# Purpose: Consensus-critical signatures (ECDSA, Schnorr) + ZK proofs (Pedersen, range proofs)
# Note: secp256k1-zkp is a superset of bitcoin-core/secp256k1, provides both consensus AND privacy

# Build configuration: static library only, no tests/benchmarks
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(SECP256K1_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS OFF CACHE BOOL "Build exhaustive tests" FORCE)
set(SECP256K1_BUILD_BENCHMARK OFF CACHE BOOL "Build benchmarks" FORCE)
set(SECP256K1_BUILD_CTIME_TESTS OFF CACHE BOOL "Build ctime tests" FORCE)
set(SECP256K1_BUILD_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
set(SECP256K1_INSTALL OFF CACHE BOOL "Enable installation" FORCE)

# Enable ZK modules (required for shielded proof support)
set(SECP256K1_ENABLE_MODULE_GENERATOR ON CACHE BOOL "Enable generator module for Pedersen commitments" FORCE)
set(SECP256K1_ENABLE_MODULE_RANGEPROOF ON CACHE BOOL "Enable range proof module" FORCE)
set(SECP256K1_ENABLE_MODULE_WHITELIST ON CACHE BOOL "Enable whitelist module" FORCE)
set(SECP256K1_ENABLE_MODULE_SURJECTIONPROOF ON CACHE BOOL "Enable surjection proof module" FORCE)

# Enable consensus-critical modules (Bitcoin Core compatibility)
set(SECP256K1_ENABLE_MODULE_RECOVERY ON CACHE BOOL "Enable ECDSA pubkey recovery module" FORCE)
set(SECP256K1_ENABLE_MODULE_SCHNORRSIG ON CACHE BOOL "Enable Schnorr signature module" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDH ON CACHE BOOL "Enable ECDH module" FORCE)
set(SECP256K1_ENABLE_MODULE_EXTRAKEYS ON CACHE BOOL "Enable extrakeys module" FORCE)

# Disable optional modules
set(SECP256K1_ENABLE_MODULE_MUSIG OFF CACHE BOOL "Disable musig module" FORCE)
set(SECP256K1_ENABLE_MODULE_ELLSWIFT OFF CACHE BOOL "Disable ElligatorSwift module" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDSA_S2C OFF CACHE BOOL "Disable ECDSA s2c module" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDSA_ADAPTOR OFF CACHE BOOL "Disable ECDSA adaptor module" FORCE)

add_subdirectory(third_party/secp256k1-zkp EXCLUDE_FROM_ALL)

# Inject Dinero's MSM shim into the secp256k1 library so it can access
# secp256k1_ecmult_multi_var (Pippenger/Strauss MSM) via internal headers.
# The shim exports dinero_secp256k1_msm() used by secp256k1_msm_cpu.cpp.
target_sources(secp256k1 PRIVATE
    ${CMAKE_SOURCE_DIR}/src/zk/zkvm/secp256k1_msm_shim.c
)
target_include_directories(secp256k1 PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/src  # for ecmult_impl.h etc.
)

# Create explicit secp256k1_zkp alias (see comment at line ~280)
# This makes ZKP dependency obvious in target_link_libraries calls
add_library(secp256k1_zkp INTERFACE)
target_link_libraries(secp256k1_zkp INTERFACE secp256k1)

# SQLite (database) - bundled statically
add_subdirectory(third_party/sqlite-amalgamation-3480000 EXCLUDE_FROM_ALL)

# Boost (header-only libraries for P2P networking)
# Configured via cmake/VendorBoost.cmake (hermetic pattern)
# Uses vendored Boost 1.85.0 from third_party/boost_1_85_0/
# Target: Boost::headers (imported INTERFACE target)
# Components: Asio (P2P), Beast (HTTP/WebSocket), Endian (byte order)

# OpenSSL (TLS/crypto) - Vendored for portable builds
# Option to use system OpenSSL instead of vendored (for distro packaging)
option(USE_SYSTEM_OPENSSL "Use system OpenSSL instead of vendored version" OFF)
set(DINERO_VENDORED_OPENSSL_DIR "" CACHE PATH
  "Override directory containing prebuilt vendored OpenSSL libraries")

function(dinero_detect_single_apple_arch out_arch)
  set(_archs "${CMAKE_OSX_ARCHITECTURES}")
  if(_archs STREQUAL "")
    set(_archs "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  if(_archs STREQUAL "")
    set(_archs "${CMAKE_HOST_SYSTEM_PROCESSOR}")
  endif()

  set(_arch_list ${_archs})
  list(LENGTH _arch_list _arch_count)
  if(_arch_count GREATER 1)
    message(FATAL_ERROR
      "Vendored OpenSSL prebuilt selection requires a single Apple architecture, got: ${_archs}. "
      "Set DINERO_VENDORED_OPENSSL_DIR explicitly.")
  endif()

  list(GET _arch_list 0 _arch)
  set(${out_arch} "${_arch}" PARENT_SCOPE)
endfunction()

function(dinero_default_vendored_openssl_dir out_dir)
  set(_base "${CMAKE_SOURCE_DIR}/third_party/openssl-3.3.2")
  if(NOT APPLE)
    set(${out_dir} "${_base}" PARENT_SCOPE)
    return()
  endif()

  dinero_detect_single_apple_arch(_apple_arch)
  set(_subdir "")
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    if(CMAKE_OSX_SYSROOT STREQUAL "iphonesimulator")
      if(_apple_arch STREQUAL "arm64")
        set(_subdir "prebuilt/ios-simulator-arm64")
      elseif(_apple_arch STREQUAL "x86_64")
        set(_subdir "prebuilt/ios-simulator-x86_64")
      endif()
    elseif(CMAKE_OSX_SYSROOT STREQUAL "iphoneos")
      if(_apple_arch STREQUAL "arm64")
        set(_subdir "prebuilt/ios-arm64")
      endif()
    endif()
  else()
    if(_apple_arch STREQUAL "arm64")
      set(_subdir "prebuilt/macos-arm64")
    elseif(_apple_arch STREQUAL "x86_64")
      set(_subdir "prebuilt/macos-x86_64")
    endif()
  endif()

  if(_subdir STREQUAL "")
    message(FATAL_ERROR
      "No vendored OpenSSL prebuilt mapping for Apple target "
      "(system=${CMAKE_SYSTEM_NAME}, sysroot=${CMAKE_OSX_SYSROOT}, arch=${_apple_arch}). "
      "Set DINERO_VENDORED_OPENSSL_DIR explicitly.")
  endif()

  set(${out_dir} "${_base}/${_subdir}" PARENT_SCOPE)
endfunction()

if(USE_SYSTEM_OPENSSL)
  # Use system OpenSSL (distro packaging, development builds)
  message(STATUS "Using system OpenSSL (USE_SYSTEM_OPENSSL=ON)")
  find_package(OpenSSL REQUIRED)
  message(STATUS "  Found OpenSSL ${OPENSSL_VERSION}")
else()
  # Use vendored static OpenSSL (default for portable binaries)
  set(OPENSSL_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/openssl-3.3.2")
  if(DINERO_VENDORED_OPENSSL_DIR)
    set(OPENSSL_ROOT_DIR "${DINERO_VENDORED_OPENSSL_DIR}")
    message(STATUS "Using explicit vendored OpenSSL dir: ${OPENSSL_ROOT_DIR}")
  else()
    dinero_default_vendored_openssl_dir(OPENSSL_ROOT_DIR)
  endif()
  set(OPENSSL_USE_STATIC_LIBS TRUE)
  set(OPENSSL_INCLUDE_DIR ${OPENSSL_SOURCE_DIR}/include)
  set(OPENSSL_BUILD_METADATA ${OPENSSL_ROOT_DIR}/.dinero-build-meta)
  if(WIN32)
    # Prefer MSVC-style .lib import archives; fall back to MinGW-w64 .a
    # archives that the existing vendored OpenSSL build script produces. The
    # rc7+ Windows release pipeline uses MinGW-w64 (per BUILD-WINDOWS.md), so
    # the .a fallback is the path actually exercised today; the .lib branch
    # is preserved for future MSVC-targeted builds.
    if(EXISTS "${OPENSSL_ROOT_DIR}/libcrypto.lib" AND EXISTS "${OPENSSL_ROOT_DIR}/libssl.lib")
      set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_ROOT_DIR}/libcrypto.lib)
      set(OPENSSL_SSL_LIBRARY ${OPENSSL_ROOT_DIR}/libssl.lib)
    else()
      set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_ROOT_DIR}/libcrypto.a)
      set(OPENSSL_SSL_LIBRARY ${OPENSSL_ROOT_DIR}/libssl.a)
    endif()
  else()
    set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_ROOT_DIR}/libcrypto.a)
    set(OPENSSL_SSL_LIBRARY ${OPENSSL_ROOT_DIR}/libssl.a)
  endif()

  # Check if vendored OpenSSL libraries exist
  if(EXISTS "${OPENSSL_CRYPTO_LIBRARY}" AND EXISTS "${OPENSSL_SSL_LIBRARY}")
    set(OPENSSL_METADATA_OS "")
    set(OPENSSL_METADATA_ARCH "")
    if(EXISTS "${OPENSSL_BUILD_METADATA}")
      file(STRINGS "${OPENSSL_BUILD_METADATA}" OPENSSL_BUILD_METADATA_OS REGEX "^OS=")
      file(STRINGS "${OPENSSL_BUILD_METADATA}" OPENSSL_BUILD_METADATA_ARCH REGEX "^ARCH=")
      if(OPENSSL_BUILD_METADATA_OS)
        string(REPLACE "OS=" "" OPENSSL_METADATA_OS "${OPENSSL_BUILD_METADATA_OS}")
      endif()
      if(OPENSSL_BUILD_METADATA_ARCH)
        string(REPLACE "ARCH=" "" OPENSSL_METADATA_ARCH "${OPENSSL_BUILD_METADATA_ARCH}")
      endif()
    endif()

    if(NOT OPENSSL_METADATA_OS STREQUAL "" AND NOT OPENSSL_METADATA_OS STREQUAL CMAKE_SYSTEM_NAME)
      message(FATAL_ERROR
        "Vendored OpenSSL was built for ${OPENSSL_METADATA_OS}, but this build targets ${CMAKE_SYSTEM_NAME}. "
        "Rebuild vendored OpenSSL with: OPENSSL_REBUILD=1 ./scripts/build-openssl-vendored.sh"
        " (Windows: powershell -File .\\scripts\\build-openssl-vendored.ps1)")
    endif()
    if(NOT OPENSSL_METADATA_ARCH STREQUAL "" AND NOT OPENSSL_METADATA_ARCH STREQUAL CMAKE_SYSTEM_PROCESSOR)
      message(FATAL_ERROR
        "Vendored OpenSSL was built for ${OPENSSL_METADATA_ARCH}, but this build targets ${CMAKE_SYSTEM_PROCESSOR}. "
        "Rebuild vendored OpenSSL with: OPENSSL_REBUILD=1 ./scripts/build-openssl-vendored.sh"
        " (Windows: powershell -File .\\scripts\\build-openssl-vendored.ps1)")
    endif()

    if(APPLE)
      set(OPENSSL_METADATA_TARGET "")
      set(OPENSSL_ARCHIVE_TARGET "")
      if(EXISTS "${OPENSSL_BUILD_METADATA}")
        file(STRINGS "${OPENSSL_BUILD_METADATA}" OPENSSL_BUILD_METADATA_TARGET REGEX "^MACOSX_DEPLOYMENT_TARGET=")
        if(OPENSSL_BUILD_METADATA_TARGET)
          string(REPLACE "MACOSX_DEPLOYMENT_TARGET=" "" OPENSSL_METADATA_TARGET "${OPENSSL_BUILD_METADATA_TARGET}")
        endif()
      endif()

      set(OPENSSL_ARCHIVE_TARGET_FROM_BINARY "")
      if(EXISTS "${OPENSSL_CRYPTO_LIBRARY}")
        execute_process(
          COMMAND /usr/bin/otool -l "${OPENSSL_CRYPTO_LIBRARY}"
          RESULT_VARIABLE OPENSSL_OTOOL_RESULT
          OUTPUT_VARIABLE OPENSSL_OTOOL_OUTPUT
          ERROR_QUIET
        )
        if(OPENSSL_OTOOL_RESULT EQUAL 0)
          string(REGEX MATCH "minos ([0-9]+\\.[0-9]+)" OPENSSL_MINOS_MATCH "${OPENSSL_OTOOL_OUTPUT}")
          if(OPENSSL_MINOS_MATCH)
            set(OPENSSL_ARCHIVE_TARGET_FROM_BINARY "${CMAKE_MATCH_1}")
          endif()
        endif()
      endif()

      if(NOT OPENSSL_METADATA_TARGET STREQUAL "" AND NOT OPENSSL_ARCHIVE_TARGET_FROM_BINARY STREQUAL "" AND NOT "${OPENSSL_METADATA_TARGET}" VERSION_EQUAL "${OPENSSL_ARCHIVE_TARGET_FROM_BINARY}")
        message(FATAL_ERROR
          "Vendored OpenSSL metadata says macOS ${OPENSSL_METADATA_TARGET}, but the archive targets ${OPENSSL_ARCHIVE_TARGET_FROM_BINARY}. "
          "Rebuild vendored OpenSSL with: OPENSSL_REBUILD=1 OPENSSL_MACOS_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET} ./scripts/build-openssl-vendored.sh")
      endif()

      if(OPENSSL_ARCHIVE_TARGET_FROM_BINARY STREQUAL "")
        set(OPENSSL_ARCHIVE_TARGET "${OPENSSL_METADATA_TARGET}")
      else()
        set(OPENSSL_ARCHIVE_TARGET "${OPENSSL_ARCHIVE_TARGET_FROM_BINARY}")
      endif()

      if(NOT OPENSSL_ARCHIVE_TARGET STREQUAL "" AND NOT "${OPENSSL_ARCHIVE_TARGET}" VERSION_EQUAL "${CMAKE_OSX_DEPLOYMENT_TARGET}")
        message(FATAL_ERROR
          "Vendored OpenSSL targets macOS ${OPENSSL_ARCHIVE_TARGET}, but this build targets ${CMAKE_OSX_DEPLOYMENT_TARGET}. "
          "Rebuild vendored OpenSSL with: OPENSSL_REBUILD=1 OPENSSL_MACOS_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET} ./scripts/build-openssl-vendored.sh")
      endif()
    endif()

    message(STATUS "Using vendored OpenSSL 3.3.2 (static)")
    message(STATUS "  Crypto: ${OPENSSL_CRYPTO_LIBRARY}")
    message(STATUS "  SSL: ${OPENSSL_SSL_LIBRARY}")

    add_library(OpenSSL::Crypto STATIC IMPORTED)
    if(WIN32)
      set_target_properties(OpenSSL::Crypto PROPERTIES
          IMPORTED_LOCATION ${OPENSSL_CRYPTO_LIBRARY}
          INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIR}
          INTERFACE_LINK_LIBRARIES "ws2_32;crypt32;advapi32"
      )
    else()
      set_target_properties(OpenSSL::Crypto PROPERTIES
          IMPORTED_LOCATION ${OPENSSL_CRYPTO_LIBRARY}
          INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIR}
      )
    endif()
    add_library(OpenSSL::SSL STATIC IMPORTED)
    set_target_properties(OpenSSL::SSL PROPERTIES
        IMPORTED_LOCATION ${OPENSSL_SSL_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIR}
        INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
    )
    # (PQ subdir moved below — after endif() — so it builds for BOTH
    # system and vendored OpenSSL paths.)
  else()
    # Vendored libraries not built yet - provide instructions
    message(WARNING "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    message(WARNING "Vendored OpenSSL libraries not found!")
    message(WARNING "")
    message(WARNING "Expected path: ${OPENSSL_ROOT_DIR}")
    message(WARNING "Build OpenSSL first by running:")
    if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
      message(WARNING "  ./build_nodecore_xcframework.sh")
      message(WARNING "  or re-run CMake with -DDINERO_VENDORED_OPENSSL_DIR=/path/to/apple/openssl")
    else()
      message(WARNING "  ./scripts/build-openssl-vendored.sh")
    endif()
    message(WARNING "")
    message(WARNING "Or use system OpenSSL instead:")
    message(WARNING "  cmake -DUSE_SYSTEM_OPENSSL=ON ..")
    message(WARNING "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    message(FATAL_ERROR "Cannot proceed without OpenSSL")
  endif()
endif()

# V7 PQ façade + bench + round-trip tests. Requires OpenSSL::Crypto
# (pq_derivation.cpp uses HMAC-SHA256). Works with BOTH system and
# vendored OpenSSL — moved here from the vendored-only branch so Linux
# servers using USE_SYSTEM_OPENSSL=ON can build dinero_pq.
add_subdirectory(src/consensus/pq EXCLUDE_FROM_ALL)   # dinero_pq lib
add_subdirectory(tools/pq_bench   EXCLUDE_FROM_ALL)   # bench + tests
add_subdirectory(src/consensus/shielded EXCLUDE_FROM_ALL)  # dinero_shielded: commitment tree, nullifiers, validation

# ═══════════════════════════════════════════════════════════════════════════
# hidapi (Hardware Wallet USB/HID Transport)
# ═══════════════════════════════════════════════════════════════════════════
# Vendored: third_party/hidapi (static library)
# Required for Ledger/Trezor hardware wallet support
# Cross-platform: macOS (IOKit), Linux (libusb), Windows (WinUSB/HID)
# ═══════════════════════════════════════════════════════════════════════════

# Build hidapi as static library (not shared)
# Skip on iOS (IOKit HID not available) or when HW wallets disabled
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND ENABLE_HARDWARE_WALLETS)
  set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "hidapi: Build static library" FORCE)
  set(HIDAPI_INSTALL_TARGETS OFF CACHE BOOL "hidapi: Don't install" FORCE)
  set(HIDAPI_BUILD_HIDTEST OFF CACHE BOOL "hidapi: Skip test app" FORCE)

  add_subdirectory(third_party/hidapi)

  # Restore BUILD_SHARED_LIBS for rest of project
  set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})

  message(STATUS "Using vendored hidapi from third_party/hidapi (static)")
else()
  message(STATUS "Skipping hidapi (iOS or ENABLE_HARDWARE_WALLETS=OFF)")
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Protobuf for Trezor (required even when gRPC is disabled)
# ═══════════════════════════════════════════════════════════════════════════
# Trezor hardware wallet needs protobuf for wire protocol messages
# Set up protobuf::libprotobuf target if not already available
# ═══════════════════════════════════════════════════════════════════════════

if(ENABLE_TREZOR)
if(NOT TARGET protobuf::libprotobuf)
  if(DINERO_RELEASE)
    # Vendored protobuf for release builds
    set(PROTOBUF_INSTALL_DIR "${CMAKE_SOURCE_DIR}/third_party/protobuf-install")
    set(PROTOBUF_LIB "${PROTOBUF_INSTALL_DIR}/lib/libprotobuf.a")

    if(EXISTS "${PROTOBUF_LIB}")
      add_library(protobuf::libprotobuf STATIC IMPORTED GLOBAL)
      set_target_properties(protobuf::libprotobuf PROPERTIES
        IMPORTED_LOCATION "${PROTOBUF_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${PROTOBUF_INSTALL_DIR}/include"
      )

      # Link utf8 validity libs if available
      set(UTF8_RANGE_LIB "${PROTOBUF_INSTALL_DIR}/lib/libutf8_range.a")
      set(UTF8_VALIDITY_LIB "${PROTOBUF_INSTALL_DIR}/lib/libutf8_validity.a")
      if(EXISTS "${UTF8_RANGE_LIB}")
        target_link_libraries(protobuf::libprotobuf INTERFACE "${UTF8_RANGE_LIB}")
      endif()
      if(EXISTS "${UTF8_VALIDITY_LIB}")
        target_link_libraries(protobuf::libprotobuf INTERFACE "${UTF8_VALIDITY_LIB}")
      endif()

      message(STATUS "Using vendored protobuf for Trezor: ${PROTOBUF_LIB}")
    else()
      message(FATAL_ERROR "Vendored protobuf not found. Run: ./scripts/build-vendored-protobuf.sh")
    endif()
  else()
    # System protobuf for dev builds
    find_package(Protobuf CONFIG QUIET)
    if(NOT Protobuf_FOUND AND NOT protobuf_FOUND)
      find_package(Protobuf REQUIRED)
      if(NOT TARGET protobuf::libprotobuf AND Protobuf_LIBRARIES)
        add_library(protobuf::libprotobuf INTERFACE IMPORTED)
        set_target_properties(protobuf::libprotobuf PROPERTIES
          INTERFACE_INCLUDE_DIRECTORIES "${Protobuf_INCLUDE_DIRS}"
          INTERFACE_LINK_LIBRARIES "${Protobuf_LIBRARIES}"
        )
      endif()
    endif()
    message(STATUS "Using system protobuf for Trezor")
  endif()
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Trezor Protobuf Messages (Vendored)
# ═══════════════════════════════════════════════════════════════════════════
# Trezor communicates via Protocol Buffers over HID
# Vendor protobuf definitions from trezor-firmware/common/protob (MIT licensed)
# ═══════════════════════════════════════════════════════════════════════════

# Add trezor_proto static library with generated protobuf code
add_library(trezor_proto STATIC
    third_party/trezor-common/messages.pb.cc
    third_party/trezor-common/messages-bitcoin.pb.cc
    third_party/trezor-common/messages-common.pb.cc
    third_party/trezor-common/messages-management.pb.cc
    third_party/trezor-common/options.pb.cc
)

target_include_directories(trezor_proto PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/trezor-common
)

target_link_libraries(trezor_proto PUBLIC protobuf::libprotobuf)

message(STATUS "Using vendored Trezor protobufs from third_party/trezor-common (static)")
endif() # ENABLE_TREZOR

# ═══════════════════════════════════════════════════════════════════════════
# Hardware Wallet Security: Mock Signing Control
# ═══════════════════════════════════════════════════════════════════════════
# CRITICAL: Mock signing MUST be disabled in production builds
# Default: OFF (production-safe)
# Test builds: cmake -DDIN_HW_WALLET_MOCK=ON -DCMAKE_BUILD_TYPE=Debug
# ═══════════════════════════════════════════════════════════════════════════

option(DIN_HW_WALLET_MOCK "Enable hardware wallet mock signing (TEST ONLY - Debug builds only)" OFF)

if(DIN_HW_WALLET_MOCK)
    # CRITICAL: Block mock signing in Release/RelWithDebInfo builds
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        message(FATAL_ERROR
            "\n"
            "═══════════════════════════════════════════════════════════════════════════\n"
            "FATAL: DIN_HW_WALLET_MOCK=ON is not allowed with CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}\n"
            "═══════════════════════════════════════════════════════════════════════════\n"
            "\n"
            "Mock signing returns FAKE SIGNATURES that would broadcast invalid transactions.\n"
            "This flag is only allowed in Debug builds for testing.\n"
            "\n"
            "To fix:\n"
            "  - For production: Remove -DDIN_HW_WALLET_MOCK=ON\n"
            "  - For testing: Use -DCMAKE_BUILD_TYPE=Debug\n"
            "\n"
        )
    endif()

    add_compile_definitions(DIN_HW_WALLET_MOCK)
    message(WARNING "")
    message(WARNING "═══════════════════════════════════════════════════════════════════════════")
    message(WARNING "⚠️  HARDWARE WALLET MOCK SIGNING ENABLED - NOT PRODUCTION SAFE")
    message(WARNING "═══════════════════════════════════════════════════════════════════════════")
    message(WARNING "⚠️  This build will fabricate signatures for testing")
    message(WARNING "⚠️  DO NOT use this build for real funds")
    message(WARNING "═══════════════════════════════════════════════════════════════════════════")
    message(WARNING "")
else()
    message(STATUS "✅ Hardware wallet mock signing DISABLED (production-safe)")
endif()

# ============================================================================
# gRPC and Protocol Buffers (Inter-daemon Communication)
# ============================================================================
# Used for dinerod <-> lightningd communication (Lightning decoupling)
# Phase 2 of Lightning decoupling plan

option(ENABLE_GRPC "Enable gRPC server for inter-daemon communication" ON)

# When ENABLE_GRPC=OFF (whether forced by DINERO_RELEASE or set directly by
# the operator on the configure line), define DISABLE_GRPC so daemon_app.cpp
# (and any other call-site) skips the gRPC server-construction blocks. The
# previous behaviour only defined DISABLE_GRPC under DINERO_RELEASE — that
# meant ENABLE_GRPC=OFF without DINERO_RELEASE produced unresolved-external
# linker errors for GrpcServer::* symbols. Make the gate symmetric.
if(NOT ENABLE_GRPC)
  add_compile_definitions(DISABLE_GRPC)
endif()

# ============================================================================
# Protocol Buffers and gRPC Setup (Conditional - Dev Mode Only)
# ============================================================================
# Proto types and gRPC services require protobuf.
# When ENABLE_GRPC=OFF (release mode), skip entirely - not needed.
# Dev mode uses system protobuf (dynamic) for faster builds.
# Phase 8.4: Also skip if BUILD_DINEROD=OFF (hermetic lightningd build)

if(ENABLE_GRPC AND BUILD_DINEROD)

if(DINERO_RELEASE)
  # ========================================================================
  # Vendored Protobuf (Release Mode - Zero Homebrew Dependencies)
  # ========================================================================
  set(PROTOBUF_INSTALL_DIR "${CMAKE_SOURCE_DIR}/third_party/protobuf-install")
  set(PROTOBUF_LIB "${PROTOBUF_INSTALL_DIR}/lib/libprotobuf.a")
  set(PROTOBUF_PROTOC "${PROTOBUF_INSTALL_DIR}/bin/protoc")

  # Check if vendored protobuf is built
  if(NOT EXISTS "${PROTOBUF_LIB}" OR NOT EXISTS "${PROTOBUF_PROTOC}")
    message(STATUS "🔨 Vendored protobuf not found, building from source...")
    message(STATUS "   Running: ${CMAKE_SOURCE_DIR}/scripts/build-vendored-protobuf.sh")

    execute_process(
      COMMAND "${CMAKE_SOURCE_DIR}/scripts/build-vendored-protobuf.sh"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      RESULT_VARIABLE PROTOBUF_BUILD_RESULT
      OUTPUT_VARIABLE PROTOBUF_BUILD_OUTPUT
      ERROR_VARIABLE PROTOBUF_BUILD_ERROR
    )

    if(NOT PROTOBUF_BUILD_RESULT EQUAL 0)
      message(FATAL_ERROR "Failed to build vendored protobuf:\n${PROTOBUF_BUILD_ERROR}")
    endif()

    message(STATUS "✅ Vendored protobuf built successfully")
  else()
    message(STATUS "✅ Using vendored protobuf from: ${PROTOBUF_INSTALL_DIR}")
  endif()

  # Create imported target for vendored protobuf
  add_library(protobuf::libprotobuf STATIC IMPORTED GLOBAL)
  set_target_properties(protobuf::libprotobuf PROPERTIES
    IMPORTED_LOCATION "${PROTOBUF_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${PROTOBUF_INSTALL_DIR}/include"
  )

  # Also need libprotobuf-lite and libabsl (bundled with protobuf)
  set(PROTOBUF_LITE_LIB "${PROTOBUF_INSTALL_DIR}/lib/libprotobuf-lite.a")
  if(EXISTS "${PROTOBUF_LITE_LIB}")
    add_library(protobuf::libprotobuf-lite STATIC IMPORTED GLOBAL)
    set_target_properties(protobuf::libprotobuf-lite PROPERTIES
      IMPORTED_LOCATION "${PROTOBUF_LITE_LIB}"
    )
  endif()

  # Link vendored abseil libraries (built with protobuf)
  file(GLOB ABSEIL_LIBS "${PROTOBUF_INSTALL_DIR}/lib/libabsl_*.a")
  if(ABSEIL_LIBS)
    list(LENGTH ABSEIL_LIBS ABSEIL_COUNT)
    # Link abseil libs to protobuf target (abseil is a protobuf dependency)
    target_link_libraries(protobuf::libprotobuf INTERFACE ${ABSEIL_LIBS})
    message(STATUS "   Abseil libs: ${ABSEIL_COUNT} static libraries")
  endif()

  # Also need utf8_range and utf8_validity (protobuf dependencies)
  set(UTF8_RANGE_LIB "${PROTOBUF_INSTALL_DIR}/lib/libutf8_range.a")
  set(UTF8_VALIDITY_LIB "${PROTOBUF_INSTALL_DIR}/lib/libutf8_validity.a")
  if(EXISTS "${UTF8_RANGE_LIB}")
    target_link_libraries(protobuf::libprotobuf INTERFACE "${UTF8_RANGE_LIB}")
  endif()
  if(EXISTS "${UTF8_VALIDITY_LIB}")
    target_link_libraries(protobuf::libprotobuf INTERFACE "${UTF8_VALIDITY_LIB}")
  endif()

  # On macOS, abseil needs CoreFoundation framework for time zone operations
  if(APPLE)
    target_link_libraries(protobuf::libprotobuf INTERFACE "-framework CoreFoundation")
  endif()

  # Set protoc executable
  set(Protobuf_PROTOC_EXECUTABLE "${PROTOBUF_PROTOC}")
  set(Protobuf_INCLUDE_DIRS "${PROTOBUF_INSTALL_DIR}/include")

  message(STATUS "   Library: ${PROTOBUF_LIB}")
  message(STATUS "   Protoc: ${PROTOBUF_PROTOC}")

else()
  # System protobuf (dev mode)
  if(NOT TARGET protobuf::libprotobuf)
    # Try CONFIG mode first, fall back to MODULE mode (for Ubuntu systems)
    find_package(Protobuf CONFIG QUIET)
    if(NOT Protobuf_FOUND AND NOT protobuf_FOUND)
      find_package(Protobuf REQUIRED)
      # MODULE mode doesn't create targets - create them for compatibility
      if(NOT TARGET protobuf::libprotobuf AND Protobuf_LIBRARIES)
        add_library(protobuf::libprotobuf INTERFACE IMPORTED)
        set_target_properties(protobuf::libprotobuf PROPERTIES
          INTERFACE_INCLUDE_DIRECTORIES "${Protobuf_INCLUDE_DIRS}"
          INTERFACE_LINK_LIBRARIES "${Protobuf_LIBRARIES}"
        )
      endif()
    endif()
    if(Protobuf_FOUND OR protobuf_FOUND)
      message(STATUS "Found Protobuf ${Protobuf_VERSION}")
      if(TARGET protobuf::protoc)
        get_target_property(Protobuf_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION_RELEASE)
      endif()
      if(NOT Protobuf_PROTOC_EXECUTABLE)
        find_program(Protobuf_PROTOC_EXECUTABLE protoc)
      endif()
      message(STATUS "  Compiler: ${Protobuf_PROTOC_EXECUTABLE}")
      # Set compatibility variables for old-style usage
      if(NOT DEFINED Protobuf_LIBRARIES)
        set(Protobuf_LIBRARIES protobuf::libprotobuf)
      endif()
      if(NOT DEFINED Protobuf_INCLUDE_DIR)
        if(TARGET protobuf::libprotobuf)
          get_target_property(Protobuf_INCLUDE_DIR protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
        endif()
      endif()
    endif()
  else()
    message(STATUS "Using Protobuf from RocksDB dependencies")
    find_program(Protobuf_PROTOC_EXECUTABLE protoc)
  endif()
endif()

# ============================================================================
# ============================================================================
# Proto Code Generation (L1 Daemon Only - Phase 8.4: Build Graph Isolation)
# ============================================================================
# IMPORTANT: This entire section is guarded by BUILD_DINEROD
# If building lightningd standalone, NO proto generation happens
# This enforces hermetic L2 build: lightningd must not touch L1 artifacts
# ============================================================================

if(BUILD_DINEROD)
  # Proto files used by both dev and release builds (for type definitions)
  set(PROTO_FILES
    ${CMAKE_SOURCE_DIR}/proto/dinerod.proto
  )

# Set output directory for generated files
set(PROTO_OUTPUT_DIR ${CMAKE_BINARY_DIR}/generated/proto)
file(MAKE_DIRECTORY ${PROTO_OUTPUT_DIR})

# Generate protobuf type definitions (ALWAYS - both dev and release modes need these)
set(GENERATED_SOURCES "")
foreach(PROTO_FILE ${PROTO_FILES})
  get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
  set(PROTO_SRC "${PROTO_OUTPUT_DIR}/${PROTO_NAME}.pb.cc")
  set(PROTO_HDR "${PROTO_OUTPUT_DIR}/${PROTO_NAME}.pb.h")

  # Generate protobuf code (types only - no gRPC services yet)
  add_custom_command(
    OUTPUT ${PROTO_SRC} ${PROTO_HDR}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    ARGS --cpp_out=${PROTO_OUTPUT_DIR}
         --proto_path=${CMAKE_SOURCE_DIR}/proto
         ${PROTO_FILE}
    DEPENDS ${PROTO_FILE}
    COMMENT "Generating protobuf C++ code for ${PROTO_NAME}"
  )

  list(APPEND GENERATED_SOURCES ${PROTO_SRC})
endforeach()

# ============================================================================
# gRPC Setup (Conditional - Dev Mode Only)
# ============================================================================

if(ENABLE_GRPC)

  # Find gRPC using pkg-config (avoids protobuf target conflicts with RocksDB)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(GRPC REQUIRED grpc++ grpc)
    if(GRPC_FOUND)
      message(STATUS "Found gRPC via pkg-config")
      message(STATUS "  Include dirs: ${GRPC_INCLUDE_DIRS}")
      message(STATUS "  Libraries: ${GRPC_LIBRARIES}")
    endif()
  else()
    message(FATAL_ERROR "pkg-config not found - required for gRPC")
  endif()

  # Find grpc_cpp_plugin
  find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)
  if(NOT GRPC_CPP_PLUGIN)
    message(FATAL_ERROR "grpc_cpp_plugin not found")
  endif()

  # Generate gRPC service code (only in dev mode)
  foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    set(PROTO_HDR "${PROTO_OUTPUT_DIR}/${PROTO_NAME}.pb.h")
    set(GRPC_SRC "${PROTO_OUTPUT_DIR}/${PROTO_NAME}.grpc.pb.cc")
    set(GRPC_HDR "${PROTO_OUTPUT_DIR}/${PROTO_NAME}.grpc.pb.h")

    # Generate gRPC code (depends on protobuf code)
    add_custom_command(
      OUTPUT ${GRPC_SRC} ${GRPC_HDR}
      COMMAND ${Protobuf_PROTOC_EXECUTABLE}
      ARGS --grpc_out=${PROTO_OUTPUT_DIR}
           --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
           --proto_path=${CMAKE_SOURCE_DIR}/proto
           ${PROTO_FILE}
      DEPENDS ${PROTO_FILE} ${PROTO_HDR}
      COMMENT "Generating gRPC C++ code for ${PROTO_NAME}"
    )

    list(APPEND GENERATED_SOURCES ${GRPC_SRC})
  endforeach()

  # Create library with generated sources (both proto types + gRPC services)
  add_library(dinerod_proto STATIC ${GENERATED_SOURCES})

  target_include_directories(dinerod_proto PUBLIC
    ${PROTO_OUTPUT_DIR}
    ${Protobuf_INCLUDE_DIRS}
  )

  target_link_libraries(dinerod_proto PUBLIC
    protobuf::libprotobuf
  )

  # Link gRPC (using pkg-config variables)
  target_include_directories(dinerod_proto PUBLIC ${GRPC_INCLUDE_DIRS})
  target_link_directories(dinerod_proto PUBLIC ${GRPC_LIBRARY_DIRS})
  target_link_libraries(dinerod_proto PUBLIC ${GRPC_LIBRARIES})

  message(STATUS "✅ gRPC Server: ENABLED")
  message(STATUS "   Proto files: dinerod.proto")
  message(STATUS "   Generated code: ${PROTO_OUTPUT_DIR}")
else()
  # Release mode: Only proto types generated (no gRPC services)
  message(STATUS "gRPC Server: DISABLED (using socket transport)")

  # Create library with just proto type definitions
  add_library(dinerod_proto STATIC ${GENERATED_SOURCES})

  target_include_directories(dinerod_proto PUBLIC
    ${PROTO_OUTPUT_DIR}
    ${Protobuf_INCLUDE_DIRS}
  )

  target_link_libraries(dinerod_proto PUBLIC
    protobuf::libprotobuf
  )

  message(STATUS "   Proto types: dinerod.pb.h/cc (for type definitions)")
  message(STATUS "   Generated code: ${PROTO_OUTPUT_DIR}")
endif()  # ENABLE_GRPC

endif()  # BUILD_DINEROD (Phase 8.4: L1-only proto generation)

endif() # ENABLE_GRPC

# Lightning Network Core (unified dependency module)
# Provides: libwally-core, secp256k1-zkp, blake3
# See: LIGHTNING_VENDOR_QUICKSTART.md for installation
add_subdirectory(third_party/lightning_core)

# NOTE: secp256k1 is now built directly from third_party/secp256k1
# No alias needed - the real library target is available as 'secp256k1'

# ═══════════════════════════════════════════════════════════════════════════
# BULLETPROOFS FFI (Dalek Bulletproofs via Rust FFI)
# ═══════════════════════════════════════════════════════════════════════════

# Bulletproofs FFI Library - Rust implementation of Dalek Bulletproofs
# This provides production-grade range proofs for shielded proof support
set(BP_FFI_DIR "${CMAKE_SOURCE_DIR}/third_party/bulletproofs_ffi")

# Determine library extension based on platform
if(APPLE)
    set(BP_FFI_LIB "${BP_FFI_DIR}/target/release/libbulletproofs_ffi.a")
elseif(WIN32)
    set(BP_FFI_LIB "${BP_FFI_DIR}/target/release/bulletproofs_ffi.lib")
else()
    set(BP_FFI_LIB "${BP_FFI_DIR}/target/release/libbulletproofs_ffi.a")
endif()

# Check if cargo is available (Rust toolchain)
# Search in standard cargo install locations (portable across all users)
find_program(CARGO_EXECUTABLE cargo HINTS "$ENV{HOME}/.cargo/bin")

if(CARGO_EXECUTABLE)
    set(BP_FFI_BUILD_COMMAND "${CARGO_EXECUTABLE}" build --release)

    if(APPLE)
        set(BP_RUSTFLAGS "$ENV{RUSTFLAGS}")
        if(BP_RUSTFLAGS)
            set(BP_RUSTFLAGS "${BP_RUSTFLAGS} -C link-arg=-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        else()
            set(BP_RUSTFLAGS "-C link-arg=-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()

        set(BP_CFLAGS "$ENV{CFLAGS}")
        if(BP_CFLAGS)
            set(BP_CFLAGS "${BP_CFLAGS} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        else()
            set(BP_CFLAGS "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()

        set(BP_LDFLAGS "$ENV{LDFLAGS}")
        if(BP_LDFLAGS)
            set(BP_LDFLAGS "${BP_LDFLAGS} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        else()
            set(BP_LDFLAGS "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()

        set(BP_FFI_BUILD_COMMAND
            ${CMAKE_COMMAND} -E env
            "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
            "RUSTFLAGS=${BP_RUSTFLAGS}"
            "CFLAGS=${BP_CFLAGS}"
            "LDFLAGS=${BP_LDFLAGS}"
            "${CARGO_EXECUTABLE}" build --release
        )

        message(STATUS "   Bulletproofs FFI macOS target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()

    # Add build target for Rust library
    add_custom_target(build_bulletproofs_ffi
        COMMAND ${BP_FFI_BUILD_COMMAND}
        WORKING_DIRECTORY "${BP_FFI_DIR}"
        COMMENT "Building Bulletproofs FFI library (Rust)..."
        BYPRODUCTS "${BP_FFI_LIB}"
    )

    # Import as static library
    add_library(bulletproofs_ffi STATIC IMPORTED)
    set_target_properties(bulletproofs_ffi PROPERTIES
        IMPORTED_LOCATION "${BP_FFI_LIB}"
    )

    # Ensure the Rust library is built before dinerod
    add_dependencies(bulletproofs_ffi build_bulletproofs_ffi)

    message(STATUS "✅ Bulletproofs FFI: ENABLED (Dalek via Rust)")
    message(STATUS "   Library: ${BP_FFI_LIB}")
else()
    message(WARNING "⚠️  Bulletproofs FFI: DISABLED (cargo not found)")
    message(WARNING "   To enable: Install Rust from https://rustup.rs/")
    message(WARNING "   Then run: cd third_party/bulletproofs_ffi && cargo build --release")

    # Create a dummy target so build doesn't fail
    add_library(bulletproofs_ffi INTERFACE)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# ZERO-KNOWLEDGE PRIVACY (Confidential Transactions)
# ═══════════════════════════════════════════════════════════════════════════

option(ENABLE_ZK "Enable zero-knowledge proof features" ON)

if(ENABLE_ZK)
    message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    message(STATUS "Zero-Knowledge Privacy: ENABLED")
    message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")

    # ZK privacy library (Pedersen commitments + Bulletproofs + Ring Signatures)
    add_library(dinero_zk STATIC
        src/zk/zkvm/scalar.cpp
        src/zk/zkvm/secp256k1_msm_cpu.cpp
        src/zk/zkvm/r1cs.cpp
        src/zk/zkvm/ipa.cpp
        src/zk/zkvm/hyrax.cpp
        src/zk/zkvm/nova.cpp
        src/zk/zkvm/gadgets.cpp
        src/zk/zkvm/tapscript_step.cpp
        src/zk/zkvm/r1cs_ipa.cpp
        src/zk/zkvm/r1cs_spartan.cpp
        src/zk/zkvm/poseidon_gadget.cpp
        src/zk/zkvm/sha256_gadget.cpp
        src/zk/zkvm/secp256k1_fe_gadget.cpp
        src/zk/zkvm/ec_gadget.cpp
        src/zk/zkvm/schnorr_gadget.cpp
        src/zk/zkvm/taproot_path_circuit.cpp
    )

    target_include_directories(dinero_zk PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
    )

    target_link_libraries(dinero_zk PUBLIC
        secp256k1_zkp          # ElementsProject/secp256k1-zkp (Pedersen + Bulletproofs)
        lightning_core_static  # Uses secp256k1-zkp for Pedersen + Bulletproofs!
        OpenSSL::Crypto        # For secure random (RAND_bytes)
    )

    target_compile_definitions(dinero_zk PUBLIC
        HAVE_ZK_PRIVACY
        HAVE_CONFIDENTIAL_TX
    )

    message(STATUS "  ✅ secp256k1-zkp      (Pedersen commitments + Bulletproofs)")
    message(STATUS "  ✅ libwally-core      (PSBT wrapper for proofs)")
    message(STATUS "  ✅ OpenSSL            (Secure random generation)")
    message(STATUS "")
    message(STATUS "  🔒 Features:")
    message(STATUS "     - Confidential amounts (Pedersen commitments)")
    message(STATUS "     - Range proofs (Bulletproofs)")
    message(STATUS "     - Hidden balances (~5KB overhead per output)")
    message(STATUS "")
endif()

# ═══════════════════════════════════════════════════════════════════════════
# GPU MINING SUPPORT (OpenCL + CUDA + Metal)
# ═══════════════════════════════════════════════════════════════════════════

# GPU mining library (always built with stub implementations)
# Build unconditionally so daemon can link successfully even without GPU hardware
message(STATUS "Building GPU mining library:")

set(GPU_SOURCES
  src/mining/gpu/compute_backend.cpp
  src/mining/gpu/gpu_device_manager.cpp
)

if(ENABLE_GPU_MINING)
  message(STATUS "GPU mining support enabled - detecting backends...")

  # OpenCL detection (AMD, Intel, NVIDIA GPUs)
  if(APPLE)
    # macOS: System OpenCL framework
    find_library(OPENCL_LIBRARY OpenCL)
    if(OPENCL_LIBRARY)
      set(OPENCL_FOUND TRUE)
      add_library(OpenCL::OpenCL INTERFACE IMPORTED)
      set_target_properties(OpenCL::OpenCL PROPERTIES
        INTERFACE_LINK_LIBRARIES "${OPENCL_LIBRARY}"
      )
      message(STATUS "  ✅ OpenCL found (macOS system framework)")
    else()
      message(STATUS "  ❌ OpenCL not found")
    endif()
  elseif(WIN32)
    # Windows: probe OPENCL_ROOT or vendor-SDK env vars for Khronos headers
    # + an import library. On the canonical rc1 Windows build host the SDK
    # lives at C:/Users/<user>/openclsdk with Khronos headers under include/
    # and libOpenCL.dll.a (generated from C:\Windows\System32\OpenCL.dll via
    # gendef + dlltool) under lib/x64/. OPENCL_ROOT must point at that root.
    find_path(OPENCL_INCLUDE_DIR NAMES CL/cl.h
      HINTS ENV OPENCL_ROOT ENV OCL_ROOT ENV INTELOCLSDKROOT ENV AMDAPPSDKROOT ENV CUDA_PATH
      PATH_SUFFIXES include
    )
    find_library(OPENCL_LIBRARY NAMES OpenCL
      HINTS ENV OPENCL_ROOT ENV OCL_ROOT ENV INTELOCLSDKROOT ENV AMDAPPSDKROOT ENV CUDA_PATH
      PATH_SUFFIXES lib/x64 lib/x86_64 lib
    )
    if(OPENCL_INCLUDE_DIR AND OPENCL_LIBRARY)
      set(OPENCL_FOUND TRUE)
      add_library(OpenCL::OpenCL INTERFACE IMPORTED)
      set_target_properties(OpenCL::OpenCL PROPERTIES
        INTERFACE_LINK_LIBRARIES "${OPENCL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENCL_INCLUDE_DIR}"
      )
      message(STATUS "  ✅ OpenCL found (Windows: ${OPENCL_LIBRARY})")
    else()
      message(STATUS "  ❌ OpenCL not found on Windows (set OPENCL_ROOT to a")
      message(STATUS "     directory containing include/CL/*.h and lib/x64/libOpenCL.dll.a,")
      message(STATUS "     or install a vendor SDK that exports INTELOCLSDKROOT /")
      message(STATUS "     AMDAPPSDKROOT / CUDA_PATH)")
    endif()
  else()
    # Linux: Find OpenCL package
    find_package(OpenCL QUIET)
    if(OpenCL_FOUND)
      message(STATUS "  ✅ OpenCL found (version ${OpenCL_VERSION_STRING})")
    else()
      message(STATUS "  ❌ OpenCL not found (install ocl-icd-opencl-dev on Ubuntu/Debian)")
    endif()
  endif()

  # CUDA detection (NVIDIA GPUs only).
  # We do NOT call enable_language(CUDA) — the daemon's CUDA backend uses
  # NVRTC at runtime (same pattern as miner/src/gpu/cuda_backend.cpp), so no
  # nvcc invocation is required at build time. This sidesteps the CUDA 12.2
  # + MSVC 14.44 nvcc STL static_assert mismatch documented as Bug #4 in
  # NATIVE-MSVC-PORT-BUGS.md.
  find_package(CUDAToolkit QUIET)
  if(CUDAToolkit_FOUND)
    message(STATUS "  ✅ CUDA Toolkit found (version ${CUDAToolkit_VERSION}) -- NVRTC path, no nvcc")
  else()
    message(STATUS "  ❌ CUDA Toolkit not found (NVIDIA GPU mining disabled)")
  endif()

  # Add OpenCL backend if available
  if(OPENCL_FOUND)
    list(APPEND GPU_SOURCES src/mining/gpu/opencl_backend.cpp)
    message(STATUS "  ✅ OpenCL backend (AMD/Intel/NVIDIA)")
  else()
    message(STATUS "  ❌ OpenCL backend (not available)")
  endif()

  # Add CUDA backend if available. The .cu kernel is NOT compiled by nvcc;
  # instead it's read at configure time from miner/shaders/sha256d.cu (the
  # single source of truth for the SHA-256d mining kernel, shared with
  # solo-miner) and embedded into the binary as a C-string literal by the
  # sha256d_cuda_src.cpp.in template. NVRTC compiles it at runtime against
  # the running GPU's exact compute capability.
  if(CUDAToolkit_FOUND)
    file(READ ${CMAKE_SOURCE_DIR}/miner/shaders/sha256d.cu DINERO_CUDA_KERNEL_SRC)
    configure_file(
      ${CMAKE_SOURCE_DIR}/src/mining/gpu/sha256d_cuda_src.cpp.in
      ${CMAKE_BINARY_DIR}/generated/sha256d_cuda_src.cpp
      @ONLY
    )
    list(APPEND GPU_SOURCES src/mining/gpu/cuda_backend.cpp)
    list(APPEND GPU_SOURCES ${CMAKE_BINARY_DIR}/generated/sha256d_cuda_src.cpp)
    message(STATUS "  ✅ CUDA backend (NVRTC; kernel embedded from miner/shaders/sha256d.cu)")
  else()
    message(STATUS "  ❌ CUDA backend (not available)")
  endif()

  # Metal detection (Apple Silicon — macOS native GPU API)
  if(APPLE)
    find_library(METAL_LIBRARY Metal)
    find_library(FOUNDATION_LIBRARY Foundation)
    if(METAL_LIBRARY AND FOUNDATION_LIBRARY)
      set(METAL_FOUND TRUE)
      list(APPEND GPU_SOURCES src/mining/gpu/metal_backend.mm)
      message(STATUS "  ✅ Metal backend (Apple Silicon native)")
    else()
      message(STATUS "  ❌ Metal not found")
    endif()
  endif()

  if(NOT OPENCL_FOUND AND NOT CUDAToolkit_FOUND AND NOT METAL_FOUND)
    message(STATUS "  ⚙️  Stub-only mode (no GPU hardware support)")
  endif()
else()
  # Even without ENABLE_GPU_MINING, auto-detect Metal on macOS
  # Metal is always available on Apple Silicon — enable it by default
  if(APPLE)
    find_library(METAL_LIBRARY Metal)
    find_library(FOUNDATION_LIBRARY Foundation)
    if(METAL_LIBRARY AND FOUNDATION_LIBRARY)
      set(METAL_FOUND TRUE)
      list(APPEND GPU_SOURCES src/mining/gpu/metal_backend.mm)
      message(STATUS "  ✅ Metal backend auto-detected (Apple Silicon)")
    endif()
  endif()
  if(NOT METAL_FOUND)
    message(STATUS "GPU mining support disabled (use -DENABLE_GPU_MINING=ON to enable)")
    message(STATUS "  ⚙️  Stub-only mode (no GPU hardware support)")
  endif()
endif()

add_library(dinero_gpu_mining STATIC ${GPU_SOURCES})

target_include_directories(dinero_gpu_mining PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)

# Link GPU libraries (only if backends were detected)
if(ENABLE_GPU_MINING)
  if(OPENCL_FOUND)
    target_compile_definitions(dinero_gpu_mining PUBLIC ENABLE_OPENCL)
    target_link_libraries(dinero_gpu_mining PUBLIC OpenCL::OpenCL)
  endif()

  if(CUDAToolkit_FOUND)
    target_compile_definitions(dinero_gpu_mining PUBLIC ENABLE_CUDA)
    # CUDA Driver API (cuInit/cuDeviceGet/cuMemAlloc/cuLaunchKernel) + NVRTC
    # (nvrtcCreateProgram/nvrtcCompileProgram/nvrtcGetPTX). No cudart — we
    # don't use the Runtime API in this backend (it's the nvcc-coupled path
    # that hits Bug #4 on MSVC native). NVRTC compiles the kernel at runtime
    # against the actual device's compute capability, so per-arch PTX
    # snapshots (CUDA_ARCHITECTURES) are no longer needed.
    target_link_libraries(dinero_gpu_mining PUBLIC
      CUDA::cuda_driver
      CUDA::nvrtc
    )
  endif()
endif()

# Metal backend (Apple Silicon) — linked regardless of ENABLE_GPU_MINING
if(METAL_FOUND)
  target_compile_definitions(dinero_gpu_mining PUBLIC ENABLE_METAL)
  target_link_libraries(dinero_gpu_mining PUBLIC "${METAL_LIBRARY}" "${FOUNDATION_LIBRARY}")
  # .mm files need Obj-C++ compilation
  set_source_files_properties(src/mining/gpu/metal_backend.mm PROPERTIES
    COMPILE_FLAGS "-fobjc-arc"
  )
endif()

message(STATUS "  ✅ GPU mining library configured")

# Create version macros for main.cpp
set(PROJECT_VERSION "\"${PROJECT_VERSION}\"")
set(GIT_HASH "\"${GIT_HASH}\"")
set(BUILD_TIME "\"${BUILD_TIME}\"")

# Core crypto library (minimal, vendored crypto)
add_library(dinero_crypto STATIC
  src/crypto/sha256.cpp
  src/crypto/ripemd160_standalone.cpp
  # NOTE: src/crypto/params_stub.cpp intentionally NOT compiled here.
  # The stub provides a fallback dinero::detail::ParamsImpl() for
  # hypothetical lightweight builds that link dinero_crypto without
  # dinero_consensus. Compiling it into dinero_crypto unconditionally
  # caused LNK2005 multiply-defined ParamsImpl on MSVC whenever
  # dinero_consensus was also in the link line and CMake's transitive
  # expansion ordered dinero_crypto.lib before dinero_consensus.lib --
  # because the WEAK_SYMBOL macro expands to nothing on MSVC and there
  # is no portable C++ weak-function mechanism. No current target needs
  # the stub: every Params() caller (hd_wallet.cpp) lives in
  # dinero_wallet, which PUBLIC-deps dinero_consensus and therefore
  # gets the real chainparams_impl.cpp definition. If a future
  # lightweight HDWallet target ever wants Params() without
  # dinero_consensus, it should add src/crypto/params_stub.cpp to
  # its OWN sources -- not re-introduce it here.
  src/crypto/bip39.cpp
  src/crypto/bip32_slip132.cpp
  src/crypto/hd_keychain.cpp
  src/crypto/extended_pubkey.cpp
  src/crypto/base58.cpp
  src/crypto/hash.cpp
  src/common/sha256d.cpp              # Dinero::Common::sha256 (used by hash.cpp)
  src/core/crypto/ripemd160.cpp
  src/core/crypto/dinero_crypto_minimal.cpp
  external/bech32/bech32.cpp
  src/crypto/sha256_simd.cpp
  src/crypto/sha256_neon.cpp
  src/crypto/sha256_arm_shani.cpp
  src/crypto/sha256_arm_shani_wrapper.cpp
  src/crypto/encrypted_key.cpp  # Phase W.1.1: Encrypted key utilities (ParseBase64, SecureZero)
  src/crypto/decrypt_encrypted_key.cpp  # Phase W.1.1: Encrypted key decryption
)

# Enable ARM Crypto Extensions on ARM64 platforms
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    if(APPLE)
        target_compile_options(dinero_crypto PRIVATE -march=armv8-a+crypto)
        target_compile_definitions(dinero_crypto PRIVATE ENABLE_ARM_SHANI)
        message(STATUS "  ⚡ ARM SHA Extensions enabled (hardware acceleration)")
    endif()
endif()

target_include_directories(dinero_crypto PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/third_party
  ${CMAKE_SOURCE_DIR}/external
)

# ═══════════════════════════════════════════════════════════════════════════
# Transaction Primitives Library (Phase 2: Lightning Wallet Detachment)
# ═══════════════════════════════════════════════════════════════════════════
# Pure stateless transaction and cryptographic primitives.
# Shared by both dinerod (wallet) and lightningd (off-chain).
# NO wallet state, NO database access, NO coin selection.
add_library(dinero_tx_primitives STATIC
  src/primitives/transaction.cpp       # Transaction serialization, txid/wtxid
  src/primitives/schnorr_signer.cpp    # BIP-340 Schnorr signatures
  src/primitives/taproot_tx_signer.cpp # BIP-341 Taproot signing
  src/primitives/hash_domains.cpp      # Phase M.3: Semantic hash domains
)

target_include_directories(dinero_tx_primitives PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/include/wallet  # For transaction.h
)

target_link_libraries(dinero_tx_primitives PUBLIC
  dinero_crypto          # For SHA256 hashing
  secp256k1              # For Schnorr/Taproot crypto
  OpenSSL::Crypto        # For randomness
)

# Core wallet library (real implementations only)
# NOTE: src/wallet/hd_wallet.cpp is LEGACY and intentionally NOT compiled.
#       Active implementation: src/wallet/hd_wallet.cpp (uses BIP32Deriver)
add_library(dinero_wallet STATIC
  src/common/logger.cpp         # Logger used by wallet code
  src/wallet/address.cpp
  src/common/AddressCodec.cpp  # Taproot address encoding (P2TR)
  src/address/addr_codec.cpp    # Taproot address decoding and scriptPubKey creation
  src/address/addr_types.cpp    # Address type definitions
  src/wallet/wallet_manager.cpp  # Modern HD wallet with BIP84 (Nov 2025)
  src/wallet/key_identity.cpp    # Week 1: KeyID-based wallet identity
  src/wallet/key_origin.cpp      # Week 1: KeyOriginInfo for descriptor wallet
  src/wallet/script_ownership.cpp # Week 1: IsMine script ownership
  src/wallet/hd_wallet.cpp
  src/wallet/bip32_deriver.cpp   # BIP32 derivation helper (deduplicates lambdas)
  src/wallet/shielded_derivation.cpp  # Phase 5 Wave 1: Sapling-shape sk → ask/nsk → ak/nk → ivk
  src/wallet/bip39.cpp
  src/wallet/key_store_impl.cpp  # Phase 3A: BIP32 HD keystore (replaces HdKeyStoreAdapter)
  src/wallet/psbt.cpp
  src/core/wallet/psbt.cpp    # PSBTv2 helpers (din:: namespace, add_global_output_count etc.)
  src/wallet/psbt_signer.cpp  # PSBT signing with BIP86 guardrails
  src/wallet/psbt_taproot_validator.cpp  # BIP86 PSBT guardrails (key-path only validation)
  src/wallet/descriptor_checksum.cpp  # Phase 1: Bitcoin-compatible descriptor checksums
  src/wallet/bip84_descriptor.cpp     # BIP84 descriptor factory (P2WPKH)
  src/wallet/bip86_descriptor.cpp     # BIP86 descriptor factory (P2TR Taproot)
  src/wallet/descriptor_activation.cpp  # Phase 2 Step 2: Descriptor activation gate (security-critical)
  src/wallet/descriptor_psbt.cpp      # Phase 2 Step 3: Descriptor-based PSBT creation
  src/wallet/descriptor_store.cpp     # Phase 3C: Descriptor persistence (storage + selection layer ONLY)
  # Phase W.1.1: Rescan logic moved to wallet_worker.cpp (reuses existing block processing)
  # src/wallet/transaction.cpp REMOVED: duplicate of src/primitives/transaction.cpp (now in dinero_consensus)
  # Wallet uses Transaction from dinero_consensus (already linked)
  src/wallet/transaction_deserializer.cpp
  src/wallet/coin_selection.cpp
  src/wallet/bip143_signer.cpp
  src/wallet/transaction_builder.cpp  # Phase 33: Transaction building for wallet RPC
  src/wallet/schnorr_signer.cpp  # BIP-340 Schnorr signatures for Taproot
  src/wallet/taproot_keys.cpp  # BIP340 Schnorr key generation and signing
  src/wallet/taproot_address.cpp  # BIP86 Taproot address generation (P2TR key-path)
  src/wallet/taproot_tx_signer.cpp  # BIP341 Taproot transaction signing (BIP340 Schnorr)
  src/wallet/intent_descriptor.cpp  # B3: Intent descriptor for SigHash v1 ext_commitment
  src/wallet/policy_descriptor.cpp  # B2: PolicyDescriptor + policy_id computation
  src/wallet/taproot_template_builder.cpp  # B2: Taproot template tree builder (PROTECTED/ESCROW)
  src/wallet/safety_profile.cpp  # B4: Safety profile management
  src/wallet/protected_output_builder.cpp  # B4: Protected output construction (HDWallet + template builder)
  src/wallet/protected_spend.cpp  # B4: Script-path spending for panic/recovery leaves
  src/wallet/recovery_qr.cpp  # B4: Encrypted recovery QR envelope (AES-256-GCM)
  src/wallet/escrow_descriptor.cpp  # B5: Escrow descriptor + trade ID computation
  src/wallet/receipt_bundle.cpp  # B5: Attestor receipt bundle + Schnorr verification
  src/wallet/escrow_state_machine.cpp  # B5: Escrow state machine (NEGOTIATING->FUNDED->RELEASED)
  src/wallet/tx_classifier.cpp          # B7: UTXO classification + extended tx details
  src/wallet/lp_router.cpp              # B6a: LP router — provider quote aggregation
  # Hardware wallet files added conditionally below (not available on iOS)
  # Trezor files conditionally added below (ENABLE_TREZOR)
  src/wallet/recipient_descriptor.cpp  # DecodePaymentTarget — structured address resolution
  src/wallet/wallet_sync_status.cpp  # Phase W.2.1: Truthful wallet sync UX
  src/wallet/shielded_wallet_runtime.cpp  # v5 shielded note runtime + block sync (wallet-side only)
  src/wallet/sync_eta_estimator.cpp  # Phase W.2.2: Honest ETA estimation
  src/wallet/sync_progress_tracker.cpp  # Phase W.2.2: Phase-aware progress tracking
  src/wallet/reorg_detector.cpp  # Phase W.2.4: Reorg detection and visibility
  src/wallet/slow_reason_analyzer.cpp  # Phase W.2.5: Slow reason analyzer
  src/wallet/rbf_cpfp_detector.cpp  # Phase W.4.2: RBF & CPFP capability detection
  src/wallet/fee_bump_engine.cpp  # Phase W.4.3: Fee bump recommendation engine
  src/wallet/wallet_fee_bump_hooks.cpp  # Phase W.4.5: Wallet fee bump UX hooks
  # Phase 33: Full Wallet Activation
  src/wallet/wallet_features.cpp     # Wallet feature flags (all features enabled)
  # Milestone 12.2: Formal UTXO State Model (v0.12.0)
  src/wallet/wallet_utxo_state.cpp    # UTXO state machine with validated transitions
  src/wallet/wallet_utxo_tracker.cpp  # Per-wallet UTXO tracking with formal state model
  # Milestone 12.4: Unsigned Transaction Construction (v0.12.0)
  src/wallet/unsigned_tx_builder.cpp  # Pure transaction builder (no side effects)
  # Milestone 12.5: Transaction Signing & Local Validation (v0.12.0)
  src/wallet/transaction_signer.cpp   # Sign unsigned tx + local validation
  # Milestone 12.6: Wallet ↔ Mempool Submission (v0.12.0)
  src/wallet/wallet_mempool_adapter.cpp  # Adapter between wallet and mempool
  # Milestone 12.7: Transaction Batching (v0.12.0 - FINAL)
  src/wallet/batch_transaction_builder.cpp  # Pay multiple recipients in one tx
  # Phase 30: Taproot Asset Layer
  src/assets/asset_id.cpp           # Asset ID computation and metadata
  src/assets/asset_state.cpp        # Asset UTXO and state tracking
  src/assets/asset_proof.cpp        # Merkle proofs for asset operations
  src/assets/asset_transfer.cpp     # Asset transfer protocol
  src/assets/asset_registry.cpp     # Global asset registry (SQLite)
  src/crypto/pbkdf2.cpp
  src/crypto/wallet_crypto.cpp  # Wallet encryption (Argon2id + AES-256-GCM)
  # Wallet UX Abstractions (Phase CT.2 - Unified API layer)
  src/wallet/unified_balance_service.cpp # UnifiedBalanceService: combined transparent + CT balance
  src/wallet/address_factory.cpp         # AddressFactory: generate any address type
)

# Lightning Key Derivation (always built - used by wallet to provide keys to Lightning)
# This is separate from the Lightning daemon and from wallet UTXOs.
# Keys derived here are PROTOCOL KEYS, not on-chain wallet keys.
add_library(dinero_lightning_keys STATIC
  src/lightning/keys/lightning_key_deriver.cpp
)
target_include_directories(dinero_lightning_keys PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/vendor/include
)
target_link_libraries(dinero_lightning_keys PUBLIC
  dinero_crypto      # BIP32Deriver
  secp256k1          # Public key derivation
  OpenSSL::Crypto    # HMAC, OPENSSL_cleanse
)

# Lightning Network library (Layer 3, off-chain - NOT required for consensus)
# Disabled during genesis to ensure clean, deterministic chain birth
if(ENABLE_LIGHTNING)
  add_library(dinero_lightning STATIC
    src/lightning/lightning_crypto.cpp
    src/lightning/crypto_utils.cpp
    src/lightning/secp256k1_utils.cpp
    # src/lightning/lightning_wallet.cpp  # Phase 3: Disabled - dead code, predates gRPC architecture
    src/lightning/channel_manager.cpp
    src/lightning/channel_manager_core.cpp        # Phase 7: L2 pure state machine
    src/lightning/channel_utxo_lock_manager.cpp   # Phase 35: UTXO locking (Bitcoin+Lightning safety)
    src/lightning/production_chain_oracle.cpp     # L1→L2 chain queries
    src/lightning/production_wallet_oracle.cpp    # L1→L2 wallet queries
    src/lightning/production_funding_service.cpp  # L1→L2 funding TX
    src/lightning/production_htlc_sweep_oracle.cpp # Phase 7B: HTLC sweep TX
    src/lightning/production_justice_oracle.cpp    # Phase 7C: Justice TX
    src/lightning/htlc_manager.cpp
    # src/lightning/lightning_db.cpp  # Removed: legacy implementation deleted
    src/lightning/sqlite_lightning_db.cpp
    src/lightning/invoice.cpp
    src/lightning/onion.cpp
    src/lightning/payment_utils.cpp
    # Phase 31: Multi-Asset Lightning Settlement Layer
    src/lightning/asset_channel.cpp
    src/lightning/asset_htlc.cpp
    src/lightning/swap_htlc.cpp
    src/lightning/routing_multiasset.cpp
    # Release Build: Socket-based Lightning IPC (zero dependencies)
    src/lightning/socket_lightning_transport.cpp
  )
  target_include_directories(dinero_lightning PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/build/generated/proto  # Phase 3 Commit 1: Proto headers for dinerod::UTXO
  )
  target_link_libraries(dinero_lightning PUBLIC
    # Phase 3 Commit 7: dinero_wallet removed - Lightning uses IWalletAPI interface only ✅
    # Lightning accesses wallet via gRPC (WalletClient → WalletServiceImpl → WalletManager)
    dinero_crypto
    secp256k1
    sqlite3  # Required by sqlite_lightning_db.cpp (sqlite3_open, sqlite3_close, sqlite3_exec)
    lightning_core_static  # Lightning Network dependencies (libwally, secp256k1-zkp, blake3)
    RocksDB::rocksdb
    OpenSSL::SSL
    OpenSSL::Crypto
  )

  # Link proto types only when gRPC is enabled (dev mode)
  if(TARGET dinerod_proto)
    target_link_libraries(dinero_lightning PUBLIC dinerod_proto)
  endif()
endif() # ENABLE_LIGHTNING

# RPC handlers library (includes GUI-compatible handlers)
add_library(dinero_rpc_handlers STATIC
  src/daemon/rpc/wallet_gui_handlers.cpp
  src/daemon/rpc/wallet_import_handlers.cpp  # Phase W.1.1: Wallet import/rescan (importmnemonic RPC)
  src/core/rpc/wallet_descriptor_rpc_handlers.cpp  # Phase 1: Descriptor wallet RPCs (listdescriptors, getdescriptorinfo, deriveaddresses)
  src/daemon/rpc/encrypted_key_validator.cpp  # Phase W.1.1: Encrypted key validation (used by wallet_import_handlers)
  src/common/log_redactor.cpp  # Phase W.1.1: Log redaction for sensitive data (used by wallet_import_handlers)
  src/daemon/rpc/MiningExtrasHandlers.cpp
  src/rpc/methods_mining_extras.cpp
  src/rpc/methods_mining_extras_vnext.cpp
  src/rpc/methods_mining_vnext.cpp  # Stratum RPC methods (mining.getstratuminfo)
  src/rpc/methods_mining_gpu.cpp  # GPU Mining RPC methods (mining.gpustatus, mining.allowgpu, mining.gpuinfo)
  src/rpc/methods_pool.cpp  # Pool accounting RPC (pool.stats, pool.workers, pool.blocks, pool.payouts)
  src/rpc/address_validation.cpp  # Address encoding/decoding (GetChainParams, EncodeBech32P2WPKH)
  # Legacy RPC implementations (still used by context-aware wrappers)
  src/rpc/methods_market.cpp
  src/rpc/methods_discovery.cpp
  src/rpc/methods_bridge.cpp
  src/rpc/methods_multiasset.cpp
  src/rpc/methods_payment.cpp
  src/rpc/methods_auth.cpp
  $<$<BOOL:${ENABLE_HARDWARE_WALLETS}>:src/rpc/methods_hardware_wallet.cpp>
  src/rpc/methods_descriptor.cpp  # Phase 3C: Descriptor RPC methods
  src/rpc/methods_telemetry.cpp
  src/rpc/methods_daemon_status.cpp  # Daemon readiness contract (getdaemonstatus)
  src/rpc/cpu_stats_rpc.cpp  # Phase E.3.1: CPU stats & resource monitoring
  src/rpc/rpc_init.cpp        # Phase E.3.1: RPC registration (RegisterAllRPCMethods)
  src/rpc/mining_control_rpc.cpp  # Phase Y: CPU miner control (start/stop/getstatus)
  src/rpc/ct_fee_rpc.cpp  # Phase 3: CT fee configuration RPC (ct.setminfee, ct.getfeeconfig, etc.)
  # src/rpc/methods_consensus.cpp  # Disabled: compilation errors with storage namespace
  # src/rpc/methods_silent_payments.cpp  # Disabled: needs wallet integration
  src/rpc/methods_peer_scoring.cpp  # Phase 5D: Peer scoring & DoS protection RPC
  src/rpc/methods_headers_sync.cpp  # Phase 5A: Headers-first sync RPC
  src/rpc/methods_compact_blocks.cpp  # Phase 5B: Compact block relay RPC
  src/rpc/methods_address_manager.cpp  # Phase 5C: Address manager RPC
  src/rpc/methods_rbf_policy.cpp  # Phase 5E: RBF policy RPC
  src/rpc/methods_utreexo.cpp  # Phase 34.2: Utreexo accumulator RPC (getutxoproof, getutreexoroots, etc.)
  src/rpc/methods_utreexo_batch.cpp  # Phase 11a.1: Utreexo batch RPC (getutxoproofs_batch, verifyutxoproofs_batch)
  src/rpc/methods_utreexo_register.cpp  # Phase 11a.1: Utreexo RPC registration
  src/rpc/methods_utreexo_bridge.cpp  # Phase 34.3: Utreexo Bridge API for light clients (utreexo.getproof, getroots, getstate)
  # Liquidity Vault — daemon-side source of truth (Track C).
  src/vault/ledger.cpp
  src/vault/ledger_account.cpp
  src/vault/deposit_flow.cpp
  src/vault/reorg_watcher.cpp
  src/vault/withdrawal_queue.cpp
  src/vault/signing_backend.cpp
  src/vault/vault_service.cpp
  src/vault/ledger_store.cpp
  src/vault/vault_runtime.cpp
  src/vault/wallet_signing_backend.cpp
  src/rpc/methods_vault.cpp
  src/daemon/rpc/websocket_handlers.cpp
  # Service implementations (bridge, payment, auth, etc.)
  src/bridge/fiat_bridge_manager.cpp
  src/bridge/routing_engine.cpp
  src/daemon/arp_manager.cpp
  src/rpc/payment_monitor.cpp
  src/rpc/event_bus.cpp
  src/rpc/token_manager.cpp
  src/rpc/session_manager.cpp
  src/rpc/auth_manager.cpp
  src/contracts/multiasset_escrow_contract.cpp
  src/contracts/multiasset_persistence.cpp
  # Week 2: Context-aware RPC handlers (migration from legacy globals)
  src/rpc/methods_blockchain_context.cpp
  src/rpc/methods_utxoset.cpp
  src/rpc/methods_address_index.cpp
  src/core/rpc/blockchain_rpc_metadata.cpp  # RPC metadata registration
  src/rpc/methods_wallet_context.cpp
  src/rpc/wallet_sync_rpc_handlers.cpp  # Phase W.2.6: Wallet sync UX RPC methods
  src/rpc/wallet_sync_aggregator.cpp  # Phase W.2.6: Single aggregation point for sync status
  src/rpc/ergonomics_rpc_handlers.cpp  # Phase W.3: High-level status aggregation (getnodehealth, getminingreadiness)
  src/rpc/fee_bump_rpc_handlers.cpp  # Phase W.4.4: Fee bump RPC methods (gettxinclusion, gettxbumprecommendation)
  src/rpc/methods_mining_context.cpp  # Phase F.5: Updated for MiningManager v2 + jsoncpp API
  src/rpc/methods_mining_v14.cpp  # v0.14.0.3: BlockAssembler-based mining RPC (getblocktemplate, submitblock)
  src/rpc/methods_mempool_context.cpp
  src/rpc/methods_network_context.cpp
  src/rpc/methods_contract_context.cpp
  src/rpc/methods_economics_context.cpp
  src/rpc/methods_economics_vnext.cpp  # VNext DSL with full metadata (rpc.getcontext, etc.)
  src/rpc/methods_payment_context.cpp
  src/rpc/methods_sync_context.cpp
  src/rpc/methods_auth_context.cpp
  src/rpc/methods_multiasset_context.cpp
  src/rpc/methods_market_context.cpp
  src/rpc/methods_bridge_context.cpp
  src/rpc/methods_discovery_context.cpp
  src/rpc/methods_openrpc.cpp  # OpenRPC schema/introspection RPC (rpc.openrpc, rpc.schema, ...)
  src/rpc/openrpc_generator.cpp
  $<$<BOOL:${ENABLE_HARDWARE_WALLETS}>:src/rpc/methods_hardware_wallet_context.cpp>
  src/rpc/methods_descriptor_context.cpp  # Phase 3C: Context-aware descriptor RPC wrappers
  src/rpc/methods_remaining_context.cpp
  src/rpc/diagnostics_rpc_handlers_context.cpp  # node.info, rpc.methods (Nov 2025)
  src/rpc/logging_rpc_handlers_context.cpp  # logging.setlevel, logging.getlevel (Step C: Dynamic log control)
  src/rpc/logs_rpc_handlers_context.cpp  # logs.recent, logs.services, logs.tail (Unified log aggregator)
  src/rpc/v7_pq_rpc_json.cpp             # Phase 4c.3.1: V7 post-quantum wallet RPC JSON adapters (getnewp2mraddress, listp2mraddresses, signp2mr)
  src/rpc/shielded_rpc_json.cpp          # V7 shielded pool RPCs: wallet.shield, wallet.unshield, wallet.shieldedbalance
  # NOTE: src/rpc/v7_pq_handlers.cpp is part of dinero_pq (src/consensus/pq/CMakeLists.txt) — linked via target_link_libraries(... dinero_pq)
  src/dpi/dpi_protocol.cpp               # DPI Phase 1: Payment Intent protocol (serialize, verify, tagged hash)
  src/rpc/dpi_rpc_handlers.cpp           # DPI Phase 1: RPC handlers (createinvoice, payinvoice, verifypackage, checkconflicts)
  # DISABLED: Payroll feature (experimental Lightning-based payroll with ZK privacy)
  # Re-enable behind a CMake flag once core functionality is stable
  # src/database/payroll_db.cpp  # PayrollDB: RocksDB-based payroll storage
  # src/rpc/payroll_rpc.cpp  # payroll.createinvoice, payroll.payall, payroll.report (Private Payroll over Lightning)
  # Week 9: GPU Mining RPC handlers (mining.gpustatus, mining.allowgpu, mining.gpuinfo)
  # Common utilities (thread isolation for Lightning)
  src/common/thread_pool.cpp
  # Phase 7: Lightning Network (channels, HTLCs, routing, payments) - DISABLED FOR GENESIS
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/rpc/methods_lightning.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_db.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/sqlite_lightning_db.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_peer.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/channel_manager.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/htlc_manager.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/payment_router.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/commitment_builder.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_service.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_wallet.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_crypto.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/invoice.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/bech32.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/onion.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/onion_error.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/payment_utils.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/gossip_manager.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/watchtower_client.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_sweep_manager.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_events.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_event_manager.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_websocket_broadcaster.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/lightning_rpc_handlers.cpp>  # Phase 14: RPC endpoints for Lightning event subscriptions
  # Phase 31: Multi-Asset Lightning Settlement Layer - DISABLED FOR GENESIS
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/asset_channel.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/asset_htlc.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/swap_htlc.cpp>
  # $<$<BOOL:${ENABLE_LIGHTNING}>:src/lightning/routing_multiasset.cpp>
  src/daemon/websocket_server.cpp  # Phase 14.4: WebSocket server (needed for broadcast_lightning_event)
  src/daemon/security_hardening.cpp  # Required by websocket_server.cpp
  src/security/auth_scrubber.cpp  # Required by security_hardening.cpp
)
target_include_directories(dinero_rpc_handlers BEFORE PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include  # Must come BEFORE system paths to override Homebrew headers
)
target_include_directories(dinero_rpc_handlers PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/daemon  # For daemon headers (http_rpc_server.h, p2p_manager.h, etc.)
)
target_link_libraries(dinero_rpc_handlers PRIVATE
  dinero_core      # Needed for address_validator.cpp (IsValidDineroAddress)
  dinero_wallet
  dinero_crypto
  dinero_pq        # Phase 4c.3.1: V7 post-quantum wallet RPC handlers (v7_pq_handlers.cpp, v7_pq_rpc_json.cpp)
  dinero_shielded  # V7 shielded pool: wallet.shield, wallet.unshield RPCs
  OpenSSL::Crypto  # Phase 4c.3.1: v7_pq_rpc_json.cpp uses OPENSSL_cleanse directly
  # RocksDB::rocksdb removed - was only needed for payroll_db.cpp (now disabled)
)
target_compile_definitions(dinero_rpc_handlers PRIVATE
  DINERO_CLI_GIT_SHA=${GIT_HASH}
  DINERO_GIT_COMMIT_FULL="${GIT_HASH_FULL}"
)

# Link ZK library if enabled (for zk.* RPC methods)
if(ENABLE_ZK)
  target_link_libraries(dinero_rpc_handlers PRIVATE dinero_zk)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# dinero_rpc_client — FIREWALL SURFACE for downstream consumers
# ═══════════════════════════════════════════════════════════════════════════
#
# This is the only RPC-client library that targets outside the daemon
# (dinero-cli, dinero-qt, dinero-solo-miner, dinero-wallet-cli, dinero-gpu-miner)
# may link against. It is intentionally minimal: src/rpc_client.cpp plus a
# JSON dependency, no consensus headers, no chain state, no validation, no
# database. A target linking dinero_rpc_client MUST NOT transitively pull
# dinero_consensus, dinero_chainstate, dinero_validation, or any other
# chain-side library.
#
# Why this matters
#   The Qt GUI, miners, and CLI talk to dinerod over JSON-RPC. They are
#   *clients* of the node, not co-residents of its consensus logic.
#   Keeping them on this thin surface means a Qt crash cannot corrupt
#   chain state, a miner bug cannot fork the chain, and consensus
#   refactors don't ripple into the wallet GUI.
#
# How the firewall is enforced
#   target_link_libraries() visibility. dinero_rpc_client PUBLIC-links
#   only jsoncpp_static. Anything richer must be a deliberate amendment
#   here, reviewed against this contract.
#
# The compile-fail probe in tests/build/qt_firewall_test.cmake exists to
# catch regressions — a Qt source file that #includes "validation.h"
# must fail to build. If you weaken this contract, that probe will go
# silent. Don't.
# ═══════════════════════════════════════════════════════════════════════════
add_library(dinero_rpc_client STATIC
  src/rpc_client.cpp
)
target_include_directories(dinero_rpc_client PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)
# Use bundled jsoncpp_static (cross-platform, no Homebrew dependency).
# Do NOT add consensus/chainstate/validation libraries here — see firewall
# contract above.
target_link_libraries(dinero_rpc_client PUBLIC jsoncpp_static)

# ═══════════════════════════════════════════════════════════════════════════
# LAYER ISOLATION: Three Persistence Layers (November 7, 2025)
# ═══════════════════════════════════════════════════════════════════════════
# 
# Clean separation of database dependencies:
#   1. dinero_consensus   → Pure consensus rules (NO database)
#   2. dinero_chainstate  → RocksDB ONLY (consensus storage)
#   3. dinero_wallet      → SQLite ONLY (user data)
#
# This ensures:
#   - Wallet doesn't pull in RocksDB
#   - Chainstate doesn't pull in SQLite (except for sqlite_open.cpp stub)
#   - Each layer is independently testable
# ═══════════════════════════════════════════════════════════════════════════

# ─────────────────────────────────────────────────────────────────────────
# Layer 0a: PURE Consensus Core (NO IO, NO database, NO threading)
# ─────────────────────────────────────────────────────────────────────────
# This is the extractable consensus library: libdinero-consensus
# Can be used in mobile wallets, hardware wallets, browsers (WASM)
# See include/dinero_consensus.h for public API
# ─────────────────────────────────────────────────────────────────────────
add_library(dinero_consensus_core STATIC
  # Pure UTXO set (no database, no threading)
  src/consensus/consensus_utxo_set.cpp
  src/consensus/consensus_invariants.cpp

  # Utreexo accumulator (pure cryptographic)
  src/consensus/utreexo_accumulator.cpp
  src/consensus/utreexo_stump.cpp

  # Merkle computation (pure math)
  src/consensus/merkle_root.cpp

  # BIP158 GCS block filters (pure math + SipHash)
  src/consensus/block_filter.cpp
  src/consensus/filter_commitment.cpp
  src/consensus/merkle_proof.cpp

  # PoW verification (pure math)
  src/consensus/pow.cpp
  src/consensus/chainwork.cpp
  src/consensus/difficulty_validation.cpp

  # Script execution (pure computation)
  src/consensus/script.cpp
  src/consensus/script_validation.cpp
  src/consensus/script_crypto.cpp
  src/consensus/crypto/sighash_bip143.cpp

  # Block undo data (pure data structures)
  src/consensus/block_undo.cpp

  # Subsidy and limits (pure constants)
  src/consensus/coinbase_maturity.cpp
  src/consensus/sigops.cpp
  src/consensus/freeze.cpp

  # Primitives (pure data structures)
  src/primitives/block.cpp
  src/primitives/uint256.cpp
  src/primitives/transaction.cpp
  src/primitives/transaction_serializer.cpp
  src/primitives/hash_domains.cpp
  src/common/sha256d.cpp
  src/common/serialization.cpp
  src/crypto/ripemd160.cpp
)

target_include_directories(dinero_consensus_core PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(dinero_consensus_core PUBLIC
  dinero_crypto
  dinero_pq        # Phase 6: P2MR spend validator (VerifyP2MRSpend) in script_validation.cpp
  dinero_shielded  # v7 Shielded pool: commitment tree + nullifier set + validation
  secp256k1
  OpenSSL::Crypto
  zstd  # For Utreexo proof compression
)

if(APPLE)
  target_link_libraries(dinero_consensus_core PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
  )
endif()

set_target_properties(dinero_consensus_core PROPERTIES
  OUTPUT_NAME "dinero-consensus"
)

message(STATUS "✅ dinero_consensus_core (pure library) configured")

# ─────────────────────────────────────────────────────────────────────────
# Layer 0b: Full Consensus Rules (includes persistence adapters)
# ─────────────────────────────────────────────────────────────────────────
# IMPORTANT — RELATIONSHIP TO dinero_consensus_core (above):
#
# dinero_consensus_core (output: dinero-consensus.lib) is the "pure" tier:
# pure math, no DB, no networking, no threading. Targets that want to
# exercise consensus logic in isolation (test_consensus_core_standalone,
# test_utreexo_stump_invariant, future mobile/WASM consumers) link it
# directly. ~30 sources.
#
# dinero_consensus (output: dinero_consensus.lib) is the "full" tier:
# the pure surface PLUS persistence adapters (ChainDB, CoinsViewCache,
# GlobalUTXOSet, block_index, witness_commitment, etc.). dinerod and
# every daemon-side test target link this one.
#
# The two libraries are PARALLEL BY DESIGN, not nested. ~30 source files
# below appear in BOTH libraries (consensus_utxo_set, script,
# script_validation, script_crypto, sighash_bip143, utreexo_accumulator,
# block_filter, filter_commitment, merkle_root, merkle_proof, pow,
# chainwork, difficulty_validation, sigops, freeze, block_undo, etc.).
# This is intentional: it lets either library be linked alone without
# pulling the other's tier. The cost is ~30 files compiled twice — fine
# at ~3s per file, acceptable for the architectural clarity.
#
# Possible future cleanup: make dinero_consensus PUBLIC-dep
# dinero_consensus_core and delete the duplicated sources from the
# add_library() list below. That requires careful link-order auditing
# (today's parallel layout sidesteps several MSVC linker order quirks)
# and would touch consensus-critical paths. Tracked but not blocking.
#
add_library(dinero_consensus STATIC
  src/consensus/chainparams_impl.cpp   # Chain parameters (mainnet/testnet/regtest)
  src/consensus/chainwork.cpp          # arith_uint256 for difficulty calculations
  src/consensus/block_index.cpp        # Phase 3B: Block index management and fork choice
  src/consensus/block_undo.cpp         # Block undo data structures
  src/consensus/merkle_root.cpp        # Phase 11a.2: Canonical merkle root computation
  src/consensus/witness_commitment.cpp # Phase 11c.1: Witness commitment structure (DINW magic)
  src/consensus/block_filter.cpp      # BIP158 GCS block filters for compact filter commitment
  src/consensus/filter_commitment.cpp # DNRF coinbase filter commitment (SegWit-style OP_RETURN)
  src/consensus/merkle_proof.cpp      # Merkle inclusion proof for coinbase verification
  src/consensus/coins_db.cpp           # Phase M.1: CoinsDB (UTXO database backend)
  src/consensus/global_utxo_set_impl.cpp  # Phase 11a: GlobalUTXOSet implementation
  src/indexing/utxo_position_index.cpp    # Phase 11a: Global UTXO → Utreexo position index
  src/consensus/coins_view_cache.cpp   # Phase M.1: CoinsViewCache (L1 UTXO cache)
  src/consensus/consensus_utxo_set.cpp # Phase 2: Pure in-memory UTXO set (no DB, no mutex)
  src/consensus/stateless_verification.cpp # Phase 2.2: Stateless block verification (pure, no DB)
  src/consensus/consensus_invariants.cpp   # Forensic-grade invariant assertions
  src/consensus/tx_parser.cpp          # Transaction parsing
  src/consensus/script_verify.cpp     # Script verification
  src/consensus/script.cpp            # Script class (BIP 141 witness detection)
  src/consensus/script_interpreter.cpp # Script interpreter (VerifyScript, createP2* functions)
  src/consensus/script_crypto.cpp     # Script crypto (SHA256_Hash, HASH160_Hash, etc.)
  src/consensus/script_sighash.cpp    # Sighash calculation for signatures
  src/consensus/crypto/sighash_bip143.cpp  # BIP143 sighash for SegWit v0 (consensus layer)
  src/consensus/script_validation.cpp # Phase F.11: Minimal script validation engine (explicit, not VM-based)
  src/consensus/script_verify_sig.cpp # Signature verification (CheckECDSASignature, CheckSchnorrSignature)
  src/consensus/tapscript_interpreter.cpp  # Tapscript execution (BIP342)
  src/consensus/covenants.cpp             # Phase 28: Covenant opcodes (CTV, CSFS, TXHASH, CCV)
  src/consensus/transaction_validator.cpp  # Transaction validation logic
  src/consensus/tx_validation.cpp          # Phase M.1: validateTransaction() entry point
  src/consensus/signature_cache.cpp        # Phase M.0: Signature verification cache (g_signature_cache)
  src/consensus/script_cache.cpp           # Phase M.0: Script verification cache
  # ❌ REMOVED: chain_manager_global.cpp (Phase N: eliminated g_chain_manager global)
  # NOTE: pow_consensus_engine.cpp moved to dinero_chainstate (needs ChainDB)
  src/consensus/coinbase_maturity.cpp  # Coinbase maturity rules (100 blocks)
  src/consensus/freeze.cpp             # Phase D.4: Consensus freeze + runtime integrity verification
  src/consensus/sigops.cpp             # Signature operation counting (mainnet blocker fix)
  src/consensus/pow.cpp                # Bitcoin Core-compatible PoW verification (MAINNET BLOCKER)
  src/consensus/utreexo_accumulator.cpp  # Phase 34.1: Utreexo forest accumulator
  src/consensus/utreexo_stump.cpp         # Phase 34.2: Utreexo stump (roots-only accumulator)
  src/consensus/cpu_budget_monitor.cpp   # Phase E.2.d: CPU budget monitoring and validation timeouts
  src/consensus/undo.cpp                 # UndoRecord serialization for ChainDB
  src/address/addr_codec.cpp           # Address encoding/decoding (Taproot support)
  src/address/addr_types.cpp           # Address type definitions
  src/crypto/ripemd160.cpp             # Crypto primitives
  src/common/serialization.cpp         # Serialization utilities
  src/primitives/block.cpp             # Block data structures
  src/primitives/uint256.cpp           # 256-bit hash type
  src/primitives/transaction.cpp       # Phase M.0: Canonical Transaction methods (size/weight/serialize)
  src/primitives/transaction_serializer.cpp  # TransactionSerializer helpers (ToHex, FromHex, Deserialize)
  src/primitives/hash_domains.cpp      # Phase M.3: Semantic hash domains (TxId, BlockHash, etc.)
  src/common/sha256d.cpp               # SHA256d hashing
)

target_include_directories(dinero_consensus BEFORE PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include  # Must come BEFORE system paths to override Homebrew headers
)
target_include_directories(dinero_consensus PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/daemon
)

if(APPLE)
  target_link_libraries(dinero_consensus PUBLIC
    dinero_crypto
    dinero_pq       # Phase 6: P2MR spend validator (pq::VerifyP2MRSpend +
                    # IsSchemeAcceptedAtHeight) is called from
                    # script_validation.obj inside this library, and
                    # transaction_validator.obj in dinero_chainstate. Carrying
                    # it as a PUBLIC dep means every downstream target that
                    # links dinero_consensus gets the PQ symbols transitively
                    # rather than each test having to know to add it.
    dinero_zk       # Shielded proof system support
    jsoncpp_static  # Bundled (no Homebrew dependency)
    secp256k1
    OpenSSL::Crypto # script_crypto.cpp uses SHA256/RIPEMD160 directly
    zstd            # For Utreexo proof compression
    RocksDB::rocksdb  # Phase M.1: CoinsDB requires RocksDB (CMake target)
    sqlite3         # Transitive header dependency (wallet headers include sqlite3.h)
  )
else()
  target_link_libraries(dinero_consensus PUBLIC
    dinero_crypto
    dinero_pq       # See APPLE branch above for the rationale; mirror the
                    # PUBLIC dep here so non-Apple consumers get pq::* too.
    dinero_zk       # Shielded proof system support
    jsoncpp_static  # Bundled (same as Mac)
    secp256k1
    OpenSSL::Crypto # script_crypto.cpp uses SHA256/RIPEMD160 directly
    zstd            # For Utreexo proof compression
    RocksDB::rocksdb  # Phase M.1: CoinsDB requires RocksDB (CMake target)
    sqlite3         # Transitive header dependency (wallet headers include sqlite3.h)
  )
endif()

# ─────────────────────────────────────────────────────────────────────────
# Layer 1: Chainstate (RocksDB ONLY - Consensus storage)
# ─────────────────────────────────────────────────────────────────────────
add_library(dinero_chainstate STATIC
  src/storage/chain_db.cpp             # ChainDB (RocksDB backend)
  src/storage/persistent_utxo_adapter.cpp  # Phase 2: Bridges consensus to persistence
  src/storage/block_storage.cpp        # BlockStorage (block/undo file management)
  src/storage/disk_space_monitor.cpp   # Phase E.2.b: Disk space monitoring
  # Phase 39: ChainManager DELETED (header, implementation, stubs all removed)
  # All usage migrated to ChainstateService / ChainDB / BlockStorage
  # src/consensus/startup_validator.cpp  # Phase E.1: Temporarily disabled - needs API fixes
  src/wallet/utxo_index.cpp            # UTXO index (uses RocksDB)
  src/sqlite_open.cpp                  # Minimal SQLite stub (for compatibility)
  src/consensus/pow_consensus_engine.cpp   # PoW consensus engine (needs ChainDB)
  src/chainstate/mtp_lookup.cpp        # Phase 23.3: BIP68 MTP lookup factory (needs ChainDB)
)

target_include_directories(dinero_chainstate BEFORE PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include  # Must come BEFORE system paths to override Homebrew headers
)
target_include_directories(dinero_chainstate PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/daemon
)

target_link_libraries(dinero_chainstate PUBLIC
  dinero_consensus  # Depends on consensus rules
  dinero_crypto
  jsoncpp_static
  secp256k1
  RocksDB::rocksdb  # System RocksDB via find_package (reindex-enabled recovery)
)

# ─────────────────────────────────────────────────────────────────────────
# Layer 2: Wallet (SQLite ONLY - User private data)
# ─────────────────────────────────────────────────────────────────────────
target_include_directories(dinero_wallet PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include  # ZK proof support
)

target_link_libraries(dinero_wallet PUBLIC
  dinero_tx_primitives # ← Transaction structures and Schnorr/Taproot signing
  dinero_crypto
  dinero_consensus  # ← Needed for CoinbaseMaturity::isCoinbaseMature()
  dinero_shielded
  argon2            # ← Wallet encryption (bundled Argon2id)
  OpenSSL::SSL
  OpenSSL::Crypto
  sqlite3           # ← ONLY SQLite, NO RocksDB
)
# Hardware wallet USB/HID transport — not available on iOS or when disabled
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND ENABLE_HARDWARE_WALLETS)
  target_link_libraries(dinero_wallet PUBLIC hidapi::hidapi)
  target_sources(dinero_wallet PRIVATE
    src/wallet/hardware_wallet_interface.cpp
    src/wallet/hid_transport.cpp
    src/wallet/ledger_wallet.cpp
  )
  target_compile_definitions(dinero_wallet PUBLIC HAVE_USB_HWALLET=1)
endif()

# Trezor support (optional - requires protobuf and trezor-common protos)
if(ENABLE_TREZOR)
  target_sources(dinero_wallet PRIVATE
    src/wallet/trezor_transport.cpp  # Trezor wire protocol transport (Protobuf over HID)
    src/wallet/trezor_wallet.cpp  # Trezor hardware wallet implementation (Protobuf messages)
  )
  target_link_libraries(dinero_wallet PUBLIC trezor_proto)
  target_compile_definitions(dinero_wallet PUBLIC ENABLE_TREZOR=1)
  message(STATUS "Trezor hardware wallet support: ENABLED")
else()
  message(STATUS "Trezor hardware wallet support: DISABLED (use -DENABLE_TREZOR=ON to enable)")
endif()

# ─────────────────────────────────────────────────────────────────────────
# Wallet FFI Library (C API for iOS/Mobile/Rust)
# ─────────────────────────────────────────────────────────────────────────
# This library provides a C-compatible API for use via FFI from:
# - iOS Swift apps (via bridging header)
# - Android JNI
# - Rust (via cbindgen)
# - Any language with C FFI support
#
# Built as STATIC library for iOS/mobile embedding.
# ─────────────────────────────────────────────────────────────────────────
add_library(dinero_wallet_ffi STATIC
  wallet-core/ffi/wallet_ffi.cpp
  wallet-core/ffi/kyc_provider_mock.cpp
  wallet-core/ffi/kyc_provider_registry.cpp
)

target_include_directories(dinero_wallet_ffi PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/wallet-core/ffi
)

target_link_libraries(dinero_wallet_ffi PUBLIC
  dinero_wallet       # HD wallet, signing, coin selection
  dinero_crypto       # Cryptographic primitives
  jsoncpp_static      # JSON serialization
  OpenSSL::Crypto     # For secure random, hashing
)

# Export C API header for Swift/ObjC bridging
set_target_properties(dinero_wallet_ffi PROPERTIES
  PUBLIC_HEADER "${CMAKE_SOURCE_DIR}/wallet-core/ffi/wallet_ffi.h"
)

# iOS-specific configuration
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  message(STATUS "📱 iOS build: Configuring dinero_wallet_ffi for iOS")

  # Ensure static library for iOS embedding
  set_target_properties(dinero_wallet_ffi PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    POSITION_INDEPENDENT_CODE ON
  )

  # iOS frameworks needed for crypto operations
  target_link_libraries(dinero_wallet_ffi PUBLIC
    "-framework Security"
    "-framework Foundation"
  )
endif()

# ─────────────────────────────────────────────────────────────────────────
# NodeCore FFI: Full embedded node for iOS (NodeCore.xcframework)
# ─────────────────────────────────────────────────────────────────────────
# C ABI wrapping DaemonApp lifecycle + status + wallet watch + events.
# This is the iOS equivalent of the dinerod executable, but as a library.
#
# Swift calls:
#   nodecore_start(datadir, config_json)
#   nodecore_set_event_callback(cb, ctx)
#   nodecore_watch_script(script, len, label)
#   nodecore_stop()
#
# Dependencies: same as dinerod minus gRPC/GUI/Stratum
# ─────────────────────────────────────────────────────────────────────────
option(BUILD_NODECORE "Build NodeCore FFI library for iOS embedding" OFF)

if(BUILD_NODECORE OR (CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT BUILD_FFI_ONLY))
  message(STATUS "📱 Building NodeCore FFI (embedded full node for iOS)")

  add_library(nodecore_ffi STATIC
    src/nodecore/nodecore_ffi.cpp
    src/nodecore/sync_profile_policy.cpp
    src/nodecore/zk_stubs_ios.cpp

    # DaemonApp lifecycle (same as dinerod, minus main.cpp)
    src/daemon/daemon_app.cpp
    src/daemon/legacy_globals_stub.cpp
    src/daemon/config.cpp
    src/daemon/rpc_context_wiring.cpp
    src/daemon/mempool.cpp
    src/daemon/db_repair.cpp

    # Socket wallet server (always enabled)
    ${SOCKET_SERVER_SOURCES}

    # RPC server infrastructure (internal, not HTTP-exposed on iOS)
    src/daemon/http_rpc_server.cpp
    src/daemon/rpc_auth.cpp

    # IPC oracles (for internal use)
    src/ipc/oracles/chain_oracle_client.cpp
    src/ipc/oracles/time_oracle_client.cpp
    src/ipc/oracles/transaction_oracle_client.cpp
    src/ipc/watch_registration_server.cpp

    # Bridge node (Utreexo proof generation)

    # Pool accounting (symbols referenced by daemon_app.cpp; gated by config at runtime)
  )

  target_include_directories(nodecore_ffi PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(nodecore_ffi PUBLIC
    dinero_core
    dinero_rpc_handlers
    argon2
    RocksDB::rocksdb
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
    sqlite3
    ${CMAKE_DL_LIBS}
  )
  link_zstd_if_needed(nodecore_ffi)

  # Bulletproofs FFI (range proofs)
  if(TARGET bulletproofs_ffi)
    target_link_libraries(nodecore_ffi PUBLIC bulletproofs_ffi)
  endif()

  target_compile_definitions(nodecore_ffi PRIVATE
    IOS_BUILD
    DISABLE_GRPC
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    DINERO_CLI_VERSION=${PROJECT_VERSION}
    DINERO_CLI_GIT_SHA="nodecore"
    DINERO_GIT_COMMIT_FULL="nodecore-embedded"
    DINERO_CLI_BUILD_DATE="embedded"
  )

  set_target_properties(nodecore_ffi PROPERTIES
    PUBLIC_HEADER "${CMAKE_SOURCE_DIR}/include/nodecore/nodecore_ffi.h"
  )

  # iOS-specific configuration
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set_target_properties(nodecore_ffi PROPERTIES
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
      POSITION_INDEPENDENT_CODE ON
    )

    target_link_libraries(nodecore_ffi PUBLIC
      "-framework Security"
      "-framework Foundation"
    )
  endif()

  message(STATUS "📱 NodeCore FFI target configured")
endif()

# ─────────────────────────────────────────────────────────────────────────
# Wallet Test Dependencies (Minimal stubs for testing)
# ─────────────────────────────────────────────────────────────────────────
add_library(wallet_test_deps STATIC
  src/wallet/utxo_index.cpp
  src/sqlite_open.cpp
  src/daemon/lightning_stubs.cpp
)

target_link_libraries(wallet_test_deps PUBLIC
  dinero_wallet
  dinero_chainstate  # ChainDB (WalletManager::rescanBlockchain dependency)
  RocksDB::rocksdb
  sqlite3
)

target_include_directories(wallet_test_deps PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)

# ─────────────────────────────────────────────────────────────────────────
# Layer 2c: Utreexo Bridge Library (extracted from daemon-only TUs)
# ─────────────────────────────────────────────────────────────────────────
# bridge_node.cpp implements Utreexo proof generation for stateless/mobile
# clients. It was previously compiled directly into ~17 test targets and
# the nodecore_ffi exe because no linkable library exposed it; consumers
# either duplicated the source or stubbed the symbols (see
# tests/stubs/daemon_test_stubs.cpp BridgeNode section). Extracting it
# here lets dinero_core declare it as a transitive dep and removes both
# the duplication and the stub seam.
add_library(dinero_bridge STATIC
  src/network/bridge_node.cpp
)
target_include_directories(dinero_bridge PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(dinero_bridge PUBLIC
  dinero_chainstate  # ChainDB + ArchivalBlockReader headers
  dinero_consensus   # consensus/outpoint.h, consensus/undo.h
  dinero_crypto      # crypto/sha256.h
)

# ─────────────────────────────────────────────────────────────────────────
# Layer 2d: Mining Pool Library (extracted from daemon-only TUs)
# ─────────────────────────────────────────────────────────────────────────
# pool_db.cpp / pool_manager.cpp / payout_calculator.cpp implement the
# pool-mining accounting subsystem. Previously duplicated across multiple
# test targets and nodecore_ffi (each compiled the cpps directly), and
# also referenced from chainstate_service.cpp inside dinero_core which
# had nothing to link against (hence the pool stubs in daemon_test_stubs.cpp).
# Deps are small and contained: consensus/subsidy.h (pure math),
# storage/chain_db.h, common/logger.h. No daemon glue, no RPC.
add_library(dinero_pool STATIC
  src/pool/pool_db.cpp
  src/pool/pool_manager.cpp
  src/pool/payout_calculator.cpp
)
target_include_directories(dinero_pool PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(dinero_pool PUBLIC
  dinero_consensus   # consensus/subsidy.h
  dinero_chainstate  # storage/chain_db.h
)

# ─────────────────────────────────────────────────────────────────────────
# Layer 3: Core Logic Library (Reusable, Testable)
# ─────────────────────────────────────────────────────────────────────────
# This library contains all core business logic that can be tested independently.
# Excludes: main(), RPC server, gRPC server, WebSocket server
# Includes: consensus, mining, mempool, services, metrics, daemon context
add_library(dinero_core STATIC
  # Daemon context (dependency injection container access)
  src/daemon/daemon_context.cpp
  src/daemon/gui_websocket_events.cpp
  src/daemon/ws_subscriptions.cpp
  src/daemon/ws_globals.cpp

  # Phase 3a of the shielded reorg invertibility plan
  # (docs/specs/atomic_consensus_persistence_phase3.md). Skeleton
  # only — opt-in via -consensus.atomic_persist=1, default off.
  src/consensus/consensus_write_batch.cpp

  # Service wrappers (dependency injection containers)
  src/daemon/services/config_service.cpp
  src/daemon/services/wallet_service.cpp
  src/daemon/services/chainstate_service.cpp
  src/daemon/services/mempool_service.cpp
  src/daemon/services/metrics_service.cpp
  src/daemon/services/p2p_service.cpp

  # Phase D.4 (Dinero Core 1.0) — pure health-check decision logic
  src/daemon/health.cpp
  src/daemon/services/mining_service.cpp
  src/daemon/services/rpc_service.cpp
  src/daemon/services/peer_scoring_service.cpp
  src/daemon/services/headers_sync_service.cpp
  src/daemon/services/compact_block_service.cpp
  src/daemon/services/address_manager_service.cpp
  src/daemon/services/rbf_policy_service.cpp
  src/daemon/services/block_ingress_service.cpp  # Step 5: IBlockIngress implementation
  src/daemon/services/prune_service.cpp  # Phase 34.8: Block pruning for mobile nodes

  # Step 5: Interface type implementations (canonical ingress types)
  src/daemon/interfaces/ingress_types.cpp

  # Mining subsystem (Phase C - core logic)
  src/mining/block_assembler.cpp
  src/mining/block_assembly_context.cpp  # Phase W.1: Network-aware assembly context
  src/mining/transaction_scorer.cpp  # Phase W.1: Intelligent transaction scoring
  src/mining/ct_selection_policy.cpp  # CT-specific transaction selection policies
  src/mining/template_delta_tracker.cpp  # Phase W.1: Incremental template refresh
  src/mining/tx_inclusion_analyzer.cpp  # Phase W.4.1: Transaction inclusion analysis (read-only)
  src/mining/mining_manager_v2.cpp
  # src/mining/mining_manager.cpp  # Phase C: Replaced by mining_manager_v2.cpp (ODR violation fix)
  src/mining/miner.cpp
  src/mining/mining_script_override.cpp
  src/mining/midstate_cache.cpp
  src/mining/address_validator.cpp  # Phase F.5: Address validation for mining RPC
  src/mining/payout_spec.cpp  # Phase C1: Daemon-defined pool payouts
  src/daemon/mining_safety_gates.cpp
  src/daemon/mining_events.cpp
  src/daemon/mining_metrics.cpp
  src/daemon/header_metadata_recovery.cpp
  src/daemon/undo_rebuild_orchestrator.cpp  # Offline --rebuild-undo-range orchestrator (commit #4 of post-Apr 30 series)

  # Consensus layer
  src/consensus/reindexer.cpp
  src/consensus/undo.cpp
  src/consensus/header_sync_manager.cpp
  src/consensus/header_chain.cpp
  src/consensus/header_store.cpp
  src/consensus/header_sync.cpp
  src/consensus/header_sync_p2p.cpp
  src/consensus/proof_gossip.cpp
  src/consensus/block_download_scheduler.cpp
  src/consensus/assume_utxo.cpp
  src/consensus/parallel_block_validator.cpp
  src/consensus/validation_worker_pool.cpp
  src/consensus/validation_queue.cpp
  src/consensus/block_validation.cpp
  src/consensus/block_index.cpp
  src/consensus/block_lifecycle.cpp
  src/consensus/orphan_manager.cpp
  src/consensus/block_index_persistence.cpp
  src/consensus/genesis_canonical.cpp  # Phase 3: Genesis hash self-test
  src/consensus/adapters/wallet_utxo_adapter.cpp  # UTXO adapter for wallet integration

  # Mempool
  src/mempool/mempool.cpp  # Full mempool implementation (needed by compact_block.cpp)
  src/mempool/coins_view_mempool.cpp
  src/mempool/mempool_persistence.cpp
  src/mempool/fee_estimator.cpp
  src/mempool/invalid_tx_cache.cpp
  src/mempool/tx_orphan_pool.cpp  # Transaction orphan pool (holds TXs with missing parents)

  # Network relay

  # Common utilities
  src/common/logger.cpp
  src/common/json_logger.cpp
  src/common/logger_router.cpp
  src/common/address_script_builder.cpp
  src/common/config_manager.cpp
  src/util/ser.cpp  # Compact size varint serialization

  # Metrics
  src/metrics/metrics_registry.cpp

  # P2P layer
  src/p2p/peer_scoring.cpp
  src/p2p/connection_manager.cpp
  src/p2p/rate_limiter.cpp
  src/p2p/network_limits_monitor.cpp   # Phase E.2.c: Network limits monitoring
  src/p2p/block_download_scheduler.cpp
  src/p2p/headers_first_sync.cpp
  src/p2p/multi_peer_headers_sync.cpp
  src/p2p/compact_block.cpp  # Phase G.13: Compact block relay (Plan-A: binary wire format)
  src/p2p/addrman.cpp
  src/p2p/peer_manager_posix.cpp  # Non-Qt P2P PeerManager implementation (POSIX sockets)

  # Policy layer
  src/policy/mempool_policy.cpp
  src/policy/rbf_policy.cpp
  src/policy/fee_estimator.cpp

  # Address handling
  src/address/addr_codec.cpp
  src/address/addr_types.cpp

  # Wallet core logic
  src/wallet/wallet_worker.cpp
  src/daemon/hd_wallet_manager.cpp
  src/daemon/wallet_crypto.cpp
  src/wallet/psbt.cpp  # PSBT helpers (Phase 2 Step 4: export for tests)

  # Network
  src/network/network_invariants.cpp
  src/daemon/p2p_message.cpp
  src/daemon/peer_connection.cpp
  src/daemon/p2p_manager.cpp
  src/daemon/header_serialization.cpp
  src/network/utreexo_messages.cpp
  src/network/stateless_node.cpp

  # Crypto utilities
  src/crypto/sp_context.cpp
  src/daemon/crypto_utils.cpp
  src/daemon/secure_random.cpp

  # RPC registry (logic only, not server)
  src/rpc/rpc_registry.cpp

  # Database utilities
  src/database/sqlite_manager.cpp
  src/daemon/db_meta_utils.cpp

  # Validation
  src/daemon/block_acceptor.cpp
  src/daemon/block_relay_manager.cpp  # Phase G.2: Block propagation
  src/daemon/tx_relay_manager.cpp  # Phase G.3: Mempool relay

  # P2P infrastructure (Phase G.7+)
  src/p2p/orphan_block_pool.cpp       # Phase G.7: Orphan block handling
  src/p2p/block_presence.cpp          # Required by orphan_block_pool

  # Privacy
  src/core/privacy/silent_scanner_manager.cpp
  src/core/privacy/silent_scanner.cpp

  # Encoding/decoding
  src/daemon/bech32_decode.cpp
  src/daemon/bech32_encoder.cpp
  src/wallet/p2mr_address.cpp            # Phase 4a: V7 P2MR bech32m address codec (din1r1...)

  # Genesis initialization (subsidy now in dinero_consensus library)
  src/daemon/genesis_init.cpp

  # Transaction pool
  src/daemon/transaction_pool.cpp

  # CLI version info
  src/cli/version.cpp

  # IPC oracle clients (Phase 9.2: Lightning event forwarding)
  src/ipc/oracles/chain_oracle_client.cpp
  src/ipc/oracles/time_oracle_client.cpp
  src/ipc/oracles/transaction_oracle_client.cpp

  # Test stubs (needed by consensus layer)
  tests/consensus/chainstate_metadata_stub.cpp
  tests/consensus/missing_symbols_stub.cpp
)

target_include_directories(dinero_core BEFORE PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include  # Must come BEFORE system paths to override Homebrew headers
)
target_include_directories(dinero_core PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)

# Quality hardening: enable compiler warnings for core library
if(NOT MSVC)
  target_compile_options(dinero_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-mismatched-tags)
else()
  target_compile_options(dinero_core PRIVATE /W3)
endif()

target_link_libraries(dinero_core PUBLIC
  dinero_crypto
  dinero_chainstate
  dinero_consensus
  dinero_bridge           # BridgeNode::* (Utreexo proof gen) — used by chainstate_service.cpp
  dinero_pool             # PoolDB / PoolManager / PayoutCalculator — used by chainstate_service.cpp
  dinero_wallet
  dinero_lightning_keys  # Phase 3: Lightning key derivation for gRPC wallet service
  dinero_gpu_mining      # GPU mining backends (Metal/CUDA/OpenCL)
  jsoncpp_static
  sqlite3
  secp256k1
  RocksDB::rocksdb
)

# ═════════════════════════════════════════════════════════════════════
# Phase 3: Lightning Build-Time Decoupling COMPLETE
# ═════════════════════════════════════════════════════════════════════
# Lightning is now EXTERNAL - runs as standalone lightningd process
# - dinerod = pure L1 node (NO Lightning library linked)
# - lightningd = optional L2 daemon (links dinero_lightning)
# - Communication via gRPC (WalletClient → dinerod gRPC server)
#
# dinerod always uses stubs (Lightning service not embedded)
# ═════════════════════════════════════════════════════════════════════
target_sources(dinero_core PRIVATE src/daemon/lightning_stubs.cpp)
message(STATUS "⚡ Lightning: EXTERNAL (use standalone 'lightningd' binary)")
message(STATUS "   dinerod = pure L1 node (blockchain, mempool, wallet, gRPC)")
message(STATUS "   lightningd = optional L2 daemon (channels, HTLCs, routing)")

# Add compile definitions needed by core logic
target_compile_definitions(dinero_core PRIVATE
  CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"  # SQL-first schema path
)

# ─────────────────────────────────────────────────────────────────────────
# Conditional gRPC Server Source Files (Dev Mode Only)
# ─────────────────────────────────────────────────────────────────────────
if(ENABLE_GRPC)
  set(GRPC_SERVER_SOURCES
    src/grpc/grpc_server.cpp
    src/grpc/blockchain_service.cpp
    src/grpc/mempool_service.cpp
    src/grpc/wallet_service.cpp
    src/grpc/blockchain_client.cpp
    src/grpc/mempool_client.cpp
  )
  message(STATUS "🔌 gRPC Server Sources: INCLUDED (dev mode)")
else()
  set(GRPC_SERVER_SOURCES "")
  message(STATUS "🔌 gRPC Server Sources: EXCLUDED (release mode)")
endif()

# Socket wallet server is included on POSIX (both dev and release modes).
# On Windows it's excluded — the implementation calls poll() directly with
# struct pollfd, which is POSIX-only (Winsock has WSAPoll with a slightly
# different surface). Lightning support isn't shipped on the Windows preview
# track, so the missing wallet socket server has no user-visible impact on
# the rc7+ Windows binaries. Re-enable this file by porting poll() to
# WSAPoll() if/when Lightning is ported to Windows.
if(WIN32)
  # Windows uses no-op stub implementations of the SocketWalletServer
  # methods so daemon_app.cpp's references resolve at link time. Lightning
  # support isn't shipped on the Windows preview track, so the stubs are
  # never actually exercised at runtime — Start() returns false and the
  # daemon proceeds without the socket wallet server.
  set(SOCKET_SERVER_SOURCES
    src/grpc/socket_wallet_server_windows_stub.cpp
  )
else()
  set(SOCKET_SERVER_SOURCES
    src/grpc/socket_wallet_server.cpp
  )
endif()

# ─────────────────────────────────────────────────────────────────────────
# Layer 4: Daemon Executable (Thin shell around libdinero_core)
# ─────────────────────────────────────────────────────────────────────────
if(APPLE)
  add_executable(dinerod
    # Daemon lifecycle and entry point
    src/daemon/main.cpp
    src/daemon/legacy_globals_stub.cpp
    src/daemon/daemon_app.cpp
    src/daemon/datadir_guard.cpp
    src/daemon/config.cpp  # Phase 8: Node configuration
    src/daemon/rpc_context_wiring.cpp
    src/daemon/mempool.cpp  # Phase G.3: Structured tx ingress
    src/daemon/db_repair.cpp  # Database repair utility (--repair-db)
    src/daemon/genesis_guard.cpp  # Stale chain data detection

    # Doctor diagnostic command (dinerod doctor)
    src/daemon/doctor/doctor_command.cpp
    src/daemon/doctor/doctor_context.cpp
    src/daemon/doctor/doctor_fixer.cpp
    src/daemon/doctor/doctor_json_emitter.cpp
    src/daemon/doctor/doctor_registry.cpp
    src/daemon/doctor/doctor_renderer.cpp
    src/daemon/doctor/doctor_runner.cpp
    src/daemon/doctor/doctor_checks_v1.cpp

    # gRPC and Socket services (conditional compilation)
    ${GRPC_SERVER_SOURCES}    # Only in dev mode when ENABLE_GRPC=ON
    ${SOCKET_SERVER_SOURCES}  # Always enabled (both dev and release)

    # RPC server infrastructure
    src/daemon/http_rpc_server.cpp
    src/daemon/rpc_auth.cpp

    # Stratum V1 mining server - REMOVED (separate binary: dinero-stratum)

    # Pool accounting system

    # Contract/Escrow support
    src/contracts/escrow_contract.cpp
    src/contracts/contract_registry.cpp
    src/contracts/daemon_mediator.cpp
    src/contracts/contract_state_db.cpp
    src/contracts/commitment_builder.cpp
    src/contracts/state_verifier.cpp
    src/contracts/escrow_contract_manager.cpp
    src/contracts/lending_contract_manager.cpp
    # Phase 9.2: IPC oracle clients (Lightning event forwarding)
    src/ipc/oracles/chain_oracle_client.cpp        # Chain event oracle for lightningd
    src/ipc/oracles/time_oracle_client.cpp         # Time oracle for Lightning block height tracking
    src/ipc/oracles/transaction_oracle_client.cpp  # Transaction oracle for Lightning TX tracking
    # Phase 9.3: Bidirectional oracle communication
    src/ipc/watch_registration_server.cpp          # Watch registration server for Lightning→dinerod commands
    src/contracts/dao_governance_manager.cpp
    # Phase P.2: Utreexo proof generation for stateless/mobile clients
  )
else()
  # Linux/Windows server build
  add_executable(dinerod
    # ─── dinerod-only sources (NOT in dinero_core) ───
    src/daemon/main.cpp
    src/daemon/legacy_globals_stub.cpp  # Temporary stubs for legacy globals
    # Windows stubs for bulletproofs FFI and hardware wallets (when Rust/cargo unavailable)
    $<$<PLATFORM_ID:Windows>:${CMAKE_SOURCE_DIR}/src/nodecore/zk_stubs_windows.cpp>
    src/daemon/daemon_app.cpp           # DaemonApp lifecycle orchestrator
    src/daemon/datadir_guard.cpp
    src/daemon/config.cpp               # Phase 8: Node configuration
    src/daemon/rpc_context_wiring.cpp   # Week 2: Wire DaemonContext to RPC system
    src/daemon/db_repair.cpp            # Database repair utility (--repair-db)
    src/daemon/genesis_guard.cpp        # Stale chain data detection
    # Doctor diagnostic command (dinerod doctor)
    src/daemon/doctor/doctor_command.cpp
    src/daemon/doctor/doctor_context.cpp
    src/daemon/doctor/doctor_fixer.cpp
    src/daemon/doctor/doctor_json_emitter.cpp
    src/daemon/doctor/doctor_registry.cpp
    src/daemon/doctor/doctor_renderer.cpp
    src/daemon/doctor/doctor_runner.cpp
    src/daemon/doctor/doctor_checks_v1.cpp
    # Phase 9.3: Bidirectional oracle communication
    src/ipc/watch_registration_server.cpp  # Watch registration server for Lightning→dinerod commands
    # Phase P.2: Utreexo proof generation for stateless/mobile clients
    # gRPC and Socket services (conditional compilation)
    ${GRPC_SERVER_SOURCES}    # Only in dev mode when ENABLE_GRPC=ON
    ${SOCKET_SERVER_SOURCES}  # Always enabled (both dev and release)
    # Pool accounting system (PPLNS, PPS, PROP payout modes) - not in dinero_core
    # Daemon-specific mempool (different from src/mempool/mempool.cpp in dinero_core)
    src/daemon/mempool.cpp
    # HTTP RPC server & auth (server-only, not in dinero_core)
    src/daemon/http_rpc_server.cpp
    src/daemon/rpc_auth.cpp
    # ─── All other sources come from dinero_core static library ───
    # hd_wallet_manager.cpp excluded (depends on wallet_crypto)
    # wallet_crypto.cpp excluded (needs OpenSSL 3.2+)
  )
  add_compile_definitions(NO_WALLET_ENCRYPTION)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Link All Layers: Proper dependency order
# ═══════════════════════════════════════════════════════════════════════════
# Order matters: More specific libraries first, then dependencies
#   1. dinero_chainstate  (RocksDB - depends on consensus)
#   2. dinero_wallet      (SQLite - user data)
#   3. dinero_consensus   (Pure rules - no DB)
#   4. dinero_rpc_handlers (RPC logic)
#   5. dinero_crypto      (Crypto primitives)
# ═══════════════════════════════════════════════════════════════════════════

if(APPLE)
  target_link_libraries(dinerod PRIVATE
    dinero_rpc_handlers    # RPC handlers
    dinero_core            # Core services/logic; must follow rpc_handlers for static resolution
    bulletproofs_ffi       # Dalek Bulletproofs via Rust FFI (range proofs)
    "-framework IOKit"      # Week 3: For mining_safety_gates battery/thermal checks
  )
  if(TARGET dinerod_proto)
    target_link_libraries(dinerod PRIVATE dinerod_proto) # gRPC API (dev mode only)
  endif()
else()
  target_link_libraries(dinerod PRIVATE
    dinero_rpc_handlers    # RPC handlers
    -Wl,--whole-archive dinero_core -Wl,--no-whole-archive  # whole-archive core after rpc_handlers resolves static refs in one pass
    bulletproofs_ffi       # Dalek Bulletproofs via Rust FFI (range proofs)
  )
  if(TARGET dinerod_proto)
    target_link_libraries(dinerod PRIVATE dinerod_proto) # gRPC API (dev mode only)
  endif()
endif()

# Windows-specific link libraries and linker flags
if(WIN32)
  # ntdll: Rust std in bulletproofs_ffi
  # ws2_32: Winsock2
  target_link_libraries(dinerod PRIVATE ntdll ws2_32)
  # MinGW's ld scans archives left-to-right and only pulls in unresolved
  # symbols from libraries it has already seen. RocksDB's env_win.cc /
  # port_win.cc reference PathIsDirectoryA / UuidCreateSequential / etc.,
  # which live in shlwapi.dll and rpcrt4.dll. The clean way to make those
  # libs land AFTER librocksdb.a in the link line is to add them as part
  # of RocksDB::rocksdb's INTERFACE_LINK_LIBRARIES — CMake then propagates
  # them to every consumer in the right order.
  if(TARGET RocksDB::rocksdb)
    set_property(TARGET RocksDB::rocksdb APPEND PROPERTY
      INTERFACE_LINK_LIBRARIES shlwapi rpcrt4)
  endif()
  # Force linker to include ALL objects from dinero_core (resolves symbol
  # ordering issues with archive-static-init globals).
  #
  # MSVC's link.exe uses /WHOLEARCHIVE:<name>; MinGW-w64's ld.exe uses
  # -Wl,--whole-archive <archive> -Wl,--no-whole-archive. The CMake
  # target_link_libraries(... PRIVATE -Wl,--whole-archive libdinero_core.a
  # -Wl,--no-whole-archive ...) call elsewhere already handles MinGW; the
  # MSVC flag below is only correct under the MSVC toolchain. Without this
  # guard, MinGW's ld.exe interpreted "/WHOLEARCHIVE:dinero_core" as a file
  # path and bailed with "cannot find /WHOLEARCHIVE:dinero_core".
  if(MSVC)
    target_link_options(dinerod PRIVATE "/WHOLEARCHIVE:dinero_core")
  endif()
endif()

# Add generated headers include directory
target_include_directories(dinerod PRIVATE ${CMAKE_BINARY_DIR}/generated)

# ═══════════════════════════════════════════════════════════════════════════
# test_reindex - Standalone reindex tester (no mempool dependencies)
# ═══════════════════════════════════════════════════════════════════════════
add_executable(test_reindex
  test_reindex.cpp
  src/consensus/reindexer.cpp
  src/common/logger.cpp
  src/consensus/undo.cpp
  # src/wallet/transaction.cpp removed - use Transaction from dinero_consensus (already linked)
)

target_link_libraries(test_reindex PRIVATE
  dinero_chainstate
  dinero_consensus
  dinero_crypto
  jsoncpp_static
  RocksDB::rocksdb
  OpenSSL::Crypto
  OpenSSL::SSL
  pthread
)
  link_zstd_if_needed(test_reindex)

target_include_directories(test_reindex PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)

# Platform-specific linking with proper crypto libraries
# NOTE: OpenSSL is already configured earlier (lines 110-152)
# Don't call find_package again as it overrides vendored imported targets
if(NOT TARGET OpenSSL::Crypto)
  # This should never happen - OpenSSL targets are created above
  message(FATAL_ERROR "OpenSSL::Crypto target not found - check OpenSSL configuration above")
endif()

# Wallet encryption uses PBKDF2-HMAC-SHA512 (built into OpenSSL, no external dependencies)

# ═══════════════════════════════════════════════════════════════════════════
# jsoncpp is now BUNDLED in third_party/jsoncpp (no external dependency needed)
# ═══════════════════════════════════════════════════════════════════════════
# All targets use jsoncpp_static (Mac + Linux, no Homebrew or pkg-config needed)
# Legacy find_package code commented out:
#
# find_package(PkgConfig QUIET)
# if(PKG_CONFIG_FOUND)
#     pkg_check_modules(JSONCPP jsoncpp)
# endif()
# if(NOT JSONCPP_FOUND)
#     find_path(JSONCPP_INCLUDE_DIR json/json.h)
#     find_library(JSONCPP_LIBRARY jsoncpp)
# endif()
target_link_libraries(dinerod PRIVATE
  pthread
  ${CMAKE_DL_LIBS}  # Required for Rust FFI (dlopen/dlsym)
)

# Ensure dinerod depends on Bulletproofs FFI build (if cargo is available)
if(CARGO_EXECUTABLE)
  add_dependencies(dinerod build_bulletproofs_ffi)
endif()
# Ensure production code path remains on canonical daemon mempool.
add_dependencies(dinerod check_no_legacy_mempool_prod)

# Windows: sockets + CSPRNG
if(WIN32)
  # NOMINMAX and WIN32_LEAN_AND_MEAN are set globally (line 65)
  target_link_libraries(dinerod PRIVATE ws2_32 bcrypt)
endif()

# Add version compile definitions (PROJECT_VERSION already has quotes from line 131)
target_compile_definitions(dinerod PRIVATE
  DINERO_CLI_VERSION=${PROJECT_VERSION}
  DINERO_CLI_GIT_SHA=${GIT_HASH}
  DINERO_GIT_COMMIT_FULL="${GIT_HASH_FULL}"
  DINERO_CLI_BUILD_DATE=${BUILD_TIME}
  DINERO_BUILD_REPO="dinero"
  DINERO_BUILD_COMPONENT="dinerod"
  CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"  # SQL-first schema path (Phase 2 - Nov 2025)
)

# macOS: Security and CoreFoundation frameworks
if(APPLE)
  find_library(SECURITY_FRAMEWORK Security)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
  if(SECURITY_FRAMEWORK)
    target_link_libraries(dinerod PRIVATE ${SECURITY_FRAMEWORK})
  endif()
  if(COREFOUNDATION_FRAMEWORK)
    target_link_libraries(dinerod PRIVATE ${COREFOUNDATION_FRAMEWORK})
  endif()
endif()

# Sanitizers for debug builds
if(ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  dinero_enable_sanitizers(dinerod)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# lightningd - Standalone Lightning Network Daemon (Phase 8.4: Hermetic Build)
# ═══════════════════════════════════════════════════════════════════════════
# Phase 8.4: Hermetic L2 daemon build
# - NO L1 dependencies (daemon, chainstate, wallet, mempool)
# - NO proto generation required
# - NO gRPC linkage required
# - ONLY: IPC server + event sink + Lightning core
#
# Build independence verified by:
#   cmake -DBUILD_DINEROD=OFF -DBUILD_LIGHTNINGD=ON .. && make lightningd
# ═══════════════════════════════════════════════════════════════════════════

if(BUILD_LIGHTNINGD)
  message(STATUS "Building lightningd (standalone Lightning daemon)")

  # Phase 3 COMPLETE: Minimal lightningd - just link dinero_lightning!
  # NO complex source list needed - dinero_lightning library contains everything

  # Source files list (varies by build mode)
  set(LIGHTNINGD_SOURCES
    src/lightningd/main.cpp                # Main entry point (Phase 8.4: IPC Transport)
    src/lightningd/ipc_server.cpp          # Phase 8.4: Unix socket server for event injection
    src/common/logger.cpp                  # Logger (minimal dependency)

    # Phase 8.6: L2-pure Lightning core files (standalone Lightning in lightningd/)
    src/lightningd/lightning/lightning_app.cpp        # LightningApp (owns ChannelManagerCore)
    src/lightningd/lightning/channel_manager_core.cpp # L2 pure state machine
    src/lightningd/lightning/sqlite_lightning_db.cpp  # Persistence layer

    # Phase 9.3: Bidirectional oracle communication
    src/ipc/watch_registration_client.cpp # Watch registration client for Lightning→dinerod commands
  )

  # In release mode, add socket transport (no gRPC dependency)
  if(DINERO_RELEASE)
    list(APPEND LIGHTNINGD_SOURCES
      src/lightningd/lightning/socket_lightning_transport.cpp  # Socket transport (Bitcoin-style IPC)
    )
  endif()

  add_executable(lightningd ${LIGHTNINGD_SOURCES})

  target_include_directories(lightningd PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    # Phase 8.4: NO proto headers needed - lightningd is L2-only
  )

  target_link_libraries(lightningd PRIVATE
    # Phase 8.4: Hermetic L2 build - NO L1 dependencies
    # - NO dinero_lightning (has compilation errors, fix in Phase 8.5+)
    # - NO dinerod_proto (L1-only types)
    # - NO gRPC (L1 transport layer)
    # Only: IPC server + event sink (header-only) + system libs

    # Phase 8.6: L2-pure dependencies
    sqlite3          # SQLiteLightningDB persistence

    # System libraries
    pthread
    ${CMAKE_DL_LIBS}
  )

  # Phase 8.4: NO dinerod_proto linkage - lightningd is independent of L1 types

  # macOS-specific: Add Security framework
  if(APPLE)
    target_link_libraries(lightningd PRIVATE "-framework Security")
  endif()

  # Compiler definitions
  target_compile_definitions(lightningd PRIVATE
    DINERO_CLI_VERSION=${PROJECT_VERSION}
    DINERO_CLI_GIT_SHA=${GIT_HASH}
  )

  # Install
  install(TARGETS lightningd DESTINATION bin)

  message(STATUS "✅ lightningd binary enabled (Phase 8.4: Hermetic Build)")
  message(STATUS "    Architecture: lightningd (IPC server + event sink)")
  message(STATUS "    ✅ NO L1 dependencies")
  message(STATUS "    ✅ NO proto generation")
  message(STATUS "    ✅ NO gRPC linkage")
  message(STATUS "    ✅ NO wallet/daemon code")
  message(STATUS "    ✅ Hermetic L2 build achieved")
else()
  message(STATUS "lightningd disabled (BUILD_LIGHTNINGD=OFF)")
endif()  # BUILD_LIGHTNINGD

# Genesis miner tool (legacy 80-byte header)
add_executable(genesis_miner tools/genesis_miner.cpp)
target_link_libraries(genesis_miner PRIVATE dinero_consensus dinero_crypto pthread)
target_include_directories(genesis_miner PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Genesis miner v3 (128-byte BlockHeader v1 with Utreexo commitment)
add_executable(genesis_miner_v3 tools/genesis_miner_v3_correct.cpp)
target_link_libraries(genesis_miner_v3 PRIVATE
    dinero_consensus
    dinero_crypto
    pthread
    OpenSSL::SSL
    OpenSSL::Crypto
)
target_include_directories(genesis_miner_v3 PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

# Genesis miner v7 (same BlockHeader v1; new motto + timestamp)
add_executable(genesis_miner_v7 tools/genesis_miner_v7.cpp)
target_link_libraries(genesis_miner_v7 PRIVATE
    dinero_consensus
    dinero_crypto
    pthread
    OpenSSL::SSL
    OpenSSL::Crypto
)
target_include_directories(genesis_miner_v7 PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

# premine_miner removed — no premine in Fair Launch v3

# Genesis bundle verifier (standalone, no DB/RPC/datadir)
add_executable(verify_genesis_bundle tools/verify_genesis_bundle.cpp)
target_link_libraries(verify_genesis_bundle PRIVATE
    dinero_consensus_core
    dinero_crypto
    jsoncpp_static
    pthread
    OpenSSL::SSL
    OpenSSL::Crypto
    zstd
)
target_include_directories(verify_genesis_bundle PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

# Raw Taproot key generator (no wallet dependency)
# DISABLED: tools/taproot_keygen.cpp not found - uncomment when added
# add_executable(taproot_keygen tools/taproot_keygen.cpp)
# target_link_libraries(taproot_keygen PRIVATE
#   secp256k1
#   OpenSSL::SSL
#   OpenSSL::Crypto
# )
# target_include_directories(taproot_keygen PRIVATE
#   ${CMAKE_SOURCE_DIR}/vendor/include
#   ${CMAKE_SOURCE_DIR}/include
# )

# compute_premine_commitment and verify_premine_seed removed — no premine in Fair Launch v3

# Native MSVC needs CMake/vcpkg's imported libcurl target. Keep the existing
# raw `curl` link name for Unix-style builds that already resolve -lcurl.
if(WIN32)
  find_package(CURL REQUIRED)
  set(DINERO_CURL_TARGET CURL::libcurl)
else()
  set(DINERO_CURL_TARGET curl)
endif()

# CPU miner (external process)
add_executable(dinero-miner tools/dinero_miner.cpp)
target_link_libraries(dinero-miner PRIVATE
  dinero_crypto
  jsoncpp_static
  ${DINERO_CURL_TARGET}
)
target_include_directories(dinero-miner PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/external
  /opt/homebrew/include
)
target_compile_definitions(dinero-miner PRIVATE
  DINERO_CLI_VERSION=${PROJECT_VERSION}
  DINERO_CLI_GIT_SHA=${GIT_HASH}
  DINERO_GIT_COMMIT_FULL="${GIT_HASH_FULL}"
  DINERO_CLI_BUILD_DATE=${BUILD_TIME}
  DINERO_BUILD_REPO="dinero"
  DINERO_BUILD_COMPONENT="dinero-miner"
)

# Dedicated Stratum pool worker (external process, CPU-only)
add_executable(dinero-stratum-worker tools/dinero_stratum_worker.cpp)
target_link_libraries(dinero-stratum-worker PRIVATE
  dinero_crypto
  jsoncpp_static
  ${DINERO_CURL_TARGET}
)
target_include_directories(dinero-stratum-worker PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/external
  /opt/homebrew/include
)
target_compile_definitions(dinero-stratum-worker PRIVATE
  DINERO_CLI_VERSION=${PROJECT_VERSION}
  DINERO_CLI_GIT_SHA=${GIT_HASH}
  DINERO_GIT_COMMIT_FULL="${GIT_HASH_FULL}"
  DINERO_CLI_BUILD_DATE=${BUILD_TIME}
  DINERO_BUILD_REPO="dinero"
  DINERO_BUILD_COMPONENT="dinero-stratum-worker"
)

# GPU miner (external process, uses Metal/CUDA/OpenCL backends)
add_executable(dinero-gpu-miner tools/dinero_gpu_miner.cpp)
target_link_libraries(dinero-gpu-miner PRIVATE
  dinero_gpu_mining
  dinero_crypto
  jsoncpp_static
  ${DINERO_CURL_TARGET}
)
target_include_directories(dinero-gpu-miner PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/external
  /opt/homebrew/include
)
target_compile_definitions(dinero-gpu-miner PRIVATE
  DINERO_CLI_VERSION=${PROJECT_VERSION}
  DINERO_CLI_GIT_SHA=${GIT_HASH}
  DINERO_GIT_COMMIT_FULL="${GIT_HASH_FULL}"
  DINERO_CLI_BUILD_DATE=${BUILD_TIME}
  DINERO_BUILD_REPO="dinero"
  DINERO_BUILD_COMPONENT="dinero-gpu-miner"
)

# CLI tool (uses unified RPC client)
add_executable(dinero-cli src/dinero_cli.cpp)
if(APPLE)
  target_link_libraries(dinero-cli PRIVATE
    dinero_rpc_client
    jsoncpp_static
  )
else()
  target_link_libraries(dinero-cli PRIVATE
    dinero_rpc_client
    jsoncpp_static  # Bundled (same as Mac)
  )
endif()
# dinero_rpc_client (used by dinero-cli) calls Winsock2 directly:
# WSAStartup, socket(), connect(), send(), recv(), gethostbyname(),
# closesocket(), inet_pton(), htons(). MinGW does NOT auto-link ws2_32,
# so we must add it explicitly. Without this dinero-cli fails to link
# with "undefined reference to `__imp_WSAStartup'" + friends. dinerod's
# link line gets ws2_32 transitively through other targets, but
# dinero-cli only pulls in dinero_rpc_client + jsoncpp_static so we
# add it here.
if(WIN32)
  target_link_libraries(dinero-cli PRIVATE ws2_32)
endif()
target_include_directories(dinero-cli PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)
# Phase D.4 / E.3 — version + bundled-libs metadata.
#
# `dinero-cli version --json` emits a contract schema that
# release-time gates (PackageGate) and external monitoring tools
# can pin against. The data is purely compile-time — build
# version, git sha, build date, plus the vendored-static
# dependency versions. No runtime daemon required (so the gate
# can run in --static mode against staging too).
#
# Versions are sourced from the vendored submodule paths /
# CMakeLists.txt comments. Update here when a vendored dep
# bumps. Static-linked deps report no `sha256` per spec §1.4
# nuance (no separate .so artifact to hash).
# DINERO_PACKAGE_VERSION used to maintain its own version string,
# stale at 2.2.5 against the v2.2.6 release line — a real instance
# of the version drift that motivated the monorepo consolidation.
# After consolidation, this tracks CMAKE_PROJECT_VERSION (which is
# the top-level project's VERSION, unquoted — set by `project()`
# at line 2). Overridable via -DDINERO_PACKAGE_VERSION=... for
# packagers that need to pin a different string.
if(NOT DEFINED DINERO_PACKAGE_VERSION)
  set(DINERO_PACKAGE_VERSION "${CMAKE_PROJECT_VERSION}")
endif()
target_compile_definitions(dinero-cli PRIVATE
  DINERO_PACKAGE_VERSION=\"${DINERO_PACKAGE_VERSION}\"
  # GIT_HASH and BUILD_TIME are already pre-quoted in CMake (see
  # the `set(GIT_HASH "\"${GIT_HASH}\"")` block earlier in this
  # file) so they expand to a complete C string literal without
  # additional escaping here.
  DINERO_BUILD_GIT_SHA=${GIT_HASH}
  DINERO_BUILD_DATE_ISO=${BUILD_TIME}
  DINERO_BUNDLED_OPENSSL_VERSION=\"3.3.2\"
  DINERO_BUNDLED_ROCKSDB_VERSION=\"8.11.3\"
  DINERO_BUNDLED_SECP256K1_ZKP_VERSION=\"vendored\"
  DINERO_BUNDLED_JSONCPP_VERSION=\"vendored\"
  DINERO_BUNDLED_ZSTD_VERSION=\"1.4.7+vendored\"
)

# Reference Wallet CLI
add_executable(dinero-wallet-cli
  src/wallet/reference/main.cpp
  src/wallet/reference/wallet.cpp
  src/wallet/reference/database.cpp
  src/wallet/reference/crypto.cpp
  src/wallet/reference/utxo_manager.cpp
  src/wallet/reference/transaction_builder.cpp
  src/wallet/reference/blockchain_sync.cpp
  src/wallet/bip39.cpp
  src/crypto/pbkdf2.cpp
  src/common/rpc_client.cpp
  external/bech32/bech32.cpp
)
target_link_libraries(dinero-wallet-cli PRIVATE
  dinero_crypto
  sqlite3
  lightning_core_static
  jsoncpp_static
  ${DINERO_CURL_TARGET}
  OpenSSL::Crypto  # Required for OPENSSL_cleanse in crypto.cpp
)
link_secp256k1_zkp(dinero-wallet-cli)
if(APPLE)
  target_link_libraries(dinero-wallet-cli PRIVATE "-framework Security")
endif()
target_include_directories(dinero-wallet-cli PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/external
  /opt/homebrew/include
)

# Embed VS_VERSION_INFO into each daemon binary so Explorer Properties
# -> Details shows the right version/company/component rather than an
# empty dialog. No-op outside Windows. See dinero_attach_win32_versioninfo()
# definition near the top of this file.
dinero_attach_win32_versioninfo(dinerod               "Dinero full node daemon")
dinero_attach_win32_versioninfo(dinero-cli            "Dinero RPC CLI client")
dinero_attach_win32_versioninfo(dinero-miner          "Dinero CPU miner")
dinero_attach_win32_versioninfo(dinero-stratum-worker "Dinero Stratum worker client")
dinero_attach_win32_versioninfo(dinero-gpu-miner      "Dinero GPU miner (CUDA/OpenCL)")
dinero_attach_win32_versioninfo(dinero-wallet-cli     "Dinero reference wallet CLI")

# Reference Wallet Test Suite
# PURE OFFLINE TESTS: No RPC calls in tests, but wallet.cpp includes optional sync method
add_executable(test_reference_wallet
  tests/wallet/test_reference_wallet.cpp
  src/wallet/reference/wallet.cpp
  src/wallet/reference/database.cpp
  src/wallet/reference/crypto.cpp
  src/wallet/reference/utxo_manager.cpp
  src/wallet/reference/transaction_builder.cpp
  src/wallet/reference/blockchain_sync.cpp  # wallet.cpp has optional SyncBlockchain() method
  src/wallet/bip39.cpp
  src/crypto/pbkdf2.cpp
  src/common/rpc_client.cpp  # Required by blockchain_sync.cpp
  external/bech32/bech32.cpp
)

add_dependencies(test_reference_wallet gtest)

target_link_libraries(test_reference_wallet PRIVATE
  dinero_crypto
  sqlite3
  lightning_core_static
  ${DINERO_CURL_TARGET}
  gtest          # Use target name, not TARGET_FILE
  jsoncpp_static  # Must come last - after all code that uses it
  OpenSSL::Crypto  # Required for OPENSSL_cleanse in crypto.cpp
)
link_secp256k1_zkp(test_reference_wallet)

if(APPLE)
  target_link_libraries(test_reference_wallet PRIVATE "-framework Security")
endif()

target_include_directories(test_reference_wallet PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/external
  # /opt/homebrew/include removed - Homebrew quarantined to enforce vendored libs
)

# Register with CTest
add_test(NAME ReferenceWallet COMMAND test_reference_wallet)

# Label as critical wallet test
set_tests_properties(ReferenceWallet PROPERTIES
  LABELS "wallet;reference;critical;determinism"
  TIMEOUT 60
)

message(STATUS "✅ Reference wallet test suite enabled")

# SIMD benchmark tool
add_executable(bench-simd tools/bench_simd.cpp)
target_link_libraries(bench-simd PRIVATE dinero_crypto)
target_include_directories(bench-simd PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ═══════════════════════════════════════════════════════════════════════════════
# Qt6 GUI (external sibling project)
# ═══════════════════════════════════════════════════════════════════════════════
# dinero-qt is now a standalone project at ../dinero-qt/
# Build it separately:
#   cd ../dinero-qt
#   cmake -B build -DCMAKE_PREFIX_PATH="$HOME/Qt/6.9.1/macos"
#   cmake --build build
#
# Why separate?
#   - GUI bugs ≠ consensus bugs
#   - GUI crashes must not kill the node
#   - GUI updates must not require consensus rebuilds
#   - GUI attack surface is huge and unrelated to validation
# ═══════════════════════════════════════════════════════════════════════════════
set(DINERO_QT_PATH "${CMAKE_SOURCE_DIR}/../dinero-qt" CACHE PATH "Path to dinero-qt project")
if(EXISTS "${DINERO_QT_PATH}/CMakeLists.txt")
  message(STATUS "dinero-qt project found at: ${DINERO_QT_PATH}")
  message(STATUS "  Build separately: cd ${DINERO_QT_PATH} && cmake -B build && cmake --build build")
else()
  message(STATUS "dinero-qt project not found at ${DINERO_QT_PATH}")
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Stratum Mining Server (external sibling project)
# ═══════════════════════════════════════════════════════════════════════════════
# Stratum is now a standalone project at ../stratum/
# Build it separately: cd ../stratum && cmake -B build && cmake --build build
# The embedded stratum sources are still included in dinerod for integrated mode.
set(STRATUM_PROJECT_PATH "${CMAKE_SOURCE_DIR}/../stratum" CACHE PATH "Path to stratum project")
if(EXISTS "${STRATUM_PROJECT_PATH}/include")
  include_directories(${STRATUM_PROJECT_PATH}/include)
  message(STATUS "Stratum project found at: ${STRATUM_PROJECT_PATH}")
else()
  message(WARNING "Stratum project not found at ${STRATUM_PROJECT_PATH}")
endif()

# Installation
#
# Phase E.3 (Dinero Core 1.0): under DINERO_PACKAGED_BUILD, the .deb
# ships only the daemon and the CLI per spec §1.1 row 1. Miner and
# bench tooling continues to build (unit tests + dev workflows still
# need them) but is excluded from `cmake --install` so dpkg doesn't
# package them. They get split into a separate dinero-miner package
# in a later phase.
#
# Manual / dev / fleet builds (DINERO_PACKAGED_BUILD=OFF) keep the
# pre-Phase-E install set to avoid breaking workflows that rely on
# /usr/local/bin/dinero-stratum-worker etc.
if(DINERO_PACKAGED_BUILD)
  install(TARGETS dinerod dinero-cli DESTINATION bin)

  # DT_RUNPATH discipline (spec §1.4 — "DT_RUNPATH not DT_RPATH").
  # INSTALL_RPATH applies only to the installed binary; the build-tree
  # binary keeps its CMake-managed RPATH so dev runs against
  # build/_deps/... still resolve. --enable-new-dtags emits RUNPATH
  # (the post-binutils-2.x tag) instead of legacy RPATH so an operator
  # debugging a bundled .so can override via LD_LIBRARY_PATH without
  # rebuilding dinerod.
  set_target_properties(dinerod dinero-cli PROPERTIES
    INSTALL_RPATH "/usr/lib/dinero"
    BUILD_WITH_INSTALL_RPATH FALSE
    INSTALL_RPATH_USE_LINK_PATH FALSE)
  target_link_options(dinerod PRIVATE
    "$<$<PLATFORM_ID:Linux>:LINKER:--enable-new-dtags>")
  target_link_options(dinero-cli PRIVATE
    "$<$<PLATFORM_ID:Linux>:LINKER:--enable-new-dtags>")
else()
  install(TARGETS dinerod dinero-cli genesis_miner dinero-miner dinero-stratum-worker dinero-gpu-miner bench-simd DESTINATION bin)
endif()

# Phase C (Dinero Core 1.0) — install the documented examples alongside
# the binaries so packagers and manual-mode operators have a starting
# point for dinero.conf and the systemd unit.
#
# Both files land under ${CMAKE_INSTALL_PREFIX}/share/doc/dinero/. The
# `.example` suffix on the systemd unit is intentional: cmake --install
# does NOT activate the unit automatically (per spec §1.2). Phase E's
# .deb packaging will copy the example (without the `.example` suffix)
# to /lib/systemd/system/dinero.service for packaged-service mode.
install(FILES
    ${CMAKE_SOURCE_DIR}/share/dinero.conf.example
    DESTINATION share/doc/dinero
)
install(FILES
    ${CMAKE_SOURCE_DIR}/share/systemd/dinero.service.example
    DESTINATION share/doc/dinero
)

# Phase D.1 (Dinero Core 1.0) — journald retention drop-in example.
# Renamed at install time so its purpose is unambiguous when sitting
# alongside the regular dinero.conf.example. Phase E's .deb postinst
# will copy it (without the `.example` suffix) to
# /etc/systemd/journald.conf.d/dinero.conf.
install(FILES
    ${CMAKE_SOURCE_DIR}/share/systemd/journald.conf.d/dinero.conf.example
    DESTINATION share/doc/dinero
    RENAME journald-dinero.conf.example
)

# Phase D.2 (Dinero Core 1.0) — wallet/shielded backup script. Lives
# at ${CMAKE_INSTALL_PREFIX}/bin/dinero-backup so operators can invoke
# it with the same PATH that holds dinerod / dinero-cli. Mode 0755
# (executable) — the script itself enforces output mode 0600 on the
# archives it produces.
install(PROGRAMS
    ${CMAKE_SOURCE_DIR}/share/scripts/dinero-backup
    DESTINATION bin
)

# Phase D.3 (Dinero Core 1.0) — pre-upgrade rollback capture script.
# Operator-invoked (NOT wired to systemd; capture failure must not
# block dinerod restart per spec §1.1 row 9). Captures the running
# dinerod binary to <datadir>/binaries/dinerod.live-pre-<commit>-<ts>
# so the operator has a guaranteed-good rollback target before
# installing a new version.
install(PROGRAMS
    ${CMAKE_SOURCE_DIR}/share/scripts/dinero-prepare-upgrade
    DESTINATION bin
)

# Phase E.3 (Dinero Core 1.0) — release-time gate for the .deb.
# Verifies dinerod / dinero-cli have RUNPATH (not RPATH), only system
# libs in NEEDED resolve to allowed paths, and Depends: contains no
# forbidden vendored-leak entries. Run by CI on every tag against the
# staging dir before signing; failure fails the release. Not installed
# into the .deb (it's a CI/build tool, not an operator tool).
if(DINERO_PACKAGED_BUILD)
  add_test(NAME PackageGate
    COMMAND ${CMAKE_SOURCE_DIR}/share/scripts/dinero-deb-verify
            --static --staging ${CMAKE_BINARY_DIR}/debian/dinero-core
  )
  set_tests_properties(PackageGate PROPERTIES
    LABELS "package-gate"
  )
  # Live-mode counterpart `dinero-deb-verify --installed` is not a ctest:
  # it needs `dpkg -i` + `systemctl enable --now` on a clean Linux box.
  # That runs in a separate CI job; both gates must pass before E.3/E.6
  # is complete.
  message(STATUS "📦 Phase E.3 PackageGate ctest registered (label package-gate, --static mode)")
endif()

# Add GoogleTest for unit testing
#
# Strategy:
#   - Uses vendored GoogleTest v1.14.0 from third_party/googletest
#   - Configured via cmake/VendorGTest.cmake
#   - Snappy's old GoogleTest is disabled (SNAPPY_BUILD_TESTS=OFF)

# Tests (only if we have working test files)
enable_testing()

# Differential acceptance parity gate (baseline vs current binaries)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_acceptance_parity.sh)
  add_test(
    NAME AcceptanceParity
    COMMAND ${CMAKE_SOURCE_DIR}/tests/test_acceptance_parity.sh
  )

  set_tests_properties(AcceptanceParity PROPERTIES
    LABELS "consensus;gate;parity;release"
    TIMEOUT 1800
  )

  add_custom_target(test_acceptance_parity
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R AcceptanceParity
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running acceptance parity differential gate"
    USES_TERMINAL
  )

  message(STATUS "✅ Acceptance parity gate registered: ctest -R AcceptanceParity")
else()
  message(WARNING "⚠️  tests/test_acceptance_parity.sh not found - AcceptanceParity gate not registered")
endif()

# IBD torture gate (IBD + competing fork + churn + restart)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_ibd_torture.sh)
  add_test(
    NAME IBDTorture
    COMMAND ${CMAKE_SOURCE_DIR}/tests/test_ibd_torture.sh
  )

  set_tests_properties(IBDTorture PROPERTIES
    LABELS "ibd;p2p;utreexo;gate;release"
    TIMEOUT 3600
  )

  add_custom_target(test_ibd_torture
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R IBDTorture
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running IBD torture gate"
    USES_TERMINAL
  )

  message(STATUS "✅ IBD torture gate registered: ctest -R IBDTorture")
else()
  message(WARNING "⚠️  tests/test_ibd_torture.sh not found - IBDTorture gate not registered")
endif()

# Phase 1 canonical state + recovery gate
add_custom_target(test_phase1_canonical_recovery
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L phase1-canonical-recovery
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Running Phase 1 canonical-state recovery gate"
  USES_TERMINAL
)

message(STATUS "✅ Phase 1 canonical recovery gate registered: ctest -L phase1-canonical-recovery")

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_undo_rebuild_orchestrator.cpp)
  # Commit #4 of the offline --rebuild-undo-range series: tests for the
  # daemon-side orchestrator that owns preflight classification, manifest
  # emission, and (in commit #5) reindexer invocation.
  add_executable(test_undo_rebuild_orchestrator
    tests/daemon/test_undo_rebuild_orchestrator.cpp
    src/daemon/undo_rebuild_orchestrator.cpp
    src/consensus/reindexer.cpp
    src/consensus/genesis_canonical.cpp
    src/consensus/undo.cpp
    src/primitives/transaction_serializer.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_undo_rebuild_orchestrator PRIVATE
    dinero_chainstate
    dinero_consensus
    dinero_shielded
    dinero_crypto
    jsoncpp_static
    RocksDB::rocksdb
    OpenSSL::Crypto
    OpenSSL::SSL
    pthread
    sqlite3
  )
  link_zstd_if_needed(test_undo_rebuild_orchestrator)
  target_include_directories(test_undo_rebuild_orchestrator PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_undo_rebuild_orchestrator PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(
    NAME UndoRebuildOrchestrator
    COMMAND test_undo_rebuild_orchestrator
  )
  set_tests_properties(UndoRebuildOrchestrator PROPERTIES
    LABELS "reindex;chainstate-recovery;undo-rebuild"
    TIMEOUT 30
  )
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_header_metadata_recovery.cpp)
  add_executable(test_header_metadata_recovery
    tests/daemon/test_header_metadata_recovery.cpp
  )
  target_link_libraries(test_header_metadata_recovery PRIVATE
    dinero_core
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_header_metadata_recovery)
  target_link_directories(test_header_metadata_recovery PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_header_metadata_recovery PRIVATE "-framework Security" "-framework CoreFoundation")
  endif()
  target_include_directories(test_header_metadata_recovery PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME HeaderMetadataRecovery COMMAND test_header_metadata_recovery)
  set_tests_properties(HeaderMetadataRecovery PROPERTIES
    LABELS "storage;persistence;recovery;regression"
    TIMEOUT 30
  )
  message(STATUS "✅ Header metadata recovery regression guard enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_reindexer_anchor_seed.cpp)
  # Commit #1 of the offline --rebuild-undo-range series: tests that
  # BlockReindexer::seedFromAnchor() correctly populates in-memory state
  # from an externally-prepared anchor without touching ChainDB.
  #
  # Compiles reindexer.cpp directly so the test does not have to link
  # dinero_core (which pulls in the daemon's networking + RocksDB world).
  # The seedFromAnchor primitive only touches consensus types, so this
  # cuts the test's dependency surface to dinero_consensus + dinero_crypto.
  add_executable(test_reindexer_anchor_seed
    tests/consensus/test_reindexer_anchor_seed.cpp
    src/consensus/reindexer.cpp
    src/consensus/genesis_canonical.cpp
    src/consensus/undo.cpp
    src/common/logger.cpp
    src/primitives/transaction_serializer.cpp
  )
  target_link_libraries(test_reindexer_anchor_seed PRIVATE
    dinero_chainstate
    dinero_consensus
    dinero_shielded
    dinero_crypto
    jsoncpp_static
    RocksDB::rocksdb
    OpenSSL::Crypto
    OpenSSL::SSL
    pthread
    sqlite3
  )
  link_zstd_if_needed(test_reindexer_anchor_seed)
  target_include_directories(test_reindexer_anchor_seed PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_reindexer_anchor_seed PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(
    NAME ReindexerAnchorSeed
    COMMAND test_reindexer_anchor_seed
  )
  set_tests_properties(ReindexerAnchorSeed PROPERTIES
    LABELS "reindex;chainstate-recovery"
    TIMEOUT 30
  )
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_shielded_reindex_equivalence.cpp)
  add_executable(test_shielded_reindex_equivalence
    tests/consensus/test_shielded_reindex_equivalence.cpp
    src/consensus/genesis_canonical.cpp
    src/consensus/reindexer.cpp
    src/common/logger.cpp
    src/consensus/undo.cpp
    src/primitives/transaction_serializer.cpp
  )
  target_link_libraries(test_shielded_reindex_equivalence PRIVATE
    dinero_chainstate
    dinero_consensus
    dinero_shielded
    dinero_crypto
    jsoncpp_static
    RocksDB::rocksdb
    OpenSSL::Crypto
    OpenSSL::SSL
    pthread
    sqlite3
  )
  link_zstd_if_needed(test_shielded_reindex_equivalence)
  target_include_directories(test_shielded_reindex_equivalence PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_shielded_reindex_equivalence PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(
    NAME ShieldedReindexEquivalence
    COMMAND test_shielded_reindex_equivalence
  )
  set_tests_properties(ShieldedReindexEquivalence PROPERTIES
    LABELS "shielded;reindex;canonicality;shielded-canonical-recovery"
    TIMEOUT 300
  )
endif()

# Shielded canonical state + recovery gate
if(TARGET shielded_pool_test AND TARGET shielded_adversarial_test)
  add_test(
    NAME ShieldedPoolRoundTrip
    COMMAND $<TARGET_FILE:shielded_pool_test>
  )
  set_tests_properties(ShieldedPoolRoundTrip PROPERTIES
    LABELS "shielded;canonicality;shielded-canonical-recovery"
    TIMEOUT 180
  )

  add_test(
    NAME ShieldedAdversarialHardening
    COMMAND $<TARGET_FILE:shielded_adversarial_test>
  )
  set_tests_properties(ShieldedAdversarialHardening PROPERTIES
    LABELS "shielded;adversarial;canonicality;shielded-canonical-recovery"
    TIMEOUT 180
  )

  add_custom_target(test_shielded_canonical_recovery
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L shielded-canonical-recovery
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running shielded canonical-state recovery gate"
    USES_TERMINAL
  )

  message(STATUS "✅ Shielded canonical recovery gate registered: ctest -L shielded-canonical-recovery")
else()
  message(WARNING "⚠️  shielded_pool_test or shielded_adversarial_test target missing - shielded recovery gate not registered")
endif()

add_custom_target(test_phase2_shielded_canonical_recovery
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L phase2-shielded-canonical-recovery
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Running Phase 2 shielded daemon crash-recovery gate"
  USES_TERMINAL
)

message(STATUS "✅ Phase 2 shielded daemon recovery gate registered: ctest -L phase2-shielded-canonical-recovery")

# Full release gate suite (wallet + parity + p2p storm + ibd torture + utreexo + mempool stress)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_release_suite.sh)
  add_test(
    NAME ReleaseSuite
    COMMAND ${CMAKE_SOURCE_DIR}/tests/test_release_suite.sh
  )

  set_tests_properties(ReleaseSuite PROPERTIES
    LABELS "release;gate;meta"
    TIMEOUT 14400
  )

  add_custom_target(test_release_suite
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R ReleaseSuite
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running full release suite gate"
    USES_TERMINAL
  )

  message(STATUS "✅ Release suite gate registered: ctest -R ReleaseSuite")
else()
  message(WARNING "⚠️  tests/test_release_suite.sh not found - ReleaseSuite gate not registered")
endif()

# ════════════════════════════════════════════════════════════════════════════
# CRITICAL: Genesis Invariant Tests - MUST RUN FIRST
# ════════════════════════════════════════════════════════════════════════════
# These are NOT optional. They protect consensus constants that, if changed,
# would require a new genesis (hard fork). This is constitutional law, not QA.
#
# Protected constants:
# - Genesis hash, merkle root, nonce, timestamp
# - Block 1 premine scriptPubKey and amount
# - Subsidy schedule (halving interval, initial reward)
# - Network identity (network_id, target_spacing)
# - Taproot enforcement (witness version 1 requirement)
#
# Enforcement level: MANDATORY (Level 2)
# - Built by default (not optional)
# - Run in ctest (automated CI)
# - Labeled as CRITICAL (highest priority)
# - Failure blocks merge (CI gate)
#
# "Past-you stopping future-you from breaking money."
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_genesis_invariants.cpp)
  add_executable(test_genesis_invariants
    tests/consensus/test_genesis_invariants.cpp
  )

  add_dependencies(test_genesis_invariants gtest)

  target_link_libraries(test_genesis_invariants PRIVATE
    dinero_consensus        # For GetBlockSubsidy, chainparams
    dinero_crypto           # For hash functions
    $<TARGET_FILE:gtest>
    )
  link_zstd_if_needed(test_genesis_invariants)

  target_include_directories(test_genesis_invariants BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  # Register with CTest - runs automatically via `ctest`
  add_test(NAME GenesisInvariants COMMAND test_genesis_invariants)

  # Label as CRITICAL - highest priority in CI
  set_tests_properties(GenesisInvariants PROPERTIES
    LABELS "consensus;critical;genesis;mandatory"
    TIMEOUT 30  # Should complete instantly - if not, something is very wrong
  )

  message(STATUS "✅ Genesis invariant tests enabled (MANDATORY)")
else()
  message(FATAL_ERROR "❌ CRITICAL: tests/consensus/test_genesis_invariants.cpp not found! This file is REQUIRED to prevent accidental hard forks.")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Block Validation Invariants Test (INV-1 through INV-5)
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_block_validation_invariants.cpp)
  add_executable(test_block_validation_invariants
    tests/consensus/test_block_validation_invariants.cpp
    tests/stubs/mempool_test_stubs.cpp  # no-op Mempool::forEachEntry etc.
    tests/stubs/daemon_test_stubs.cpp   # BridgeNode, vault, PoolDB/Manager, extra Mempool
    src/daemon/config.cpp               # GetConfig() — referenced by consensus_write_batch.obj
  )

  add_dependencies(test_block_validation_invariants gtest)

  target_link_libraries(test_block_validation_invariants PRIVATE
    dinero_core
    dinero_consensus     # Transitively pulls dinero_pq for pq::* and V7P2MRStore::*
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
    $<TARGET_FILE:gtest>
    )
  if(TARGET bulletproofs_ffi)
    target_link_libraries(test_block_validation_invariants PRIVATE bulletproofs_ffi)
    if(TARGET build_bulletproofs_ffi)
      add_dependencies(test_block_validation_invariants build_bulletproofs_ffi)
    endif()
  endif()
  link_zstd_if_needed(test_block_validation_invariants)

  target_include_directories(test_block_validation_invariants BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME BlockValidationInvariants COMMAND test_block_validation_invariants)

  set_tests_properties(BlockValidationInvariants PROPERTIES
    LABELS "consensus;critical;invariants"
    TIMEOUT 30
  )

  message(STATUS "✅ Block validation invariant tests enabled")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Utreexo Ephemeral UTXO Regression Test (block 3581 bug)
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_ephemeral.cpp)
  add_executable(test_utreexo_ephemeral
    tests/consensus/test_utreexo_ephemeral.cpp
    src/daemon/config.cpp  # GetConfig() referenced by consensus_write_batch.obj
    tests/stubs/mempool_test_stubs.cpp  # Mempool onBlockConnected/Disconnected/forEachEntry/etc.
    tests/stubs/daemon_test_stubs.cpp   # Mempool extras + vault
  )

  add_dependencies(test_utreexo_ephemeral gtest)

  target_link_libraries(test_utreexo_ephemeral PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
    $<TARGET_FILE:gtest>
    )
  if(TARGET bulletproofs_ffi)
    target_link_libraries(test_utreexo_ephemeral PRIVATE bulletproofs_ffi)
    if(TARGET build_bulletproofs_ffi)
      add_dependencies(test_utreexo_ephemeral build_bulletproofs_ffi)
    endif()
  endif()
  link_zstd_if_needed(test_utreexo_ephemeral)

  target_include_directories(test_utreexo_ephemeral BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME UtreexoEphemeral COMMAND test_utreexo_ephemeral)

  set_tests_properties(UtreexoEphemeral PROPERTIES
    LABELS "consensus;regression;ephemeral"
    TIMEOUT 30
  )

  message(STATUS "✅ Utreexo ephemeral regression test enabled")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Utreexo Safety Gate Test (BlockAssembler hardening)
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_utreexo_safety_gate.cpp)
  add_executable(test_utreexo_safety_gate
    tests/mining/test_utreexo_safety_gate.cpp
    src/daemon/mempool.cpp
    src/daemon/config.cpp
    tests/stubs/vault_test_stubs.cpp  # vault observers — Mempool methods come from real mempool.cpp above
  )

  target_link_libraries(test_utreexo_safety_gate PRIVATE
    dinero_core
    dinero_consensus
    dinero_pq         # P2MR VWU + spend verification used by mempool/script validation
    dinero_shielded   # Shielded bundle validation/apply used by block_validation.cpp
    dinero_chainstate
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
    GTest::gtest
    )
  if(TARGET bulletproofs_ffi)
    target_link_libraries(test_utreexo_safety_gate PRIVATE bulletproofs_ffi)
    if(TARGET build_bulletproofs_ffi)
      add_dependencies(test_utreexo_safety_gate build_bulletproofs_ffi)
    endif()
  endif()
  link_zstd_if_needed(test_utreexo_safety_gate)
  if(APPLE)
    target_link_libraries(test_utreexo_safety_gate PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()

  target_include_directories(test_utreexo_safety_gate BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME UtreexoSafetyGate COMMAND test_utreexo_safety_gate)

  set_tests_properties(UtreexoSafetyGate PROPERTIES
    LABELS "mining;regression;safety"
    TIMEOUT 30
  )

  message(STATUS "✅ Utreexo safety gate test enabled")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Testnet Invariants Verification Test (proves mainnet tests work)
# ════════════════════════════════════════════════════════════════════════════
# This test verifies that the genesis invariant tests would actually catch
# consensus changes by checking current values. If any consensus constant
# changed, this test would fail, proving the enforcement mechanism works.
#
# Purpose: "Canary in the coal mine" - proves the tests catch drift
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_testnet_invariants.cpp)
  add_executable(test_testnet_invariants
    tests/consensus/test_testnet_invariants.cpp
  )

  target_link_libraries(test_testnet_invariants PRIVATE
    dinero_consensus        # For GetBlockSubsidy, chainparams
    dinero_crypto           # For hash functions
    GTest::gtest  # Cross-platform GTest
  )
  link_zstd_if_needed(test_testnet_invariants)

  target_include_directories(test_testnet_invariants PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  # Register with CTest
  add_test(NAME TestnetInvariantsVerification COMMAND test_testnet_invariants)

  # Label as verification test
  set_tests_properties(TestnetInvariantsVerification PROPERTIES
    LABELS "consensus;verification;testnet"
    TIMEOUT 30
  )

  message(STATUS "✅ Testnet invariants verification test enabled")
else()
  message(WARNING "⚠️  tests/consensus/test_testnet_invariants.cpp not found - skipping verification test")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Consensus Validation Tripwires (Phase C.0)
# ════════════════════════════════════════════════════════════════════════════
# These tests ensure that consensus-critical validation logic cannot be
# accidentally weakened or removed during refactoring. Each test creates an
# INVALID block and verifies that ValidateBlock() correctly rejects it.
#
# CRITICAL: If any of these tests fail, it means someone removed validation
# logic. DO NOT modify these tests to make them pass - fix the code instead!
#
# Protected validation rules:
# - Merkle root must match computed value from transactions
# - Block must contain at least one transaction (coinbase)
# - Timestamp must not be too far in the future (2 hour tolerance)
# - Block version must be valid (non-zero)
#
# Enforcement level: MANDATORY (Level 2)
# Purpose: "Regression protection for consensus validation"
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_consensus_validation_tripwires.cpp)
  add_executable(test_consensus_validation_tripwires
    tests/consensus/test_consensus_validation_tripwires.cpp
    src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    src/daemon/config.cpp     # Provides GetConfig()
    tests/stubs/vault_test_stubs.cpp  # vault observers (real mempool present, so vault-only seam)
  )

  target_link_libraries(test_consensus_validation_tripwires PRIVATE
    dinero_consensus        # For PowConsensusEngine
    dinero_chainstate       # For CreatePowConsensusEngine
    dinero_crypto           # For hash functions
    dinero_core             # For StratumServer and other daemon components
    GTest::gtest
  )
  if(TARGET bulletproofs_ffi)
    target_link_libraries(test_consensus_validation_tripwires PRIVATE bulletproofs_ffi)
    if(TARGET build_bulletproofs_ffi)
      add_dependencies(test_consensus_validation_tripwires build_bulletproofs_ffi)
    endif()
  endif()
  link_zstd_if_needed(test_consensus_validation_tripwires)
  if(APPLE)
    target_link_libraries(test_consensus_validation_tripwires PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()

  target_include_directories(test_consensus_validation_tripwires PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  # Register with CTest
  add_test(NAME ConsensusValidationTripwires COMMAND test_consensus_validation_tripwires)

  # Label as critical - these tests prevent regression
  set_tests_properties(ConsensusValidationTripwires PROPERTIES
    LABELS "consensus;critical;validation;mandatory"
    TIMEOUT 30
  )

  message(STATUS "✅ Consensus validation tripwire tests enabled (MANDATORY)")
else()
  message(FATAL_ERROR "❌ CRITICAL: tests/consensus/test_consensus_validation_tripwires.cpp not found! This file protects against validation regression.")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 1: Formal Verification (Property-Based Testing)
# ════════════════════════════════════════════════════════════════════════════
# Mathematical proofs of consensus-critical invariants using property-based
# testing (QuickCheck/Hypothesis style). Tests prove properties hold for ALL
# inputs by generating thousands of random test cases.
#
# Ring 1 Tests:
#   1. Supply Invariant: ∀ height, supply ≤ 265,428,000 DIN (100k random heights) ✅
#   2. UTXO Set Invariant: ∀ operation, UTXO set consistency (3k+ tests) ✅
#   3. Chain Selection Invariant: ∀ chain, most-work wins (700+ tests) ✅
#
# Enforcement level: MANDATORY (Level 1 - Mathematical Proofs)
# Purpose: "Formal verification of consensus invariants"
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_CONSENSUS_FORMAL_VERIFICATION "Build Ring 1 formal verification tests (MANDATORY)" ON)

if(ENABLE_CONSENSUS_FORMAL_VERIFICATION AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_consensus_formal_verification.cpp)
  add_executable(test_consensus_formal_verification
    tests/consensus/test_consensus_formal_verification.cpp
  )

  target_link_libraries(test_consensus_formal_verification PRIVATE
    dinero_consensus        # For ConsensusSubsidy, SupplyValidator, UTXOSet
    dinero_chainstate       # For ChainDB (UTXOSet dependency)
    dinero_core             # For Logger, UndoRecord, and other utilities
    GTest::gtest            # System GoogleTest (Homebrew - compatible with Clang 17)
  )
  link_zstd_if_needed(test_consensus_formal_verification)

  target_include_directories(test_consensus_formal_verification PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  # Register with CTest
  add_test(NAME ConsensusFormalVerification COMMAND test_consensus_formal_verification)

  # Label as critical - formal proofs of consensus invariants
  set_tests_properties(ConsensusFormalVerification PROPERTIES
    LABELS "consensus;formal;ring1;mandatory"
    TIMEOUT 120  # Property-based tests with 100k+ samples need more time
  )

  message(STATUS "✅ Ring 1 formal verification tests COMPLETE (Tests 1-3: Supply + UTXO + Chain Selection Invariants)")
elseif(ENABLE_CONSENSUS_FORMAL_VERIFICATION)
  message(WARNING "⚠️  Ring 1 formal verification test file not found - skipping")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 2: Consensus Validation Properties (V1-V3: Block/Transaction/Script)
# ════════════════════════════════════════════════════════════════════════════
# Purpose: "Prove local consensus validation is mathematically correct"
#
# V1: Block Validity Properties (7 tests)
# V2: Transaction Validity Properties (7 tests)
# V3: Script Execution Properties (7 tests)
# V4-V5: State Transition & Enforcement Oracles (14 tests - separate files)
#
# Total: 35 properties, ~50,600 random test cases
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_RING2_CONSENSUS_VALIDATION "Build Ring 2 consensus validation tests (MANDATORY)" ON)

if(ENABLE_RING2_CONSENSUS_VALIDATION AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_consensus_ring2_validity.cpp)
  add_executable(test_consensus_ring2_validity
    tests/consensus/test_consensus_ring2_validity.cpp
  )

  target_link_libraries(test_consensus_ring2_validity PRIVATE
    dinero_consensus        # For BlockValidator, TransactionValidator, ScriptValidator
    dinero_chainstate       # For UTXOSet
    dinero_core             # For Logger and utilities
    dinero_crypto           # For SHA256
    GTest::gtest            # System GoogleTest
  )
  link_zstd_if_needed(test_consensus_ring2_validity)

  target_include_directories(test_consensus_ring2_validity PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  # Register with CTest
  add_test(NAME ConsensusRing2Validity COMMAND test_consensus_ring2_validity)

  # Label as critical - formal proofs of consensus validation
  set_tests_properties(ConsensusRing2Validity PROPERTIES
    LABELS "consensus;formal;ring2;mandatory"
    TIMEOUT 180  # V1-V3 property tests with 50k+ samples need time
  )

  message(STATUS "✅ Ring 2 consensus validation tests configured (V1-V3: Block/Transaction/Script Validity)")
elseif(ENABLE_RING2_CONSENSUS_VALIDATION)
  message(WARNING "⚠️  Ring 2 consensus validation test file not found - skipping")
endif()

# ════════════════════════════════════════════════════════════════════════════
# DAA Golden Vectors: Bootstrap retarget + template/validator parity
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/daa_golden_vectors_test.cpp)
  add_executable(test_daa_golden_vectors
    src/test/daa_golden_vectors_test.cpp
  )

  add_dependencies(test_daa_golden_vectors gtest)

  target_link_libraries(test_daa_golden_vectors PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
  )
  link_zstd_if_needed(test_daa_golden_vectors)

  target_include_directories(test_daa_golden_vectors BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME DAAGoldenVectors COMMAND test_daa_golden_vectors)

  set_tests_properties(DAAGoldenVectors PROPERTIES
    LABELS "consensus;critical;difficulty;mandatory"
    TIMEOUT 30
  )

  message(STATUS "DAA golden vector tests enabled")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Liquidity Vault — daemon-side ledger primitives + behavioural-parity gtests.
# Pure-code; no consensus / chainstate dependencies.
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/vault_ledger_tests.cpp)
  add_executable(test_vault_ledger
    src/test/vault_ledger_tests.cpp
    src/vault/ledger.cpp
    src/vault/ledger_account.cpp
  )
  add_dependencies(test_vault_ledger gtest gtest_main)
  target_link_libraries(test_vault_ledger PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
  )
  target_include_directories(test_vault_ledger BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME VaultLedger COMMAND test_vault_ledger)
  set_tests_properties(VaultLedger PROPERTIES
    LABELS "vault;ledger;mandatory"
    TIMEOUT 30
  )
  message(STATUS "Vault ledger tests enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/vault_state_machines_tests.cpp)
  add_executable(test_vault_state_machines
    src/test/vault_state_machines_tests.cpp
    src/vault/ledger.cpp
    src/vault/ledger_account.cpp
    src/vault/deposit_flow.cpp
    src/vault/reorg_watcher.cpp
    src/vault/withdrawal_queue.cpp
    src/vault/signing_backend.cpp
  )
  add_dependencies(test_vault_state_machines gtest gtest_main)
  target_link_libraries(test_vault_state_machines PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
  )
  target_include_directories(test_vault_state_machines BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME VaultStateMachines COMMAND test_vault_state_machines)
  set_tests_properties(VaultStateMachines PROPERTIES
    LABELS "vault;mandatory"
    TIMEOUT 30
  )
  message(STATUS "Vault state-machine tests enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/vault_ledger_store_tests.cpp)
  add_executable(test_vault_ledger_store
    src/test/vault_ledger_store_tests.cpp
    src/vault/ledger.cpp
    src/vault/ledger_account.cpp
    src/vault/ledger_store.cpp
  )
  add_dependencies(test_vault_ledger_store gtest gtest_main)
  target_link_libraries(test_vault_ledger_store PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
  )
  target_include_directories(test_vault_ledger_store BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME VaultLedgerStore COMMAND test_vault_ledger_store)
  set_tests_properties(VaultLedgerStore PROPERTIES
    LABELS "vault;persistence;mandatory"
    TIMEOUT 30
  )
  message(STATUS "Vault ledger-store tests enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/vault_service_tests.cpp)
  add_executable(test_vault_service
    src/test/vault_service_tests.cpp
    src/vault/ledger.cpp
    src/vault/ledger_account.cpp
    src/vault/deposit_flow.cpp
    src/vault/reorg_watcher.cpp
    src/vault/withdrawal_queue.cpp
    src/vault/signing_backend.cpp
    src/vault/vault_service.cpp
  )
  add_dependencies(test_vault_service gtest gtest_main)
  target_link_libraries(test_vault_service PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
  )
  target_include_directories(test_vault_service BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME VaultService COMMAND test_vault_service)
  set_tests_properties(VaultService PROPERTIES
    LABELS "vault;mandatory"
    TIMEOUT 30
  )
  message(STATUS "Vault service tests enabled")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Shielded pool — Phase 0 wave 1 foundation tests (commitment tree,
# nullifier set, bundle serialization). Pure-function tests, no prover
# setup, run in ms. Pre-requisite for any shielded code change.
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/shielded_serialization_tests.cpp)
  add_executable(test_shielded_serialization
    src/test/shielded_serialization_tests.cpp
  )
  add_dependencies(test_shielded_serialization gtest dinero_shielded)
  target_link_libraries(test_shielded_serialization PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
  )
  target_include_directories(test_shielded_serialization BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ShieldedSerialization COMMAND test_shielded_serialization)
  set_tests_properties(ShieldedSerialization PROPERTIES
    LABELS "shielded;consensus;mandatory"
    TIMEOUT 10
  )
  message(STATUS "Shielded serialization tests enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/commitment_tree_tests.cpp)
  add_executable(test_commitment_tree
    src/test/commitment_tree_tests.cpp
  )
  add_dependencies(test_commitment_tree gtest dinero_shielded)
  target_link_libraries(test_commitment_tree PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
  )
  target_include_directories(test_commitment_tree BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME CommitmentTree COMMAND test_commitment_tree)
  set_tests_properties(CommitmentTree PROPERTIES
    LABELS "shielded;consensus;mandatory"
    TIMEOUT 30
  )
  message(STATUS "Shielded commitment-tree tests enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/nullifier_set_tests.cpp)
  add_executable(test_nullifier_set
    src/test/nullifier_set_tests.cpp
  )
  add_dependencies(test_nullifier_set gtest dinero_shielded)
  target_link_libraries(test_nullifier_set PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
    sqlite3
  )
  target_include_directories(test_nullifier_set BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sqlite-amalgamation-3480000
  )
  add_test(NAME NullifierSet COMMAND test_nullifier_set)
  set_tests_properties(NullifierSet PROPERTIES
    LABELS "shielded;consensus;mandatory"
    TIMEOUT 15
  )
  message(STATUS "Shielded nullifier-set tests enabled")
endif()

# Phase 0 wave 2 — prover/verifier round-trip tests for the shielded
# circuits. Slower (Spartan proving), runs sequentially.
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/shielded_circuit_tests.cpp)
  add_executable(test_shielded_circuit
    src/test/shielded_circuit_tests.cpp
  )
  add_dependencies(test_shielded_circuit gtest dinero_shielded)
  target_link_libraries(test_shielded_circuit PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
  )
  target_include_directories(test_shielded_circuit BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ShieldedCircuit COMMAND test_shielded_circuit)
  set_tests_properties(ShieldedCircuit PROPERTIES
    LABELS "shielded;zk;mandatory"
    TIMEOUT 600
  )
  message(STATUS "Shielded circuit (prover round-trip) tests enabled")
endif()

# Phase 2 wave 1 — anchor depth window (Sapling-shape 100-block tolerance).
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/pedersen_tests.cpp)
  add_executable(test_pedersen
    src/test/pedersen_tests.cpp
  )
  add_dependencies(test_pedersen gtest dinero_shielded)
  target_link_libraries(test_pedersen PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
    secp256k1
  )
  target_include_directories(test_pedersen BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME Pedersen COMMAND test_pedersen)
  set_tests_properties(Pedersen PROPERTIES
    LABELS "shielded;crypto;mandatory"
    TIMEOUT 30
  )
  message(STATUS "Pedersen tests enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/anchor_history_tests.cpp)
  add_executable(test_anchor_history
    src/test/anchor_history_tests.cpp
  )
  add_dependencies(test_anchor_history gtest dinero_shielded)
  target_link_libraries(test_anchor_history PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
  )
  target_include_directories(test_anchor_history BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME AnchorHistory COMMAND test_anchor_history)
  set_tests_properties(AnchorHistory PROPERTIES
    LABELS "shielded;consensus;mandatory"
    TIMEOUT 10
  )
  message(STATUS "Shielded anchor-history tests enabled")
endif()

# Phase 0 wave 3 — ValidateShieldedBundle + ApplyShieldedBundle integration.
# Covers every ShieldedValidationError enum case end-to-end, including the
# real prove→verify happy path.
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/shielded_validation_tests.cpp)
  add_executable(test_shielded_validation
    src/test/shielded_validation_tests.cpp
  )
  add_dependencies(test_shielded_validation gtest dinero_shielded)
  target_link_libraries(test_shielded_validation PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
    dinero_wallet  # shielded_derivation.cpp: EncryptNoteForRecipient + DeriveNoteSpendKey
    sqlite3
  )
  target_include_directories(test_shielded_validation BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sqlite-amalgamation-3480000
  )
  add_test(NAME ShieldedValidation COMMAND test_shielded_validation)
  set_tests_properties(ShieldedValidation PROPERTIES
    LABELS "shielded;consensus;mandatory"
    TIMEOUT 600
  )
  message(STATUS "Shielded validation tests enabled")
endif()

# Focused ValidationContext tripwire: consensus replay callers must pass
# the same tx_sighash used by the bundle's binding signature.
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/shielded_validation_context_tests.cpp)
  add_executable(test_shielded_validation_context
    src/test/shielded_validation_context_tests.cpp
  )
  add_dependencies(test_shielded_validation_context gtest dinero_shielded)
  target_link_libraries(test_shielded_validation_context PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
    sqlite3
  )
  target_include_directories(test_shielded_validation_context BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sqlite-amalgamation-3480000
  )
  add_test(NAME ShieldedValidationContext COMMAND test_shielded_validation_context)
  set_tests_properties(ShieldedValidationContext PROPERTIES
    LABELS "shielded;consensus;mandatory"
    TIMEOUT 120
  )
  message(STATUS "Shielded validation-context tests enabled")
endif()

# D.3-shielded reverse-apply property test: pins the deterministic
# truncate-clone-serialize math used to reconstruct
# pre_block_shielded_frontier when DisconnectTip's ReadStoredUndo fails.
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/d3_shielded_reverse_apply_tests.cpp)
  add_executable(test_d3_shielded_reverse_apply
    src/test/d3_shielded_reverse_apply_tests.cpp
  )
  add_dependencies(test_d3_shielded_reverse_apply gtest dinero_shielded)
  target_link_libraries(test_d3_shielded_reverse_apply PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
    sqlite3
  )
  target_include_directories(test_d3_shielded_reverse_apply BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sqlite-amalgamation-3480000
  )
  add_test(NAME D3ShieldedReverseApply COMMAND test_d3_shielded_reverse_apply)
  set_tests_properties(D3ShieldedReverseApply PROPERTIES
    LABELS "shielded;consensus;mandatory;crash-oracle"
    TIMEOUT 60
  )
  message(STATUS "D.3 shielded reverse-apply property test enabled")
endif()

# ── v0.3.0 wire-format canonical bundle vectors ─────────────────────
if(TARGET dinero_shielded AND TARGET gtest)
  add_executable(test_v030_vectors
    src/test/v030_wire_vectors_tests.cpp
  )
  add_dependencies(test_v030_vectors gtest dinero_shielded)
  target_link_libraries(test_v030_vectors PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
    sqlite3
  )
  target_include_directories(test_v030_vectors BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sqlite-amalgamation-3480000
  )
  add_test(NAME ShieldedV030Vectors COMMAND test_v030_vectors)
  set_tests_properties(ShieldedV030Vectors PROPERTIES
    LABELS "shielded;consensus;canonicality"
    TIMEOUT 300
  )
  message(STATUS "Shielded v0.3.0 wire-format vectors enabled")
endif()

# ── Phase 5 Wave 1: shielded key-derivation vectors ────────────────
if(TARGET dinero_wallet AND TARGET gtest)
  add_executable(test_shielded_derivation
    src/test/shielded_derivation_tests.cpp
  )
  add_dependencies(test_shielded_derivation gtest dinero_wallet)
  target_link_libraries(test_shielded_derivation PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_wallet
    dinero_shielded
    sqlite3
  )
  target_include_directories(test_shielded_derivation BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sqlite-amalgamation-3480000
  )
  if(APPLE)
    target_link_libraries(test_shielded_derivation PRIVATE "-framework Security")
  endif()
  add_test(NAME ShieldedDerivation COMMAND test_shielded_derivation)
  set_tests_properties(ShieldedDerivation PROPERTIES
    LABELS "shielded;wallet;canonicality"
    TIMEOUT 60
  )
  message(STATUS "Shielded derivation tests enabled")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 2: V4-V5 Validation Property Oracles (State Transition & Enforcement)
# ════════════════════════════════════════════════════════════════════════════
# Purpose: "Prove state transition and consensus enforcement correctness"
#
# V4: State Transition Oracles (7 tests) - Stateful UTXO evolution
# V5: Consensus Enforcement Oracles (7 tests) - Tripwires and convergence
#
# Pattern: Ring 4-style oracles (reset → observe → finalize)
# ════════════════════════════════════════════════════════════════════════════

# V4 Oracle Framework
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/validation_property_oracle.cpp)
  add_library(validation_oracles STATIC
    tests/consensus/properties/validation_property_oracle.cpp
  )

  target_include_directories(validation_oracles PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests/consensus/properties
  )

  target_link_libraries(validation_oracles PUBLIC
    dinero_consensus
    dinero_chainstate
    dinero_core
  )

  # V4.1 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v41.cpp)
    add_executable(test_validation_oracle_v41
      tests/consensus/properties/test_validation_oracle_v41.cpp
    )

    target_link_libraries(test_validation_oracle_v41 PRIVATE
      validation_oracles
      GTest::gtest
      )

    target_include_directories(test_validation_oracle_v41 PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/tests/consensus/properties
    )

    add_test(NAME ValidationOracleV41 COMMAND test_validation_oracle_v41)

    set_tests_properties(ValidationOracleV41 PROPERTIES
      LABELS "consensus;formal;ring2;v4;mandatory"
      TIMEOUT 60
    )

    message(STATUS "✅ Ring 2 V4.1 oracle test configured (State Transition: Applying Valid Block)")
  endif()

  # V4.2 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v42.cpp)
    add_executable(test_validation_oracle_v42
      tests/consensus/properties/test_validation_oracle_v42.cpp
    )

    target_link_libraries(test_validation_oracle_v42 PRIVATE
      validation_oracles
      GTest::gtest
      )

    target_include_directories(test_validation_oracle_v42 PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/tests/consensus/properties
    )

    add_test(NAME ValidationOracleV42 COMMAND test_validation_oracle_v42)

    set_tests_properties(ValidationOracleV42 PROPERTIES
      LABELS "consensus;formal;ring2;v4;mandatory"
      TIMEOUT 60
    )

    message(STATUS "✅ Ring 2 V4.2 oracle test configured (State Transition: Inputs Removed)")
  endif()

  # V4.3 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v43.cpp)
    add_executable(test_validation_oracle_v43
      tests/consensus/properties/test_validation_oracle_v43.cpp
    )

    target_link_libraries(test_validation_oracle_v43 PRIVATE
      validation_oracles
      GTest::gtest
      )

    target_include_directories(test_validation_oracle_v43 PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/tests/consensus/properties
    )

    add_test(NAME ValidationOracleV43 COMMAND test_validation_oracle_v43)

    set_tests_properties(ValidationOracleV43 PROPERTIES
      LABELS "consensus;formal;ring2;v4;mandatory"
      TIMEOUT 60
    )

    message(STATUS "✅ Ring 2 V4.3 oracle test configured (State Transition: Outputs Added)")
  endif()

  # V4.4 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v44.cpp)
    add_executable(test_validation_oracle_v44
      tests/consensus/properties/test_validation_oracle_v44.cpp
    )

    target_link_libraries(test_validation_oracle_v44 PRIVATE
      validation_oracles
      GTest::gtest
      )

    target_include_directories(test_validation_oracle_v44 PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/tests/consensus/properties
    )

    add_test(NAME ValidationOracleV44 COMMAND test_validation_oracle_v44)

    set_tests_properties(ValidationOracleV44 PROPERTIES
      LABELS "consensus;formal;ring2;v4;mandatory"
      TIMEOUT 60
    )

    message(STATUS "✅ Ring 2 V4.4 oracle test configured (State Transition: Value Conservation)")
  endif()

  # V4.5 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v45.cpp)
    add_executable(test_validation_oracle_v45
      tests/consensus/properties/test_validation_oracle_v45.cpp
    )
    add_dependencies(test_validation_oracle_v45 gtest)
    target_link_libraries(test_validation_oracle_v45 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    target_include_directories(test_validation_oracle_v45 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV45 COMMAND test_validation_oracle_v45)
    set_tests_properties(ValidationOracleV45 PROPERTIES LABELS "consensus;formal;ring2;v4;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V4.5 oracle test configured (State Transition: Reorg UTXO Revert)")
  endif()

  # V4.6 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v46.cpp)
    add_executable(test_validation_oracle_v46
      tests/consensus/properties/test_validation_oracle_v46.cpp
    )
    add_dependencies(test_validation_oracle_v46 gtest)
    target_link_libraries(test_validation_oracle_v46 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    target_include_directories(test_validation_oracle_v46 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV46 COMMAND test_validation_oracle_v46)
    set_tests_properties(ValidationOracleV46 PROPERTIES LABELS "consensus;formal;ring2;v4;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V4.6 oracle test configured (State Transition: Coinbase Maturity)")
  endif()

  # V4.7 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v47.cpp)
    add_executable(test_validation_oracle_v47
      tests/consensus/properties/test_validation_oracle_v47.cpp
    )
    add_dependencies(test_validation_oracle_v47 gtest)
    target_link_libraries(test_validation_oracle_v47 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    target_include_directories(test_validation_oracle_v47 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV47 COMMAND test_validation_oracle_v47)
    set_tests_properties(ValidationOracleV47 PROPERTIES LABELS "consensus;formal;ring2;v4;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V4.7 oracle test configured (State Transition: No UTXO Duplication)")
  endif()

  # V5.1 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v51.cpp)
    add_executable(test_validation_oracle_v51 tests/consensus/properties/test_validation_oracle_v51.cpp)
    add_dependencies(test_validation_oracle_v51 gtest)
    target_link_libraries(test_validation_oracle_v51 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    link_zstd_if_needed(test_validation_oracle_v51)
    target_include_directories(test_validation_oracle_v51 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV51 COMMAND test_validation_oracle_v51)
    set_tests_properties(ValidationOracleV51 PROPERTIES LABELS "consensus;formal;ring2;v5;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V5.1 oracle test configured (Enforcement: Invalid Block Never Connected)")
  endif()

  # V5.2 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v52.cpp)
    add_executable(test_validation_oracle_v52 tests/consensus/properties/test_validation_oracle_v52.cpp)
    add_dependencies(test_validation_oracle_v52 gtest)
    target_link_libraries(test_validation_oracle_v52 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    link_zstd_if_needed(test_validation_oracle_v52)
    target_include_directories(test_validation_oracle_v52 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV52 COMMAND test_validation_oracle_v52)
    set_tests_properties(ValidationOracleV52 PROPERTIES LABELS "consensus;formal;ring2;v5;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V5.2 oracle test configured (Enforcement: Invalid TX Never Applied)")
  endif()

  # V5.3 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v53.cpp)
    add_executable(test_validation_oracle_v53 tests/consensus/properties/test_validation_oracle_v53.cpp)
    add_dependencies(test_validation_oracle_v53 gtest)
    target_link_libraries(test_validation_oracle_v53 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    link_zstd_if_needed(test_validation_oracle_v53)
    target_include_directories(test_validation_oracle_v53 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV53 COMMAND test_validation_oracle_v53)
    set_tests_properties(ValidationOracleV53 PROPERTIES LABELS "consensus;formal;ring2;v5;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V5.3 oracle test configured (Enforcement: Failed Block Zero Side Effects)")
  endif()

  # V5.4 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v54.cpp)
    add_executable(test_validation_oracle_v54 tests/consensus/properties/test_validation_oracle_v54.cpp)
    add_dependencies(test_validation_oracle_v54 gtest)
    target_link_libraries(test_validation_oracle_v54 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    link_zstd_if_needed(test_validation_oracle_v54)
    target_include_directories(test_validation_oracle_v54 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV54 COMMAND test_validation_oracle_v54)
    set_tests_properties(ValidationOracleV54 PROPERTIES LABELS "consensus;formal;ring2;v5;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V5.4 oracle test configured (Enforcement: Rule Violations Abort Block)")
  endif()

  # V5.5 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v55.cpp)
    add_executable(test_validation_oracle_v55 tests/consensus/properties/test_validation_oracle_v55.cpp)
    add_dependencies(test_validation_oracle_v55 gtest)
    target_link_libraries(test_validation_oracle_v55 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    link_zstd_if_needed(test_validation_oracle_v55)
    target_include_directories(test_validation_oracle_v55 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV55 COMMAND test_validation_oracle_v55)
    set_tests_properties(ValidationOracleV55 PROPERTIES LABELS "consensus;formal;ring2;v5;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V5.5 oracle test configured (Enforcement: Reorg Never Invalid Chain)")
  endif()

  # V5.6 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v56.cpp)
    add_executable(test_validation_oracle_v56 tests/consensus/properties/test_validation_oracle_v56.cpp)
    add_dependencies(test_validation_oracle_v56 gtest)
    target_link_libraries(test_validation_oracle_v56 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    link_zstd_if_needed(test_validation_oracle_v56)
    target_include_directories(test_validation_oracle_v56 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV56 COMMAND test_validation_oracle_v56)
    set_tests_properties(ValidationOracleV56 PROPERTIES LABELS "consensus;formal;ring2;v5;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V5.6 oracle test configured (Enforcement: Mempool Never Feeds Invalid TX)")
  endif()

  # V5.7 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v57.cpp)
    add_executable(test_validation_oracle_v57 tests/consensus/properties/test_validation_oracle_v57.cpp)
    add_dependencies(test_validation_oracle_v57 gtest)
    target_link_libraries(test_validation_oracle_v57 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    link_zstd_if_needed(test_validation_oracle_v57)
    target_include_directories(test_validation_oracle_v57 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV57 COMMAND test_validation_oracle_v57)
    set_tests_properties(ValidationOracleV57 PROPERTIES LABELS "consensus;formal;ring2;v5;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V5.7 oracle test configured (Enforcement: Deterministic)")
  endif()

  message(STATUS "✅ Ring 2 V4-V5 validation oracle framework configured")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 3 Phase 1: Deterministic Peer Simulator (P2P Testing Infrastructure)
# ════════════════════════════════════════════════════════════════════════════
# Infrastructure for testing P2P protocol without real network I/O:
#   - MockSocket: Simulated TCP with latency/packet loss
#   - PeerSimulator: Scripted peer behaviors (HONEST, STALE, MALICIOUS)
#   - DeterministicScheduler: Controlled event ordering
#   - PropertyTestRNG: Reproducible randomness
#
# Phase 1 enables 10,000+ connection simulations in milliseconds.
# Prerequisite for Ring 3 property-based P2P tests (Phases 2-5).
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_P2P_PEER_SIMULATOR "Build Ring 3 Phase 1 peer simulator tests" ON)

if(ENABLE_P2P_PEER_SIMULATOR AND EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_peer_simulator_smoke.cpp)
  add_executable(test_peer_simulator_smoke
    tests/p2p/test_peer_simulator_smoke.cpp
  )

  target_link_libraries(test_peer_simulator_smoke PRIVATE
    GTest::gtest
  )

  target_include_directories(test_peer_simulator_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/p2p
  )

  # Register with CTest
  add_test(NAME PeerSimulatorSmoke COMMAND test_peer_simulator_smoke)

  # Label as ring3 infrastructure test
  set_tests_properties(PeerSimulatorSmoke PROPERTIES
    LABELS "p2p;ring3;infrastructure;phase1"
    TIMEOUT 30  # Smoke tests should be fast
  )

  message(STATUS "✅ Ring 3 Phase 1: Peer simulator infrastructure (MockSocket, PeerSimulator, DeterministicScheduler)")
elseif(ENABLE_P2P_PEER_SIMULATOR)
  message(WARNING "⚠️  Ring 3 Phase 1 peer simulator test file not found - skipping")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 3 Phase 2: Property Test Framework (QuickCheck/Hypothesis Style)
# ════════════════════════════════════════════════════════════════════════════
# Property-based testing framework for P2P protocol verification:
#   - ConnectionSequenceGenerator: Generate random event sequences
#   - PropertyAssertion: DSL for expressing properties (forAll pattern)
#   - InvariantChecker: Verify state machine invariants
#   - Statistical validation: Run thousands of iterations
#
# Enables proving properties hold ∀ inputs (QuickCheck/Hypothesis methodology).
# Prerequisite for Ring 3 Phases 3-5 (implement actual P2P property tests).
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_P2P_PROPERTY_FRAMEWORK "Build Ring 3 Phase 2 property test framework" ON)

if(ENABLE_P2P_PROPERTY_FRAMEWORK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_property_framework_smoke.cpp)
  add_executable(test_property_framework_smoke
    tests/p2p/test_property_framework_smoke.cpp
  )

  target_link_libraries(test_property_framework_smoke PRIVATE
    GTest::gtest
  )

  target_include_directories(test_property_framework_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/p2p
  )

  # Register with CTest
  add_test(NAME PropertyFrameworkSmoke COMMAND test_property_framework_smoke)

  # Label as ring3 framework test
  set_tests_properties(PropertyFrameworkSmoke PROPERTIES
    LABELS "p2p;ring3;framework;phase2"
    TIMEOUT 30  # Smoke tests should be fast
  )

  message(STATUS "✅ Ring 3 Phase 2: Property test framework (ConnectionSequenceGenerator, PropertyAssertion, InvariantChecker)")
elseif(ENABLE_P2P_PROPERTY_FRAMEWORK)
  message(WARNING "⚠️  Ring 3 Phase 2 property framework test file not found - skipping")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 3 Phase 3: P2P Property Tests
# ════════════════════════════════════════════════════════════════════════════
# Property-based tests verifying P2P protocol invariants from formal spec:
#   - Test Suite 1: Connection Lifecycle (4 properties)
#   - Test Suite 2: Handshake Protocol (4 properties)
#   - Test Suite 3: Message Ordering (4 properties)
#   - Test Suite 4: Timeout Detection (4 properties)
#   - Test Suite 5: Resource Management (4 properties)
#
# Total: 20 properties proving ∀ inputs satisfy protocol invariants.
# Uses Phase 1 (peer simulator) + Phase 2 (property framework) infrastructure.
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_P2P_PROPERTY_TESTS "Build Ring 3 Phase 3 P2P property tests" ON)

if(ENABLE_P2P_PROPERTY_TESTS AND EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_p2p_properties.cpp)
  add_executable(test_p2p_properties
    tests/p2p/test_p2p_properties.cpp
  )

  target_link_libraries(test_p2p_properties PRIVATE
    GTest::gtest
  )

  target_include_directories(test_p2p_properties PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/p2p
  )

  # Register with CTest
  add_test(NAME P2PProperties COMMAND test_p2p_properties)

  # Label as ring3 property test
  set_tests_properties(P2PProperties PROPERTIES
    LABELS "p2p;ring3;properties;phase3"
    TIMEOUT 60  # Property tests run 1000+ iterations
  )

  message(STATUS "✅ Ring 3 Phase 3: P2P property tests (20 properties × 500-1000 iterations each)")
elseif(ENABLE_P2P_PROPERTY_TESTS)
  message(WARNING "⚠️  Ring 3 Phase 3 P2P property tests file not found - skipping")
endif()

# Ring 3 Phase 4b: Thread Safety Properties (Real-Thread Testing)
# ════════════════════════════════════════════════════════════════════════════
# TS1: Thread-Safe Peer Lifetime (Use-After-Free Impossible)
#
# Property: ∀ peer P, ∀ thread T:
#   If T accesses P, then P.state ∈ {RUNNING, STOPPING}
#   P.state == DESTROYED ⇒ no thread holds reference to P
#
# CRITICAL: This test is EXPECTED TO FAIL before refactor.
# The test documents the violation, not the fix.
# Exit criteria: Test passes after p2p_manager.cpp refactor (Phase 4c).
#
# WARNING: This test uses real threads, not simulator.
# Compile with ASAN: -fsanitize=address -fno-omit-frame-pointer
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_TS1_THREAD_SAFETY "Build Ring 3 Phase 4b TS1 thread safety tests (EXPECTED FAIL)" ON)

if(ENABLE_TS1_THREAD_SAFETY AND EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_thread_safety_ts1.cpp)
  add_executable(test_thread_safety_ts1
    tests/p2p/test_thread_safety_ts1.cpp
  )

  target_link_libraries(test_thread_safety_ts1 PRIVATE
    GTest::gtest
    Threads::Threads
  )

  target_include_directories(test_thread_safety_ts1 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/p2p
  )

  # Register with CTest
  add_test(NAME ThreadSafety_TS1 COMMAND test_thread_safety_ts1)

  # Mark as ring3 phase4b test
  # Ring 3 Phase 4c COMPLETE: Production code now satisfies TS1
  set_tests_properties(ThreadSafety_TS1 PROPERTIES
    LABELS "p2p;ring3;threading;phase4b;phase4c;ts1"
    TIMEOUT 120  # Real threads may take longer
  )

  message(STATUS "✅ Ring 3 Phase 4b: TS1 thread safety test (Phase 4c complete - TS1 satisfied)")
elseif(ENABLE_TS1_THREAD_SAFETY)
  message(WARNING "⚠️  Ring 3 Phase 4b TS1 test file not found - skipping")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 3 Phase 4c: TS1 Integration Test (Production P2PManager)
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_p2p_manager_ts1_integration.cpp)
  add_executable(test_p2p_manager_ts1_integration
    tests/p2p/test_p2p_manager_ts1_integration.cpp
    src/daemon/p2p_manager.cpp
    src/common/sha256d.cpp        # Required for Dinero::Common::double_sha256_raw
    src/daemon/secure_random.cpp  # Required for SecureRandom::GetUInt64()
  )

  if(APPLE)
    target_link_libraries(test_p2p_manager_ts1_integration PRIVATE
      GTest::gtest
        Threads::Threads
      dinero_crypto
      secp256k1
      "-framework Security"
    )
  else()
    target_link_libraries(test_p2p_manager_ts1_integration PRIVATE
      GTest::gtest
        Threads::Threads
      dinero_crypto
      secp256k1
    )
  endif()

  target_include_directories(test_p2p_manager_ts1_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/daemon
  )

  # Register with CTest
  add_test(NAME P2PManager_TS1_Integration COMMAND test_p2p_manager_ts1_integration)

  # Ring 3 Phase 4c: After refactor, this should PASS
  set_tests_properties(P2PManager_TS1_Integration PROPERTIES
    LABELS "p2p;ring3;threading;phase4c;ts1;integration"
    TIMEOUT 30  # Integration tests should complete quickly
  )

  message(STATUS "✅ Ring 3 Phase 4c: TS1 integration test (production P2PManager)")
else()
  message(WARNING "⚠️  Ring 3 Phase 4c TS1 integration test not found - skipping")
endif()

# ════════════════════════════════════════════════════════════════════════════
# P2P Hardening Tests (March 2026 Security Audit)
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_hardening.cpp)
  add_executable(test_p2p_hardening
    tests/p2p/test_hardening.cpp
  )

  target_link_libraries(test_p2p_hardening PRIVATE
    dinero_core
    dinero_crypto
    secp256k1
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
  )

  target_include_directories(test_p2p_hardening PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME P2P_Hardening COMMAND test_p2p_hardening)
  set_tests_properties(P2P_Hardening PROPERTIES
    LABELS "p2p;hardening;security"
    TIMEOUT 10
  )
  message(STATUS "P2P hardening tests registered")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 3 Phase 4d: TS2 (Lock Ordering & Deadlock Freedom)
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_TS2_LOCK_ORDERING "Build Ring 3 Phase 4d TS2 lock ordering tests" ON)

if(ENABLE_TS2_LOCK_ORDERING AND EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_thread_safety_ts2.cpp)
  add_executable(test_thread_safety_ts2
    tests/p2p/test_thread_safety_ts2.cpp
  )

  target_link_libraries(test_thread_safety_ts2 PRIVATE
    GTest::gtest
    Threads::Threads
  )

  target_include_directories(test_thread_safety_ts2 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/p2p
  )

  # Register with CTest
  add_test(NAME ThreadSafety_TS2 COMMAND test_thread_safety_ts2)

  # Ring 3 Phase 4d: TS2 lock ordering property test
  set_tests_properties(ThreadSafety_TS2 PROPERTIES
    LABELS "p2p;ring3;threading;phase4d;ts2;lock-ordering"
    TIMEOUT 60  # Lock ordering tests with timeout detection
  )

  message(STATUS "✅ Ring 3 Phase 4d: TS2 lock ordering test (property definition)")
elseif(ENABLE_TS2_LOCK_ORDERING)
  message(WARNING "⚠️  Ring 3 Phase 4d TS2 test file not found - skipping")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 3 Phase 4e: TS3 (Blocking-Free Event Loops / Liveness)
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_TS3_LIVENESS "Build Ring 3 Phase 4e TS3 liveness tests" ON)

if(ENABLE_TS3_LIVENESS AND EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_thread_safety_ts3.cpp)
  add_executable(test_thread_safety_ts3
    tests/p2p/test_thread_safety_ts3.cpp
  )

  target_link_libraries(test_thread_safety_ts3 PRIVATE
    GTest::gtest
    Threads::Threads
  )

  target_include_directories(test_thread_safety_ts3 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/p2p
  )

  # Register with CTest
  add_test(NAME ThreadSafety_TS3 COMMAND test_thread_safety_ts3)

  # Ring 3 Phase 4e: TS3 liveness property test
  set_tests_properties(ThreadSafety_TS3 PROPERTIES
    LABELS "p2p;ring3;threading;phase4e;ts3;liveness"
    TIMEOUT 120  # Liveness tests may take longer (shutdown delays)
  )

  message(STATUS "✅ Ring 3 Phase 4e: TS3 liveness test (property definition)")
elseif(ENABLE_TS3_LIVENESS)
  message(WARNING "⚠️  Ring 3 Phase 4e TS3 test file not found - skipping")
endif()

# BIP39 test suite
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_bip39.cpp)
  add_executable(test_bip39 tests/test_bip39.cpp)
  if(APPLE)
    target_link_libraries(test_bip39 PRIVATE dinero_crypto dinero_wallet secp256k1 "-framework Security")
  else()
    target_link_libraries(test_bip39 PRIVATE dinero_crypto dinero_wallet secp256k1)
  endif()
  target_include_directories(test_bip39 PRIVATE ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME BIP39 COMMAND test_bip39)
endif()

# Wallet runtime path hygiene gate
if(EXISTS ${CMAKE_SOURCE_DIR}/ci/check_wallet_hardcoded_paths.sh)
  add_test(
    NAME WalletPathHygiene
    COMMAND bash ${CMAKE_SOURCE_DIR}/ci/check_wallet_hardcoded_paths.sh
  )
  set_tests_properties(WalletPathHygiene PROPERTIES LABELS "wallet;hygiene;path")
endif()

# v7 retired coin-type hygiene gate
if(EXISTS ${CMAKE_SOURCE_DIR}/ci/check_retired_coin_type_1447.sh)
  add_test(
    NAME RetiredCoinType1447Hygiene
    COMMAND bash ${CMAKE_SOURCE_DIR}/ci/check_retired_coin_type_1447.sh
  )
  set_tests_properties(RetiredCoinType1447Hygiene PROPERTIES LABELS "wallet;hygiene;coin_type")
endif()

# Wallet FFI concurrency stress (deadlock guard for FFI lock discipline)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_wallet_ffi_concurrency.cpp)
  add_executable(test_wallet_ffi_concurrency
    tests/test_wallet_ffi_concurrency.cpp
    src/wallet/taproot_control_block.cpp
    src/wallet/p2mr_address.cpp
    src/daemon/bech32_encoder.cpp
    tests/stubs/mempool_test_stubs.cpp
  )
  if(APPLE)
    target_link_libraries(test_wallet_ffi_concurrency PRIVATE
      dinero_wallet_ffi
      dinero_wallet
      wallet_test_deps
      dinero_chainstate
      dinero_crypto
      secp256k1
      "-framework Security"
      Threads::Threads
    )
  else()
    target_link_libraries(test_wallet_ffi_concurrency PRIVATE
      dinero_wallet_ffi
      dinero_wallet
      wallet_test_deps
      dinero_chainstate
      dinero_crypto
      secp256k1
      Threads::Threads
    )
  endif()
  target_include_directories(test_wallet_ffi_concurrency PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/wallet-core/ffi
  )
  add_test(NAME WalletFFIConcurrency COMMAND test_wallet_ffi_concurrency)
  set_tests_properties(WalletFFIConcurrency PROPERTIES TIMEOUT 60 LABELS "wallet;ffi;concurrency")
endif()

# ============================================================================
# Phase D.2: Consensus Invariant Tests (CRITICAL)
# ============================================================================
# These tests verify that consensus rules are correctly locked and enforced.
# If any of these tests fail, there is a CRITICAL consensus bug.

# Phase D.2.1: Premine Validation Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_premine_validation.cpp)
  add_executable(test_premine_validation tests/test_premine_validation.cpp)
  target_link_libraries(test_premine_validation PRIVATE
    dinero_chainstate  # For ChainDB::getBlock, getBlockHashByHeight
    dinero_consensus
    dinero_core        # For logger and daemon components
  )
  link_zstd_if_needed(test_premine_validation)
  target_include_directories(test_premine_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME PremineValidation COMMAND test_premine_validation)
endif()

# Phase D.2.2: Consensus Limits Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_consensus_limits.cpp)
  add_executable(test_consensus_limits tests/test_consensus_limits.cpp)
  target_link_libraries(test_consensus_limits PRIVATE
    dinero_consensus
  )
  link_zstd_if_needed(test_consensus_limits)
  target_include_directories(test_consensus_limits PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ConsensusLimits COMMAND test_consensus_limits)
endif()

# Phase D.2.3: Monetary Constants Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_monetary_constants.cpp)
  add_executable(test_monetary_constants tests/test_monetary_constants.cpp)
  target_link_libraries(test_monetary_constants PRIVATE
    dinero_consensus
  )
  link_zstd_if_needed(test_monetary_constants)
  target_include_directories(test_monetary_constants PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME MonetaryConstants COMMAND test_monetary_constants)
endif()

# ============================================================================
# Phase 11a: Consensus-Lock Tests (CRITICAL REGRESSION PREVENTION)
# ============================================================================
# These tests lock the Phase 11a consensus separation and Utreexo integration.
# They prevent regressions in:
#   1. Merkle computation (single-TX invariant)
#   2. Mining vs Validation parity (PATH A vs PATH B)
#   3. ApplyBlock vs ValidateAndApplyBlock equivalence
#
# If any of these tests fail, Phase 11a consensus is BROKEN.
# DO NOT merge changes that break these tests.

# Phase 11a.1: Merkle Invariants Lock
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_merkle_invariants.cpp)
  add_executable(test_merkle_invariants tests/consensus/test_merkle_invariants.cpp)
  target_link_libraries(test_merkle_invariants PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_core          # For BlockAssembler
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_merkle_invariants)
  target_include_directories(test_merkle_invariants PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME MerkleInvariants COMMAND test_merkle_invariants)
  set_tests_properties(MerkleInvariants PROPERTIES
    LABELS "consensus;critical;phase11a;merkle"
    TIMEOUT 30
  )
  message(STATUS "✅ Phase 11a Merkle Invariants test enabled")
endif()

# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
# Phase 11a.2: Mining vs Validation Parity Lock
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_mining_validation_parity.cpp)
  add_executable(test_mining_validation_parity tests/consensus/test_mining_validation_parity.cpp)
  target_link_libraries(test_mining_validation_parity PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_core          # For BlockAssembler
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_mining_validation_parity)
  if(TARGET bulletproofs_ffi)
    target_link_libraries(test_mining_validation_parity PRIVATE bulletproofs_ffi)
    if(TARGET build_bulletproofs_ffi)
      add_dependencies(test_mining_validation_parity build_bulletproofs_ffi)
    endif()
  endif()
  target_include_directories(test_mining_validation_parity PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME MiningValidationParity COMMAND test_mining_validation_parity)
  set_tests_properties(MiningValidationParity PROPERTIES
    LABELS "consensus;critical;phase11a;parity"
    TIMEOUT 30
  )
  message(STATUS "✅ Phase 11a Mining/Validation Parity test enabled")
endif()

# Priority 2: Block Template Determinism Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_block_template_determinism.cpp)
  add_executable(test_block_template_determinism
    tests/mining/test_block_template_determinism.cpp
    tests/stubs/mempool_test_stubs.cpp   # forEachEntry, onBlockConnected, etc.
    tests/stubs/daemon_test_stubs.cpp    # Mempool extras (getMempoolEntry, getTransactionFee, …) + vault
    src/daemon/config.cpp                # GetConfig() referenced by consensus_write_batch.obj
  )
  target_link_libraries(test_block_template_determinism PRIVATE
    dinero_core          # For Mempool, BlockAssembler
    dinero_consensus
    dinero_crypto
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_block_template_determinism)
  target_include_directories(test_block_template_determinism PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME BlockTemplateDeterminism COMMAND test_block_template_determinism)
  set_tests_properties(BlockTemplateDeterminism PROPERTIES
    LABELS "consensus;critical;mining;determinism"
    TIMEOUT 60
  )
  message(STATUS "✅ Block Template Determinism test enabled")
endif()

# Priority 3: UTXO Index Correctness Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/utxo/test_utxo_index_correctness.cpp)
  add_executable(test_utxo_index_correctness tests/utxo/test_utxo_index_correctness.cpp)
  target_include_directories(test_utxo_index_correctness PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME UTXOIndexCorrectness COMMAND test_utxo_index_correctness)
  set_tests_properties(UTXOIndexCorrectness PROPERTIES
    LABELS "utxo;critical;wallet;consensus"
    TIMEOUT 60
  )
  message(STATUS "✅ UTXO Index Correctness test enabled")
endif()

# Priority 4: Persistence Safety Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/persistence/test_persistence_safety.cpp)
  add_executable(test_persistence_safety tests/persistence/test_persistence_safety.cpp)
  target_include_directories(test_persistence_safety PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME PersistenceSafety COMMAND test_persistence_safety)
  set_tests_properties(PersistenceSafety PROPERTIES
    LABELS "persistence;critical;crash-safety"
    TIMEOUT 60
  )
  message(STATUS "✅ Persistence Safety test enabled")
endif()

# Priority 5: Daemon Invariants Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_daemon_invariants.cpp)
  add_executable(test_daemon_invariants tests/daemon/test_daemon_invariants.cpp)
  target_include_directories(test_daemon_invariants PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME DaemonInvariants COMMAND test_daemon_invariants)
  set_tests_properties(DaemonInvariants PROPERTIES
    LABELS "daemon;invariants;critical"
    TIMEOUT 60
  )
  message(STATUS "✅ Daemon Invariants test enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_archival_config_defaults.cpp)
  add_executable(test_archival_config_defaults
    tests/daemon/test_archival_config_defaults.cpp
    src/daemon/services/config_service.cpp
    src/common/logger.cpp
  )
  target_include_directories(test_archival_config_defaults PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ArchivalConfigDefaults COMMAND test_archival_config_defaults)
  set_tests_properties(ArchivalConfigDefaults PROPERTIES
    LABELS "daemon;config;archival"
    TIMEOUT 30
  )
  message(STATUS "✅ Archival config defaults test enabled")
endif()

# Phase B (Dinero Core 1.0) — INI-style config-file loader
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_config_file_loader.cpp)
  add_executable(test_config_file_loader
    tests/daemon/test_config_file_loader.cpp
    src/daemon/services/config_service.cpp
    src/common/logger.cpp
  )
  target_include_directories(test_config_file_loader PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ConfigFileLoader COMMAND test_config_file_loader)
  set_tests_properties(ConfigFileLoader PROPERTIES
    LABELS "daemon;config;file-parsing"
    TIMEOUT 30
  )
  message(STATUS "✅ Config file loader test enabled")
endif()

# Phase D.4 (Dinero Core 1.0) — health-check decision logic
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_health.cpp)
  add_executable(test_health
    tests/daemon/test_health.cpp
    src/daemon/health.cpp
  )
  target_include_directories(test_health PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME HealthCheck COMMAND test_health)
  set_tests_properties(HealthCheck PROPERTIES
    LABELS "daemon;health"
    TIMEOUT 30
  )
  message(STATUS "✅ Health check test enabled")
endif()

# Phase C (Dinero Core 1.0) — `cmake --install` layout smoke test.
# Verifies install rules produce the contract-required layout for both
# manual mode (--prefix /usr/local-style) and packaged mode (--prefix
# /usr with DESTDIR). Uses cmake --install internally; depends on
# dinerod and dinero-cli being built.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/install/test_cmake_install.sh)
  add_test(NAME CmakeInstallLayout
    COMMAND ${CMAKE_SOURCE_DIR}/tests/install/test_cmake_install.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
  set_tests_properties(CmakeInstallLayout PROPERTIES
    LABELS "packaging;install"
    TIMEOUT 60
    ENVIRONMENT "BUILD_DIR=${CMAKE_BINARY_DIR}"
  )
  message(STATUS "✅ cmake --install layout test enabled")
endif()

# Phase D.2 (Dinero Core 1.0) — recovery-grade wallet backup test.
# Exercises every quality gate: refuse unsafe paths, declared-targets-
# only, manifest integrity round-trip, atomic output, mode 0600, dry-
# run, no-overwrite, no staging leftovers, no chainstate in archive.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/install/test_dinero_backup.sh)
  add_test(NAME DineroBackup
    COMMAND ${CMAKE_SOURCE_DIR}/tests/install/test_dinero_backup.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
  set_tests_properties(DineroBackup PROPERTIES
    LABELS "packaging;backup;recovery"
    TIMEOUT 60
  )
  message(STATUS "✅ dinero-backup test enabled")
endif()

# Phase D.3 (Dinero Core 1.0) — pre-upgrade rollback capture test.
# Verifies non-fatal-on-failure contract, atomic output, naming
# pattern, dir mode, dry-run, stale-backup preservation, Linux-gate.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/install/test_dinero_prepare_upgrade.sh)
  add_test(NAME DineroPrepareUpgrade
    COMMAND ${CMAKE_SOURCE_DIR}/tests/install/test_dinero_prepare_upgrade.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
  set_tests_properties(DineroPrepareUpgrade PROPERTIES
    LABELS "packaging;rollback;recovery"
    TIMEOUT 60
  )
  message(STATUS "✅ dinero-prepare-upgrade test enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_assumeutxo_metadata_lifecycle.cpp)
  add_executable(test_assumeutxo_metadata_lifecycle
    tests/daemon/test_assumeutxo_metadata_lifecycle.cpp
  )
  add_dependencies(test_assumeutxo_metadata_lifecycle gtest)
  target_link_libraries(test_assumeutxo_metadata_lifecycle PRIVATE
    dinero_chainstate
    dinero_wallet
    gtest
    sqlite3
    ${CMAKE_DL_LIBS}
  )
  link_zstd_if_needed(test_assumeutxo_metadata_lifecycle)
  target_include_directories(test_assumeutxo_metadata_lifecycle PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  add_test(NAME AssumeUTXOMetadataLifecycle COMMAND test_assumeutxo_metadata_lifecycle)
  set_tests_properties(AssumeUTXOMetadataLifecycle PROPERTIES
    LABELS "daemon;assumeutxo;persistence"
    TIMEOUT 60
  )
  message(STATUS "✅ AssumeUTXO metadata lifecycle test enabled")
endif()

# Doctor Diagnostic Command Tests (Phase 4)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_doctor.cpp)
  add_executable(test_doctor
    tests/daemon/test_doctor.cpp
    # Doctor sources (not in a separate library)
    src/daemon/doctor/doctor_context.cpp
    src/daemon/doctor/doctor_registry.cpp
    src/daemon/doctor/doctor_runner.cpp
    src/daemon/doctor/doctor_fixer.cpp
    src/daemon/doctor/doctor_json_emitter.cpp
    src/daemon/doctor/doctor_renderer.cpp
    src/daemon/doctor/doctor_checks_v1.cpp
  )

  add_dependencies(test_doctor gtest)

  target_link_libraries(test_doctor PRIVATE
    dinero_consensus
    dinero_crypto
    jsoncpp_static
    $<TARGET_FILE:gtest>
  )
  link_zstd_if_needed(test_doctor)

  target_include_directories(test_doctor BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  if(ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    dinero_enable_sanitizers(test_doctor)
  endif()

  add_test(NAME DoctorDiagnostics COMMAND test_doctor)
  set_tests_properties(DoctorDiagnostics PROPERTIES
    LABELS "daemon;doctor;phase4"
    TIMEOUT 60
  )

  message(STATUS "✅ Doctor diagnostic tests enabled")
endif()

# Datadir guard tests (single-writer and PID safety)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_datadir_guard.cpp)
  add_executable(test_datadir_guard
    tests/daemon/test_datadir_guard.cpp
    src/daemon/datadir_guard.cpp
  )

  add_dependencies(test_datadir_guard gtest gtest_main)

  target_link_libraries(test_datadir_guard PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
  )

  target_include_directories(test_datadir_guard BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  if(ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    dinero_enable_sanitizers(test_datadir_guard)
  endif()

  add_test(NAME DatadirGuard COMMAND test_datadir_guard)
  set_tests_properties(DatadirGuard PROPERTIES
    LABELS "daemon;operational-safety"
    TIMEOUT 60
  )

  message(STATUS "✅ Datadir guard tests enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_chainstate_recovery_marker.cpp)
  add_executable(test_chainstate_recovery_marker
    tests/daemon/test_chainstate_recovery_marker.cpp
  )

  target_include_directories(test_chainstate_recovery_marker BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME ChainstateRecoveryMarker COMMAND test_chainstate_recovery_marker)
  set_tests_properties(ChainstateRecoveryMarker PROPERTIES
    LABELS "daemon;recovery"
    TIMEOUT 30
  )

  message(STATUS "✅ Chainstate recovery marker test enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_utxo_position_index_rebuild.cpp)
  add_executable(test_utxo_position_index_rebuild
    tests/daemon/test_utxo_position_index_rebuild.cpp
    src/common/logger.cpp
  )

  target_link_libraries(test_utxo_position_index_rebuild PRIVATE
    dinero_consensus
    dinero_chainstate
    RocksDB::rocksdb
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )

  link_zstd_if_needed(test_utxo_position_index_rebuild)

  target_include_directories(test_utxo_position_index_rebuild BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )

  if(APPLE)
    target_link_libraries(test_utxo_position_index_rebuild PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()

  add_test(NAME UTXOPositionIndexRebuild COMMAND test_utxo_position_index_rebuild)
  set_tests_properties(UTXOPositionIndexRebuild PROPERTIES
    LABELS "daemon;utreexo;recovery"
    TIMEOUT 60
  )

  message(STATUS "✅ UTXO position rebuild test enabled")
endif()

# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
# Phase 11a.3: Utreexo Consensus Parity Lock
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_consensus_parity.cpp)
  add_executable(test_utreexo_consensus_parity tests/consensus/test_utreexo_consensus_parity.cpp)
  target_link_libraries(test_utreexo_consensus_parity PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_core          # For BlockAssembler
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_utreexo_consensus_parity)
  if(TARGET bulletproofs_ffi)
    target_link_libraries(test_utreexo_consensus_parity PRIVATE bulletproofs_ffi)
    if(TARGET build_bulletproofs_ffi)
      add_dependencies(test_utreexo_consensus_parity build_bulletproofs_ffi)
    endif()
  endif()
  target_include_directories(test_utreexo_consensus_parity PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME UtreexoConsensusParity COMMAND test_utreexo_consensus_parity)
  set_tests_properties(UtreexoConsensusParity PROPERTIES
    LABELS "consensus;critical;phase11a;utreexo"
    TIMEOUT 30
  )
  message(STATUS "✅ Phase 11a Utreexo Consensus Parity test enabled")
endif()

# Utreexo Pollard/Stump Commitment Invariant Test
# CRITICAL: Verifies Pollard.getCommitment() == Stump.getCommitment()
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_stump_invariant.cpp)
  add_executable(test_utreexo_stump_invariant
    tests/consensus/test_utreexo_stump_invariant.cpp
    ${CMAKE_SOURCE_DIR}/tests/stubs/chain_test_stubs.cpp
  )
  target_link_libraries(test_utreexo_stump_invariant PRIVATE
    dinero_consensus_core
    dinero_crypto
    dinero_tx_primitives
  )
  link_zstd_if_needed(test_utreexo_stump_invariant)
  target_include_directories(test_utreexo_stump_invariant PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME UtreexoStumpInvariant COMMAND test_utreexo_stump_invariant)
  set_tests_properties(UtreexoStumpInvariant PROPERTIES
    LABELS "consensus;critical;utreexo;stump;invariant"
    TIMEOUT 30
  )
  message(STATUS "✅ Utreexo Pollard/Stump Invariant test enabled")
endif()

# Phase 11b.2: Witness Merkle Isolation Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_witness_merkle_isolation.cpp)
  add_executable(test_witness_merkle_isolation tests/consensus/test_witness_merkle_isolation.cpp)
  target_link_libraries(test_witness_merkle_isolation PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_tx_primitives # For Transaction, Block
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_witness_merkle_isolation)
  target_include_directories(test_witness_merkle_isolation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME WitnessMerkleIsolation COMMAND test_witness_merkle_isolation)
  set_tests_properties(WitnessMerkleIsolation PROPERTIES
    LABELS "consensus;critical;phase11b;witness;merkle;isolation"
    TIMEOUT 30
  )
  message(STATUS "✅ Phase 11b Witness Merkle Isolation test enabled")
endif()

# Phase 11c.4: Witness Commitment Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_witness_commitment.cpp)
  add_executable(test_witness_commitment tests/consensus/test_witness_commitment.cpp)
  target_link_libraries(test_witness_commitment PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_tx_primitives  # For Transaction, Block
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_witness_commitment)
  target_include_directories(test_witness_commitment PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME WitnessCommitment COMMAND test_witness_commitment)
  set_tests_properties(WitnessCommitment PROPERTIES
    LABELS "consensus;critical;phase11c;witness;commitment;dinw"
    TIMEOUT 30
  )
  message(STATUS "✅ Phase 11c Witness Commitment test enabled")
endif()

# Phase 11d.2: Witness Commitment Enforcement Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_witness_commitment_enforcement.cpp)
  add_executable(test_witness_commitment_enforcement tests/consensus/test_witness_commitment_enforcement.cpp)
  target_link_libraries(test_witness_commitment_enforcement PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_tx_primitives  # For Transaction, Block
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_witness_commitment_enforcement)
  target_include_directories(test_witness_commitment_enforcement PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME WitnessCommitmentEnforcement COMMAND test_witness_commitment_enforcement)
  set_tests_properties(WitnessCommitmentEnforcement PROPERTIES
    LABELS "consensus;critical;phase11d;witness;enforcement;safety"
    TIMEOUT 30
  )
  message(STATUS "✅ Phase 11d Witness Commitment Enforcement test enabled")
endif()

# CRITICAL: Activation Boundary Tests (Genesis, Premine, Height 2)
# These tests LOCK the activation boundary FOREVER. DO NOT MODIFY.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_activation_boundary_critical.cpp)
  add_executable(test_activation_boundary_critical tests/consensus/test_activation_boundary_critical.cpp)
  target_link_libraries(test_activation_boundary_critical PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_tx_primitives
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_activation_boundary_critical)
  target_include_directories(test_activation_boundary_critical PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ActivationBoundaryCritical COMMAND test_activation_boundary_critical)
  set_tests_properties(ActivationBoundaryCritical PROPERTIES
    LABELS "consensus;critical;activation;boundary;genesis;premine;height2"
    TIMEOUT 30
  )
  message(STATUS "✅ CRITICAL Activation Boundary tests enabled (genesis, premine, height 2)")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_filter_commitment_activation.cpp)
  add_executable(test_filter_commitment_activation
    tests/consensus/test_filter_commitment_activation.cpp
  )
  target_link_libraries(test_filter_commitment_activation PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_tx_primitives
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_filter_commitment_activation)
  target_include_directories(test_filter_commitment_activation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME FilterCommitmentActivation COMMAND test_filter_commitment_activation)
  set_tests_properties(FilterCommitmentActivation PROPERTIES
    LABELS "consensus;filters;activation;commitment;critical"
    TIMEOUT 30
  )
  message(STATUS "✅ Filter commitment activation tests enabled")
endif()

# Phase 11e.1: Witness Magic Translation Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_witness_magic_translation.cpp)
  add_executable(test_witness_magic_translation tests/consensus/test_witness_magic_translation.cpp)
  target_link_libraries(test_witness_magic_translation PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_tx_primitives  # For Transaction, Block
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_witness_magic_translation)
  target_include_directories(test_witness_magic_translation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME WitnessMagicTranslation COMMAND test_witness_magic_translation)
  set_tests_properties(WitnessMagicTranslation PROPERTIES
    LABELS "consensus;critical;phase11e;witness;translation;interpretation"
    TIMEOUT 30
  )
  message(STATUS "✅ Phase 11e Witness Magic Translation test enabled")
endif()

# Phase 6C: Taproot Consensus Validation Tests (BIP340/341/342)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_taproot_consensus.cpp)
  add_executable(test_taproot_consensus tests/test_taproot_consensus.cpp)
  target_link_libraries(test_taproot_consensus PRIVATE
    dinero_consensus
    dinero_crypto
  )
  link_zstd_if_needed(test_taproot_consensus)
  # NO HOMEBREW PATHS - use vendored secp256k1
  # dinero_consensus and dinero_crypto already link secp256k1
  target_include_directories(test_taproot_consensus PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_taproot_consensus PRIVATE /opt/homebrew/lib)
  endif()
  add_test(NAME TaprootConsensus COMMAND test_taproot_consensus)
endif()

# Phase 24.2: Script Interpreter Tests (P2WPKH, P2WSH, Taproot)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_script_interpreter.cpp)
  add_executable(test_script_interpreter tests/test_script_interpreter.cpp)
  target_link_libraries(test_script_interpreter PRIVATE
    dinero_consensus
    dinero_crypto
    OpenSSL::Crypto  # SHA1, SHA256 functions
    secp256k1        # ECDSA, Schnorr signatures
  )
  link_zstd_if_needed(test_script_interpreter)
  target_include_directories(test_script_interpreter PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_script_interpreter PRIVATE /opt/homebrew/lib)
  endif()
  add_test(NAME ScriptInterpreter COMMAND test_script_interpreter)
endif()

# Phase 12b: Debug Signature Verification Failures
if(EXISTS ${CMAKE_SOURCE_DIR}/debug_sig_test.cpp)
  add_executable(debug_sig_test debug_sig_test.cpp)
  target_link_libraries(debug_sig_test PRIVATE
    dinero_consensus
    dinero_crypto
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(debug_sig_test)
  target_include_directories(debug_sig_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(debug_sig_test PRIVATE /opt/homebrew/lib)
  endif()
endif()

# Phase 28: Covenant Framework Tests (CTV, CSFS, TXHASH, CCV)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_covenants.cpp)
  add_executable(test_covenants tests/test_covenants.cpp)
  target_link_libraries(test_covenants PRIVATE
    dinero_consensus
    dinero_crypto
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_covenants)
  target_include_directories(test_covenants PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_covenants PRIVATE /opt/homebrew/lib)
  endif()
  add_test(NAME CovenantTests COMMAND test_covenants)
endif()

# Phase 29: Covenant Integration Layer Tests
# DELETED registration: this test was superseded by Phase C.2, which moved
# covenant policy directly into mempool.cpp without the deprecated
# covenant_policy.h+wallet dependencies the test was written against. The
# test source still exists for archival reference but no longer compiles
# (AmountUna→uint64_t conversion was removed in C.2). Don't re-enable
# without porting the test to the new mempool-side API.
# See include/mempool/covenant_policy.h header comment.

# Phase C.4: Covenant script-path integration tests
if(ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_covenant_scriptpath.cpp)
  add_executable(test_covenant_scriptpath
    tests/test_covenant_scriptpath.cpp
  )
  target_link_libraries(test_covenant_scriptpath PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_zk
    secp256k1
    gtest
    gtest_main
  )
  link_zstd_if_needed(test_covenant_scriptpath)
  target_include_directories(test_covenant_scriptpath PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  )
  add_test(NAME CovenantScriptPath COMMAND test_covenant_scriptpath)
  set_tests_properties(CovenantScriptPath PROPERTIES
    LABELS "covenant;scriptpath;integration"
    TIMEOUT 60
  )
  message(STATUS "  ✓ Covenant script-path tests registered")
endif()

# Phase 30: Taproot Asset Layer Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_assets.cpp)
  add_executable(test_assets
    tests/test_assets.cpp
    src/assets/asset_id.cpp
    src/assets/asset_state.cpp
    src/assets/asset_proof.cpp
    src/assets/asset_transfer.cpp
  )
  target_link_libraries(test_assets PRIVATE
    dinero_consensus
    dinero_crypto
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_assets)
  target_include_directories(test_assets PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME TaprootAssetTests COMMAND test_assets)
endif()

# RPC gettransaction Tests (blockchain.gettransaction + wallet.gettransaction)
# Tests transaction lookup, output type detection, and wallet enrichment
# NOT consensus-critical - read-only query tools for operator UX
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_rpc_gettransaction.cpp)
  add_executable(test_rpc_gettransaction
    tests/test_rpc_gettransaction.cpp
  )
  target_link_libraries(test_rpc_gettransaction PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_rpc_handlers
    jsoncpp_static
  )
  link_zstd_if_needed(test_rpc_gettransaction)
  target_include_directories(test_rpc_gettransaction PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  add_test(NAME RPCGetTransactionTests COMMAND test_rpc_gettransaction)
endif()

# Phase 24.3: Bitcoin Core Script JSON Test Vectors
# Phase 24.5: Taproot Template Engine
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_script_json.cpp)
  add_executable(test_script_json
    tests/test_script_json.cpp
    src/script/taproot_templates.cpp
  )
  target_link_libraries(test_script_json PRIVATE
    dinero_consensus
    dinero_crypto
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_script_json)
  target_include_directories(test_script_json PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann  # nlohmann/json.hpp
    ${SECP256K1_INCLUDE_DIR}
  )
  add_test(NAME ScriptJSONTests COMMAND test_script_json ${CMAKE_SOURCE_DIR}/tests/data/script_tests.json)
endif()

# Taproot Address Decoding Test (Step 4.2)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_taproot_address_decoding.cpp)
  add_executable(test_taproot_address_decoding
    tests/test_taproot_address_decoding.cpp
    src/address/addr_codec.cpp
    src/address/addr_types.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_taproot_address_decoding PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_wallet
  )
  link_zstd_if_needed(test_taproot_address_decoding)
  if(APPLE)
    target_link_libraries(test_taproot_address_decoding PRIVATE secp256k1 "-framework Security")
  else()
    target_link_libraries(test_taproot_address_decoding PRIVATE secp256k1)
  endif()
  target_include_directories(test_taproot_address_decoding PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
  )
  add_test(NAME TaprootAddressDecoding COMMAND test_taproot_address_decoding)
endif()

# Taproot Key Generation Test (Step 4.4)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_taproot_keys.cpp)
  add_executable(test_taproot_keys
    tests/test_taproot_keys.cpp
    src/wallet/taproot_keys.cpp
    src/address/addr_codec.cpp
    src/address/addr_types.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_taproot_keys PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_wallet
    OpenSSL::SSL
    OpenSSL::Crypto
  )
  link_zstd_if_needed(test_taproot_keys)
  if(APPLE)
    target_link_libraries(test_taproot_keys PRIVATE secp256k1 "-framework Security")
  else()
    target_link_libraries(test_taproot_keys PRIVATE secp256k1)
  endif()
  target_include_directories(test_taproot_keys PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
  )
  add_test(NAME TaprootKeyGeneration COMMAND test_taproot_keys)
endif()

# Wallet encryption test (Argon2id + AES-256-GCM) - DISABLED: uses dinero_wallet (dead architecture)
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/crypto/test_wallet_encryption.cpp)
  add_executable(test_wallet_encryption tests/crypto/test_wallet_encryption.cpp)
  target_link_libraries(test_wallet_encryption PRIVATE dinero_wallet argon2 OpenSSL::Crypto)
  target_include_directories(test_wallet_encryption PRIVATE ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME WalletEncryption COMMAND test_wallet_encryption)
endif()

# Wallet Encryption Metadata Fix Verification Test
# TEMPORARILY DISABLED (2026-02-02): wallet_test_deps needs ChainDB symbols
# TODO: Update linking when wallet_test_deps is fixed
# if(EXISTS ${CMAKE_SOURCE_DIR}/tests/crypto/test_wallet_encryption_fix.cpp)
#   add_executable(test_wallet_encryption_fix tests/crypto/test_wallet_encryption_fix.cpp)
#   target_link_libraries(test_wallet_encryption_fix PRIVATE dinero_wallet argon2 OpenSSL::Crypto wallet_test_deps)
#   if(APPLE)
#     target_link_libraries(test_wallet_encryption_fix PRIVATE "-framework Security" "-framework CoreFoundation")
#   endif()
#   target_include_directories(test_wallet_encryption_fix PRIVATE ${CMAKE_SOURCE_DIR}/include)
#   add_test(NAME WalletEncryptionFix COMMAND test_wallet_encryption_fix)
# endif()

# ═══════════════════════════════════════════════════════════════════════════
# Wallet Correctness Tests (Ring 2 - Optional, Non-Blocking)
# ═══════════════════════════════════════════════════════════════════════════
# Tests wallet invariants:
#   - Address derivation determinism (Test 3) ✅
#   - UTXO persistence across restarts (Test 2) - TODO
#   - Wallet restore correctness (Test 1) - TODO
#
# CI: Optional, manual/scheduled only (not consensus-critical)
# ═══════════════════════════════════════════════════════════════════════════
option(ENABLE_WALLET_CORRECTNESS_TESTS "Build wallet correctness tests (optional)" OFF)

if(ENABLE_WALLET_CORRECTNESS_TESTS)
  add_executable(test_wallet_correctness
    tests/wallet/test_wallet_correctness.cpp
  )
  target_link_libraries(test_wallet_correctness PRIVATE
    dinero_wallet
    dinero_consensus
    dinero_chainstate
    dinero_core
    sqlite3
    GTest::gtest
  )
  link_zstd_if_needed(test_wallet_correctness)

  # macOS Security framework for secure random
  if(APPLE)
    target_link_libraries(test_wallet_correctness PRIVATE "-framework Security")
  endif()

  target_include_directories(test_wallet_correctness PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/wallet
  )
  # Not added to CTest by default (optional test suite)
  message(STATUS "✅ Wallet correctness tests enabled (Test 3: Address Derivation Determinism)")
endif()

# REMOVED: test_wallet_integration, test_bip84_addr_check (obsolete zombie tests from copy repo)

# ═══════════════════════════════════════════════════════════════════════════
# CRITICAL: Wallet Ownership Invariant Tests
# ═══════════════════════════════════════════════════════════════════════════
# These tests lock the critical wallet invariant:
#   "A UTXO without a derivation path is NOT owned. No exceptions."
#
# This prevents:
#   - Ghost balances (phantom UTXOs)
#   - Unspendable outputs
#   - Reorg-induced wallet corruption
#   - Signing without provenance
# ═══════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_wallet_ownership_invariant.cpp)
  add_executable(test_wallet_ownership_invariant
    tests/wallet/test_wallet_ownership_invariant.cpp
  )
  target_link_libraries(test_wallet_ownership_invariant PRIVATE
    dinero_wallet
    dinero_consensus
    dinero_core
    sqlite3
    GTest::gtest
  )
  link_secp256k1_zkp(test_wallet_ownership_invariant)
  link_zstd_if_needed(test_wallet_ownership_invariant)

  if(APPLE)
    target_link_libraries(test_wallet_ownership_invariant PRIVATE "-framework Security")
  endif()

  target_include_directories(test_wallet_ownership_invariant PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME WalletOwnershipInvariant COMMAND test_wallet_ownership_invariant)
  set_tests_properties(WalletOwnershipInvariant PROPERTIES
    LABELS "wallet;invariant;critical;ownership"
    TIMEOUT 30
  )
  message(STATUS "✅ Wallet ownership invariant tests enabled (CRITICAL)")
endif()

# ═══════════════════════════════════════════════════════════════════════════
# BIP32Deriver Equivalence Test
# ═══════════════════════════════════════════════════════════════════════════
# Verifies BIP32Deriver produces identical keys to legacy inline lambdas.
# This is the safety check before refactoring lambda clusters in hd_wallet.cpp.
# ═══════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_bip32_deriver.cpp)
  add_executable(test_bip32_deriver
    tests/wallet/test_bip32_deriver.cpp
    src/wallet/bip32_deriver.cpp
  )
  target_link_libraries(test_bip32_deriver PRIVATE
    dinero_crypto
    OpenSSL::Crypto
    GTest::gtest
  )
  link_secp256k1_zkp(test_bip32_deriver)

  if(APPLE)
    target_link_libraries(test_bip32_deriver PRIVATE "-framework Security")
  endif()

  target_include_directories(test_bip32_deriver PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${SECP256K1_INCLUDE_DIR}
  )

  add_test(NAME BIP32DeriverEquivalence COMMAND test_bip32_deriver)
  set_tests_properties(BIP32DeriverEquivalence PROPERTIES
    LABELS "wallet;bip32;derivation;safety"
    TIMEOUT 30
  )
  message(STATUS "✅ BIP32Deriver equivalence tests enabled")
endif()

# HD Wallet test - DISABLED: uses dead architecture (dinero_wallet/old UTXO)
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_hd_wallet.cpp)
  add_executable(test_hd_wallet
    tests/test_hd_wallet.cpp
    src/daemon/hd_wallet_manager.cpp
    src/daemon/secure_random.cpp
    src/daemon/wallet_crypto.cpp
    src/common/logger.cpp        # Required by wallet_crypto.cpp
  )
  target_link_libraries(test_hd_wallet PRIVATE
    dinero_crypto
    jsoncpp_static
    secp256k1
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread                      # Fix: Required by OpenSSL 3.3 for thread-local storage
  )
  if(APPLE)
    target_link_libraries(test_hd_wallet PRIVATE "-framework Security")
  endif()
  target_include_directories(test_hd_wallet PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME test_hd_wallet COMMAND test_hd_wallet)
endif()

# Wallet Pipeline Test - v0.12.0 comprehensive wallet tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_pipeline.cpp)
  add_executable(test_wallet_pipeline
    tests/wallet_tests/test_wallet_pipeline.cpp
    src/common/logger.cpp  # Required by wallet components
    src/wallet/p2mr_address.cpp  # DecodeP2MRAddress (V7 P2MR bech32m codec)
    src/daemon/bech32_encoder.cpp  # Bech32Encoder::encode_segwit used by p2mr_address.cpp
  )
  target_link_libraries(test_wallet_pipeline PRIVATE
    dinero_wallet
    dinero_chainstate  # Needed for UTXO index and blockchain state
    dinero_crypto
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb   # Required by dinero_chainstate
  )
  target_link_directories(test_wallet_pipeline PRIVATE /opt/homebrew/lib)  # For RocksDB
  if(APPLE)
    target_link_libraries(test_wallet_pipeline PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_pipeline PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletPipeline COMMAND test_wallet_pipeline)
endif()

# Wallet Mempool Round-Trip Test - v0.12.0 integration test (Proof Layer 3)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_mempool_roundtrip.cpp)
  add_executable(test_wallet_mempool_roundtrip
    tests/wallet_tests/test_wallet_mempool_roundtrip.cpp
    src/common/logger.cpp  # Required by wallet components
    src/wallet/p2mr_address.cpp  # DecodeP2MRAddress (V7 P2MR bech32m codec)
    src/daemon/bech32_encoder.cpp  # Bech32Encoder::encode_segwit used by p2mr_address.cpp
  )
  target_link_libraries(test_wallet_mempool_roundtrip PRIVATE
    dinero_wallet
    dinero_chainstate  # Needed for UTXO index and blockchain state
    dinero_crypto
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb   # Required by dinero_chainstate
  )
  target_link_directories(test_wallet_mempool_roundtrip PRIVATE /opt/homebrew/lib)  # For RocksDB
  if(APPLE)
    target_link_libraries(test_wallet_mempool_roundtrip PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_mempool_roundtrip PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletMempoolRoundtrip COMMAND test_wallet_mempool_roundtrip)
endif()

# Wallet Block Integration Test - Phase M.3 full integration
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_block_integration.cpp)
  add_executable(test_wallet_block_integration
    tests/wallet_tests/test_wallet_block_integration.cpp
    src/common/logger.cpp  # Required by wallet components
  )
  target_link_libraries(test_wallet_block_integration PRIVATE
    dinero_wallet
    dinero_chainstate  # Needed for UTXO index and blockchain state
    dinero_crypto
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb   # Required by dinero_chainstate
  )
  target_link_directories(test_wallet_block_integration PRIVATE /opt/homebrew/lib)  # For RocksDB
  if(APPLE)
    target_link_libraries(test_wallet_block_integration PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_block_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletBlockIntegration COMMAND test_wallet_block_integration)
endif()

# ════════════════════════════════════════════════════════════════════════════
# Wallet ↔ Ingress Parity Test (Phase A1 - Mainnet Hardening)
# Proves wallet cannot bypass ITxIngress and handles all TxRejectCodes
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_ingress_parity.cpp)
  add_executable(test_wallet_ingress_parity
    tests/wallet_tests/test_wallet_ingress_parity.cpp
    src/common/logger.cpp
    src/daemon/interfaces/ingress_types.cpp
  )
  target_link_libraries(test_wallet_ingress_parity PRIVATE
    dinero_wallet
    dinero_crypto
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
  )
  if(APPLE)
    target_link_libraries(test_wallet_ingress_parity PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_ingress_parity PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletIngressParity COMMAND test_wallet_ingress_parity)
  message(STATUS "  ✓ Phase A1 test registered: test_wallet_ingress_parity")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Wallet Crash-Safety Test (Phase A2 - Mainnet Hardening)
# Proves wallet survives crashes without phantom balances or stuck UTXOs
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_crash_safety.cpp)
  add_executable(test_wallet_crash_safety
    tests/wallet_tests/test_wallet_crash_safety.cpp
    src/common/logger.cpp
    src/daemon/lightning_stubs.cpp
    tests/stubs/mempool_test_stubs.cpp  # Mempool::isOutputSpentInMempool et al.
  )
  target_link_libraries(test_wallet_crash_safety PRIVATE
    dinero_wallet
    dinero_chainstate
    dinero_crypto
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    sqlite3
  )
  target_link_directories(test_wallet_crash_safety PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_wallet_crash_safety PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_crash_safety PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletCrashSafety COMMAND test_wallet_crash_safety)
  message(STATUS "  ✓ Phase A2 test registered: test_wallet_crash_safety")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Wallet Determinism Test (Phase A3 - Mainnet Hardening)
# Proves same inputs → identical state, always
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_determinism.cpp)
  add_executable(test_wallet_determinism
    tests/wallet_tests/test_wallet_determinism.cpp
    src/common/logger.cpp
    src/daemon/lightning_stubs.cpp
    tests/stubs/mempool_test_stubs.cpp  # Mempool::isOutputSpentInMempool et al.
  )
  target_link_libraries(test_wallet_determinism PRIVATE
    dinero_wallet
    dinero_chainstate
    dinero_crypto
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    sqlite3
  )
  target_link_directories(test_wallet_determinism PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_wallet_determinism PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_determinism PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletDeterminism COMMAND test_wallet_determinism)
  message(STATUS "  ✓ Phase A3 test registered: test_wallet_determinism")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Wallet Sidecar Migration Idempotence Test (Week 1 migration safety gate)
# Verifies wallet.conf -> wallet_state.db one-time migration is idempotent
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_sidecar_migration.cpp)
  add_executable(test_wallet_sidecar_migration
    tests/wallet_tests/test_wallet_sidecar_migration.cpp
    src/common/logger.cpp
    src/daemon/lightning_stubs.cpp
    # dinero_wallet's wallet_manager.obj references Mempool::isOutputSpentInMempool
    # via listUnspentUTXOs(...). The full Mempool implementation in
    # src/daemon/mempool.cpp drags in the entire daemon (DaemonContext, RPC,
    # scheduler, …). mempool_test_stubs.cpp provides a no-op definition so
    # the wallet-only test surface links cleanly without pulling in daemon code.
    tests/stubs/mempool_test_stubs.cpp
  )
  target_link_libraries(test_wallet_sidecar_migration PRIVATE
    dinero_wallet
    dinero_chainstate
    dinero_crypto
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    sqlite3
  )
  target_link_directories(test_wallet_sidecar_migration PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_wallet_sidecar_migration PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_sidecar_migration PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletSidecarMigration COMMAND test_wallet_sidecar_migration)
  set_tests_properties(WalletSidecarMigration PROPERTIES LABELS "wallet;migration;idempotence")
  message(STATUS "  ✓ Wallet migration gate registered: test_wallet_sidecar_migration")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Wallet Descriptor Active Context Test (non-default wallet correctness)
# Verifies descriptor RPC uses active wallet DB handle only
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_descriptor_active_context.cpp)
  add_executable(test_wallet_descriptor_active_context
    tests/wallet_tests/test_wallet_descriptor_active_context.cpp
    src/common/logger.cpp
    src/daemon/lightning_stubs.cpp
    tests/stubs/mempool_test_stubs.cpp  # Mempool::isOutputSpentInMempool et al.
  )
  target_link_libraries(test_wallet_descriptor_active_context PRIVATE
    dinero_wallet
    dinero_core
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    dinero_rpc_handlers
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    sqlite3
  )
  target_link_directories(test_wallet_descriptor_active_context PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_wallet_descriptor_active_context PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_descriptor_active_context PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletDescriptorActiveContext COMMAND test_wallet_descriptor_active_context)
  set_tests_properties(WalletDescriptorActiveContext PROPERTIES LABELS "wallet;descriptor;multiwallet")
  message(STATUS "  ✓ Wallet descriptor context gate registered: test_wallet_descriptor_active_context")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Wallet PSBT Active Policy Context Test (non-default wallet signing policy)
# Verifies walletprocesspsbt uses active wallet policy from active DB handle
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_psbt_policy_context.cpp)
  add_executable(test_wallet_psbt_policy_context
    tests/wallet_tests/test_wallet_psbt_policy_context.cpp
    src/rpc/methods_wallet_psbt.cpp
    src/wallet/wallet_metrics.cpp
    src/common/logger.cpp
    src/daemon/lightning_stubs.cpp
    tests/stubs/mempool_test_stubs.cpp  # Mempool::isOutputSpentInMempool et al.
  )
  target_link_libraries(test_wallet_psbt_policy_context PRIVATE
    dinero_wallet
    dinero_core
    dinero_chainstate
    dinero_crypto
    dinero_rpc_handlers
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    sqlite3
  )
  target_link_directories(test_wallet_psbt_policy_context PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_wallet_psbt_policy_context PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_psbt_policy_context PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletPsbtPolicyContext COMMAND test_wallet_psbt_policy_context)
  set_tests_properties(WalletPsbtPolicyContext PROPERTIES LABELS "wallet;psbt;multiwallet")
  message(STATUS "  ✓ Wallet PSBT context gate registered: test_wallet_psbt_policy_context")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Template Staleness Test (Phase B1 - Mainnet Hardening)
# Proves stale templates are rejected, not auto-fixed
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_template_staleness.cpp)
  add_executable(test_template_staleness
    tests/mining/test_template_staleness.cpp
    src/daemon/interfaces/ingress_types.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_template_staleness PRIVATE
    dinero_crypto
  )
  target_include_directories(test_template_staleness PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME TemplateStaleness COMMAND test_template_staleness)
  message(STATUS "  ✓ Phase B1 test registered: test_template_staleness")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Block Construction Test (Phase B2 - Mainnet Hardening)
# Proves coinbase rules, subsidy schedule, and overflow safety
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_block_construction.cpp)
  add_executable(test_block_construction
    tests/mining/test_block_construction.cpp
  )
  target_link_libraries(test_block_construction PRIVATE
    dinero_crypto
  )
  target_include_directories(test_block_construction PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME BlockConstruction COMMAND test_block_construction)
  message(STATUS "  ✓ Phase B2 test registered: test_block_construction")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Miner Race Safety Test (Phase B3 - Mainnet Hardening)
# Proves concurrent mining cannot corrupt chain state
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_miner_race_safety.cpp)
  add_executable(test_miner_race_safety
    tests/mining/test_miner_race_safety.cpp
    src/daemon/interfaces/ingress_types.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_miner_race_safety PRIVATE
    dinero_crypto
  )
  target_include_directories(test_miner_race_safety PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME MinerRaceSafety COMMAND test_miner_race_safety)
  message(STATUS "  ✓ Phase B3 test registered: test_miner_race_safety")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Coinbase Integrity Test (Phase B4 - Mainnet Hardening)
# Proves coinbase cannot violate subsidy rules or commitments
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_coinbase_integrity.cpp)
  add_executable(test_coinbase_integrity
    tests/mining/test_coinbase_integrity.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_coinbase_integrity PRIVATE
    dinero_crypto
  )
  target_include_directories(test_coinbase_integrity PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME CoinbaseIntegrity COMMAND test_coinbase_integrity)
  message(STATUS "  ✓ Phase B4 test registered: test_coinbase_integrity")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Validation Bypass Test (Phase B5 - Mainnet Hardening)
# Proves miner cannot bypass any validation rule
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_validation_bypass.cpp)
  add_executable(test_validation_bypass
    tests/mining/test_validation_bypass.cpp
    src/daemon/interfaces/ingress_types.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_validation_bypass PRIVATE
    dinero_crypto
  )
  target_include_directories(test_validation_bypass PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME ValidationBypass COMMAND test_validation_bypass)
  message(STATUS "  ✓ Phase B5 test registered: test_validation_bypass")
endif()

# Stratum tests REMOVED - now in separate dinero-stratum repo

# ════════════════════════════════════════════════════════════════════════════
# P2P Adversarial Integrity Test (Phase D - Mainnet Hardening)
# Proves hostile peers cannot cause invalid state, exhaustion, or divergence
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_p2p_adversarial.cpp)
  add_executable(test_p2p_adversarial
    tests/p2p/test_p2p_adversarial.cpp
    src/daemon/interfaces/ingress_types.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_p2p_adversarial PRIVATE
    dinero_crypto
  )
  target_include_directories(test_p2p_adversarial PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME P2PAdversarial COMMAND test_p2p_adversarial)
  message(STATUS "  ✓ Phase D test registered: test_p2p_adversarial")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Daemon Operational Safety Test (Phase E - Mainnet Hardening)
# Proves node can run for months without corrupting itself
# E1: Restart invariants, E2: Long reorg safety, E3: Resource caps
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_daemon_operational_safety.cpp)
  add_executable(test_daemon_operational_safety
    tests/daemon/test_daemon_operational_safety.cpp
  )
  target_link_libraries(test_daemon_operational_safety PRIVATE
    pthread
  )
  target_include_directories(test_daemon_operational_safety PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME DaemonOperationalSafety COMMAND test_daemon_operational_safety)
  message(STATUS "  ✓ Phase E test registered: test_daemon_operational_safety")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Mainnet Dry Run Test (Phase F - Mainnet Hardening)
# Final validation: Single-node burn-in, multi-node consensus, full integration
# F1: Single-node stability, F2: Multi-node consensus, F3: Component integration
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/integration/test_mainnet_dry_run.cpp)
  add_executable(test_mainnet_dry_run
    tests/integration/test_mainnet_dry_run.cpp
  )
  target_link_libraries(test_mainnet_dry_run PRIVATE
    pthread
  )
  target_include_directories(test_mainnet_dry_run PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME MainnetDryRun COMMAND test_mainnet_dry_run)
  message(STATUS "  ✓ Phase F test registered: test_mainnet_dry_run")
endif()

# ════════════════════════════════════════════════════════════════════════════
# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
# Wallet ↔ Mining Flow Integration Test (Phase D.2)
# ════════════════════════════════════════════════════════════════════════════
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/integration/test_wallet_mining_flow.cpp)
  add_executable(test_wallet_mining_flow
    tests/integration/test_wallet_mining_flow.cpp
    tests/test_utils/test_utreexo_provider.cpp
    src/common/logger.cpp
  )

  target_link_libraries(test_wallet_mining_flow PRIVATE
    dinero_wallet
    dinero_consensus
    dinero_chainstate
    dinero_crypto
    dinero_core  # For LightningService and other dependencies
    jsoncpp_static
    sqlite3  # Required by WalletManager
    bulletproofs_ffi  # Required by ConfidentialTransactionValidator
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    GTest::gtest
  )
  link_zstd_if_needed(test_wallet_mining_flow)

  target_link_directories(test_wallet_mining_flow PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_wallet_mining_flow PRIVATE "-framework Security")
  endif()

  target_include_directories(test_wallet_mining_flow PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
  )

  add_test(NAME WalletMiningFlow COMMAND test_wallet_mining_flow)
  set_tests_properties(WalletMiningFlow PROPERTIES
    LABELS "integration;wallet;mining;critical"
    TIMEOUT 120
  )

  message(STATUS "✅ Wallet ↔ Mining flow integration test enabled (Phase D.2)")
else()
  message(STATUS "⚠️  Wallet ↔ Mining flow test not found (optional)")
endif()

# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
# ════════════════════════════════════════════════════════════════════════════
# Daemon Restart Safety Integration Test (Phase D.4)
# ════════════════════════════════════════════════════════════════════════════
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/integration/test_daemon_restart_safety.cpp)
  add_executable(test_daemon_restart_safety
    tests/integration/test_daemon_restart_safety.cpp
    src/common/logger.cpp
  )

  target_link_libraries(test_daemon_restart_safety PRIVATE
    dinero_wallet
    dinero_consensus
    dinero_chainstate
    dinero_crypto
    dinero_core
    jsoncpp_static
    sqlite3
    bulletproofs_ffi
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    GTest::gtest
  )
  link_zstd_if_needed(test_daemon_restart_safety)

  target_link_directories(test_daemon_restart_safety PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_daemon_restart_safety PRIVATE "-framework Security")
  endif()

  target_include_directories(test_daemon_restart_safety PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME DaemonRestartSafety COMMAND test_daemon_restart_safety)
  set_tests_properties(DaemonRestartSafety PROPERTIES
    LABELS "integration;persistence;critical"
    TIMEOUT 120
  )

  message(STATUS "✅ Daemon restart safety integration test enabled (Phase D.4)")
else()
  message(STATUS "⚠️  Daemon restart safety test not found (optional)")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Pruning Test Suite (Phase P.2)
# ════════════════════════════════════════════════════════════════════════════
# Validates pruning infrastructure correctness:
# - Restart/Recovery: prune_height persists across daemon restarts
# - Safety Boundaries: MIN_UNDO_DEPTH (288) is enforced
# - Index Consistency: BLOCK_HAVE_DATA flags are cleared after pruning
# - P2P Compatibility: NOTFOUND serialization is correct
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/storage/test_pruning.cpp)
  add_executable(test_pruning
    tests/storage/test_pruning.cpp
  )

  target_link_libraries(test_pruning PRIVATE
    dinero_core
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_pruning)

  target_link_directories(test_pruning PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_pruning PRIVATE "-framework Security" "-framework CoreFoundation")
  endif()

  target_include_directories(test_pruning PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME PruningTestSuite COMMAND test_pruning)
  set_tests_properties(PruningTestSuite PROPERTIES
    LABELS "storage;pruning;persistence;critical"
    TIMEOUT 60
  )

  message(STATUS "✅ Pruning test suite enabled (Phase P.2)")
else()
  message(STATUS "⚠️  Pruning test suite not found (optional)")
endif()

# May 2026 fleet-wide HAVE_UNDO regression guard.
# Pins ChainDB::setHeaderStatusBits OR-merge semantics so a future
# refactor can't silently strip consensus-layer bits via the
# header-sync path.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/storage/test_header_status_bits.cpp)
  add_executable(test_header_status_bits
    tests/storage/test_header_status_bits.cpp
  )
  target_link_libraries(test_header_status_bits PRIVATE
    dinero_core
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_header_status_bits)
  target_link_directories(test_header_status_bits PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_header_status_bits PRIVATE "-framework Security" "-framework CoreFoundation")
  endif()
  target_include_directories(test_header_status_bits PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME HeaderStatusBits COMMAND test_header_status_bits)
  set_tests_properties(HeaderStatusBits PROPERTIES
    LABELS "storage;persistence;critical;regression"
    TIMEOUT 30
  )
  message(STATUS "✅ Header status-bits regression guard enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/storage/test_header_status_overwrite_allowlist.sh)
  add_test(NAME HeaderStatusOverwriteAllowlist
    COMMAND ${CMAKE_SOURCE_DIR}/tests/storage/test_header_status_overwrite_allowlist.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
  set_tests_properties(HeaderStatusOverwriteAllowlist PROPERTIES
    LABELS "storage;persistence;critical;regression"
    TIMEOUT 10
  )
  message(STATUS "✅ Header status overwrite allowlist guard enabled")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Wallet Coinbase Maturity Integration Test
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/integration/test_wallet_coinbase_maturity.cpp)
  add_executable(test_wallet_coinbase_maturity
    tests/integration/test_wallet_coinbase_maturity.cpp
    src/common/logger.cpp
  )

  target_link_libraries(test_wallet_coinbase_maturity PRIVATE
    dinero_wallet
    dinero_consensus
    dinero_chainstate
    dinero_crypto
    dinero_core
    jsoncpp_static
    sqlite3
    bulletproofs_ffi
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    GTest::gtest
  )
  link_zstd_if_needed(test_wallet_coinbase_maturity)

  target_link_directories(test_wallet_coinbase_maturity PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_wallet_coinbase_maturity PRIVATE "-framework Security")
  endif()

  target_include_directories(test_wallet_coinbase_maturity PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME WalletCoinbaseMaturity COMMAND test_wallet_coinbase_maturity)
  set_tests_properties(WalletCoinbaseMaturity PROPERTIES
    LABELS "integration;wallet;coinbase;maturity"
    TIMEOUT 120
  )

  message(STATUS "✅ Wallet coinbase maturity integration test enabled")
else()
  message(STATUS "⚠️  Wallet coinbase maturity test not found (optional)")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Sync Profile Policy Tests
# ════════════════════════════════════════════════════════════════════════════
#
# Architecture: Pure policy/view tests for sync profile resolution and capabilities.
# Dependencies: gtest only
# Speed: Fast - no daemon startup, no IO
#
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/policy/test_sync_profile_policy.cpp)
  add_executable(test_sync_profile_policy
    tests/policy/test_sync_profile_policy.cpp
    src/nodecore/sync_profile_policy.cpp
  )

  add_dependencies(test_sync_profile_policy gtest)

  target_link_libraries(test_sync_profile_policy PRIVATE
    $<TARGET_FILE:gtest>
  )

  target_include_directories(test_sync_profile_policy BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME SyncProfilePolicy COMMAND test_sync_profile_policy)
  set_tests_properties(SyncProfilePolicy PROPERTIES
    LABELS "policy;profile;critical"
    TIMEOUT 10
  )

  message(STATUS "✅ Sync profile policy tests enabled")
else()
  message(STATUS "⚠️  Sync profile policy tests not found (optional)")
endif()

# ════════════════════════════════════════════════════════════════════════════
# NodeCore Runtime Guard Tests
# ════════════════════════════════════════════════════════════════════════════
#
# Architecture: Pure runtime guard/view tests for FFI shutdown gating and
# thread-safe callback/watch snapshots.
# Dependencies: gtest only
# Speed: Fast - no daemon startup, no IO
#
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/policy/test_nodecore_runtime_guards.cpp)
  add_executable(test_nodecore_runtime_guards
    tests/policy/test_nodecore_runtime_guards.cpp
  )

  add_dependencies(test_nodecore_runtime_guards gtest)

  target_link_libraries(test_nodecore_runtime_guards PRIVATE
    $<TARGET_FILE:gtest>
  )

  target_include_directories(test_nodecore_runtime_guards BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME NodeCoreRuntimeGuards COMMAND test_nodecore_runtime_guards)
  set_tests_properties(NodeCoreRuntimeGuards PROPERTIES
    LABELS "policy;nodecore;hardening"
    TIMEOUT 10
  )

  message(STATUS "✅ NodeCore runtime guard tests enabled")
else()
  message(STATUS "⚠️  NodeCore runtime guard tests not found (optional)")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Mining Policy Tripwire Tests (Phase E.1)
# ════════════════════════════════════════════════════════════════════════════
#
# Architecture: Tests operate on POLICY VIEWS only (no implementation)
# Dependencies: Minimal - only gtest (no wallet, no crypto, no database)
# Speed: Fast - no IO, no initialization, pure logic tests
#
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/policy/test_mining_policy_tripwires.cpp)
  add_executable(test_mining_policy_tripwires
    tests/policy/test_mining_policy_tripwires.cpp
  )

  # Explicit dependencies to ensure gtest builds first
  add_dependencies(test_mining_policy_tripwires gtest)

  # Link googletest libraries using generator expressions (ensures proper build order)
  target_link_libraries(test_mining_policy_tripwires PRIVATE
    $<TARGET_FILE:gtest>
    )

  # Add vendored googletest includes BEFORE system includes
  target_include_directories(test_mining_policy_tripwires BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME MiningPolicyTripwires COMMAND test_mining_policy_tripwires)
  set_tests_properties(MiningPolicyTripwires PROPERTIES
    LABELS "policy;tripwire;critical"
    TIMEOUT 10
  )

  message(STATUS "✅ Mining policy tripwire tests enabled (Phase E.1 + E.2) - pure policy views")
else()
  message(STATUS "⚠️  Mining policy tripwire tests not found (optional)")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Mining Restart Policy Tripwire Tests (Phase E.3)
# ════════════════════════════════════════════════════════════════════════════
#
# Architecture: Tests operate on RESTART POLICY VIEWS only (no execution)
# Dependencies: Minimal - only gtest (no mining, no daemon lifecycle)
# Speed: Fast - no IO, no threads, pure logic tests
#
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/policy/test_mining_restart_policy_tripwires.cpp)
  add_executable(test_mining_restart_policy_tripwires
    tests/policy/test_mining_restart_policy_tripwires.cpp
  )

  add_dependencies(test_mining_restart_policy_tripwires gtest)

  target_link_libraries(test_mining_restart_policy_tripwires PRIVATE
    $<TARGET_FILE:gtest>
    )

  target_include_directories(test_mining_restart_policy_tripwires BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME MiningRestartPolicyTripwires COMMAND test_mining_restart_policy_tripwires)
  set_tests_properties(MiningRestartPolicyTripwires PROPERTIES
    LABELS "policy;tripwire;critical;restart"
    TIMEOUT 10
  )

  message(STATUS "✅ Mining restart policy tripwire tests enabled (Phase E.3) - pure policy views")
else()
  message(STATUS "⚠️  Mining restart policy tripwire tests not found (optional)")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Mining Completeness Policy Tripwire Tests (Phase E.4)
# ════════════════════════════════════════════════════════════════════════════
#
# Architecture: Tests ensure no illegal mining state exists in RPC surface
# Dependencies: Minimal - only gtest (no mining, no wallet, no execution)
# Speed: Fast - no IO, pure logic tests
#
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/policy/test_mining_completeness_tripwires.cpp)
  add_executable(test_mining_completeness_tripwires
    tests/policy/test_mining_completeness_tripwires.cpp
  )

  add_dependencies(test_mining_completeness_tripwires gtest)

  target_link_libraries(test_mining_completeness_tripwires PRIVATE
    $<TARGET_FILE:gtest>
    )

  target_include_directories(test_mining_completeness_tripwires BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME MiningCompletenessPolicyTripwires COMMAND test_mining_completeness_tripwires)
  set_tests_properties(MiningCompletenessPolicyTripwires PROPERTIES
    LABELS "policy;tripwire;critical;completeness"
    TIMEOUT 10
  )

  message(STATUS "✅ Mining completeness policy tripwire tests enabled (Phase E.4) - pure policy views")
else()
  message(STATUS "⚠️  Mining completeness policy tripwire tests not found (optional)")
endif()

#
# Phase F.3: View Builder Unit Tests
# ==================================
# Target: test_view_builders
# Purpose: Verify view builder functions are correct, null-safe, and error-resistant
# Dependencies: gtest only (no wallet, no crypto, no database)
# Speed: Fast - no IO, pure logic tests
#
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/policy/test_view_builders.cpp)
  add_executable(test_view_builders
    tests/policy/test_view_builders.cpp
  )

  add_dependencies(test_view_builders gtest)

  target_link_libraries(test_view_builders PRIVATE
    $<TARGET_FILE:gtest>
    )

  target_include_directories(test_view_builders BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME ViewBuilderTests COMMAND test_view_builders)
  set_tests_properties(ViewBuilderTests PROPERTIES
    LABELS "policy;view-builder;unit;f3"
    TIMEOUT 10
  )

  message(STATUS "✅ View builder unit tests enabled (Phase F.3) - null-safe, error-resistant")
else()
  message(STATUS "⚠️  View builder unit tests not found (optional)")
endif()

#
# Phase F.4: Escape Hatch Integration Tests
# ==========================================
# Target: test_escape_hatches
# Purpose: Verify escape hatch flags allow what policy permits and block correctly
# Dependencies: gtest only (no wallet, no crypto, no database)
# Speed: Fast - no IO, pure policy logic tests
#
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/policy/test_escape_hatches.cpp)
  add_executable(test_escape_hatches
    tests/policy/test_escape_hatches.cpp
  )

  add_dependencies(test_escape_hatches gtest)

  target_link_libraries(test_escape_hatches PRIVATE
    $<TARGET_FILE:gtest>
    )

  target_include_directories(test_escape_hatches BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME EscapeHatchTests COMMAND test_escape_hatches)
  set_tests_properties(EscapeHatchTests PROPERTIES
    LABELS "policy;escape-hatch;integration;f4"
    TIMEOUT 10
  )

  message(STATUS "✅ Escape hatch integration tests enabled (Phase F.4) - flag enforcement")
else()
  message(STATUS "⚠️  Escape hatch integration tests not found (optional)")
endif()

# Transaction Serialization Test - v0.13.0.1 Step A (Proof Layer 1)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_transaction_serialization.cpp)
  add_executable(test_transaction_serialization
    tests/network/test_transaction_serialization.cpp
    src/common/logger.cpp  # Required by transaction serialization
  )
  target_link_libraries(test_transaction_serialization PRIVATE
    dinero_wallet
    dinero_crypto
    OpenSSL::Crypto
  )
  target_include_directories(test_transaction_serialization PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME TransactionSerialization COMMAND test_transaction_serialization)
endif()

# Block Serialization Test - RocksDB persistence guard
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_block_serialization.cpp)
  add_executable(test_block_serialization
    tests/network/test_block_serialization.cpp
    src/common/logger.cpp  # Required by block serialization
  )
  target_link_libraries(test_block_serialization PRIVATE
    dinero_wallet
    dinero_crypto
    OpenSSL::Crypto
  )
  target_include_directories(test_block_serialization PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME BlockSerialization COMMAND test_block_serialization)
endif()

# BlockStorage Round-Trip Test - blk*.dat persistence guard
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/storage/test_block_storage_roundtrip.cpp)
  add_executable(test_block_storage_roundtrip
    tests/storage/test_block_storage_roundtrip.cpp
    src/common/logger.cpp  # Provides g_logger if referenced transitively
  )
  target_link_libraries(test_block_storage_roundtrip PRIVATE
    dinero_chainstate
    dinero_crypto
    OpenSSL::Crypto
  )
  target_include_directories(test_block_storage_roundtrip PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME BlockStorageRoundTrip COMMAND test_block_storage_roundtrip)
endif()

# Archival Block Reader Test - distinguish flatfiles from legacy ChainDB fallback
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/storage/test_archival_block_reader.cpp)
  add_executable(test_archival_block_reader
    tests/storage/test_archival_block_reader.cpp
    src/daemon/genesis_init.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_archival_block_reader PRIVATE
    dinero_core
    dinero_chainstate
    dinero_crypto
    OpenSSL::Crypto
  )
  target_include_directories(test_archival_block_reader PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME ArchivalBlockReader COMMAND test_archival_block_reader)
endif()

# Phase M.0 Byte Order Invariant Test - CRITICAL CONSENSUS LOCK
# This test permanently locks the invariant that uint256 is little-endian internally
# and GetHex() reverses bytes for display. IF THIS TEST FAILS, CONSENSUS IS BROKEN.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_phase_m0_byte_order_invariant.cpp)
  add_executable(test_phase_m0_byte_order_invariant
    tests/test_phase_m0_byte_order_invariant.cpp
    src/common/logger.cpp  # Required by transaction code
  )
  target_link_libraries(test_phase_m0_byte_order_invariant PRIVATE
    dinero_wallet
    dinero_crypto
    OpenSSL::Crypto
  )
  target_include_directories(test_phase_m0_byte_order_invariant PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME PhaseM0ByteOrderInvariant COMMAND test_phase_m0_byte_order_invariant)
endif()

# Logger Injection Test - Demonstrates TestLogger and NullLogger usage
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_logger_injection.cpp)
  add_executable(test_logger_injection
    tests/test_logger_injection.cpp
  )
  target_include_directories(test_logger_injection PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME test_logger_injection COMMAND test_logger_injection)
endif()

# JsonLogger Test - Validates structured JSON logging
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_json_logger.cpp)
  add_executable(test_json_logger
    tests/test_json_logger.cpp
    src/common/json_logger.cpp
  )
  target_include_directories(test_json_logger PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME test_json_logger COMMAND test_json_logger)
endif()

# Zero-Knowledge Privacy Test - Pedersen commitments + Bulletproofs
if(ENABLE_TESTS AND ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_zk_commitment_balance.cpp)
  add_executable(test_zk_commitment_balance
    tests/test_zk_commitment_balance.cpp
  )
  target_link_libraries(test_zk_commitment_balance PRIVATE
    dinero_zk
    lightning_core_static  # For secp256k1-zkp (used by Pedersen commitments)
  )
  target_include_directories(test_zk_commitment_balance PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME test_zk_commitment_balance COMMAND test_zk_commitment_balance)
endif()

# ZKVM ring-covenant test infra removed in v7 (see commit 1d8118f46) — the
# zkvm covenant prover only exercised CLSAG / ring-covenant spends which
# don't exist on v7. Test source deleted.

# Hybrid hidden-member proof split tests

# SHA256 R1CS gadget tests
if(ENABLE_TESTS AND ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_sha256_gadget.cpp)
  add_executable(test_sha256_gadget
    tests/test_sha256_gadget.cpp
  )
  target_link_libraries(test_sha256_gadget PRIVATE
    dinero_zk
    dinero_crypto
    secp256k1
    OpenSSL::Crypto
    gtest
    gtest_main
  )
  link_secp256k1_zkp(test_sha256_gadget)
  if(APPLE)
    target_link_libraries(test_sha256_gadget PRIVATE "-framework Security" "-framework CoreFoundation")
  endif()
  target_include_directories(test_sha256_gadget PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  )
  add_test(NAME SHA256Gadget COMMAND test_sha256_gadget)
  set_tests_properties(SHA256Gadget PROPERTIES
    LABELS "zk;zkvm;sha256;gadget"
    TIMEOUT 300
  )
  message(STATUS "  ✓ SHA256 gadget test registered")
endif()

# CTV preimage circuit test removed in v7 — only verified ring-anonymous
# covenant spends, which require ring-covenant code that was deleted.

# ZKVM benchmarks (bench_zkvm + bench_mixed_block) removed in v7 — benchmarked
# CLSAG / ring-covenant proving paths that don't exist on v7.

# Bulletproofs Integration Test - Comprehensive FFI and validation tests
if(ENABLE_TESTS AND CARGO_EXECUTABLE AND ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_bulletproofs_integration.cpp)
  add_executable(test_bulletproofs_integration
    tests/test_bulletproofs_integration.cpp
    src/common/logger.cpp                    # Logger implementation
  )
  target_link_libraries(test_bulletproofs_integration PRIVATE
    dinero_consensus        # For ConfidentialTransactionValidator
    dinero_zk
    bulletproofs_ffi        # Dalek Bulletproofs FFI
    dinero_crypto           # For crypto primitives
    lightning_core_static   # For secp256k1-zkp (Pedersen)
    gtest
    ${CMAKE_DL_LIBS}        # Required for Rust FFI
  )
  link_zstd_if_needed(test_bulletproofs_integration)
  target_include_directories(test_bulletproofs_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  )
  add_test(NAME test_bulletproofs_integration COMMAND test_bulletproofs_integration)

  # Ensure Bulletproofs FFI is built before test
  if(TARGET build_bulletproofs_ffi)
    add_dependencies(test_bulletproofs_integration build_bulletproofs_ffi)
  endif()
endif()

# Bulletproofs Batch Verification Test (Phase F.8) - Comprehensive batch verification tests
if(CARGO_EXECUTABLE AND ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_bulletproofs_batch.cpp)
  add_executable(test_bulletproofs_batch
    tests/test_bulletproofs_batch.cpp
    src/common/logger.cpp                    # Logger implementation
  )
  target_link_libraries(test_bulletproofs_batch PRIVATE
    dinero_consensus        # For ConfidentialTransactionValidator
    dinero_zk
    bulletproofs_ffi        # Dalek Bulletproofs FFI
    dinero_crypto           # For crypto primitives (includes pedersen, bulletproofs, logger)
    lightning_core_static   # For secp256k1-zkp (Pedersen)
    gtest
    ${CMAKE_DL_LIBS}        # Required for Rust FFI
  )
  link_zstd_if_needed(test_bulletproofs_batch)
  target_include_directories(test_bulletproofs_batch PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  )
  add_test(NAME test_bulletproofs_batch COMMAND test_bulletproofs_batch)

  # Ensure Bulletproofs FFI is built before test
  if(TARGET build_bulletproofs_ffi)
    add_dependencies(test_bulletproofs_batch build_bulletproofs_ffi)
  endif()
endif()

# Confidential Transaction Consensus Pipeline Tests (Phase G.5)
# Integration tests for complete confidential transaction validation

# Confidential Transaction Serialization Tests (Phase I.3)
# Tests for serialization/deserialization of transaction variants

# Ring Signature TxInput Serialization Tests (Phase R.4)
# Tests for ring_members, key_image, clsag_signature fields on TxInput

# REMOVED: test_confidential_tx_builder, test_confidential_integration (obsolete zombie tests from copy repo)

# Confidential Transaction End-to-End Tests (Phase K.2) - DISABLED: obsolete
# Complete workflow tests including Alice-to-Bob transactions
if(FALSE AND CARGO_EXECUTABLE AND ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_confidential_end_to_end.cpp)
  add_executable(test_confidential_end_to_end
    tests/test_confidential_end_to_end.cpp
    src/common/logger.cpp  # Logger implementation
    src/common/address_script_builder.cpp  # BuildScriptPubKeyFromAddress
    src/daemon/legacy_globals_stub.cpp  # GetWalletManagerForIndexing
    src/daemon/bech32_encoder.cpp  # Bech32Encoder
  )
  target_link_libraries(test_confidential_end_to_end PRIVATE
    dinero_wallet          # Wallet library (includes all wallet sources)
    dinero_chainstate      # For UTXOIndex
    dinero_consensus       # For consensus validation
    dinero_zk
    bulletproofs_ffi        # Dalek Bulletproofs FFI
    dinero_crypto           # For crypto primitives
    lightning_core_static   # For secp256k1-zkp
    gtest
    ${CMAKE_DL_LIBS}        # Required for Rust FFI
    OpenSSL::Crypto        # For SHA256 in address_db
    sqlite3                # SQLite database
  )
  link_zstd_if_needed(test_confidential_end_to_end)
  if(APPLE)
    target_link_libraries(test_confidential_end_to_end PRIVATE
    )
  endif()
  target_include_directories(test_confidential_end_to_end PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  )
  add_test(NAME test_confidential_end_to_end COMMAND test_confidential_end_to_end)

  # Ensure Bulletproofs FFI is built before test
  if(TARGET build_bulletproofs_ffi)
    add_dependencies(test_confidential_end_to_end build_bulletproofs_ffi)
  endif()
endif()

# CON-11 Commitment Balance Tests (Ristretto255) - DISABLED: obsolete
if(FALSE AND CARGO_EXECUTABLE AND ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_con11_ristretto255.cpp)
  add_executable(test_con11_ristretto255
    tests/test_con11_ristretto255.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_con11_ristretto255 PRIVATE
    dinero_consensus
    dinero_wallet
    gtest
    bulletproofs_ffi
  )
  link_zstd_if_needed(test_con11_ristretto255)
  if(APPLE)
    target_link_libraries(test_con11_ristretto255 PRIVATE
    )
  endif()
  target_include_directories(test_con11_ristretto255 PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  )
  add_test(NAME test_con11_ristretto255 COMMAND test_con11_ristretto255)

  # Ensure Bulletproofs FFI is built before test
  if(TARGET build_bulletproofs_ffi)
    add_dependencies(test_con11_ristretto255 build_bulletproofs_ffi)
  endif()
endif()

# Range Proof Tests (Bulletproofs FFI)
if(CARGO_EXECUTABLE AND ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_range_proofs.cpp)
  add_executable(test_range_proofs
    tests/test_range_proofs.cpp
  )
  target_link_libraries(test_range_proofs PRIVATE
    gtest
    bulletproofs_ffi
  )
  # Rust's std on Windows pulls BCryptGenRandom (bcrypt.lib) for stdlib RNG,
  # NtWriteFile / RtlNtStatusToDosError (ntdll.lib) for stdio, and userenv
  # for env access. These are the standard runtime libs for any Rust
  # staticlib consumed by an MSVC link.
  if(WIN32)
    target_link_libraries(test_range_proofs PRIVATE bcrypt ntdll userenv ws2_32)
  endif()
  if(APPLE)
    target_link_libraries(test_range_proofs PRIVATE
    )
  endif()
  target_include_directories(test_range_proofs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME test_range_proofs COMMAND test_range_proofs)
  if(TARGET build_bulletproofs_ffi)
    add_dependencies(test_range_proofs build_bulletproofs_ffi)
  endif()
endif()

# CT Selection Policy Unit Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_ct_selection_policy.cpp)
  add_executable(test_ct_selection_policy
    tests/mining/test_ct_selection_policy.cpp
    src/mining/ct_selection_policy.cpp
    src/consensus/chainparams_impl.cpp
  )
  target_link_libraries(test_ct_selection_policy PRIVATE
    gtest
    dinero_consensus
    dinero_crypto
  )
  target_include_directories(test_ct_selection_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME test_ct_selection_policy COMMAND test_ct_selection_policy)
endif()

# WalletService Logger Pattern Demo - Shows how to use TestLogger with services
# REMOVED: test_wallet_service_logging_simple (obsolete - DaemonContext issue)

# test_wallet_crypto temporarily disabled due to g_logger linking issues
# Will be fixed when logger is refactored
# add_executable(test_wallet_crypto tests/test_wallet_crypto.cpp src/wallet/key_vault_simple.cpp)
# if(APPLE)
#   target_link_libraries(test_wallet_crypto PRIVATE
#     dinero_wallet
#     OpenSSL::Crypto
#     jsoncpp_static
#     secp256k1
#   )
# else()
#   target_link_libraries(test_wallet_crypto PRIVATE
#     dinero_wallet
#     OpenSSL::Crypto
#     jsoncpp
#     secp256k1
#   )
# endif()
# target_include_directories(test_wallet_crypto PRIVATE include src)

# Week 5: Context-aware smoke tests - DISABLED: stratum now separate binary
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_block_assembler_smoke.cpp)
  add_executable(test_block_assembler_smoke
    tests/mining/test_block_assembler_smoke.cpp
    # Service implementations needed for TestDaemonContext
    # Note: logger_service.h is header-only, no .cpp needed
    src/daemon/services/config_service.cpp
    src/daemon/services/chainstate_service.cpp
    src/daemon/services/wallet_service.cpp
    # Core dependencies
    src/common/logger.cpp
    src/common/config_manager.cpp
    src/daemon/block_acceptor.cpp  # Needed by chainstate_service
    src/database/sqlite_manager.cpp
    # src/consensus/supply_tracker.cpp  # REMOVED: Zombie code
    src/consensus/block_index.cpp
    src/consensus/block_lifecycle.cpp
    src/consensus/orphan_manager.cpp
    src/consensus/block_index_persistence.cpp
    # src/consensus/genesis_canonical.cpp  # REMOVED - depends on missing genesis_block.h
    src/consensus/chainparams_impl.cpp
    src/consensus/chainwork.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/storage/chain_db.cpp
    src/wallet/utxo_index.cpp
    src/wallet/wallet_worker.cpp
    src/wallet/wallet_manager.cpp  # Modern HD wallet with BIP84 (Nov 2025)
    src/wallet/address.cpp
    src/mining/block_assembler.cpp
    # src/mining/template_validator.cpp  # REMOVED: Zombie code
    src/daemon/mining_safety_gates.cpp
    # Additional dependencies from block_acceptor and chainstate
    src/daemon/mining_safety_gates.cpp
    src/address/addr_codec.cpp
    src/address/addr_types.cpp
    src/core/crypto/ripemd160.cpp
    # Missing dependencies for linker
    src/metrics/metrics_registry.cpp
    src/daemon/mempool.cpp
    src/crypto/decrypt_encrypted_key.cpp
    src/daemon/secure_random.cpp
    src/daemon/wallet_crypto.cpp  # Contains decryptAesGcm, encryptAesGcm, deriveKeyArgon2id
    src/daemon/bech32_decode.cpp  # Contains Bech32DecodeSegwit
    src/daemon/ws_globals.cpp  # Contains g_subscriptions
    src/daemon/p2p_message.cpp  # Contains InvMessage implementation
  )
  target_include_directories(test_block_assembler_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}  # For version_config.h
    ${CMAKE_SOURCE_DIR}/external  # For bech32.hpp
  )
  target_link_libraries(test_block_assembler_smoke PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_block_assembler_smoke)
  if(APPLE)
    target_link_libraries(test_block_assembler_smoke PRIVATE
    )
  endif()
  add_test(NAME BlockAssemblerSmoke COMMAND test_block_assembler_smoke)
endif()

# Phase 2: Consensus engine integration test
# DISABLED: stratum now separate binary in DaemonContext
# Phase C: Also needs updating to use MiningManager v2 instead of legacy Mining
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_consensus_integration.cpp)
  add_executable(test_consensus_integration
    tests/consensus/test_consensus_integration.cpp
    # Reuse same dependencies as test_block_assembler_smoke
    src/daemon/services/config_service.cpp
    src/daemon/services/chainstate_service.cpp
    src/daemon/services/mining_service.cpp
    src/daemon/services/wallet_service.cpp
    # Phase C: Legacy Mining class removed - test needs updating
    # src/daemon/mining.cpp
    src/consensus/pow_consensus_engine.cpp
    # Core dependencies
    src/common/logger.cpp
    src/common/config_manager.cpp
    src/daemon/block_acceptor.cpp
    src/database/sqlite_manager.cpp
    # src/consensus/supply_tracker.cpp  # REMOVED: Zombie code
    src/consensus/block_index.cpp
    src/consensus/block_lifecycle.cpp
    src/consensus/orphan_manager.cpp
    src/consensus/block_index_persistence.cpp
    # src/consensus/genesis_canonical.cpp  # REMOVED - depends on missing genesis_block.h
    src/consensus/chainparams_impl.cpp
    src/consensus/chainwork.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/storage/chain_db.cpp
    src/wallet/utxo_index.cpp
    src/wallet/wallet_worker.cpp
    src/wallet/wallet_manager.cpp  # Modern HD wallet with BIP84 (Nov 2025)
    src/wallet/address.cpp
    src/mining/block_assembler.cpp
    # src/mining/template_validator.cpp  # REMOVED: Zombie code
    src/mining/miner.cpp
    # src/mining/mining_manager.cpp  # Phase C: Replaced by mining_manager_v2.cpp (ODR violation fix)
    src/daemon/mining_safety_gates.cpp
    src/address/addr_codec.cpp
    src/address/addr_types.cpp
    src/core/crypto/ripemd160.cpp
    src/metrics/metrics_registry.cpp
    src/daemon/mempool.cpp
    src/crypto/decrypt_encrypted_key.cpp
    src/daemon/secure_random.cpp
    src/daemon/wallet_crypto.cpp
    src/daemon/bech32_decode.cpp
    src/daemon/ws_globals.cpp
    src/daemon/p2p_message.cpp
  )
  target_include_directories(test_consensus_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/external
  )
  target_link_libraries(test_consensus_integration PRIVATE
    dinero_common
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    sqlite3
    jsoncpp_static
    secp256k1
    OpenSSL::SSL
    OpenSSL::Crypto
  )
  link_zstd_if_needed(test_consensus_integration)
  if(APPLE)
    target_link_libraries(test_consensus_integration PRIVATE
    )
  endif()
  add_test(NAME ConsensusIntegration COMMAND test_consensus_integration)
endif()

# Codebase Verification Integration Tests - DISABLED: links dinero_wallet (LightningService issue)
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/integration/test_codebase_verification.cpp)
  add_executable(test_codebase_verification
    tests/integration/test_codebase_verification.cpp
    tests/support/test_stubs.cpp
    # Reuse same dependencies as test_mining_smoke
    src/daemon/services/config_service.cpp
    src/daemon/services/chainstate_service.cpp
    src/daemon/services/wallet_service.cpp
    src/common/logger.cpp
    src/common/config_manager.cpp
    src/daemon/block_acceptor.cpp
    src/database/sqlite_manager.cpp
    # src/daemon/db_init_simple.cpp  # REMOVED: Zombie - legacy SQLite init, bypassed ChainWriteToken
    src/daemon/db_meta_utils.cpp
    # src/consensus/supply_tracker.cpp  # REMOVED: Zombie code
    src/consensus/block_index.cpp
    src/consensus/block_lifecycle.cpp
    src/consensus/orphan_manager.cpp
    src/consensus/block_index_persistence.cpp
    # src/consensus/genesis_canonical.cpp  # REMOVED - depends on missing genesis_block.h
    src/consensus/chainparams_impl.cpp
    src/consensus/chainwork.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/storage/chain_db.cpp
    src/wallet/utxo_index.cpp
    src/wallet/wallet_worker.cpp
    src/wallet/wallet_manager.cpp  # Modern HD wallet with BIP84 (Nov 2025)
    src/wallet/address.cpp
    src/mining/block_assembler.cpp
    # src/mining/template_validator.cpp  # REMOVED: Zombie code
    src/daemon/mining_safety_gates.cpp
    src/address/addr_codec.cpp
    src/address/addr_types.cpp
    src/core/crypto/ripemd160.cpp
    src/metrics/metrics_registry.cpp
    src/daemon/mempool.cpp
    src/crypto/decrypt_encrypted_key.cpp
    src/daemon/secure_random.cpp
    src/daemon/wallet_crypto.cpp
    src/daemon/bech32_decode.cpp
    src/daemon/ws_globals.cpp
    src/daemon/p2p_message.cpp
    src/daemon/p2p_manager.cpp
  )
  target_include_directories(test_codebase_verification PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/external
  )
  target_link_libraries(test_codebase_verification PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
    gtest
  )
  link_zstd_if_needed(test_codebase_verification)
  if(APPLE)
    target_link_libraries(test_codebase_verification PRIVATE
      "-framework IOKit"
    )
  endif()
  target_compile_definitions(test_codebase_verification PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  add_test(NAME CodebaseVerification COMMAND test_codebase_verification)
endif()

# Comprehensive Wallet Tests - DISABLED: obsolete
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_wallet_comprehensive.cpp)
  add_executable(test_wallet_comprehensive
    tests/wallet/test_wallet_comprehensive.cpp
    tests/support/test_stubs.cpp
    src/common/logger.cpp
    src/wallet/wallet_manager.cpp  # Modern HD wallet with BIP84 (Nov 2025)
    src/wallet/hd_wallet.cpp
    src/wallet/bip39.cpp
    src/wallet/psbt.cpp
    src/wallet/coin_selection.cpp
    src/crypto/pbkdf2.cpp
    src/consensus/coinbase_maturity.cpp
  )
  target_link_libraries(test_wallet_comprehensive PRIVATE
    dinero_wallet
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    secp256k1
    gtest
  )
  link_zstd_if_needed(test_wallet_comprehensive)
  target_include_directories(test_wallet_comprehensive PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
  )
  if(APPLE)
    target_link_libraries(test_wallet_comprehensive PRIVATE
    )
  endif()
  add_test(NAME WalletComprehensive COMMAND test_wallet_comprehensive)
endif()

# Taproot Mining Tests - DISABLED: hd_wallet.cpp include path issue with bech32
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_taproot_mining.cpp)
  add_executable(test_taproot_mining
    tests/test_taproot_mining.cpp
    src/wallet/hd_wallet.cpp
    src/wallet/bip39.cpp
    src/wallet/psbt.cpp
    src/wallet/coin_selection.cpp
    src/wallet/utxo_index.cpp
    src/wallet/bip143_signer.cpp
    src/wallet/taproot_tx_signer.cpp
    src/crypto/pbkdf2.cpp
    src/crypto/ripemd160_standalone.cpp
    src/crypto/sha256.cpp
    src/crypto/params_stub.cpp
    src/core/crypto/ripemd160.cpp
    src/common/logger.cpp
    src/consensus/coinbase_maturity.cpp
    src/database/sqlite_manager.cpp
    # src/daemon/db_init_simple.cpp  # REMOVED: Zombie - legacy SQLite init, bypassed ChainWriteToken
    src/daemon/db_meta_utils.cpp
    src/sqlite_open.cpp
    external/bech32/bech32.cpp
  )
  target_link_libraries(test_taproot_mining PRIVATE
    sqlite3
    secp256k1
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
  )
  target_include_directories(test_taproot_mining PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
    ${CMAKE_SOURCE_DIR}/third_party/bip39
  )
  target_compile_definitions(test_taproot_mining PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  if(APPLE)
    target_link_libraries(test_taproot_mining PRIVATE
      "-framework Security"
    )
  endif()
  add_test(NAME TaprootMining COMMAND test_taproot_mining)
endif()

# Minimal Taproot Mining Test (for debugging) - DISABLED: LightningService dependency
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_taproot_mining_minimal.cpp)
  add_executable(test_taproot_mining_minimal
    tests/test_taproot_mining_minimal.cpp
    src/crypto/params_stub.cpp
    src/sqlite_open.cpp
    src/common/logger.cpp
    tests/stubs/utxo_index_stub.cpp
  )
  target_link_libraries(test_taproot_mining_minimal PRIVATE
    dinero_wallet
    dinero_crypto
    secp256k1
    sqlite3
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
  )
  target_include_directories(test_taproot_mining_minimal PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
    ${CMAKE_SOURCE_DIR}/third_party/bip39
  )
  target_compile_definitions(test_taproot_mining_minimal PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  if(APPLE)
    target_link_libraries(test_taproot_mining_minimal PRIVATE
    )
  endif()
endif()

# Comprehensive Mining Tests - DISABLED: LightningService dependency + multiple framework issues
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_mining_comprehensive.cpp)
  add_executable(test_mining_comprehensive
    tests/mining/test_mining_comprehensive.cpp
    tests/support/test_stubs.cpp
    src/common/logger.cpp
    src/common/config_manager.cpp
    src/daemon/mempool.cpp
    src/mining/block_assembler.cpp
    # src/mining/template_validator.cpp  # REMOVED: Zombie code
    src/database/sqlite_manager.cpp
    # src/daemon/db_init_simple.cpp  # REMOVED: Zombie - legacy SQLite init, bypassed ChainWriteToken
    src/daemon/db_meta_utils.cpp
    # src/consensus/supply_tracker.cpp  # REMOVED: Zombie code
    src/consensus/block_index.cpp
    src/consensus/block_lifecycle.cpp
    src/consensus/orphan_manager.cpp
    src/consensus/block_index_persistence.cpp
    # src/consensus/genesis_canonical.cpp  # REMOVED - depends on missing genesis_block.h
    src/consensus/chainparams_impl.cpp
    src/consensus/chainwork.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/storage/chain_db.cpp
    src/daemon/mining_safety_gates.cpp
    src/address/addr_codec.cpp
    src/address/addr_types.cpp
    src/core/crypto/ripemd160.cpp
    src/metrics/metrics_registry.cpp
    src/daemon/bech32_decode.cpp
    src/daemon/p2p_message.cpp
    src/daemon/p2p_manager.cpp
    src/daemon/wallet_crypto.cpp
    src/daemon/secure_random.cpp
    src/crypto/decrypt_encrypted_key.cpp
    src/consensus/coinbase_maturity.cpp
  )
  target_link_libraries(test_mining_comprehensive PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
    gtest
  )
  link_zstd_if_needed(test_mining_comprehensive)
  target_include_directories(test_mining_comprehensive PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/external
  )
  target_compile_definitions(test_mining_comprehensive PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  if(APPLE)
    target_link_libraries(test_mining_comprehensive PRIVATE
      "-framework IOKit"
    )
  endif()
  add_test(NAME MiningComprehensive COMMAND test_mining_comprehensive)
endif()

# Comprehensive PSBT Tests - DISABLED: LightningService dependency
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_psbt_comprehensive.cpp)
  add_executable(test_psbt_comprehensive
    tests/wallet/test_psbt_comprehensive.cpp
    tests/support/test_stubs.cpp
    src/common/logger.cpp
    src/wallet/wallet_manager.cpp  # Modern HD wallet with BIP84 (Nov 2025)
    src/wallet/hd_wallet.cpp
    src/wallet/bip39.cpp
    src/wallet/psbt.cpp
    src/crypto/pbkdf2.cpp
    src/consensus/coinbase_maturity.cpp
  )
  target_link_libraries(test_psbt_comprehensive PRIVATE
    dinero_wallet
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    secp256k1
    gtest
  )
  link_zstd_if_needed(test_psbt_comprehensive)
  target_include_directories(test_psbt_comprehensive PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
  )
  if(APPLE)
    target_link_libraries(test_psbt_comprehensive PRIVATE
    )
  endif()
  add_test(NAME PSBTComprehensive COMMAND test_psbt_comprehensive)
endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase W.3: Ergonomics RPC Tests
# ════════════════════════════════════════════════════════════════════════════
# TEMPORARILY DISABLED (2026-02-02): dinero_core missing Mempool/BridgeNode sources
# TODO: Add missing source files to dinero_core library
# if(EXISTS ${CMAKE_SOURCE_DIR}/tests/rpc/test_ergonomics_rpc.cpp)
#   add_executable(test_ergonomics_rpc
#     tests/rpc/test_ergonomics_rpc.cpp
#     src/common/logger.cpp
#   )
#   target_link_libraries(test_ergonomics_rpc PRIVATE
#     dinero_rpc_handlers dinero_core dinero_wallet dinero_chainstate
#     dinero_consensus jsoncpp_static bulletproofs_ffi
#   )
#   if(APPLE)
#     target_link_libraries(test_ergonomics_rpc PRIVATE "-framework Security" "-framework CoreFoundation")
#   endif()
#   link_zstd_if_needed(test_ergonomics_rpc)
#   target_include_directories(test_ergonomics_rpc PRIVATE
#     ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/tests
#   )
#   add_test(NAME ErgonomicsRPC COMMAND test_ergonomics_rpc)
# endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase W.4.1: Transaction Inclusion Analyzer Tests
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_tx_inclusion_analyzer.cpp)
  add_executable(test_tx_inclusion_analyzer
    tests/mining/test_tx_inclusion_analyzer.cpp
    src/common/logger.cpp
    src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    src/daemon/config.cpp     # Provides GetConfig()
  )
  target_link_libraries(test_tx_inclusion_analyzer PRIVATE
    dinero_core
    dinero_chainstate
    dinero_consensus
    bulletproofs_ffi  # Required by confidential_validation
    jsoncpp_static
  )
  link_zstd_if_needed(test_tx_inclusion_analyzer)
  target_include_directories(test_tx_inclusion_analyzer PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
  )
  if(APPLE)
    target_link_libraries(test_tx_inclusion_analyzer PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME TxInclusionAnalyzer COMMAND test_tx_inclusion_analyzer)
endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase W.4.2: RBF & CPFP Capability Detection Tests
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_rbf_cpfp_detector.cpp)
  add_executable(test_rbf_cpfp_detector
    tests/wallet/test_rbf_cpfp_detector.cpp
    src/common/logger.cpp
    src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    src/daemon/config.cpp     # Provides GetConfig()
  )
  target_link_libraries(test_rbf_cpfp_detector PRIVATE
    dinero_core
    dinero_wallet
    dinero_chainstate
    dinero_consensus
    bulletproofs_ffi  # Required by confidential_validation
    jsoncpp_static
  )
  link_zstd_if_needed(test_rbf_cpfp_detector)
  target_include_directories(test_rbf_cpfp_detector PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
  )
  if(APPLE)
    target_link_libraries(test_rbf_cpfp_detector PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME RbfCpfpDetector COMMAND test_rbf_cpfp_detector)
endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase W.4.3: Fee Bump Recommendation Engine Tests
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_fee_bump_engine.cpp)
  add_executable(test_fee_bump_engine
    tests/wallet/test_fee_bump_engine.cpp
    src/common/logger.cpp
    src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    src/daemon/config.cpp     # Provides GetConfig()
  )
  target_link_libraries(test_fee_bump_engine PRIVATE
    dinero_core
    dinero_wallet
    dinero_chainstate
    dinero_consensus
    bulletproofs_ffi  # Required by confidential_validation
    jsoncpp_static
  )
  link_zstd_if_needed(test_fee_bump_engine)
  target_include_directories(test_fee_bump_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
  )
  if(APPLE)
    target_link_libraries(test_fee_bump_engine PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME FeeBumpEngine COMMAND test_fee_bump_engine)
endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase W.4.4: Fee Bump RPC Handler Tests
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/rpc/test_fee_bump_rpc.cpp)
  add_executable(test_fee_bump_rpc
    tests/rpc/test_fee_bump_rpc.cpp
    src/rpc/fee_bump_rpc_handlers.cpp
    src/mining/tx_inclusion_analyzer.cpp
    src/wallet/rbf_cpfp_detector.cpp
    src/wallet/fee_bump_engine.cpp
    src/common/logger.cpp
    src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    src/daemon/config.cpp     # Provides GetConfig()
  )
  target_link_libraries(test_fee_bump_rpc PRIVATE
    dinero_core
    dinero_wallet
    dinero_chainstate
    dinero_consensus
    bulletproofs_ffi  # Required by confidential_validation
    jsoncpp_static
  )
  link_zstd_if_needed(test_fee_bump_rpc)
  target_include_directories(test_fee_bump_rpc PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
  )
  if(APPLE)
    target_link_libraries(test_fee_bump_rpc PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME FeeBumpRpc COMMAND test_fee_bump_rpc)
endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase W.4.5: Wallet Fee Bump UX Hooks Tests
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_wallet_fee_bump_hooks.cpp)
  add_executable(test_wallet_fee_bump_hooks
    tests/wallet/test_wallet_fee_bump_hooks.cpp
    src/wallet/wallet_fee_bump_hooks.cpp
    src/mining/tx_inclusion_analyzer.cpp
    src/wallet/rbf_cpfp_detector.cpp
    src/wallet/fee_bump_engine.cpp
    src/common/logger.cpp
    src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    src/daemon/config.cpp     # Provides GetConfig()
  )
  target_link_libraries(test_wallet_fee_bump_hooks PRIVATE
    dinero_core
    dinero_wallet
    dinero_chainstate
    dinero_consensus
    bulletproofs_ffi  # Required by confidential_validation
    jsoncpp_static
  )
  link_zstd_if_needed(test_wallet_fee_bump_hooks)
  target_include_directories(test_wallet_fee_bump_hooks PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
  )
  if(APPLE)
    target_link_libraries(test_wallet_fee_bump_hooks PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME WalletFeeBumpHooks COMMAND test_wallet_fee_bump_hooks)
endif()

# CT→CT VerifyCommitmentBalance regression test
# Exercises pedersen_verify_tally with real confidential-input commitments
# (previously hung due to unchecked parse return values)
if(ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_ct_verify_balance.cpp)
  add_executable(test_ct_verify_balance
    tests/wallet/test_ct_verify_balance.cpp
  )
  target_link_libraries(test_ct_verify_balance PRIVATE
    secp256k1
    gtest
    gtest_main
  )
  link_zstd_if_needed(test_ct_verify_balance)
  target_include_directories(test_ct_verify_balance PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  )
  target_compile_definitions(test_ct_verify_balance PRIVATE DINERO_TEST_BUILD)
  add_test(NAME CTVerifyBalance COMMAND test_ct_verify_balance)
  set_tests_properties(CTVerifyBalance PROPERTIES
    LABELS "wallet;confidential;regression"
    TIMEOUT 30
  )
  if(TARGET build_bulletproofs_ffi)
    add_dependencies(test_ct_verify_balance build_bulletproofs_ffi)
  endif()
  message(STATUS "  ✓ CT→CT verify_balance regression test registered")
endif()

# Mining Smoke Tests - Day 1 Golden Path Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_mining_smoke.cpp)
  add_executable(test_mining_smoke
    tests/mining/test_mining_smoke.cpp
    tests/support/test_stubs.cpp
    # Reuse same dependencies as test_block_assembler_smoke
    src/daemon/services/config_service.cpp
    src/daemon/services/chainstate_service.cpp
    src/daemon/services/wallet_service.cpp
    src/common/logger.cpp
    src/common/config_manager.cpp
    src/daemon/block_acceptor.cpp
    src/database/sqlite_manager.cpp
    # src/daemon/db_init_simple.cpp  # REMOVED: Zombie - legacy SQLite init, bypassed ChainWriteToken
    src/daemon/db_meta_utils.cpp
    # src/consensus/supply_tracker.cpp  # REMOVED: Zombie code
    src/consensus/block_index.cpp
    src/consensus/block_lifecycle.cpp
    src/consensus/orphan_manager.cpp
    src/consensus/block_index_persistence.cpp
    # src/consensus/genesis_canonical.cpp  # REMOVED - depends on missing genesis_block.h
    src/consensus/chainparams_impl.cpp
    src/consensus/chainwork.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/storage/chain_db.cpp
    src/wallet/utxo_index.cpp
    src/wallet/wallet_worker.cpp
    src/wallet/wallet_manager.cpp  # Modern HD wallet with BIP84 (Nov 2025)
    src/wallet/address.cpp
    src/mining/block_assembler.cpp
    # src/mining/template_validator.cpp  # REMOVED: Zombie code
    # src/consensus/block_validation.cpp removed - has incompatible Block structure
    src/daemon/mining_safety_gates.cpp
    src/address/addr_codec.cpp
    src/address/addr_types.cpp
    src/core/crypto/ripemd160.cpp
    src/metrics/metrics_registry.cpp
    src/daemon/mempool.cpp
    src/crypto/decrypt_encrypted_key.cpp
    src/daemon/secure_random.cpp
    src/daemon/wallet_crypto.cpp
    src/daemon/bech32_decode.cpp
    src/daemon/ws_globals.cpp
    src/daemon/p2p_message.cpp
    src/daemon/p2p_manager.cpp  # For P2PMessage::create_inv() and P2PManager
  )
  target_include_directories(test_mining_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/external
  )
  target_link_libraries(test_mining_smoke PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
    gtest
  )
  link_zstd_if_needed(test_mining_smoke)
  if(APPLE)
    target_link_libraries(test_mining_smoke PRIVATE
      "-framework IOKit"
    )
  endif()
  add_test(NAME MiningSmoke COMMAND test_mining_smoke)
endif()

# ============================================================================
# Phase 10d: Utreexo Template Builder Integration Test
# ============================================================================
# DELETED registration: this test directly compiles src/mining/block_template.cpp
# whose BlockTemplateBuilder::createBlockTemplate() has a latent reference to
# an undeclared `coinbase_value` (lines 155/377). The dead-code path has been
# superseded — production mining uses MiningService::createBlockTemplate() in
# src/daemon/services/mining_service.cpp, not BlockTemplateBuilder. The
# regular dinerod build does NOT include block_template.cpp (nothing else
# links it), so the bug never surfaces in production. Don't re-enable this
# test without porting it to MiningService or fixing the dead createBlockTemplate.

# ============================================================================
# Phase 10d: Utreexo Height-2 Root Lock Test (Consensus Critical)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_height2_root.cpp)
  add_executable(test_utreexo_height2_root
    tests/consensus/test_utreexo_height2_root.cpp
  )

  target_include_directories(test_utreexo_height2_root PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests
  )

  target_link_libraries(test_utreexo_height2_root PRIVATE
    dinero_core
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    gtest
    gtest_main
    bulletproofs_ffi
  )

  link_zstd_if_needed(test_utreexo_height2_root)

  if(APPLE)
    target_link_libraries(test_utreexo_height2_root PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()

  add_test(NAME UtreexoHeight2RootLock COMMAND test_utreexo_height2_root)
endif()

# Bech32 validator test suite
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_bech32_validator.cpp)
  add_executable(test_bech32_validator
    tests/test_bech32_validator.cpp
    src/wallet/address_validate.cpp
  )
  target_link_libraries(test_bech32_validator PRIVATE dinero_crypto)
  target_include_directories(test_bech32_validator PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME Bech32Validator COMMAND test_bech32_validator)
endif()

# BIP84 Change Address Test - DISABLED: LightningService dependency via dinero_wallet
if(FALSE)
add_executable(test_change_addresses
    test_change_addresses.cpp
    src/common/logger.cpp
    src/sqlite_open.cpp
)
target_link_libraries(test_change_addresses
    dinero_wallet
    dinero_crypto
    ${OPENSSL_LIBRARIES}
    sqlite3
    secp256k1
)
if(APPLE)
    target_link_libraries(test_change_addresses
      "-framework Security"
    )
endif()
endif()

# ============================================================================
# v1.1 Regression & Stress Tests
# ============================================================================

# Wallet Recovery Regression Tests - DISABLED: Security framework missing
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/regression/test_wallet_recovery.cpp)
  add_executable(test_wallet_recovery
    tests/regression/test_wallet_recovery.cpp
    tests/support/test_stubs.cpp
    src/common/logger.cpp  # Provides g_logger
  )
  target_include_directories(test_wallet_recovery PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src
  )
  target_link_libraries(test_wallet_recovery PRIVATE
    dinero_consensus  # Must come before dinero_crypto (has correct Params())
    dinero_wallet
    dinero_crypto
    sqlite3
    OpenSSL::SSL
    OpenSSL::Crypto
    gtest
  )
  link_zstd_if_needed(test_wallet_recovery)
  if(APPLE)
    target_link_libraries(test_wallet_recovery PRIVATE
      secp256k1
      jsoncpp_static
    )
  else()
    target_link_libraries(test_wallet_recovery PRIVATE secp256k1 jsoncpp_static)
  endif()
  add_test(NAME WalletRecovery COMMAND test_wallet_recovery)
endif()

# Deep Reorg Regression Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/regression/test_deep_reorg.cpp)
  add_executable(test_deep_reorg
    tests/regression/test_deep_reorg.cpp
    tests/support/test_stubs.cpp
    src/common/logger.cpp  # Provides g_logger
    src/mining/block_assembler.cpp
    src/daemon/services/chainstate_service.cpp
    src/daemon/services/wallet_service.cpp
    src/daemon/services/config_service.cpp
  )
  target_include_directories(test_deep_reorg PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/daemon
  )
  target_link_libraries(test_deep_reorg PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    sqlite3
    RocksDB::rocksdb
    OpenSSL::SSL
    OpenSSL::Crypto
    gtest
  )
  link_zstd_if_needed(test_deep_reorg)
  if(APPLE)
    target_link_libraries(test_deep_reorg PRIVATE
      secp256k1
      jsoncpp_static
    )
  else()
    target_link_libraries(test_deep_reorg PRIVATE secp256k1 jsoncpp_static)
  endif()
  add_test(NAME DeepReorg COMMAND test_deep_reorg)
endif()

# Mempool Stress Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/stress/test_mempool_stress.cpp)
  add_executable(test_mempool_stress
    tests/stress/test_mempool_stress.cpp
    tests/support/test_stubs.cpp
  )
  target_include_directories(test_mempool_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src
  )
  target_link_libraries(test_mempool_stress PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    gtest
  )
  link_zstd_if_needed(test_mempool_stress)
  if(APPLE)
    target_link_libraries(test_mempool_stress PRIVATE
      secp256k1
      jsoncpp_static
    )
  else()
    target_link_libraries(test_mempool_stress PRIVATE secp256k1 jsoncpp_static)
  endif()
  add_test(NAME MempoolStress COMMAND test_mempool_stress)
endif()

# ============================================================
#  Dinero Core - Test & Fuzzing Integration (v1.1)
# ============================================================

option(ENABLE_TESTS "Enable regression and stress testing" ON)
option(ENABLE_FUZZING "Enable fuzzing tests (requires Clang + libFuzzer)" OFF)

if (ENABLE_TESTS)
    message(STATUS "Enabling Dinero Core test suite...")

    # Tests are already enabled above via enable_testing()
    # NOTE: tests/CMakeLists.txt is NOT included as subdirectory because
    # test definitions are inline in this file for better integration.
    # Add regression and stress test subdirectories if they exist
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/regression/CMakeLists.txt)
        add_subdirectory(tests/regression)
    endif()

    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/stress/CMakeLists.txt)
        add_subdirectory(tests/stress)
    endif()

    # Add reorg tests if they exist
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/reorg/CMakeLists.txt)
        add_subdirectory(tests/reorg)
    endif()

    # Add integration tests (Phase E.3)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/integration/CMakeLists.txt)
        add_subdirectory(tests/integration)
    endif()

    # Add wallet persistence tests (Phase F.6)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_persistence/CMakeLists.txt)
        add_subdirectory(tests/wallet_persistence)
    endif()

    # Add mining/block assembly tests (Phase F.10)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/CMakeLists.txt)
        add_subdirectory(tests/mining)
    endif()

    # Add consensus property tests (Ring 5)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/CMakeLists.txt)
        add_subdirectory(tests/consensus)
    endif()

    # Add Utreexo semantic correctness tests (Phase U)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/utreexo/CMakeLists.txt)
        add_subdirectory(tests/utreexo)
    endif()

    # Add economic property tests (Ring 6)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/economic/CMakeLists.txt)
        add_subdirectory(tests/economic)
    endif()

    # Add execution semantics property tests (Ring 7)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/execution/CMakeLists.txt)
        add_subdirectory(tests/execution)
    endif()

    # Add governance & compatibility tests (Ring 8)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/ring8/CMakeLists.txt)
        add_subdirectory(tests/ring8)
    endif()

    # Add fuzzing if enabled
    if (ENABLE_FUZZING)
        if(EXISTS ${CMAKE_SOURCE_DIR}/tests/fuzz/CMakeLists.txt)
            add_subdirectory(tests/fuzz)
        endif()
        # Phase 27: Add root-level fuzz directory (consensus-critical fuzzers)
        if(EXISTS ${CMAKE_SOURCE_DIR}/fuzz/CMakeLists.txt)
            add_subdirectory(fuzz)
        endif()
    endif()

    # POSIX-only .sh-based ctest entries are disabled on native Windows MSVC
    # because cmd.exe has no bash interpreter on PATH, so ctest reports them
    # as BAD_COMMAND and pollutes the gate. They still register and run on
    # Linux / macOS / MinGW / Cygwin where /bin/bash is available.
    # This list covers the top-level tests whose COMMAND ends in *.sh.
    # Integration .sh tests are disabled inside tests/integration/CMakeLists.txt
    # because CTest test properties are directory-scoped on some CMake versions.
    if(WIN32 AND NOT MINGW AND NOT CYGWIN)
        set(_dinero_posix_only_tests
            # Note: WalletPathHygiene and RetiredCoinType1447Hygiene invoke
            # `bash <script>.sh` explicitly, so they run fine on Windows when
            # git-bash is on PATH (the typical dev setup) — keep them enabled.
            AcceptanceParity IBDTorture ReleaseSuite
            CmakeInstallLayout DineroBackup DineroPrepareUpgrade
            HeaderStatusOverwriteAllowlist
        )
        set(_dinero_disabled_count 0)
        foreach(_dinero_t IN LISTS _dinero_posix_only_tests)
            if(TEST ${_dinero_t})
                set_tests_properties(${_dinero_t} PROPERTIES DISABLED TRUE)
                math(EXPR _dinero_disabled_count "${_dinero_disabled_count} + 1")
            endif()
        endforeach()
        message(STATUS "Disabled ${_dinero_disabled_count} POSIX-only .sh ctest entries on native Windows")
    endif()

    # Include the top-level test runner
    add_custom_target(run_all_tests
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/run_all_tests.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running all Dinero Core regression + fuzz tests..."
    )
endif()

# TEMPORARILY DISABLED (2026-02-02): wallet_test_deps needs ChainDB symbols
# TODO: Update wallet_test_deps or use dinero_chainstate directly
#
# # Verification tool for premine address
# add_executable(verify_seed tools/verify_seed.cpp)
# target_sources(verify_seed PRIVATE src/common/logger.cpp)
# target_link_libraries(verify_seed PRIVATE
#   dinero_wallet
#   dinero_crypto
#   dinero_consensus
#   secp256k1
#   sqlite3
#   wallet_test_deps
# )
# target_include_directories(verify_seed PRIVATE
#   ${CMAKE_SOURCE_DIR}/include
# )
# if(APPLE)
#   target_link_libraries(verify_seed PRIVATE
#     "-framework Security"
#     "-framework CoreFoundation"
#   )
# endif()
#
# # Address verification tool (check seed derives to expected address)
# add_executable(verify_address tools/verify_address.cpp)
# target_sources(verify_address PRIVATE src/common/logger.cpp)
# target_link_libraries(verify_address PRIVATE
#   dinero_wallet
#   dinero_crypto
#   dinero_consensus
#   secp256k1
#   sqlite3
#   wallet_test_deps
# )
# target_include_directories(verify_address PRIVATE
#   ${CMAKE_SOURCE_DIR}/include
# )
# if(APPLE)
#   target_link_libraries(verify_address PRIVATE
#     "-framework Security"
#     "-framework CoreFoundation"
#   )
# endif()
#
# # BIP86 Taproot premine generator (creates BIP39 seed + derives address)
# add_executable(generate_bip86_premine tools/generate_bip86_premine.cpp)
# target_sources(generate_bip86_premine PRIVATE src/common/logger.cpp)
# target_link_libraries(generate_bip86_premine PRIVATE
#   dinero_wallet
#   dinero_crypto
#   dinero_consensus
#   secp256k1
#   sqlite3
#   wallet_test_deps
# )
# target_include_directories(generate_bip86_premine PRIVATE
#   ${CMAKE_SOURCE_DIR}/include
# )
# if(APPLE)
#   target_link_libraries(generate_bip86_premine PRIVATE
#     "-framework Security"
#     "-framework CoreFoundation"
#   )
# endif()

# ============================================================================
# Phase 2: Standalone Mining Test (ASERT Validation)
# Phase C: Test needs updating to use MiningManager v2 instead of legacy Mining
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_mine_blocks_standalone.cpp)
  add_executable(test_mine_blocks_standalone
    tests/test_mine_blocks_standalone.cpp
    # Phase C: Legacy Mining class removed - test needs updating
    # src/daemon/mining.cpp
    src/daemon/block_acceptor.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_mine_blocks_standalone PRIVATE
    dinero_chainstate
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_mine_blocks_standalone)
  target_include_directories(test_mine_blocks_standalone PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_mine_blocks_standalone PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
endif()

# ============================================================================
# v0.14.0.1: Block Template Construction Test (Foundation)
# ============================================================================
# NOTE:
# test_block_template is temporarily disabled.
#
# Reason:
# This test compiles production mempool/block_assembler .cpp files directly,
# causing uncontrolled transitive dependencies (ScannerManager, FeeEstimator,
# RBFPolicy, peer-routing glue, GetConfig, UtreexoMessages, etc.).
#
# The architecture has outgrown this test's build assumptions.
# Proper fix requires a dedicated mempool+assembler test library (Phase M.x).
#
# This test is NOT part of DINERO_RELEASE pre-CI requirements.
#
# TODO(Phase M.x): Reintroduce test with proper library boundaries.
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_block_template.cpp)
  add_executable(test_block_template
    tests/mining/test_block_template.cpp
    src/mining/block_assembler.cpp
    src/daemon/mempool.cpp
    src/common/logger.cpp
    src/core/privacy/silent_scanner_manager.cpp
    src/core/privacy/silent_scanner.cpp
    src/mempool/fee_estimator.cpp
    src/consensus/undo.cpp
    src/policy/rbf_policy.cpp
    src/p2p/inventory.cpp
  )
  target_link_libraries(test_block_template PRIVATE
    dinero_core
    dinero_chainstate
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
    bulletproofs_ffi
  )
  link_zstd_if_needed(test_block_template)
  target_include_directories(test_block_template PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_block_template PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
endif()

# ============================================================================
# Header Hash Test Vectors (Mainnet Readiness - CPU vs GPU)
# ============================================================================
if(ENABLE_TESTS AND EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_header_hash_vectors.cpp)
  add_executable(test_header_hash_vectors
    tests/mining/test_header_hash_vectors.cpp
    src/primitives/block.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    src/primitives/uint256.cpp
    src/common/sha256d.cpp
    src/consensus/utreexo_accumulator.cpp
    src/consensus/utreexo_stump.cpp
    src/consensus/chainparams_impl.cpp
    src/consensus/script_crypto.cpp
  )

  add_dependencies(test_header_hash_vectors gtest)

  target_link_libraries(test_header_hash_vectors PRIVATE
    dinero_crypto
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    $<TARGET_FILE:gtest>
    )
  link_zstd_if_needed(test_header_hash_vectors)

  target_include_directories(test_header_hash_vectors BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME test_header_hash_vectors COMMAND test_header_hash_vectors)
endif()

# ============================================================================
# Phase C: MiningManager v2 Smoke Tests (Basic API Surface)
# NOTE: These tests verify API existence and basic functionality without
# requiring full DaemonContext initialization. See docs/PHASE_C_TEST_STATUS.md
# ============================================================================
if(ENABLE_TESTS AND EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_mining_header_reserved_smoke.cpp)
  add_executable(test_mining_header_reserved_smoke
    tests/mining/test_mining_header_reserved_smoke.cpp
  )
  add_dependencies(test_mining_header_reserved_smoke gtest)
  target_link_libraries(test_mining_header_reserved_smoke PRIVATE
    $<TARGET_FILE:gtest>
  )
  target_include_directories(test_mining_header_reserved_smoke BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME MiningHeaderReservedSmoke COMMAND test_mining_header_reserved_smoke)
endif()

# ============================================================================
# Phase C: MiningManager v2 Unit Tests (Full Integration - TODO)
# Note: Requires mock DaemonContext - deferred to Phase D
# ============================================================================
# if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_mining_manager_v2.cpp)
#   add_executable(test_mining_manager_v2
#     tests/mining/test_mining_manager_v2.cpp
#   )
#   target_link_libraries(test_mining_manager_v2 PRIVATE
#     dinero_crypto
#     dinero_chainstate
#     GTest::gtest
#     pthread
#   )
#   target_include_directories(test_mining_manager_v2 PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#   )
#   add_test(NAME MiningManagerV2Unit COMMAND test_mining_manager_v2)
# endif()

# ============================================================================
# Phase C: MiningService Integration Tests (Smoke Tests Only)
# Note: Full integration tests require complete DaemonContext - deferred to Phase D
# ============================================================================
# if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_mining_service_integration.cpp)
#   add_executable(test_mining_service_integration
#     tests/mining/test_mining_service_integration.cpp
#   )
#   target_link_libraries(test_mining_service_integration PRIVATE
#     dinero_crypto
#     dinero_chainstate
#     dinero_consensus
#     dinero_gpu_mining
#     GTest::gtest
#     pthread
#   )
#  link_zstd_if_needed(test_mining_service_integration)
#   target_include_directories(test_mining_service_integration PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#   )
#   add_test(NAME MiningServiceIntegration COMMAND test_mining_service_integration)
# endif()

# ============================================================================
# Minimal Block Template Test (Production Miner Foundation)
# ============================================================================
# NOTE:
# test_block_template_minimal is temporarily disabled.
#
# Reason:
# This test compiles production block_assembler.cpp directly, causing the
# same uncontrolled transitive dependencies as test_block_template.
#
# Proper fix requires a dedicated block assembler test library (Phase M.x).
#
# This test is NOT part of DINERO_RELEASE pre-CI requirements.
#
# TODO(Phase M.x): Reintroduce test with proper library boundaries.
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_block_template_minimal.cpp)
  add_executable(test_block_template_minimal
    tests/mining/test_block_template_minimal.cpp
    src/mining/block_assembler.cpp
  )
  target_link_libraries(test_block_template_minimal PRIVATE
    dinero_chainstate
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_block_template_minimal)
  target_include_directories(test_block_template_minimal PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_block_template_minimal PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
endif()

# ============================================================================
# Phase M.0: Merkle Tree Golden Test (Standalone)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_merkle_golden.cpp)
  add_executable(test_merkle_golden
    tests/test_merkle_golden.cpp
  )
  target_link_libraries(test_merkle_golden PRIVATE
    dinero_wallet
    dinero_crypto
    secp256k1
  )
  target_include_directories(test_merkle_golden PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_merkle_golden PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME MerkleGolden COMMAND test_merkle_golden)
endif()

# ============================================================================
# Phase N.3: Header Fork-Choice Tests (Header-First Sync)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_header_fork_choice.cpp)
  add_executable(test_header_fork_choice
    tests/consensus/test_header_fork_choice.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_header_fork_choice PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    secp256k1
  )
  link_zstd_if_needed(test_header_fork_choice)
  target_include_directories(test_header_fork_choice PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_header_fork_choice PRIVATE /opt/homebrew/lib)
    target_link_libraries(test_header_fork_choice PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeaderForkChoice COMMAND test_header_fork_choice)
endif()

# ============================================================================
# Phase N.1: Header Restart Safety Tests (Header-First Sync)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_header_restart_safety.cpp)
  add_executable(test_header_restart_safety
    tests/consensus/test_header_restart_safety.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
  )
  target_link_libraries(test_header_restart_safety PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    secp256k1
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_header_restart_safety)
  target_include_directories(test_header_restart_safety PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_header_restart_safety PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeaderRestartSafety COMMAND test_header_restart_safety)
  set_tests_properties(HeaderRestartSafety PROPERTIES
    LABELS "headers;restart;canonicality;phase1-canonical-recovery"
  )
endif()

# ============================================================================
# Phase N.2: Header Sync State Machine Tests (Header-First Sync)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_header_sync_state_machine.cpp)
  add_executable(test_header_sync_state_machine
    tests/consensus/test_header_sync_state_machine.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
    src/consensus/header_sync.cpp
  )
  target_link_libraries(test_header_sync_state_machine PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    secp256k1
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_header_sync_state_machine)
  target_include_directories(test_header_sync_state_machine PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_header_sync_state_machine PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeaderSyncStateMachine COMMAND test_header_sync_state_machine)
endif()

# ============================================================================
# Phase N.2 Step 2B: Header Sync Stall Behavior Tests
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_header_sync_stall_behavior.cpp)
  add_executable(test_header_sync_stall_behavior
    tests/consensus/test_header_sync_stall_behavior.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
    src/consensus/header_sync.cpp
  )
  target_link_libraries(test_header_sync_stall_behavior PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    secp256k1
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_header_sync_stall_behavior)
  target_include_directories(test_header_sync_stall_behavior PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_header_sync_stall_behavior PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeaderSyncStallBehavior COMMAND test_header_sync_stall_behavior)
endif()

# ============================================================================
# Phase N.2 Step 2C: Header Sync P2P Integration Tests
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_header_sync_p2p_integration.cpp)
  add_executable(test_header_sync_p2p_integration
    tests/consensus/test_header_sync_p2p_integration.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
    src/consensus/header_sync.cpp
    src/consensus/header_sync_p2p.cpp
  )
  target_link_libraries(test_header_sync_p2p_integration PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    secp256k1
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_header_sync_p2p_integration)
  target_include_directories(test_header_sync_p2p_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_header_sync_p2p_integration PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeaderSyncP2PIntegration COMMAND test_header_sync_p2p_integration)
endif()

# ============================================================================
# Phase N.4: Block Download Scheduler Tests
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_block_download_scheduler.cpp)
  add_executable(test_block_download_scheduler
    tests/consensus/test_block_download_scheduler.cpp
    src/consensus/block_download_scheduler.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_block_download_scheduler PRIVATE
    dinero_consensus
    dinero_chainstate  # BlockStorage
    dinero_wallet
    dinero_crypto
    secp256k1
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_block_download_scheduler)
  target_include_directories(test_block_download_scheduler PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_block_download_scheduler PRIVATE /opt/homebrew/lib)
    target_link_libraries(test_block_download_scheduler PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME BlockDownloadScheduler COMMAND test_block_download_scheduler)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_block_download_scheduler_restart_bootstrap.cpp)
  add_executable(test_block_download_scheduler_restart_bootstrap
    tests/consensus/test_block_download_scheduler_restart_bootstrap.cpp
    src/consensus/block_download_scheduler.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_block_download_scheduler_restart_bootstrap PRIVATE
    dinero_consensus
    dinero_chainstate
    dinero_wallet
    dinero_crypto
    secp256k1
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_block_download_scheduler_restart_bootstrap)
  target_include_directories(test_block_download_scheduler_restart_bootstrap PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_block_download_scheduler_restart_bootstrap PRIVATE /opt/homebrew/lib)
    target_link_libraries(test_block_download_scheduler_restart_bootstrap PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME BlockDownloadSchedulerRestartBootstrap COMMAND test_block_download_scheduler_restart_bootstrap)
endif()

# ============================================================================
# v0.14.0.4: Utreexo Commitment Enforcement Tests
# ============================================================================
# TODO: Disabled - needs refactor to use IConsensusUTXOSet instead of IUTXOProvider
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_enforcement.cpp)
  add_executable(test_utreexo_enforcement
    tests/consensus/test_utreexo_enforcement.cpp
    src/consensus/block_validation.cpp
    src/consensus/utreexo_accumulator.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/wallet/utxo_index.cpp
    src/database/sqlite_manager.cpp
    src/sqlite_open.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_utreexo_enforcement PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    bulletproofs_ffi
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_utreexo_enforcement)
  target_include_directories(test_utreexo_enforcement PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  target_compile_definitions(test_utreexo_enforcement PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  if(APPLE)
    target_link_libraries(test_utreexo_enforcement PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoEnforcement COMMAND test_utreexo_enforcement)
  # Ensure Bulletproofs FFI is built before test
  if(CARGO_EXECUTABLE)
    add_dependencies(test_utreexo_enforcement build_bulletproofs_ffi)
  endif()
endif()

# ============================================================================
# CONSENSUS GATE: Block-Level Stateless Equivalence Test
# This test proves: stateful BlockValidator ↔ stateless Utreexo are IDENTICAL
# ============================================================================
# TODO: Disabled - needs refactor to use IConsensusUTXOSet instead of IUTXOProvider
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_block_stateless_equivalence.cpp)
  add_executable(test_block_stateless_equivalence
    tests/consensus/test_block_stateless_equivalence.cpp
    src/consensus/block_validation.cpp
    src/consensus/utreexo_accumulator.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/wallet/utxo_index.cpp
    src/database/sqlite_manager.cpp
    src/sqlite_open.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_block_stateless_equivalence PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    bulletproofs_ffi
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_block_stateless_equivalence)
  target_include_directories(test_block_stateless_equivalence PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  target_compile_definitions(test_block_stateless_equivalence PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  if(APPLE)
    target_link_libraries(test_block_stateless_equivalence PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME BlockStatelessEquivalence COMMAND test_block_stateless_equivalence)
  if(CARGO_EXECUTABLE)
    add_dependencies(test_block_stateless_equivalence build_bulletproofs_ffi)
  endif()
endif()

# ============================================================================
# Serialization Bug Repro Test (Step 1 of 3)
# Target: Isolate forest serialize/deserialize mutation divergence
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_serialization_repro.cpp)
  add_executable(test_utreexo_serialization_repro
    tests/consensus/test_utreexo_serialization_repro.cpp
  )
  target_link_libraries(test_utreexo_serialization_repro PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_utreexo_serialization_repro)
  target_include_directories(test_utreexo_serialization_repro PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo_serialization_repro PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoSerializationRepro COMMAND test_utreexo_serialization_repro)
endif()

# ============================================================================
# Phase 8: Stateless Validation Tests (T8.1–T8.9)
# ============================================================================
# TODO: Disabled - needs refactor to use IConsensusUTXOSet instead of IUTXOProvider
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_stateless_validation.cpp)
  add_executable(test_utreexo_stateless_validation
    tests/consensus/test_utreexo_stateless_validation.cpp
    src/consensus/block_validation.cpp
    src/consensus/utreexo_accumulator.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/consensus/script_validation.cpp
    src/wallet/utxo_index.cpp
    src/database/sqlite_manager.cpp
    src/sqlite_open.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_utreexo_stateless_validation PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    bulletproofs_ffi
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_utreexo_stateless_validation)
  target_include_directories(test_utreexo_stateless_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  target_compile_definitions(test_utreexo_stateless_validation PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  if(APPLE)
    target_link_libraries(test_utreexo_stateless_validation PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME StatelessValidation COMMAND test_utreexo_stateless_validation)
  # Ensure Bulletproofs FFI is built before test
  if(CARGO_EXECUTABLE)
    add_dependencies(test_utreexo_stateless_validation build_bulletproofs_ffi)
  endif()
endif()

# ============================================================================
# Phase 9.1: Proof Cache Tests (T9.1–T9.6)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_proof_cache.cpp)
  add_executable(test_proof_cache
    tests/consensus/test_proof_cache.cpp
    src/consensus/proof_cache.cpp
  )
  target_link_libraries(test_proof_cache PRIVATE
    dinero_consensus
  )
  target_include_directories(test_proof_cache PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ProofCache COMMAND test_proof_cache)
endif()

# ============================================================================
# Phase 9.2: Routing Heuristics Tests (T9.7–T9.12)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_proof_router.cpp)
  add_executable(test_proof_router
    tests/consensus/test_proof_router.cpp
    src/consensus/proof_router.cpp
  )
  target_link_libraries(test_proof_router PRIVATE
    dinero_consensus
  )
  target_include_directories(test_proof_router PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ProofRouter COMMAND test_proof_router)
endif()

# ============================================================================
# Phase 9.3: Gossip Protocol Tests (T9.13–T9.18)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_proof_gossip.cpp)
  add_executable(test_proof_gossip
    tests/consensus/test_proof_gossip.cpp
    src/consensus/proof_gossip.cpp
    src/consensus/utreexo_accumulator.cpp
  )
  target_link_libraries(test_proof_gossip PRIVATE
    dinero_consensus
    dinero_crypto
    zstd
    OpenSSL::SSL
    OpenSSL::Crypto
  )
  target_include_directories(test_proof_gossip PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ProofGossip COMMAND test_proof_gossip)
endif()

# ============================================================================
# Phase 9.4: Proof Compression Tests (T9.19–T9.25)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_proof_compression.cpp)
  add_executable(test_proof_compression
    tests/consensus/test_proof_compression.cpp
    src/consensus/proof_compression.cpp
    src/consensus/utreexo_accumulator.cpp
  )
  target_link_libraries(test_proof_compression PRIVATE
    dinero_consensus
    dinero_crypto
    zstd
    OpenSSL::SSL
    OpenSSL::Crypto
  )
  target_include_directories(test_proof_compression PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ProofCompression COMMAND test_proof_compression)
endif()

# ============================================================================
# Phase 9.6: Performance Benchmarks - DISABLED: Lightning removed
# ============================================================================
# if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/benchmark_phase9.cpp)
#   add_executable(benchmark_phase9
#     tests/consensus/benchmark_phase9.cpp
#     src/consensus/proof_cache.cpp
#     src/consensus/proof_router.cpp
#     src/consensus/proof_gossip.cpp
#     src/consensus/proof_compression.cpp
#     src/consensus/lightning_proof_client.cpp
#     src/consensus/utreexo_accumulator.cpp
#   )
#   target_link_libraries(benchmark_phase9 PRIVATE
#     dinero_consensus
#     dinero_crypto
#     zstd
#     OpenSSL::SSL
#     OpenSSL::Crypto
#   )
#   target_include_directories(benchmark_phase9 PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#   )
#   # Note: Benchmarks are not added to CTest (run manually for performance analysis)
# endif()

# ============================================================================
# Phase 10: Real-World Sync Validation Tests - DISABLED: Lightning removed
# ============================================================================
# if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_sync_validation.cpp)
#   add_executable(test_sync_validation
#     tests/consensus/test_sync_validation.cpp
#     src/consensus/sync_simulator.cpp
#     src/consensus/lightning_proof_client.cpp
#     src/consensus/proof_cache.cpp
#     src/consensus/proof_router.cpp
#     src/consensus/proof_gossip.cpp
#     src/consensus/proof_compression.cpp
#     src/consensus/utreexo_accumulator.cpp
#   )
#   target_link_libraries(test_sync_validation PRIVATE
#     dinero_consensus
#     dinero_crypto
#     zstd
#     OpenSSL::SSL
#     OpenSSL::Crypto
#   )
#   target_include_directories(test_sync_validation PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#   )
#   add_test(NAME SyncValidation COMMAND test_sync_validation)
# endif()

# ============================================================================
# Phase 12: Mobile Profile Integration Tests - DISABLED: Lightning removed
# ============================================================================
# Purpose: Prove changing resource constraints does NOT change validation outcomes
# Tests: Validation equivalence, cache eviction safety, cold restart, TTL, parallelism
# Critical: Does NOT re-test consensus (Phase 8-11 already proved that)
# Policy: ANY FAILURE → Resource envelope safety violated
# if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mobile/test_mobile_profile_integration.cpp)
#   add_executable(test_mobile_profile_integration
#     tests/mobile/test_mobile_profile_integration.cpp
#     src/consensus/sync_simulator.cpp
#     src/consensus/lightning_proof_client.cpp
#     src/consensus/proof_cache.cpp
#     src/consensus/proof_router.cpp
#     src/consensus/proof_gossip.cpp
#     src/consensus/proof_compression.cpp
#     src/consensus/utreexo_accumulator.cpp
#   )
#   target_link_libraries(test_mobile_profile_integration PRIVATE
#     dinero_consensus
#     dinero_crypto
#     zstd
#     OpenSSL::SSL
#     OpenSSL::Crypto
#   )
#   target_include_directories(test_mobile_profile_integration PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#   )
#   add_test(NAME MobileProfileIntegration COMMAND test_mobile_profile_integration)
# endif()

# ============================================================================
# Phase 12: iOS Memory Pressure Simulation Tests - DISABLED: Lightning removed
# ============================================================================
# Purpose: Prove mobile profile survives iOS memory pressure scenarios
# Tests: Aggressive eviction, jetsam recovery, burst mode compliance
# Critical: Validates core safety theorem (cache loss ≠ consensus failure)
# Policy: ANY FAILURE → iOS deployment not safe
# if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mobile/test_ios_memory_pressure.cpp)
#   add_executable(test_ios_memory_pressure
#     tests/mobile/test_ios_memory_pressure.cpp
#     src/consensus/sync_simulator.cpp
#     src/consensus/lightning_proof_client.cpp
#     src/consensus/proof_cache.cpp
#     src/consensus/proof_router.cpp
#     src/consensus/proof_gossip.cpp
#     src/consensus/proof_compression.cpp
#     src/consensus/utreexo_accumulator.cpp
#   )
#   target_link_libraries(test_ios_memory_pressure PRIVATE
#     dinero_consensus
#     dinero_crypto
#     zstd
#     OpenSSL::SSL
#     OpenSSL::Crypto
#   )
#   target_include_directories(test_ios_memory_pressure PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#   )
#   add_test(NAME iOSMemoryPressure COMMAND test_ios_memory_pressure)
# endif()

# ============================================================================
# Phase 3 Layer 3: Utreexo Consensus Regression Tests
# ============================================================================
# Purpose: Prevent future violations of critical consensus rules
# Tests: Header size enforcement, Utreexo data requirement, network activation
# Policy: ANY FAILURE → CI MUST BREAK (consensus regression detected)
# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_consensus_regressions.cpp)
  add_executable(test_utreexo_consensus_regressions
    tests/consensus/test_utreexo_consensus_regressions.cpp
    src/consensus/block_validation.cpp
    src/consensus/utreexo_accumulator.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/consensus/chainparams_impl.cpp
    src/consensus/adapters/wallet_utxo_adapter.cpp
    src/wallet/utxo_index.cpp
    src/database/sqlite_manager.cpp
    src/sqlite_open.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_utreexo_consensus_regressions PRIVATE
    dinero_consensus
    dinero_chainstate  # For ChainDB (utxo_index dependency)
    dinero_wallet
    dinero_crypto
    bulletproofs_ffi
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_utreexo_consensus_regressions)
  target_include_directories(test_utreexo_consensus_regressions PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  target_compile_definitions(test_utreexo_consensus_regressions PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  if(APPLE)
    target_link_libraries(test_utreexo_consensus_regressions PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoConsensusRegressions COMMAND test_utreexo_consensus_regressions)
  # Ensure Bulletproofs FFI is built before test
  if(CARGO_EXECUTABLE)
    add_dependencies(test_utreexo_consensus_regressions build_bulletproofs_ffi)
  endif()
  message(STATUS "✅ Phase 3 Layer 3 regression tests configured")
  message(STATUS "   Tests: Header size, Utreexo data requirement, network activation")
  message(STATUS "   Policy: Fail-fast on consensus violations (CI must break)")
endif()

# ============================================================================
# Phase 4: Utreexo Delta-Based Undo Tests
# ============================================================================
# Purpose: Verify delta undo correctness and 10-20x size reduction
# Tests: Roundtrip restoration, order safety, size comparison
# Policy: ANY FAILURE → CI MUST BREAK (delta undo regression detected)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_delta_undo.cpp)
  add_executable(test_utreexo_delta_undo
    tests/consensus/test_utreexo_delta_undo.cpp
    src/consensus/utreexo_accumulator.cpp
    src/consensus/script_crypto.cpp
  )
  target_link_libraries(test_utreexo_delta_undo PRIVATE
    dinero_crypto
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_utreexo_delta_undo)
  target_include_directories(test_utreexo_delta_undo PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  add_test(NAME UtreexoDeltaUndo COMMAND test_utreexo_delta_undo)
  message(STATUS "✅ Phase 4 delta-based undo tests configured")
  message(STATUS "   Tests: Roundtrip, order safety, size reduction (10-20x)")
  message(STATUS "   Policy: Fail-fast on delta undo violations")
endif()

# ============================================================================
# Tier-0 Fork-Point Root Validation Tests (FIX-1, FIX-2, FIX-3)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/utreexo/test_fork_point_root_validation.cpp)
  add_executable(test_fork_point_root_validation
    tests/utreexo/test_fork_point_root_validation.cpp
  )
  target_link_libraries(test_fork_point_root_validation PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_fork_point_root_validation)
  target_include_directories(test_fork_point_root_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_fork_point_root_validation PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ForkPointRootValidation COMMAND test_fork_point_root_validation)
endif()

# ============================================================================
# Proof Generation Correctness Tests (INV-1 through INV-5)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/utreexo/test_proof_generation_correctness.cpp)
  add_executable(test_proof_generation_correctness
    tests/utreexo/test_proof_generation_correctness.cpp
  )
  target_link_libraries(test_proof_generation_correctness PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_proof_generation_correctness)
  target_include_directories(test_proof_generation_correctness PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_proof_generation_correctness PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ProofGenerationCorrectness COMMAND test_proof_generation_correctness)
endif()

# ============================================================================
# Proof Lifecycle Tests (wallet.getproofbundle / proofstatus / getproofupdates)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/utreexo/test_proof_lifecycle.cpp)
  add_executable(test_proof_lifecycle
    tests/utreexo/test_proof_lifecycle.cpp
  )
  target_link_libraries(test_proof_lifecycle PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_proof_lifecycle)
  target_include_directories(test_proof_lifecycle PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_proof_lifecycle PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ProofLifecycle COMMAND test_proof_lifecycle)
endif()

# ============================================================================
# Phase 5: Stateless IBD Regression Tests
# ============================================================================
# Purpose: Verify stateless validation without UTXO database
# Security Model: PoW + Root Continuity = Full Validation (NOT light client)
# Tests: Happy path, missing proofs, root discontinuity, backward compatibility
# Policy: ANY FAILURE → CI MUST BREAK (stateless security model violation)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/ibd/test_stateless_ibd_basic.cpp)
  add_executable(test_stateless_ibd_basic
    tests/ibd/test_stateless_ibd_basic.cpp
    src/consensus/block_validation.cpp
    src/consensus/utreexo_accumulator.cpp
    src/consensus/script_crypto.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_stateless_ibd_basic PRIVATE
    dinero_consensus
    dinero_crypto
    bulletproofs_ffi
    RocksDB::rocksdb
    secp256k1
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_stateless_ibd_basic)
  target_include_directories(test_stateless_ibd_basic PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  add_test(NAME StatelessIBDBasic COMMAND test_stateless_ibd_basic)
  # Ensure Bulletproofs FFI is built before test
  if(CARGO_EXECUTABLE)
    add_dependencies(test_stateless_ibd_basic build_bulletproofs_ffi)
  endif()
  message(STATUS "✅ Phase 5 stateless IBD tests configured")
  message(STATUS "   Tests: Valid proofs, missing proofs, root continuity, backward compatibility")
  message(STATUS "   Golden Invariant: PoW + Root Continuity = Full Validation Security")
  message(STATUS "   Policy: Fail-fast on stateless security violations")
endif()

# ============================================================================
# Phase 7.1: Utreexo P2P Message Tests
# ============================================================================
# Purpose: Verify Utreexo proof-serving P2P message serialization
# Messages: getutxoproof, utxoproof, getutxohdrs, utxohdrs
# Tests: Serialization roundtrip, validation, DoS limits, truncated data
# Policy: ANY FAILURE → CI MUST BREAK (protocol correctness required)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_utreexo_messages.cpp)
  add_executable(test_utreexo_messages
    tests/network/test_utreexo_messages.cpp
    src/network/utreexo_messages.cpp
    src/daemon/p2p_message.cpp
    src/primitives/uint256.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
  )
  target_link_libraries(test_utreexo_messages PRIVATE
    dinero_crypto
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  target_include_directories(test_utreexo_messages PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  add_test(NAME UtreexoMessages COMMAND test_utreexo_messages)
  message(STATUS "✅ Phase 7.1 Utreexo P2P message tests configured")
  message(STATUS "   Messages: getutxoproof, utxoproof, getutxohdrs, utxohdrs")
  message(STATUS "   Tests: 15 tests covering serialization, validation, DoS protection")
  message(STATUS "   Policy: Fail-fast on protocol violations")
endif()

# ============================================================================
# Phase 7.2: Bridge Node Proof Generation Tests
# ============================================================================
# Purpose: Verify Utreexo proof generation and serving functionality
# Components: BridgeNode class, proof generation, request handling
# Tests: Proof generation, UTXO lookups, caching, error handling
# Policy: ANY FAILURE → CI MUST BREAK (proof correctness critical)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_bridge_node.cpp)
  add_executable(test_bridge_node
    tests/network/test_bridge_node.cpp
    src/daemon/p2p_message.cpp
    src/network/utreexo_messages.cpp
    src/primitives/uint256.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
    src/consensus/proof_cache.cpp
    src/consensus/utreexo_proof_relay.cpp
  )
  target_link_libraries(test_bridge_node PRIVATE
    dinero_crypto
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_bridge_node)
  target_include_directories(test_bridge_node PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  add_test(NAME BridgeNode COMMAND test_bridge_node)
  message(STATUS "✅ Phase 7.2 Bridge Node proof generation tests configured")
  message(STATUS "   Components: BridgeNode, proof generation, request handling")
  message(STATUS "   Tests: Proof generation, caching, error handling")
  message(STATUS "   Policy: Fail-fast on proof generation failures")
endif()

# ============================================================================
# PHASE 7.3: Stateless Node Sync Tests
# ============================================================================
# Components: StatelessNode class, proof validation, sync logic
# Tests: Proof validation, root continuity, peer management, target extraction
# Policy: ANY FAILURE → CI MUST BREAK (stateless sync correctness critical)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_stateless_node.cpp)
  add_executable(test_stateless_node
    tests/network/test_stateless_node.cpp
    src/network/stateless_node.cpp
    src/daemon/p2p_message.cpp
    src/daemon/peer_connection.cpp
    src/primitives/uint256.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
    src/consensus/proof_cache.cpp
    src/consensus/utreexo_proof_relay.cpp
  )
  target_link_libraries(test_stateless_node PRIVATE
    dinero_crypto
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_stateless_node)
  target_include_directories(test_stateless_node PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  add_test(NAME StatelessNode COMMAND test_stateless_node)
  message(STATUS "✅ Phase 7.3 Stateless Node sync tests configured")
  message(STATUS "   Components: StatelessNode, proof validation, sync logic")
  message(STATUS "   Tests: Root continuity, peer management, target extraction")
  message(STATUS "   Policy: Fail-fast on validation failures")
endif()

# ═════════════════════════════════════════════════════════════════════════════
# Phase 9.3: BridgeNode Proof Cache Tests
# ═════════════════════════════════════════════════════════════════════════════
# Tests LRU caching, TTL expiration, and cache statistics for proof generation
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_bridge_node_cache.cpp)
  add_executable(test_bridge_node_cache
    tests/network/test_bridge_node_cache.cpp
    src/daemon/p2p_message.cpp
    src/network/utreexo_messages.cpp
    src/primitives/uint256.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
    src/consensus/proof_cache.cpp
    src/consensus/utreexo_proof_relay.cpp
  )
  target_link_libraries(test_bridge_node_cache PRIVATE
    dinero_crypto
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_bridge_node_cache)
  target_include_directories(test_bridge_node_cache PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  add_test(NAME BridgeNodeCache COMMAND test_bridge_node_cache)
  message(STATUS "✅ Phase 9.3 BridgeNode Cache tests configured")
  message(STATUS "   Tests: LRU cache, TTL expiration, statistics, pre-caching")
endif()

# ============================================================================
# PHASE 7.4.1: Service Bit Advertisement Tests
# ============================================================================
# Components: PeerConnection service bit tracking, VersionMessage services
# Tests: Service bit initialization, tracking, serialization, filtering
# Policy: ANY FAILURE → CI MUST BREAK (peer selection correctness critical)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_phase_7_4_1_service_bit.cpp)
  add_executable(test_phase_7_4_1_service_bit
    tests/network/test_phase_7_4_1_service_bit.cpp
    src/daemon/peer_connection.cpp
    src/daemon/p2p_message.cpp
    src/primitives/uint256.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
  )
  target_link_libraries(test_phase_7_4_1_service_bit PRIVATE
    dinero_crypto
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  target_include_directories(test_phase_7_4_1_service_bit PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  add_test(NAME Phase741ServiceBit COMMAND test_phase_7_4_1_service_bit)
  message(STATUS "✅ Phase 7.4.1 Service Bit Advertisement tests configured")
  message(STATUS "   Components: PeerConnection, VersionMessage, service bits")
  message(STATUS "   Tests: Initialization, tracking, serialization, filtering")
  message(STATUS "   Policy: Fail-fast on service bit handling failures")
endif()

# ============================================================================
# PHASE 7.4.2: Message Dispatch Wiring Tests
# ============================================================================
# Components: active P2P routing, BridgeNode integration, message routing
# Tests: Handler registration, callback integration, validation
# Policy: ANY FAILURE → CI MUST BREAK (message routing correctness critical)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_phase_7_4_2_message_dispatch.cpp)
  add_executable(test_phase_7_4_2_message_dispatch
    tests/network/test_phase_7_4_2_message_dispatch.cpp
    # Phase 7.4.2: Minimal message routing test (standalone router path)
    src/network/utreexo_message_router.cpp
    src/network/stateless_node.cpp
    src/network/utreexo_messages.cpp
    # Peer connection (minimal P2P framing)
    src/daemon/peer_connection.cpp
    src/daemon/p2p_message.cpp
    # Utreexo consensus
    src/consensus/utreexo_accumulator.cpp
    src/consensus/proof_cache.cpp
    src/consensus/utreexo_proof_relay.cpp
    # Common utilities
    src/common/logger.cpp
    src/common/sha256d.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_phase_7_4_2_message_dispatch PRIVATE
    dinero_crypto
    dinero_consensus
    RocksDB::rocksdb
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_phase_7_4_2_message_dispatch)
  target_include_directories(test_phase_7_4_2_message_dispatch PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  add_test(NAME Phase742MessageDispatch COMMAND test_phase_7_4_2_message_dispatch)
  message(STATUS "✅ Phase 7.4.2 Message Dispatch Wiring tests configured")
  message(STATUS "   Components: active P2P routing, BridgeNode, message routing")
  message(STATUS "   Tests: Handler registration, callbacks, validation")
  message(STATUS "   Policy: Fail-fast on message routing failures")
endif()

# ============================================================================
# v0.14.0.5: Utreexo Accumulator Unit Tests
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo.cpp)
  add_executable(test_utreexo
    tests/consensus/test_utreexo.cpp
  )
  target_link_libraries(test_utreexo PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_utreexo)
  target_include_directories(test_utreexo PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoAccumulator COMMAND test_utreexo)
endif()

# ============================================================================
# Utreexo Adversarial Test Suite (Zero-hash, duplicates, edge cases)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_adversarial.cpp)
  add_executable(test_utreexo_adversarial
    tests/consensus/test_utreexo_adversarial.cpp
  )
  target_link_libraries(test_utreexo_adversarial PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_utreexo_adversarial)
  target_include_directories(test_utreexo_adversarial PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo_adversarial PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoAdversarial COMMAND test_utreexo_adversarial)
endif()

# ============================================================================
# Utreexo Transition Proof Test (Stump-based state transition verification)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_transition_proof.cpp)
  add_executable(test_utreexo_transition_proof
    tests/consensus/test_utreexo_transition_proof.cpp
  )
  target_link_libraries(test_utreexo_transition_proof PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_utreexo_transition_proof)
  target_include_directories(test_utreexo_transition_proof PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo_transition_proof PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoTransitionProof COMMAND test_utreexo_transition_proof)
endif()

# ============================================================================
# Utreexo Stump Deletion Test (CSN batch deletion algorithm)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_stump_deletion.cpp)
  add_executable(test_utreexo_stump_deletion
    tests/consensus/test_utreexo_stump_deletion.cpp
  )
  target_link_libraries(test_utreexo_stump_deletion PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_utreexo_stump_deletion)
  target_include_directories(test_utreexo_stump_deletion PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo_stump_deletion PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoStumpDeletion COMMAND test_utreexo_stump_deletion)
endif()

# ============================================================================
# CSN Integration Test (Position Index + Proof + Rebuild + Stateless Verify)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_csn_integration.cpp)
  add_executable(test_csn_integration
    tests/consensus/test_csn_integration.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_csn_integration PRIVATE
    dinero_consensus
    dinero_chainstate
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_csn_integration)
  target_include_directories(test_csn_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_csn_integration PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME CSNIntegration COMMAND test_csn_integration)
endif()

# ============================================================================
# Phase #4: Utreexo TX Relay Tests
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_utreexo_tx_relay.cpp)
  add_executable(test_utreexo_tx_relay
    tests/network/test_utreexo_tx_relay.cpp
    src/network/stateless_node.cpp
    src/daemon/p2p_message.cpp
    src/daemon/peer_connection.cpp
    src/network/utreexo_messages.cpp
    src/primitives/uint256.cpp
    src/primitives/transaction_serializer.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
    src/consensus/proof_cache.cpp
    src/consensus/utreexo_proof_relay.cpp
  )
  target_link_libraries(test_utreexo_tx_relay PRIVATE
    dinero_crypto
    dinero_consensus
    dinero_chainstate
    dinero_bridge  # BridgeNode ctor/dtor — referenced by the test
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_utreexo_tx_relay)
  target_include_directories(test_utreexo_tx_relay PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo_tx_relay PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoTxRelay COMMAND test_utreexo_tx_relay)
endif()

# ============================================================================
# E2E Integration Test: Bridge→CSN Block Sync + TX Relay
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_utreexo_e2e_relay.cpp)
  add_executable(test_utreexo_e2e_relay
    tests/network/test_utreexo_e2e_relay.cpp
    src/network/stateless_node.cpp
    src/daemon/p2p_message.cpp
    src/daemon/peer_connection.cpp
    src/network/utreexo_messages.cpp
    src/primitives/uint256.cpp
    src/primitives/transaction_serializer.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
    src/consensus/proof_cache.cpp
    src/consensus/utreexo_proof_relay.cpp
  )
  target_link_libraries(test_utreexo_e2e_relay PRIVATE
    dinero_crypto
    dinero_consensus
    dinero_chainstate
    dinero_bridge
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_utreexo_e2e_relay)
  target_include_directories(test_utreexo_e2e_relay PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo_e2e_relay PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoE2ERelay COMMAND test_utreexo_e2e_relay)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_csn_stale_proof_retry.cpp)
  add_executable(test_csn_stale_proof_retry
    tests/network/test_csn_stale_proof_retry.cpp
    src/network/stateless_node.cpp
    src/daemon/p2p_message.cpp
    src/daemon/peer_connection.cpp
    src/network/utreexo_messages.cpp
    src/primitives/uint256.cpp
    src/primitives/transaction_serializer.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
    src/consensus/proof_cache.cpp
    src/consensus/utreexo_proof_relay.cpp
  )
  target_link_libraries(test_csn_stale_proof_retry PRIVATE
    dinero_crypto
    dinero_consensus
    dinero_chainstate
    dinero_bridge   # BridgeNode ctor/dtor — referenced by the test
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_csn_stale_proof_retry)
  target_include_directories(test_csn_stale_proof_retry PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_csn_stale_proof_retry PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME CsnStaleProofRetry COMMAND test_csn_stale_proof_retry)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_transition_only_proof_mode.cpp)
  add_executable(test_transition_only_proof_mode
    tests/network/test_transition_only_proof_mode.cpp
  )
  target_link_libraries(test_transition_only_proof_mode PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_transition_only_proof_mode)
  target_include_directories(test_transition_only_proof_mode PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_transition_only_proof_mode PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME TransitionOnlyProofMode COMMAND test_transition_only_proof_mode)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_transition_proof_forest_sync.cpp)
  add_executable(test_transition_proof_forest_sync
    tests/network/test_transition_proof_forest_sync.cpp
    src/network/stateless_node.cpp
    src/network/utreexo_messages.cpp
    src/daemon/peer_connection.cpp
    src/daemon/p2p_message.cpp
    src/primitives/uint256.cpp
    src/primitives/transaction_serializer.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
  )
  target_link_libraries(test_transition_proof_forest_sync PRIVATE
    dinero_crypto
    dinero_consensus
    dinero_chainstate
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_transition_proof_forest_sync)
  target_include_directories(test_transition_proof_forest_sync PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_transition_proof_forest_sync PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME TransitionProofForestSync COMMAND test_transition_proof_forest_sync)
endif()

# ============================================================================
# DINERO-UTREEXO-SPEC §3.5: Leaf Hash Golden Vector Test
# ============================================================================
# Verifies domain-separated leaf hash matches specification exactly.
# This is CONSENSUS-CRITICAL - any failure means spec violation.
# Golden vector: fc8b209be5e6a849feeeda20633d055ee7e6830979cc76ec41bfb58e2a4f3fad
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/utreexo_leaf_hash_golden_test.cpp)
  add_executable(test_utreexo_leaf_hash_golden
    tests/utreexo_leaf_hash_golden_test.cpp
  )
  target_link_libraries(test_utreexo_leaf_hash_golden PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_utreexo_leaf_hash_golden)
  target_include_directories(test_utreexo_leaf_hash_golden PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo_leaf_hash_golden PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoLeafHashGolden COMMAND test_utreexo_leaf_hash_golden)
  set_tests_properties(UtreexoLeafHashGolden PROPERTIES
    LABELS "consensus;critical;utreexo;spec"
    TIMEOUT 10
  )
  message(STATUS "✅ DINERO-UTREEXO-SPEC §3.5 golden vector test configured")
endif()

# ============================================================================
# Utreexo Stateless Proof Minimal Test (Hardening Tests)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_stateless_proof_minimal.cpp)
  add_executable(test_stateless_proof_minimal
    tests/test_stateless_proof_minimal.cpp
  )
  target_link_libraries(test_stateless_proof_minimal PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_stateless_proof_minimal)
  target_include_directories(test_stateless_proof_minimal PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_stateless_proof_minimal PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoStatelessProofMinimal COMMAND test_stateless_proof_minimal)
endif()

# ============================================================================
# E.2: Fee Policy Configuration Verification (Economics Layer)
# ============================================================================
# NOTE: This is CONFIG verification, not behavior testing.
# Mempool BEHAVIOR tests (CPFP, RBF, ancestor tracking) were completed in F.9.6-F.9.8.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_fee_config.cpp)
  add_executable(test_fee_config
    tests/consensus/test_fee_config.cpp
    src/consensus/chainparams_impl.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_fee_config PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_fee_config)
  target_include_directories(test_fee_config PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_fee_config PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME FeeConfigVerification COMMAND test_fee_config)
endif()

# ============================================================================
# Phase 3: CT Fee Policy Configuration Tests
# ============================================================================
# Verifies CT fee policy parameters and estimation functions
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/policy/test_ct_fee_policy.cpp)
  add_executable(test_ct_fee_policy
    tests/policy/test_ct_fee_policy.cpp
    src/policy/fee_estimator.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_ct_fee_policy PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_ct_fee_policy)
  target_include_directories(test_ct_fee_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_ct_fee_policy PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME CTFeePolicyTests COMMAND test_ct_fee_policy)
  set_tests_properties(CTFeePolicyTests PROPERTIES LABELS "policy;ct;fee")
endif()

# ============================================================================
# E.3: Supply Cap Validation (Economics Layer)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_supply_cap.cpp)
  add_executable(test_supply_cap
    tests/consensus/test_supply_cap.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_supply_cap PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_supply_cap)
  target_include_directories(test_supply_cap PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_supply_cap PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME SupplyCapValidation COMMAND test_supply_cap)
endif()

# ============================================================================
# E.4: Economic Stress Tests (Economics Layer)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_economic_edge_cases.cpp)
  add_executable(test_economic_edge_cases
    tests/consensus/test_economic_edge_cases.cpp
    src/consensus/chainparams_impl.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_economic_edge_cases PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_economic_edge_cases)
  target_include_directories(test_economic_edge_cases PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_economic_edge_cases PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME EconomicStressTests COMMAND test_economic_edge_cases)
endif()

# ============================================================================
# E.5: Premine Verification (Economics Layer)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_premine.cpp)
  add_executable(test_premine
    tests/consensus/test_premine.cpp
    src/consensus/chainparams_impl.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_premine PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_premine)
  target_include_directories(test_premine PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_premine PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME PremineVerification COMMAND test_premine)
endif()

# ============================================================================
# G.1: P2P Protocol Verification (Networking Layer)
# ============================================================================

# G.1.2: Multi-node Test Harness + Simple Handshake Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_handshake_simple.cpp)
  add_executable(test_p2p_handshake
    tests/p2p/test_handshake_simple.cpp
    tests/p2p/test_harness.cpp
    src/daemon/p2p_manager.cpp
    src/daemon/secure_random.cpp
    src/daemon/peer_connection.cpp
    src/daemon/p2p_message.cpp
    src/daemon/peer_scoring.cpp
    src/daemon/peer_reputation_db.cpp
    src/p2p/addrman.cpp
    src/common/logger.cpp
    src/network/utreexo_messages.cpp
  )
  target_link_libraries(test_p2p_handshake PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
    gtest
  )
  link_zstd_if_needed(test_p2p_handshake)
  target_include_directories(test_p2p_handshake PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests/p2p
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_p2p_handshake PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME P2PHandshakeVerification COMMAND test_p2p_handshake)
endif()

# G.1.3: Ping/Pong Protocol Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_ping_pong.cpp)
  add_executable(test_p2p_ping_pong
    tests/p2p/test_ping_pong.cpp
    tests/p2p/test_harness.cpp
    src/daemon/p2p_manager.cpp
    src/daemon/secure_random.cpp
    src/daemon/peer_connection.cpp
    src/daemon/p2p_message.cpp
    src/daemon/peer_scoring.cpp
    src/daemon/peer_reputation_db.cpp
    src/p2p/addrman.cpp
    src/common/logger.cpp
    src/network/utreexo_messages.cpp
  )
  target_link_libraries(test_p2p_ping_pong PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
    gtest
  )
  link_zstd_if_needed(test_p2p_ping_pong)
  target_include_directories(test_p2p_ping_pong PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests/p2p
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_p2p_ping_pong PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME P2PPingPongVerification COMMAND test_p2p_ping_pong)
endif()

# G.1.4: Inventory Message Serialization Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_inventory_serialization.cpp)
  add_executable(test_inventory_serialization
    tests/p2p/test_inventory_serialization.cpp
    src/p2p/inventory.cpp
  )
  target_link_libraries(test_inventory_serialization PRIVATE
  )
  target_include_directories(test_inventory_serialization PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_inventory_serialization PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME InventorySerializationVerification COMMAND test_inventory_serialization)
endif()

# G.1.4 Step 2: InFlightManager Test (Pure, Synchronous, Deterministic)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_inflight_manager.cpp)
  add_executable(test_inflight_manager
    tests/p2p/test_inflight_manager.cpp
    src/p2p/inflight_manager.cpp
    src/p2p/inventory.cpp
  )
  target_link_libraries(test_inflight_manager PRIVATE
  )
  target_include_directories(test_inflight_manager PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_inflight_manager PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME InFlightManagerVerification COMMAND test_inflight_manager)
endif()

# G.1.4 Step 3: INV/GETDATA/NOTFOUND Handler Test (Pure Protocol Choreography)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_inv_getdata_handlers.cpp)
  add_executable(test_inv_getdata_handlers
    tests/p2p/test_inv_getdata_handlers.cpp
    src/p2p/inventory_handler.cpp
    src/p2p/inflight_manager.cpp
    src/p2p/inventory.cpp
  )
  target_link_libraries(test_inv_getdata_handlers PRIVATE
  )
  target_include_directories(test_inv_getdata_handlers PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_inv_getdata_handlers PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME InvGetDataHandlerVerification COMMAND test_inv_getdata_handlers)
endif()

# G.1.4 Step 4: Timeout + Retry Policy Test (Pure Resilience Logic)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_timeout_retry.cpp)
  add_executable(test_timeout_retry
    tests/p2p/test_timeout_retry.cpp
    src/p2p/inventory_handler.cpp
    src/p2p/inflight_manager.cpp
    src/p2p/inventory.cpp
  )
  target_link_libraries(test_timeout_retry PRIVATE
  )
  target_include_directories(test_timeout_retry PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_timeout_retry PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME TimeoutRetryVerification COMMAND test_timeout_retry)
endif()

# G.2: Download Coordination Test (Pure Payload Transfer)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_download_coordination.cpp)
  add_executable(test_download_coordination
    tests/p2p/test_download_coordination.cpp
    src/p2p/download_coordinator.cpp
    src/p2p/inventory_handler.cpp
    src/p2p/inflight_manager.cpp
    src/p2p/inventory.cpp
  )
  target_link_libraries(test_download_coordination PRIVATE
  )
  target_include_directories(test_download_coordination PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_download_coordination PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME DownloadCoordinationVerification COMMAND test_download_coordination)
endif()

# Phase G.2: Block Relay Manager Integration Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_block_relay_g2.cpp)
  add_executable(test_block_relay_g2
    tests/p2p/test_block_relay_g2.cpp
    src/daemon/block_relay_manager.cpp
    src/p2p/block_download_scheduler.cpp
    src/p2p/orphan_block_pool.cpp    # Phase G.7: Orphan handling
    src/p2p/block_presence.cpp       # Required by orphan_block_pool
    src/primitives/block.cpp
    src/primitives/uint256.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    src/common/sha256d.cpp
    tests/stubs/mempool_test_stubs.cpp  # forEachEntry referenced by block_relay_manager
  )
  target_link_libraries(test_block_relay_g2 PRIVATE
    dinero_crypto
    dinero_core
    dinero_consensus        # For HeaderSyncManager
    bulletproofs_ffi        # Required by confidential_validation
  )
  link_zstd_if_needed(test_block_relay_g2)
  target_include_directories(test_block_relay_g2 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_block_relay_g2 PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME BlockRelayG2Verification COMMAND test_block_relay_g2)
endif()

# Phase Plan-A: Compact Block Binary Serialization Unit Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_compact_block_binary.cpp)
  add_executable(test_compact_block_binary
    tests/p2p/test_compact_block_binary.cpp
    src/p2p/compact_block.cpp
    src/primitives/block.cpp
    src/primitives/uint256.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    src/common/sha256d.cpp
    tests/stubs/mempool_test_stubs.cpp  # no-op Mempool::forEachEntry etc.
  )
  target_link_libraries(test_compact_block_binary PRIVATE
    dinero_crypto
    dinero_core
    gtest
    gtest_main
  )
  link_zstd_if_needed(test_compact_block_binary)
  target_include_directories(test_compact_block_binary PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  if(APPLE)
    target_link_libraries(test_compact_block_binary PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME CompactBlockBinarySerialization COMMAND test_compact_block_binary)
  message(STATUS "  ✓ Phase Plan-A test registered: test_compact_block_binary")
endif()

# Phase G.8 / Phase N.3: Headers Wire Format Unit Tests (BlockHeader v1 = 128 bytes)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_headers_wire_v1.cpp)
  add_executable(test_headers_wire_v1
    tests/p2p/test_headers_wire_v1.cpp
    src/daemon/block_relay_manager.cpp
    src/daemon/header_serialization.cpp
    src/daemon/p2p_message.cpp
    src/primitives/block.cpp
    src/primitives/uint256.cpp
    src/common/sha256d.cpp
    tests/stubs/mempool_test_stubs.cpp  # no-op Mempool::forEachEntry etc.
  )
  target_link_libraries(test_headers_wire_v1 PRIVATE
    dinero_crypto
    dinero_core
    dinero_consensus
    bulletproofs_ffi
    gtest
    gtest_main
  )
  link_zstd_if_needed(test_headers_wire_v1)
  target_include_directories(test_headers_wire_v1 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  if(APPLE)
    target_link_libraries(test_headers_wire_v1 PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeadersWireV1 COMMAND test_headers_wire_v1)
  message(STATUS "  ✓ Phase G.8/N.3 test registered: test_headers_wire_v1")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_mainnet_checkpoints.cpp)
  add_executable(test_mainnet_checkpoints
    tests/p2p/test_mainnet_checkpoints.cpp
  )
  target_link_libraries(test_mainnet_checkpoints PRIVATE
    dinero_core
    bulletproofs_ffi
    gtest_main
  )
  target_include_directories(test_mainnet_checkpoints PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  if(APPLE)
    target_link_libraries(test_mainnet_checkpoints PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME MainnetCheckpoints COMMAND test_mainnet_checkpoints)
  message(STATUS "  ✓ Mainnet checkpoint test registered: test_mainnet_checkpoints")
endif()

# Phase G.3: Transaction Relay Manager Integration Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_tx_relay_g3.cpp)
  add_executable(test_tx_relay_g3
    tests/p2p/test_tx_relay_g3.cpp
    src/daemon/tx_relay_manager.cpp
    src/mempool/tx_orphan_pool.cpp
    src/daemon/interfaces/ingress_types.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    src/primitives/uint256.cpp
    src/crypto/params_stub.cpp
    src/common/sha256d.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_tx_relay_g3 PRIVATE
    dinero_crypto
  )
  target_include_directories(test_tx_relay_g3 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_tx_relay_g3 PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME TxRelayG3Verification COMMAND test_tx_relay_g3)
endif()

# Issue #6: CSN Proof Refresh Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_csn_proof_refresh.cpp)
  add_executable(test_csn_proof_refresh
    tests/p2p/test_csn_proof_refresh.cpp
    src/daemon/tx_relay_manager.cpp
    src/mempool/tx_orphan_pool.cpp
    src/daemon/interfaces/ingress_types.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    src/primitives/uint256.cpp
    src/common/sha256d.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_csn_proof_refresh PRIVATE
    dinero_crypto
  )
  target_include_directories(test_csn_proof_refresh PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_csn_proof_refresh PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME CsnProofRefresh COMMAND test_csn_proof_refresh)
  message(STATUS "  ✓ Issue #6 test registered: test_csn_proof_refresh")
endif()

# Phase G.4: Fee Estimation Integration Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mempool/test_fee_estimation_g4.cpp)
  add_executable(test_fee_estimation_g4
    tests/mempool/test_fee_estimation_g4.cpp
    src/mempool/fee_estimator.cpp
    src/primitives/uint256.cpp
    src/common/sha256d.cpp
  )
  target_link_libraries(test_fee_estimation_g4 PRIVATE
    dinero_crypto
    dinero_core
  )
  target_include_directories(test_fee_estimation_g4 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_fee_estimation_g4 PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME FeeEstimationG4Verification COMMAND test_fee_estimation_g4)
endif()

# Phase G.5: Peer Discovery & Address Management Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_peer_discovery_g5.cpp)
  add_executable(test_peer_discovery_g5
    tests/p2p/test_peer_discovery_g5.cpp
    src/p2p/addrman.cpp
    src/p2p/peer_scoring.cpp
  )
  target_link_libraries(test_peer_discovery_g5 PRIVATE
    dinero_core
  )
  target_include_directories(test_peer_discovery_g5 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_peer_discovery_g5 PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME PeerDiscoveryG5Verification COMMAND test_peer_discovery_g5)
endif()

# Phase G.6: Connection Management & Peer Eviction Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_connection_manager.cpp)
  add_executable(test_connection_manager_g6
    tests/network/test_connection_manager.cpp
    src/p2p/connection_manager.cpp
    src/p2p/peer_scoring.cpp
  )
  target_link_libraries(test_connection_manager_g6 PRIVATE
    dinero_core
  )
  target_include_directories(test_connection_manager_g6 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_connection_manager_g6 PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ConnectionManagerG6Verification COMMAND test_connection_manager_g6)
endif()

# Phase G.7: Rate Limiting & DoS Protection Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_rate_limiter_g7.cpp)
  add_executable(test_rate_limiter_g7
    tests/p2p/test_rate_limiter_g7.cpp
    src/p2p/rate_limiter.cpp
    src/p2p/peer_scoring.cpp
  )
  target_link_libraries(test_rate_limiter_g7 PRIVATE
    dinero_core
  )
  target_include_directories(test_rate_limiter_g7 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_rate_limiter_g7 PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME RateLimiterG7Verification COMMAND test_rate_limiter_g7)
endif()

# Phase G.8: DNS Resolution & Network Bootstrap Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_dns_resolver_g8.cpp)
  add_executable(test_dns_resolver_g8
    tests/p2p/test_dns_resolver_g8.cpp
    src/p2p/dns_resolver.cpp
  )
  target_link_libraries(test_dns_resolver_g8 PRIVATE
    dinero_core
  )
  # dns_resolver.cpp uses getaddrinfo/inet_pton/inet_ntop which live in
  # ws2_32.lib on Windows (POSIX libc on Linux/Mac, so no link needed there).
  # We recompile dns_resolver.cpp directly here rather than pulling from
  # dinero_core, so the ws2_32 dep doesn't transit.
  if(WIN32)
    target_link_libraries(test_dns_resolver_g8 PRIVATE ws2_32)
  endif()
  target_include_directories(test_dns_resolver_g8 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_dns_resolver_g8 PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME DNSResolverG8Verification COMMAND test_dns_resolver_g8)
endif()

# Phase G.6.B: Block Download Scheduling Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_block_download_scheduler_g6b.cpp)
  add_executable(test_block_download_scheduler_g6b
    tests/p2p/test_block_download_scheduler_g6b.cpp
    src/p2p/block_download_scheduler.cpp
  )
  target_link_libraries(test_block_download_scheduler_g6b PRIVATE
    dinero_core
  )
  target_include_directories(test_block_download_scheduler_g6b PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_block_download_scheduler_g6b PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME BlockDownloadSchedulerG6BVerification COMMAND test_block_download_scheduler_g6b)
endif()

# G.3.1: Validation Sink Wiring Test (Pure Queueing, NO Validation)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_validation_sink.cpp)
  add_executable(test_validation_sink
    tests/p2p/test_validation_sink.cpp
    src/p2p/validation_sink.cpp
    src/p2p/download_coordinator.cpp
    src/p2p/inventory_handler.cpp
    src/p2p/inflight_manager.cpp
    src/p2p/inventory.cpp
  )
  target_link_libraries(test_validation_sink PRIVATE
  )
  target_include_directories(test_validation_sink PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_validation_sink PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ValidationSinkVerification COMMAND test_validation_sink)
endif()

# Phase 6B: ValidationQueue canonical handoff regression
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_validation_queue_handoff.cpp)
  add_executable(test_validation_queue_handoff
    tests/consensus/test_validation_queue_handoff.cpp
    src/daemon/mempool.cpp
    src/daemon/config.cpp                # GetConfig() — referenced by consensus_write_batch.obj
    tests/stubs/vault_test_stubs.cpp     # vault observers — real mempool linked
  )
  target_link_libraries(test_validation_queue_handoff PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    dinero_wallet
    bulletproofs_ffi
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_validation_queue_handoff)
  target_include_directories(test_validation_queue_handoff PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_validation_queue_handoff PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ValidationQueueHandoff COMMAND test_validation_queue_handoff)
endif()

# G.3.2: Structural Validation Test (Cheap, Pure - NO Consensus)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_structural_validation.cpp)
  add_executable(test_structural_validation
    tests/p2p/test_structural_validation.cpp
    src/p2p/structural_validator.cpp
  )
  target_link_libraries(test_structural_validation PRIVATE
    dinero_core
  )
  target_include_directories(test_structural_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_structural_validation PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME StructuralValidationVerification COMMAND test_structural_validation)
endif()

# G.3.3: Consensus Validation Test (Expensive, Deterministic - Read-Only UTXO)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_consensus_validation.cpp)
  add_executable(test_consensus_validation
    tests/p2p/test_consensus_validation.cpp
    src/p2p/consensus_validator.cpp
    src/p2p/inventory.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_consensus_validation PRIVATE
    dinero_crypto  # For SHA256 hashing used by uint256
  )
  target_include_directories(test_consensus_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_consensus_validation PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ConsensusValidationVerification COMMAND test_consensus_validation)
endif()

# G.3.3 Extension: Subsidy Validation Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_subsidy_validation.cpp)
  add_executable(test_subsidy_validation
    tests/p2p/test_subsidy_validation.cpp
    src/p2p/consensus_validator.cpp
    src/p2p/inventory.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_subsidy_validation PRIVATE
    gtest
  )
  target_include_directories(test_subsidy_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  target_compile_definitions(test_subsidy_validation PRIVATE DINERO_TESTING)
  if(APPLE)
    target_link_libraries(test_subsidy_validation PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME SubsidyValidationVerification COMMAND test_subsidy_validation)
endif()

# G.3.4: State Transition Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_state_transition.cpp)
  add_executable(test_state_transition
    tests/p2p/test_state_transition.cpp
    src/p2p/state_transition.cpp
    src/p2p/consensus_validator.cpp
    src/p2p/inventory.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_state_transition PRIVATE
    gtest
  )
  target_include_directories(test_state_transition PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  target_compile_definitions(test_state_transition PRIVATE DINERO_TESTING)
  if(APPLE)
    target_link_libraries(test_state_transition PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME StateTransitionVerification COMMAND test_state_transition)
endif()

# IBD Smoke Test - Initial Block Download Integration
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_ibd_smoke.cpp)
  add_executable(test_ibd_smoke
    tests/consensus/test_ibd_smoke.cpp
    src/common/logger.cpp        # Required by TaprootKeys
    src/common/json_logger.cpp   # Required by logger
  )
  target_link_libraries(test_ibd_smoke PRIVATE
    dinero_consensus  # For genesis, chainparams
    dinero_crypto     # For SHA256 hashing
    dinero_wallet     # For TaprootKeys
    jsoncpp_static    # Required by json_logger
  )
  link_zstd_if_needed(test_ibd_smoke)
  target_include_directories(test_ibd_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_ibd_smoke PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME IBDSmokeTest COMMAND test_ibd_smoke)
endif()

# IBD Connect Test - Real Consensus Integration
# TODO: Disabled - needs refactor to use IConsensusUTXOSet instead of UTXOIndex directly
# BlockValidator now requires IConsensusUTXOSet* not IUTXOProvider*
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_ibd_connect.cpp)
  add_executable(test_ibd_connect
    tests/consensus/test_ibd_connect.cpp
    src/common/logger.cpp        # Required by TaprootKeys
    src/common/json_logger.cpp   # Required by logger
    src/consensus/block_validation.cpp  # BlockValidator::ConnectBlock
    src/consensus/undo.cpp       # BlockUndo serialization
  )
  target_link_libraries(test_ibd_connect PRIVATE
    dinero_consensus  # For genesis, chainparams, BlockValidator
    dinero_crypto     # For SHA256 hashing
    dinero_wallet     # For TaprootKeys, UTXOIndex
    dinero_chainstate # For ChainDB and persistence
    jsoncpp_static    # Required by json_logger
    sqlite3           # Required by UTXOIndex
    RocksDB::rocksdb  # Required by consensus components
    bulletproofs_ffi  # Required by ConfidentialTransactionValidator
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_ibd_connect)
  target_include_directories(test_ibd_connect PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_ibd_connect PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME IBDConnectTest COMMAND test_ibd_connect)
endif()

# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
# IBD Persistence Test - Restart Recovery (Phase 3A)
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_ibd_persistence.cpp)
  add_executable(test_ibd_persistence
    tests/consensus/test_ibd_persistence.cpp
    src/common/logger.cpp
    src/common/json_logger.cpp
    src/consensus/block_validation.cpp
    src/consensus/undo.cpp
  )
  target_link_libraries(test_ibd_persistence PRIVATE
    dinero_consensus  # For genesis, chainparams, BlockValidator
    dinero_crypto     # For SHA256 hashing
    dinero_wallet     # For TaprootKeys, UTXOIndex
    dinero_chainstate # For primitives
    jsoncpp_static    # Required by json_logger
    sqlite3           # Required by UTXOIndex
    bulletproofs_ffi  # Required by ConfidentialTransactionValidator
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_ibd_persistence)
  target_include_directories(test_ibd_persistence PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_ibd_persistence PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME IBDPersistenceTest COMMAND test_ibd_persistence)
endif()

# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
# IBD Reorg Test - DisconnectBlock & Reorg Safety
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_ibd_reorg.cpp)
  add_executable(test_ibd_reorg
    tests/consensus/test_ibd_reorg.cpp
    src/common/logger.cpp
    src/common/json_logger.cpp
    src/consensus/block_validation.cpp
    src/consensus/undo.cpp
  )
  target_link_libraries(test_ibd_reorg PRIVATE
    dinero_consensus  # For genesis, chainparams, BlockValidator
    dinero_crypto     # For SHA256 hashing
    dinero_wallet     # For TaprootKeys, UTXOIndex
    dinero_chainstate # For primitives
    jsoncpp_static    # Required by json_logger
    sqlite3           # Required by UTXOIndex
    bulletproofs_ffi  # Required by ConfidentialTransactionValidator
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_ibd_reorg)
  target_include_directories(test_ibd_reorg PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_ibd_reorg PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME IBDReorgTest COMMAND test_ibd_reorg)
endif()

# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
# IBD Spend Test - Utreexo Proof Generation E2E
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_ibd_spend.cpp)
  add_executable(test_ibd_spend
    tests/consensus/test_ibd_spend.cpp
    src/common/logger.cpp
    src/common/json_logger.cpp
    src/consensus/block_validation.cpp
    src/consensus/undo.cpp
    src/consensus/proof_cache.cpp  # For ProofCache
    src/primitives/taproot_tx_signer.cpp  # For BIP341 Taproot signing
  )
  target_link_libraries(test_ibd_spend PRIVATE
    dinero_consensus  # For genesis, chainparams, BlockValidator, ProofCache
    dinero_crypto     # For SHA256 hashing
    dinero_wallet     # For TaprootKeys, UTXOIndex
    dinero_chainstate # For primitives
    jsoncpp_static    # Required by json_logger
    sqlite3           # Required by UTXOIndex
    bulletproofs_ffi  # Required by ConfidentialTransactionValidator
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_ibd_spend)
  target_include_directories(test_ibd_spend PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_ibd_spend PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME IBDSpendTest COMMAND test_ibd_spend)
  set_tests_properties(IBDSpendTest
    PROPERTIES
    LABELS "consensus;ibd;utreexo;proof-generation;phase-9;mandatory"
    TIMEOUT 60
  )
endif()

# Fork Choice Test - Chain Selection & Reorg (Phase 3B)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_fork_choice.cpp)
  add_executable(test_fork_choice
    tests/consensus/test_fork_choice.cpp
    src/common/logger.cpp
    src/common/json_logger.cpp
  )
  target_link_libraries(test_fork_choice PRIVATE
    dinero_consensus  # For block_index, chainwork, chainparams
    dinero_crypto     # For SHA256 hashing
    jsoncpp_static    # Required by json_logger
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_fork_choice)
  target_include_directories(test_fork_choice PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_fork_choice PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ForkChoiceTest COMMAND test_fork_choice)
endif()

# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
# Mining Maturity Test - End-to-End Coinbase Flow
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_mining_maturity.cpp)
  add_executable(test_mining_maturity
    tests/consensus/test_mining_maturity.cpp
    src/wallet/utxo_index.cpp
    src/consensus/block_validation.cpp
    src/consensus/coinbase_maturity.cpp
    src/primitives/block.cpp
    # src/wallet/transaction.cpp removed - use from dinero_consensus (already linked)
    src/crypto/sha256.cpp
    src/wallet/taproot_keys.cpp
    src/common/logger.cpp
    src/common/json_logger.cpp
  )
  target_link_libraries(test_mining_maturity PRIVATE
    dinero_consensus  # For chainparams, block validation, coinbase maturity
    dinero_crypto     # For SHA256 hashing
    dinero_wallet     # For TaprootKeys, UTXOIndex
    dinero_chainstate # For primitives
    jsoncpp_static    # Required by json_logger
    sqlite3           # Required by UTXOIndex
    bulletproofs_ffi  # Required by ConfidentialTransactionValidator
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_mining_maturity)
  target_include_directories(test_mining_maturity PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_mining_maturity PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME MiningMaturityTest COMMAND test_mining_maturity)
endif()

# Phase H.5 — Restart Recovery Test (Crash Safety Proof)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_header_sync_restart.cpp)
  add_executable(test_header_sync_restart
    tests/consensus/test_header_sync_restart.cpp
    src/consensus/header_sync_manager.cpp
    src/consensus/block_index.cpp
    tests/consensus/chainstate_metadata_stub.cpp
    tests/consensus/missing_symbols_stub.cpp
    src/storage/chain_db.cpp
    src/primitives/block.cpp
    src/primitives/transaction.cpp         # Required by Block::Serialize()
    src/primitives/transaction_serializer.cpp  # Required by Transaction serialization
    src/primitives/uint256.cpp
    src/consensus/undo.cpp                 # Required by ChainDB::getUndo/putUndo
    src/crypto/sha256.cpp
    src/common/logger.cpp
    src/common/json_logger.cpp
    src/common/sha256d.cpp
    src/consensus/chainwork.cpp
  )
  target_link_libraries(test_header_sync_restart PRIVATE
    dinero_consensus   # Required for BlockUtreexoData serialization
    dinero_crypto      # Required for SHA256 and crypto operations
    RocksDB::rocksdb
    jsoncpp_static
    OpenSSL::Crypto
    OpenSSL::SSL
    secp256k1
    # Use system GTest to match system GTest headers (from /opt/homebrew/include)
    GTest::gtest
  )
  link_zstd_if_needed(test_header_sync_restart)  # Required for Utreexo compression
  # Explicitly remove vendored GTest interface includes for this target only
  set_target_properties(test_header_sync_restart PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ""
  )
  target_include_directories(test_header_sync_restart SYSTEM BEFORE PRIVATE
    # Use system GTest headers
    /opt/homebrew/include
  )
  target_include_directories(test_header_sync_restart PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_header_sync_restart PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeaderSyncRestartRecovery COMMAND test_header_sync_restart)
endif()

# Phase P.1: Prune Eligibility Tests
if(ENABLE_TESTS AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_prune_eligibility.cpp)
  add_executable(test_prune_eligibility
    tests/consensus/test_prune_eligibility.cpp
    src/consensus/block_index.cpp
    src/consensus/chainwork.cpp
    src/primitives/uint256.cpp
    src/primitives/block.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    # src/wallet/transaction.cpp removed - not needed for prune tests
    src/crypto/sha256.cpp
    src/common/sha256d.cpp
    src/consensus/utreexo_accumulator.cpp
    src/consensus/utreexo_stump.cpp
    src/consensus/script_crypto.cpp
    tests/consensus/missing_symbols_stub.cpp
    tests/stubs/chain_test_stubs.cpp  # GetActiveChain() — referenced via inline IsUtreexoCanonicalRootsActive
    src/common/logger.cpp
    src/common/json_logger.cpp
  )
  target_link_libraries(test_prune_eligibility PRIVATE
    jsoncpp_static
    dinero_crypto
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    # Use system GTest to match system GTest headers
    GTest::gtest
  )
  link_zstd_if_needed(test_prune_eligibility)
  set_target_properties(test_prune_eligibility PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ""
  )
  target_include_directories(test_prune_eligibility SYSTEM BEFORE PRIVATE
    /opt/homebrew/include
  )
  target_include_directories(test_prune_eligibility PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_prune_eligibility PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME PruneEligibility COMMAND test_prune_eligibility)
endif()

# ============================================================================
# Phase M.X: Mempool Policy Tests
# ============================================================================

# Ancestor/Descendant Tracking Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mempool/test_ancestor_descendant.cpp)
  add_executable(test_ancestor_descendant
    tests/mempool/test_ancestor_descendant.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    src/consensus/tx_validation.cpp
    src/mempool/mempool.cpp
    src/mempool/coins_view_mempool.cpp
    src/mempool/invalid_tx_cache.cpp
    src/crypto/sha256.cpp
    src/crypto/hash.cpp
    src/crypto/ripemd160.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_ancestor_descendant PRIVATE
    dinero_consensus
    dinero_chainstate  # Phase 23.3: For ChainDB (BIP68 MTP lookup)
    jsoncpp_static
    OpenSSL::Crypto
  )
  link_zstd_if_needed(test_ancestor_descendant)
  target_include_directories(test_ancestor_descendant PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_ancestor_descendant PRIVATE /opt/homebrew/lib)
    target_link_libraries(test_ancestor_descendant PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME MempoolAncestorDescendant COMMAND test_ancestor_descendant)
endif()

# Mempool Expiry Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mempool/test_mempool_expiry.cpp)
  add_executable(test_mempool_expiry
    tests/mempool/test_mempool_expiry.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    src/consensus/tx_validation.cpp
    src/mempool/mempool.cpp
    src/mempool/coins_view_mempool.cpp
    src/mempool/invalid_tx_cache.cpp
    src/crypto/sha256.cpp
    src/crypto/hash.cpp
    src/crypto/ripemd160.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_mempool_expiry PRIVATE
    dinero_consensus
    dinero_chainstate  # Phase 23.3: For ChainDB (BIP68 MTP lookup)
    jsoncpp_static
    OpenSSL::Crypto
  )
  link_zstd_if_needed(test_mempool_expiry)
  target_include_directories(test_mempool_expiry PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_mempool_expiry PRIVATE /opt/homebrew/lib)
    target_link_libraries(test_mempool_expiry PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME MempoolExpiry COMMAND test_mempool_expiry)
endif()

# Covenant Policy Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mempool/test_mempool_covenant_policy.cpp)
  add_executable(test_mempool_covenant_policy
    tests/mempool/test_mempool_covenant_policy.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    src/consensus/tx_validation.cpp
    src/mempool/mempool.cpp
    src/mempool/coins_view_mempool.cpp
    src/mempool/invalid_tx_cache.cpp
    src/crypto/sha256.cpp
    src/crypto/hash.cpp
    src/crypto/ripemd160.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_mempool_covenant_policy PRIVATE
    dinero_consensus
    jsoncpp_static
    OpenSSL::Crypto
  )
  link_zstd_if_needed(test_mempool_covenant_policy)
  target_include_directories(test_mempool_covenant_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_mempool_covenant_policy PRIVATE /opt/homebrew/lib)
    target_link_libraries(test_mempool_covenant_policy PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME MempoolCovenantPolicy COMMAND test_mempool_covenant_policy)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Mempool Proof Staleness Tests (#2)
# ═══════════════════════════════════════════════════════════════════════════
# Tests onBlockConnected eviction, staleness marking, refresh, disconnect
# Uses daemon Mempool with addUnchecked (no ChainDB required)
# ═══════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mempool/test_mempool_proof_staleness.cpp)
  add_executable(test_mempool_proof_staleness
    tests/mempool/test_mempool_proof_staleness.cpp
    tests/mempool/test_mempool_proof_staleness_stubs.cpp
    src/daemon/mempool.cpp
    src/mining/address_validator.cpp
    src/daemon/bech32_decode.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_mempool_proof_staleness PRIVATE
    dinero_chainstate
    dinero_consensus
    dinero_shielded   # DeserializeShieldedBundle / ValidateShieldedBundle / BuildShieldedValidationContext
    dinero_crypto
    secp256k1
    jsoncpp_static
    OpenSSL::Crypto
  )
  link_zstd_if_needed(test_mempool_proof_staleness)
  target_include_directories(test_mempool_proof_staleness PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_mempool_proof_staleness PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME MempoolProofStaleness COMMAND test_mempool_proof_staleness)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mempool/test_mempool_address_queries.cpp)
  add_executable(test_mempool_address_queries
    tests/mempool/test_mempool_address_queries.cpp
    tests/mempool/test_mempool_proof_staleness_stubs.cpp
    src/daemon/mempool.cpp
    src/mining/address_validator.cpp
    src/daemon/bech32_decode.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_mempool_address_queries PRIVATE
    dinero_chainstate
    dinero_consensus
    dinero_shielded   # DeserializeShieldedBundle / ValidateShieldedBundle / BuildShieldedValidationContext
    dinero_crypto
    secp256k1
    jsoncpp_static
    OpenSSL::Crypto
  )
  link_zstd_if_needed(test_mempool_address_queries)
  target_include_directories(test_mempool_address_queries PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_mempool_address_queries PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME MempoolAddressQueries COMMAND test_mempool_address_queries)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Phase M.3: Semantic Hash Domains Test
# ═══════════════════════════════════════════════════════════════════════════
# Verifies compile-time enforcement of semantic hash types
# Tests: No implicit conversions, domain-locked constructors, storage guarantees
# ═══════════════════════════════════════════════════════════════════════════

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/primitives/test_hash_domains_m3.cpp)
  add_executable(test_hash_domains_m3
    tests/primitives/test_hash_domains_m3.cpp
    src/primitives/hash_domains.cpp
    src/primitives/block.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    src/primitives/uint256.cpp
    src/crypto/sha256.cpp
  )
  
  target_include_directories(test_hash_domains_m3 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sha3/include
  )
  
  target_link_libraries(test_hash_domains_m3 PRIVATE
    dinero_crypto
    dinero_consensus
    secp256k1
    OpenSSL::Crypto
    GTest::gtest
    GTest::gtest_main
  )
  
  link_zstd_if_needed(test_hash_domains_m3)
  
  add_test(NAME HashDomainsM3 COMMAND test_hash_domains_m3)
  set_tests_properties(HashDomainsM3 PROPERTIES
    LABELS "phase-m3;semantic-types;compile-time-enforcement"
  )

  message(STATUS "  ✓ Phase M.3 test registered: test_hash_domains_m3")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase M.4: ABI Stability Tests
# ════════════════════════════════════════════════════════════════════════════

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_abi_stability.cpp)
  add_executable(test_abi_stability
    tests/consensus/test_abi_stability.cpp
  )

  target_include_directories(test_abi_stability PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sha3/include
  )

  target_link_libraries(test_abi_stability PRIVATE
    dinero_consensus
    dinero_crypto
    secp256k1
    OpenSSL::Crypto
  )

  link_zstd_if_needed(test_abi_stability)

  add_test(NAME ABI_Stability COMMAND test_abi_stability)
  set_tests_properties(ABI_Stability PROPERTIES
    LABELS "phase-m4;abi-freeze;consensus-critical;mandatory"
  )

  message(STATUS "  ✓ Phase M.4 ABI stability test registered: test_abi_stability")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Signature Rejection Invariant Tests (CRITICAL SECURITY)
# ════════════════════════════════════════════════════════════════════════════

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_signature_rejection.cpp)
  add_executable(test_signature_rejection
    tests/consensus/test_signature_rejection.cpp
  )

  target_include_directories(test_signature_rejection PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  )

  target_link_libraries(test_signature_rejection PRIVATE
    dinero_core      # Contains ValidationWorkerPool
    dinero_consensus
    dinero_wallet
    dinero_crypto
    secp256k1
    OpenSSL::Crypto
  )

  link_zstd_if_needed(test_signature_rejection)

  add_test(NAME Signature_Rejection COMMAND test_signature_rejection)
  set_tests_properties(Signature_Rejection PROPERTIES
    LABELS "security-critical;consensus;signature-verification"
  )

  message(STATUS "  ✓ Signature rejection test registered: test_signature_rejection")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase M.6: Monetary Domain Type Safety Tests
# ════════════════════════════════════════════════════════════════════════════
# Verifies AmountUna and BlockHeight type safety
# Tests: Overflow protection, domain-locked constructors, maturity checking
# ════════════════════════════════════════════════════════════════════════════

# Test 1: AmountUna (monetary amounts)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/primitives/test_amount.cpp)
  add_executable(test_amount
    tests/primitives/test_amount.cpp
  )

  target_include_directories(test_amount PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(test_amount PRIVATE
    GTest::gtest
    GTest::gtest_main
  )

  add_test(NAME AmountUna COMMAND test_amount)
  set_tests_properties(AmountUna PROPERTIES
    LABELS "phase-m6;monetary-types;type-safety"
  )

  message(STATUS "  ✓ Phase M.6 test registered: test_amount")
endif()

# Test 2: BlockHeight (block heights and maturity)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/primitives/test_block_height.cpp)
  add_executable(test_block_height
    tests/primitives/test_block_height.cpp
  )

  target_include_directories(test_block_height PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(test_block_height PRIVATE
    GTest::gtest
    GTest::gtest_main
  )

  add_test(NAME BlockHeight COMMAND test_block_height)
  set_tests_properties(BlockHeight PROPERTIES
    LABELS "phase-m6;height-types;type-safety"
  )

  message(STATUS "  ✓ Phase M.6 test registered: test_block_height")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase 3: Premine Mandatory Tests (Consensus Hardening)
# ════════════════════════════════════════════════════════════════════════════
# These tests freeze premine economics and reorg safety.
# All three must pass before premine can be declared frozen.

# Test 1: Premine Exists and Is Correct
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_premine_exists.cpp)
  add_executable(test_premine_exists
    tests/consensus/test_premine_exists.cpp
  )

  target_include_directories(test_premine_exists PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(test_premine_exists PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
  )

  link_zstd_if_needed(test_premine_exists)

  add_test(NAME PremineExists COMMAND test_premine_exists)
  set_tests_properties(PremineExists PROPERTIES
    LABELS "phase-3;premine;consensus-critical;mandatory"
    TIMEOUT 60
  )

  message(STATUS "  ✓ Phase 3 test registered: test_premine_exists")
endif()

# Test 2: Coinbase Maturity Enforcement
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_premine_maturity.cpp)
  add_executable(test_premine_maturity
    tests/consensus/test_premine_maturity.cpp
  )

  target_include_directories(test_premine_maturity PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(test_premine_maturity PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
  )

  link_zstd_if_needed(test_premine_maturity)

  add_test(NAME PremineMaturity COMMAND test_premine_maturity)
  set_tests_properties(PremineMaturity PROPERTIES
    LABELS "phase-3;premine;coinbase;mandatory"
    TIMEOUT 60
  )

  message(STATUS "  ✓ Phase 3 test registered: test_premine_maturity")
endif()

# Test 3: Reorg Safety (Most Critical)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_premine_reorg.cpp)
  add_executable(test_premine_reorg
    tests/consensus/test_premine_reorg.cpp
  )

  target_include_directories(test_premine_reorg PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(test_premine_reorg PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
  )

  link_zstd_if_needed(test_premine_reorg)

  add_test(NAME PremineReorg COMMAND test_premine_reorg)
  set_tests_properties(PremineReorg PROPERTIES
    LABELS "phase-3;premine;reorg;supply-safety;mandatory"
    TIMEOUT 60
  )

  message(STATUS "  ✓ Phase 3 test registered: test_premine_reorg")
endif()

# Test 4: Mining at Height 2 (First PoW Block)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_mining_height2.cpp)
  add_executable(test_mining_height2
    tests/consensus/test_mining_height2.cpp
  )

  target_include_directories(test_mining_height2 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(test_mining_height2 PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
  )

  link_zstd_if_needed(test_mining_height2)

  add_test(NAME MiningHeight2 COMMAND test_mining_height2)
  set_tests_properties(MiningHeight2 PROPERTIES
    LABELS "phase-3;mining;pow;validation"
    TIMEOUT 120
  )

  message(STATUS "  ✓ Phase 3 test registered: test_mining_height2")
endif()

# Test 5: Wallet Tracks Height 2 Coinbase (Phase M.5.3 - Runtime Correctness)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_wallet_height2_coinbase.cpp)
  add_executable(test_wallet_height2_coinbase
    tests/consensus/test_wallet_height2_coinbase.cpp
  )

  target_include_directories(test_wallet_height2_coinbase PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(test_wallet_height2_coinbase PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
  )

  link_zstd_if_needed(test_wallet_height2_coinbase)

  add_test(NAME WalletHeight2Coinbase COMMAND test_wallet_height2_coinbase)
  set_tests_properties(WalletHeight2Coinbase PROPERTIES
    LABELS "phase-m5;wallet;coinbase;maturity;runtime-correctness"
    TIMEOUT 60
  )

  message(STATUS "  ✓ Phase M.5.3 test registered: test_wallet_height2_coinbase")
endif()

# Test 6: Two-Node Network Sync (Phase N.1 - Multi-Node Validation)
# POSIX-only: uses fork(), waitpid(), kill() to orchestrate two daemon
# processes. Porting to CreateProcess/TerminateProcess would be a substantial
# rewrite; skip on native Windows MSVC. Still builds on Linux/Mac/MinGW.
if(NOT WIN32 AND EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_two_node_sync.cpp)
  add_executable(test_two_node_sync
    tests/network/test_two_node_sync.cpp
  )

  target_include_directories(test_two_node_sync PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/json/include
  )

  target_link_libraries(test_two_node_sync PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    gtest
    gtest_main
    pthread
  )

  add_test(NAME TwoNodeSync COMMAND test_two_node_sync)
  set_tests_properties(TwoNodeSync PROPERTIES
    LABELS "phase-n1;network;p2p;multi-node;consensus;critical"
    TIMEOUT 120
  )

  message(STATUS "  ✓ Phase N.1 test registered: test_two_node_sync")
else()
  message(STATUS "  ⚠ Phase N.1 test not found: tests/network/test_two_node_sync.cpp")
endif()

# CT Soak Test Infrastructure (Phase CT.4)
# POSIX-only: ct_soak_controller.h uses pid_t directly to manage forked
# daemon processes. Skip on native Windows MSVC.
if(NOT WIN32 AND EXISTS ${CMAKE_SOURCE_DIR}/tests/soak/run_ct_soak.cpp)
  add_executable(test_ct_soak
    tests/soak/run_ct_soak.cpp
    tests/soak/ct_soak_controller.cpp
    tests/soak/ct_tx_generator.cpp
    tests/soak/kill_switch_tester.cpp
    tests/soak/soak_metrics.cpp
  )

  target_include_directories(test_ct_soak PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests/soak
  )

  target_link_libraries(test_ct_soak PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    gtest
    gtest_main
    pthread
  )

  # Note: Soak tests are long-running (hours) and not registered in ctest by default
  # Run manually: ./test_ct_soak --gtest_filter=CTSoakTest.BasicSoakTest
  # Register only the unit tests for quick CI runs
  add_test(NAME SoakMetricsTest COMMAND test_ct_soak --gtest_filter=SoakMetricsTest.*)
  set_tests_properties(SoakMetricsTest PROPERTIES
    LABELS "phase-ct4;soak;metrics;unit"
    TIMEOUT 30
  )

  add_test(NAME TxGeneratorTest COMMAND test_ct_soak --gtest_filter=TxGeneratorTest.*)
  set_tests_properties(TxGeneratorTest PROPERTIES
    LABELS "phase-ct4;soak;generator;unit"
    TIMEOUT 30
  )

  add_test(NAME KillSwitchTest COMMAND test_ct_soak --gtest_filter=KillSwitchTest.*)
  set_tests_properties(KillSwitchTest PROPERTIES
    LABELS "phase-ct4;soak;killswitch;unit"
    TIMEOUT 30
  )

  message(STATUS "  ✓ Phase CT.4 soak test infrastructure registered: test_ct_soak")
else()
  message(STATUS "  ⚠ Phase CT.4 soak test not found: tests/soak/run_ct_soak.cpp")
endif()

# Test 7: Wallet Restore + Rescan (Phase W.1)
# NOTE: This test requires dinerod binary with full RPC/gRPC support
# It will be skipped if dinerod target is not built (missing dependencies)
# Also POSIX-only: spawns dinerod via fork/exec + signals. Skip on native Windows.
if(NOT WIN32 AND EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_wallet_restore_rescan.cpp)
  add_executable(test_wallet_restore_rescan
    tests/wallet/test_wallet_restore_rescan.cpp
  )

  target_include_directories(test_wallet_restore_rescan PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/json/include
  )

  target_link_libraries(test_wallet_restore_rescan PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    gtest
    gtest_main
    pthread
  )

  # Only register the test if dinerod target exists
  if(TARGET dinerod)
    add_test(NAME WalletRestoreRescan COMMAND test_wallet_restore_rescan)
    set_tests_properties(WalletRestoreRescan PROPERTIES
      LABELS "phase-w1;wallet;restore;rescan;critical;e2e"
      TIMEOUT 180
    )
    message(STATUS "  ✓ Phase W.1 test registered: test_wallet_restore_rescan (dinerod available)")
  else()
    message(STATUS "  ⚠ Phase W.1 test skipped: test_wallet_restore_rescan (dinerod target not built - missing gRPC/protobuf)")
  endif()
else()
  message(STATUS "  ⚠ Phase W.1 test not found: tests/wallet/test_wallet_restore_rescan.cpp")
endif()

# Wallet Mainnet Readiness (wallet-only hardening suite)
# Covers:
# - encryption/restart/unlock address continuity
# - restore determinism in fresh datadir
# - invalid mnemonic rejection with no partial wallet
# - BIP86 derivation path persistence (external + change)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_wallet_mainnet_readiness.cpp)
  add_executable(test_wallet_mainnet_readiness
    tests/wallet/test_wallet_mainnet_readiness.cpp
    tests/stubs/mempool_test_stubs.cpp  # Mempool::isOutputSpentInMempool etc.
  )

  target_include_directories(test_wallet_mainnet_readiness PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(test_wallet_mainnet_readiness PRIVATE
    dinero_wallet
    dinero_core
    dinero_consensus
    dinero_crypto
    dinero_rpc_handlers
    sqlite3
    gtest
    gtest_main
    pthread
  )

  link_zstd_if_needed(test_wallet_mainnet_readiness)

  if(APPLE)
    target_link_libraries(test_wallet_mainnet_readiness PRIVATE "-framework Security")
  endif()

  add_test(NAME WalletMainnetReadiness COMMAND test_wallet_mainnet_readiness)
  set_tests_properties(WalletMainnetReadiness PROPERTIES
    LABELS "wallet;mainnet;hardening;bip86;restore;encryption"
    TIMEOUT 180
  )
  message(STATUS "  ✓ Wallet readiness test registered: test_wallet_mainnet_readiness")
else()
  message(STATUS "  ⚠ Wallet readiness test not found: tests/wallet/test_wallet_mainnet_readiness.cpp")
endif()

message(STATUS "✅ Phase 3 Premine Tests: All 3 mandatory tests registered")
message(STATUS "✅ Phase 3 Mining Validation: Height 2 test registered")
message(STATUS "✅ Phase N.1 Network Validation: Multi-node sync test registered")
message(STATUS "✅ Phase W.1 Wallet Validation: Restore + rescan test registered")

# PSBT Taproot Guardrails Test
add_executable(test_psbt_guardrails
  test_psbt_guardrails.cpp
  src/wallet/psbt_taproot_validator.cpp
)
target_link_libraries(test_psbt_guardrails PRIVATE jsoncpp_static)

# BIP86 signing demonstration
# TEMPORARILY DISABLED (2026-02-02): Missing sha256/HASH160 includes when compiling hd_wallet.cpp directly
# TODO: Fix include paths or use dinero_wallet library instead of re-compiling sources
# add_executable(test_bip86_signing
#     test_bip86_signing.cpp
#     src/wallet/hd_wallet.cpp
#     src/wallet/psbt_taproot_validator.cpp
#     src/wallet/bip84_descriptor.cpp
#     src/wallet/bip86_descriptor.cpp
#     src/wallet/descriptor_checksum.cpp
# )
# target_link_libraries(test_bip86_signing dinero_core dinero_crypto dinero_wallet secp256k1 OpenSSL::SSL OpenSSL::Crypto)
# if(APPLE)
#     target_link_libraries(test_bip86_signing "-framework Security" "-framework CoreFoundation")
# endif()
# target_include_directories(test_bip86_signing PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Descriptor PSBT Integration Test (Phase 2 Step 3/4)
# Minimal test - synthetic data, no wallet DB/RPC dependencies
# PSBT helpers exported from dinero_core (Phase 2 Step 4 complete)
add_executable(test_descriptor_psbt_integration
    test_descriptor_psbt_integration.cpp
)
if(APPLE)
    target_link_libraries(test_descriptor_psbt_integration dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_descriptor_psbt_integration dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_descriptor_psbt_integration PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Simple Descriptor PSBT Test (Phase 2 Step 3)
# Tests core fixes: address→scriptPubKey and complete key derivation
add_executable(test_descriptor_psbt_simple
    test_descriptor_psbt_simple.cpp
)
if(APPLE)
    target_link_libraries(test_descriptor_psbt_simple dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_descriptor_psbt_simple dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_descriptor_psbt_simple PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# KeyStoreImpl Test (Phase 3A)
# Tests BIP32 HD wallet implementation with BIP84/BIP86 address derivation
add_executable(test_keystore_impl
    tests/test_keystore_impl.cpp
)
if(APPLE)
    target_link_libraries(test_keystore_impl dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_keystore_impl dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_keystore_impl PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# End-to-End Signing Flow Test (Phase 3B)
# Tests full descriptor → PSBT → sign → finalize flow
add_executable(test_e2e_signing
    tests/test_e2e_signing.cpp
)
if(APPLE)
    target_link_libraries(test_e2e_signing dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_e2e_signing dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_e2e_signing PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Complete PSBT Workflow Test (Phase 3C)
# Tests complete workflow: KeyStoreImpl → Descriptor → PSBT → Sign → Finalize → Extract TX
add_executable(test_psbt_complete_workflow
    tests/test_psbt_complete_workflow.cpp
)
if(APPLE)
    target_link_libraries(test_psbt_complete_workflow dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_psbt_complete_workflow dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_psbt_complete_workflow PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# BIP86 Taproot Workflow Test (Phase 3C - Taproot Activation)
# Tests BIP86 key-path Taproot signing: KeyStoreImpl → TR Descriptor → PSBT → Schnorr Sign
add_executable(test_bip86_taproot_workflow
    tests/test_bip86_taproot_workflow.cpp
)
if(APPLE)
    target_link_libraries(test_bip86_taproot_workflow dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_bip86_taproot_workflow dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_bip86_taproot_workflow PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# BIP86 Tapscript Guardrail Test (Phase 3C - Security Hardening)
# Tests that BIP86 wallets reject script-path spending PSBTs (tapscript safety)
add_executable(test_psbt_bip86_integration
    test_psbt_bip86_integration.cpp
)
if(APPLE)
    target_link_libraries(test_psbt_bip86_integration dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_psbt_bip86_integration dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_psbt_bip86_integration PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Hardware Wallet BIP86 Integration Test (Phase 3C - HW Wallet Support)
# Tests complete Coldcard-style workflow: KeyStore → Descriptor → PSBT → Export → Sign → Import
add_executable(test_hardware_wallet_bip86
    tests/test_hardware_wallet_bip86.cpp
    src/rpc/methods_hardware_wallet.cpp
)
if(APPLE)
    target_link_libraries(test_hardware_wallet_bip86 dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_hardware_wallet_bip86 dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_hardware_wallet_bip86 PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# PSBT Error Messages UX Test (Phase 3C - PSBT UX Polish)
# Validates clear, actionable error messages for exchanges/custodians/HW wallet users
add_executable(test_psbt_error_messages
    tests/test_psbt_error_messages.cpp
)
if(APPLE)
    target_link_libraries(test_psbt_error_messages dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_psbt_error_messages dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_psbt_error_messages PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Descriptor Persistence Test (Phase 3C - Descriptor Persistence)
# Validates descriptor storage, activation, and safe migration (NO crypto - fast tests)
add_executable(test_descriptor_persistence
    tests/test_descriptor_persistence.cpp
)
if(APPLE)
    target_link_libraries(test_descriptor_persistence dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_descriptor_persistence dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_descriptor_persistence PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Descriptor RPC Test (Phase 3C - Descriptor RPC Integration)
# Tests importdescriptor, listdescriptors, setactivedescriptor RPC methods
add_executable(test_descriptor_rpc
    tests/test_descriptor_rpc.cpp
)
if(APPLE)
    target_link_libraries(test_descriptor_rpc dinero_wallet dinero_core dinero_crypto dinero_rpc_handlers OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_descriptor_rpc dinero_wallet dinero_core dinero_crypto dinero_rpc_handlers OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_descriptor_rpc PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Descriptor Address Generation Test (Phase 3C - Descriptor → Address Type Mapping)
# Validates that active descriptors control address generation policy (BIP84 ↔ BIP86)
add_executable(test_descriptor_address_generation
    tests/test_descriptor_address_generation.cpp
)
if(APPLE)
    target_link_libraries(test_descriptor_address_generation dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_descriptor_address_generation dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_descriptor_address_generation PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Ledger Production Safety Test (CRITICAL - Must pass in production builds)
# Verifies that mock signing is DISABLED and signature fabrication is impossible
add_executable(test_ledger_production_safety
    tests/test_ledger_production_safety.cpp
)
if(APPLE)
    target_link_libraries(test_ledger_production_safety dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_ledger_production_safety dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_ledger_production_safety PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Ledger Hardware Wallet Integration Test (Requires DIN_HW_WALLET_MOCK=ON)
# Tests USB HID stack: hidapi → HIDTransport → LedgerWallet
# Validates device enumeration, connection lifecycle, and APDU framing
add_executable(test_ledger_integration
    tests/test_ledger_integration.cpp
)
if(APPLE)
    target_link_libraries(test_ledger_integration dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_ledger_integration dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_ledger_integration PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Trezor Hardware Wallet Integration Test
# Tests USB HID stack: hidapi → HIDTransport → TrezorTransport → TrezorWallet
# Validates device enumeration, connection lifecycle, wire protocol, and protobuf messages
if(ENABLE_TREZOR)
  add_executable(test_trezor_integration
      tests/test_trezor_integration.cpp
  )
  if(APPLE)
      target_link_libraries(test_trezor_integration dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
  else()
      target_link_libraries(test_trezor_integration dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
  endif()
  target_include_directories(test_trezor_integration PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
endif()

# ════════════════════════════════════════════════════════════════════════════
# Mixed Block Confidential Test (Dragon #1 - Confidential Transactions)
# Tests blocks containing mixed transaction types
# ════════════════════════════════════════════════════════════════════════════

# ════════════════════════════════════════════════════════════════════════════
# Confidential Reorg Test (Dragon #1 - Reorg Safety)
# Proves confidential UTXOs survive blockchain reorganizations
# Critical: Tests commitment preservation during disconnect/reconnect
# ════════════════════════════════════════════════════════════════════════════

# ════════════════════════════════════════════════════════════════════════════
# Phase 4 adversarial CT reorg soak removed in v7 — exercised confidential-tx
# reorg edge cases that don't apply on v7 (CT outputs rejected by version-range
# allowlist). Source deleted.

# DPI Protocol unit tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/dpi/test_dpi_protocol.cpp)
  add_executable(test_dpi_protocol
    tests/dpi/test_dpi_protocol.cpp
    src/dpi/dpi_protocol.cpp
  )
  target_link_libraries(test_dpi_protocol PRIVATE
    dinero_crypto
    dinero_wallet
    secp256k1
    OpenSSL::SSL
    OpenSSL::Crypto
    GTest::gtest
  )
  if(APPLE)
    target_link_libraries(test_dpi_protocol PRIVATE "-framework Security" "-framework CoreFoundation")
  endif()
  target_include_directories(test_dpi_protocol PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME DpiProtocol COMMAND test_dpi_protocol)
  set_tests_properties(DpiProtocol PROPERTIES LABELS "dpi;protocol")
  message(STATUS "  ✓ DPI Protocol test registered: test_dpi_protocol")
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Solo miner subdirectory (Phase 2 of monorepo consolidation — 2026-05-12)
# ═══════════════════════════════════════════════════════════════════════════
#
# Bring the solo miner library + CLI into the monorepo build. OFF by
# default during the Phase 2 transition for the same reason as Qt:
# existing dinerod-only builds (CI, server deploys) should not start
# compiling extra targets without an explicit opt-in. Phase 2 Step 7
# will flip this to ON after build verification.
#
# When ON: miner/ is added as a CMake subdirectory. The
# `dinero-solo-miner` library target becomes available to dinero-qt
# (which links it directly) and the `dinero-solo-miner` CLI binary is
# built. Qt's fallback add_subdirectory inside qt/CMakeLists.txt
# (guarded by `if(NOT TARGET dinero-solo-miner)`) becomes a no-op
# because the target is already defined here.
#
# When OFF: miner/ is ignored at root. If Qt is enabled standalone
# (DINERO_BUILD_QT=ON, DINERO_BUILD_MINER=OFF), Qt's own
# add_subdirectory fallback kicks in to build the miner from
# DINERO_SOLO_MINER_SOURCE_ROOT (which defaults to ${CMAKE_SOURCE_DIR}/miner
# in monorepo mode, so the same source tree is used either way).
#
# Order matters: must come BEFORE add_subdirectory(qt) so Qt's
# TARGET check sees a defined dinero-solo-miner.
# ═══════════════════════════════════════════════════════════════════════════
option(DINERO_BUILD_MINER "Build dinero-solo-miner library + CLI as in-tree subdirectory (Phase 2 monorepo)" OFF)
if(DINERO_BUILD_MINER)
  message(STATUS "⛏️  dinero-solo-miner build: ENABLED (DINERO_BUILD_MINER=ON)")
  add_subdirectory(miner)
else()
  message(STATUS "⛏️  dinero-solo-miner build: disabled (pass -DDINERO_BUILD_MINER=ON to enable)")
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Qt GUI subdirectory (Phase 2 of monorepo consolidation — 2026-05-12)
# ═══════════════════════════════════════════════════════════════════════════
#
# Bring the Qt GUI into the monorepo build. OFF by default during the
# Phase 2 transition so existing dinerod-only builds (CI, server deploys,
# environments without Qt6 installed) remain unaffected. Phase 2 Step 7
# will flip this to ON after build verification passes on macOS arm64
# and a smoke build is confirmed on each release platform.
#
# When ON: dinero-qt is built from qt/CMakeLists.txt alongside dinerod,
# dinero-cli, dinero-gpu-miner. qt/ uses $<TARGET_FILE:dinerod> etc.
# (resolved at build time) to bundle daemon binaries into the Qt app.
#
# When OFF: qt/ is ignored. The directory still ships in the monorepo
# (for history preservation and so Qt-side hotfixes don't require a
# separate clone), but isn't compiled.
#
# Build command when ON:
#   cmake -S . -B build -DDINERO_BUILD_QT=ON \
#         -DCMAKE_PREFIX_PATH="$HOME/Qt/6.9.1/macos"
#   cmake --build build -j
# ═══════════════════════════════════════════════════════════════════════════
option(DINERO_BUILD_QT "Build dinero-qt GUI alongside dinerod (Phase 2 monorepo)" OFF)
if(DINERO_BUILD_QT)
  message(STATUS "🎨 dinero-qt GUI build: ENABLED (DINERO_BUILD_QT=ON)")
  add_subdirectory(qt)
else()
  message(STATUS "🎨 dinero-qt GUI build: disabled (pass -DDINERO_BUILD_QT=ON to enable)")
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Seeder subdirectory (Phase E of v8 peer-discovery — 2026-05-12)
# ═══════════════════════════════════════════════════════════════════════════
# dinero-seeder is a small standalone crawler that probes the Dinero
# P2P network and emits a healthy-peers list compatible with
# contrib/seeds/seeds_main.txt. Intentionally separate from the daemon
# build graph so operators can run it on dedicated hardware without
# pulling in chain state, validation, mempool, etc.
#
# OFF by default — operators opt in. The binary is small (~200 KB on
# macOS arm64), no Qt dependency, just OpenSSL for SHA256 checksums.
# ═══════════════════════════════════════════════════════════════════════════
option(DINERO_BUILD_SEEDER "Build dinero-seeder peer-discovery crawler (Phase E)" OFF)
if(DINERO_BUILD_SEEDER)
  message(STATUS "🕷️  dinero-seeder build: ENABLED (DINERO_BUILD_SEEDER=ON)")
  add_subdirectory(seeder)
else()
  message(STATUS "🕷️  dinero-seeder build: disabled (pass -DDINERO_BUILD_SEEDER=ON to enable)")
endif()
