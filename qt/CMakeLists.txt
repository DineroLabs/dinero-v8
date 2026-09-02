# ═══════════════════════════════════════════════════════════════════════════════
# Dinero-Qt - Desktop Wallet GUI
# ═══════════════════════════════════════════════════════════════════════════════
#
# Standalone Qt6 GUI wallet for DineroCoin.
# Connects to dinerod via RPC - completely separate from consensus code.
#
# Why separate?
#   - GUI bugs ≠ consensus bugs
#   - GUI crashes must not kill the node
#   - GUI updates must not require consensus rebuilds
#   - GUI attack surface is huge and unrelated to validation
#
# Build:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$HOME/Qt/6.9.1/macos"
#   cmake --build build -j8
#
# ═══════════════════════════════════════════════════════════════════════════════

cmake_minimum_required(VERSION 3.21)
# In a monorepo release the root owns the shared version. Keep a standalone
# fallback for legacy direct builds of qt/.
if(NOT DEFINED DINERO_MONOREPO_VERSION)
  set(DINERO_MONOREPO_VERSION 8.1.10)
endif()
project(dinero-qt VERSION ${DINERO_MONOREPO_VERSION} LANGUAGES CXX)

option(DIN_ENABLE_LIQUIDITY_VAULT_UI
  "Expose the custodial Liquidity Vault panel in dinero-qt" OFF)

# ─────────────────────────────────────────────────────────────────────────────
# Qt firewall — directory-scope include reset (Phase 2 Step 7 finding, 2026-05-12)
# ─────────────────────────────────────────────────────────────────────────────
# Reset directory-level INCLUDE_DIRECTORIES inherited from the parent
# (root) scope. Root CMakeLists.txt:313 has
#   include_directories(${CMAKE_SOURCE_DIR}/include)
# which is a directory-level (not target-level) call. Under add_subdirectory,
# CMake propagates that into qt's scope, silently leaking the daemon's
# consensus/chainstate/validation header tree into every target defined
# below — a real firewall breach that the QtFirewallProbe CTest caught
# during the first end-to-end monorepo build (commit b84ce919).
#
# Resetting here at the top of qt/CMakeLists.txt, before any target is
# defined, isolates qt from root's global include surface. qt then
# explicitly adds the include paths it actually needs via
# target_include_directories() further down (currently just the solo
# miner headers and optional QRENCODE). An audit of qt/src and qt/tests
# confirmed no real qt code #includes anything from root's globals — the
# leakage was structural, not functional.
#
# Trade-off: any future qt source that wants a root-global header (e.g.
# something from third_party/) must now wire it explicitly via
# target_include_directories. That's the correct pattern.
# ─────────────────────────────────────────────────────────────────────────────
set_property(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "")

if(APPLE)
  set(DINERO_TOR_BUNDLE_DIR "" CACHE PATH
      "Verified Tor Expert Bundle directory produced by scripts/fetch-tor-expert-bundle.sh")
  enable_language(OBJCXX)
endif()

include(CTest)

find_package(Git QUIET)

function(resolve_git_identity REPO_PATH PREFIX)
  if(GIT_FOUND AND EXISTS "${REPO_PATH}/.git")
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
      WORKING_DIRECTORY ${REPO_PATH}
      OUTPUT_VARIABLE ${PREFIX}_GIT_HASH
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
      WORKING_DIRECTORY ${REPO_PATH}
      OUTPUT_VARIABLE ${PREFIX}_GIT_FULL
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
  else()
    set(${PREFIX}_GIT_HASH "unknown")
    set(${PREFIX}_GIT_FULL "unknown")
  endif()
  set(${PREFIX}_GIT_HASH "${${PREFIX}_GIT_HASH}" PARENT_SCOPE)
  set(${PREFIX}_GIT_FULL "${${PREFIX}_GIT_FULL}" PARENT_SCOPE)
endfunction()

function(prefer_first_existing_binary OUT_VAR)
  foreach(CANDIDATE IN LISTS ARGN)
    if(CANDIDATE AND EXISTS "${CANDIDATE}")
      set(${OUT_VAR} "${CANDIDATE}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${OUT_VAR} "" PARENT_SCOPE)
endfunction()

# Source-root defaults: monorepo-aware.
#
# When this CMakeLists is included via add_subdirectory(qt) from the
# monorepo root, the daemon source and the solo miner are siblings of
# qt/ inside the same tree. When this CMakeLists is built standalone
# (legacy mode, retained until v8.0.0 ships and external builds are
# retired), source roots fall back to the historical
# `${CMAKE_CURRENT_SOURCE_DIR}/..` layout.
if(NOT PROJECT_IS_TOP_LEVEL)
  # Monorepo: qt/ lives inside the dinero tree; miner/ is a peer of qt/.
  set(_DEFAULT_DINERO_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
  set(_DEFAULT_DINERO_SOLO_MINER_SOURCE_ROOT "${CMAKE_SOURCE_DIR}/miner")
  set(DINERO_QT_RELEASE_REPO "dinero-v8")
else()
  # Standalone qt-only build (the original mode).
  set(_DEFAULT_DINERO_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../dinero")
  set(_DEFAULT_DINERO_SOLO_MINER_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../dinero-solo-miner")
  set(DINERO_QT_RELEASE_REPO "dinero-qt")
endif()

set(DINERO_SOURCE_ROOT "${_DEFAULT_DINERO_SOURCE_ROOT}" CACHE PATH "Source checkout for bundled dinerod/gpu miner")
set(DINERO_SOLO_MINER_SOURCE_ROOT "${_DEFAULT_DINERO_SOLO_MINER_SOURCE_ROOT}" CACHE PATH "Source checkout for embedded solo miner")

# ─────────────────────────────────────────────────────────────────────────────
# PRE-v8.0.0 RELEASE BLOCKER (logged 2026-05-12, consolidation Phase 2)
# ─────────────────────────────────────────────────────────────────────────────
# DINERO_MINER_SOURCE_ROOT, DINERO_STRATUM_SOURCE_ROOT, and DINERO_SV2_SOURCE_ROOT
# remain external siblings during the Phase 2 consolidation work. dinero-miner
# (Windows CPU miner) ships inside the official Windows Qt bundle today by
# embedding a pre-built binary from this external repo. Before v8.0.0 final,
# decide one of:
#   (a) Subtree-merge dinero-miner into the monorepo, so the unified tag
#       produces the Windows CPU miner from the same commit.
#   (b) Accept that the v8.0.0 "one tag = all binaries" promise has an
#       asterisk on Windows, with dinero-miner pinned to an external version.
#   (c) Drop the bundled CPU miner from the Windows package entirely.
# Until that decision is made, "one tag = all binaries" is conditional on
# Windows. Stratum and dinero-sv2 stay external by design; only dinero-miner
# is the unresolved policy item.
# ─────────────────────────────────────────────────────────────────────────────
set(DINERO_MINER_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../dinero-miner" CACHE PATH "Source checkout for bundled CPU miner (external, pre-v8 release blocker)")
set(DINERO_STRATUM_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../stratum" CACHE PATH "Source checkout for standalone stratum release checks (external)")
set(DINERO_SV2_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../dinero-sv2" CACHE PATH "Source checkout for bundled SV2 pool miners (external)")

set(DINERO_CORE_CHAIN_BUNDLE_HEADER "${DINERO_SOURCE_ROOT}/include/consensus/chain_bundle_generated.h")
set(DINERO_CORE_CHAIN_IDENTITY_HEADER "${DINERO_SOURCE_ROOT}/include/consensus/chain_identity.h")
set(DINERO_SOLO_CHAIN_IDENTITY_HEADER "${DINERO_SOLO_MINER_SOURCE_ROOT}/include/solo_miner/chain_identity.h")

option(DINERO_QT_ENABLE_SOLO_CUDA "Build the embedded solo miner with CUDA support on Windows/Linux" ON)
if(DINERO_QT_ENABLE_SOLO_CUDA AND NOT APPLE)
  set(MINER_ENABLE_CUDA ON CACHE BOOL "Enable CUDA mining backend for embedded solo miner" FORCE)
endif()

if(EXISTS "${DINERO_CORE_CHAIN_BUNDLE_HEADER}" AND
   EXISTS "${DINERO_CORE_CHAIN_IDENTITY_HEADER}" AND
   EXISTS "${DINERO_SOLO_CHAIN_IDENTITY_HEADER}")
  file(READ "${DINERO_CORE_CHAIN_BUNDLE_HEADER}" DINERO_CORE_CHAIN_BUNDLE_TEXT)
  file(READ "${DINERO_CORE_CHAIN_IDENTITY_HEADER}" DINERO_CORE_CHAIN_IDENTITY_TEXT)
  file(READ "${DINERO_SOLO_CHAIN_IDENTITY_HEADER}" DINERO_SOLO_CHAIN_IDENTITY_TEXT)

  string(REGEX MATCH "GENESIS_BLOCK_HASH[^\"]*\"([0-9a-fA-F]+)\""
         _CORE_MAINNET_MATCH "${DINERO_CORE_CHAIN_BUNDLE_TEXT}")
  set(DINERO_CORE_MAINNET_GENESIS "${CMAKE_MATCH_1}")
  string(REGEX MATCH "kTestnetGenesisHash[^\"]*\"([0-9a-fA-F]+)\""
         _CORE_TESTNET_MATCH "${DINERO_CORE_CHAIN_IDENTITY_TEXT}")
  set(DINERO_CORE_TESTNET_GENESIS "${CMAKE_MATCH_1}")

  string(REGEX MATCH "kMainnetGenesisHash[^\"]*\"([0-9a-fA-F]+)\""
         _SOLO_MAINNET_MATCH "${DINERO_SOLO_CHAIN_IDENTITY_TEXT}")
  set(DINERO_SOLO_MAINNET_GENESIS "${CMAKE_MATCH_1}")
  string(REGEX MATCH "kTestnetGenesisHash[^\"]*\"([0-9a-fA-F]+)\""
         _SOLO_TESTNET_MATCH "${DINERO_SOLO_CHAIN_IDENTITY_TEXT}")
  set(DINERO_SOLO_TESTNET_GENESIS "${CMAKE_MATCH_1}")
  string(REGEX MATCH "kRegtestGenesisHash[^\"]*\"([0-9a-fA-F]+)\""
         _SOLO_REGTEST_MATCH "${DINERO_SOLO_CHAIN_IDENTITY_TEXT}")
  set(DINERO_SOLO_REGTEST_GENESIS "${CMAKE_MATCH_1}")

  if(NOT DINERO_CORE_MAINNET_GENESIS OR
     NOT DINERO_CORE_TESTNET_GENESIS OR
     NOT DINERO_SOLO_MAINNET_GENESIS OR
     NOT DINERO_SOLO_TESTNET_GENESIS OR
     NOT DINERO_SOLO_REGTEST_GENESIS)
    message(FATAL_ERROR
      "Could not parse chain identity headers.\n"
      "  core bundle: ${DINERO_CORE_CHAIN_BUNDLE_HEADER}\n"
      "  core identity: ${DINERO_CORE_CHAIN_IDENTITY_HEADER}\n"
      "  solo identity: ${DINERO_SOLO_CHAIN_IDENTITY_HEADER}")
  endif()

  if(NOT DINERO_SOLO_MAINNET_GENESIS STREQUAL DINERO_CORE_MAINNET_GENESIS OR
     NOT DINERO_SOLO_REGTEST_GENESIS STREQUAL DINERO_CORE_MAINNET_GENESIS OR
     NOT DINERO_SOLO_TESTNET_GENESIS STREQUAL DINERO_CORE_TESTNET_GENESIS)
    message(FATAL_ERROR
      "Embedded solo miner chain identity is stale.\n"
      "Run: python ${DINERO_SOURCE_ROOT}/tools/sync_chain_identity_headers.py\n"
      "Core mainnet/regtest: ${DINERO_CORE_MAINNET_GENESIS}\n"
      "Solo mainnet:         ${DINERO_SOLO_MAINNET_GENESIS}\n"
      "Solo regtest:         ${DINERO_SOLO_REGTEST_GENESIS}\n"
      "Core testnet:         ${DINERO_CORE_TESTNET_GENESIS}\n"
      "Solo testnet:         ${DINERO_SOLO_TESTNET_GENESIS}")
  endif()
else()
  message(WARNING
    "Skipping solo miner chain identity parity check because one or more headers are missing:\n"
    "  ${DINERO_CORE_CHAIN_BUNDLE_HEADER}\n"
    "  ${DINERO_CORE_CHAIN_IDENTITY_HEADER}\n"
    "  ${DINERO_SOLO_CHAIN_IDENTITY_HEADER}")
endif()

# In-tree shortcut: if dinerod / dinero-cli / dinero-gpu-miner are CMake
# targets in this build (monorepo mode), use $<TARGET_FILE:...> generator
# expressions. These resolve at build time so first-configure ordering
# doesn't break Qt's bundle step. Standalone mode falls back to the
# prefer_first_existing_binary legacy logic below.

if(TARGET dinerod)
  set(DINEROD_BINARY "$<TARGET_FILE:dinerod>" CACHE FILEPATH "Bundled dinerod binary (from in-tree CMake target)" FORCE)
else()
  prefer_first_existing_binary(DINEROD_BINARY_AUTO
    "${DINERO_SOURCE_ROOT}/build/dinerod"
    "${DINERO_SOURCE_ROOT}/build/bin/dinerod"
    "${DINERO_SOURCE_ROOT}/build-release/dinerod"
  )

  set(DINEROD_BINARY_NEEDS_REFRESH OFF)
  if(NOT DEFINED DINEROD_BINARY OR DINEROD_BINARY STREQUAL "")
    set(DINEROD_BINARY_NEEDS_REFRESH ON)
  elseif(DINEROD_BINARY_AUTO)
    file(REAL_PATH "${DINERO_SOURCE_ROOT}" DINERO_SOURCE_ROOT_REAL)
    file(REAL_PATH "${DINEROD_BINARY}" DINEROD_BINARY_REAL BASE_DIRECTORY "${CMAKE_BINARY_DIR}")
    file(REAL_PATH "${DINEROD_BINARY_AUTO}" DINEROD_BINARY_AUTO_REAL BASE_DIRECTORY "${CMAKE_BINARY_DIR}")

    if(DINEROD_BINARY_REAL MATCHES "^${DINERO_SOURCE_ROOT_REAL}/build" AND
       NOT DINEROD_BINARY_REAL STREQUAL DINEROD_BINARY_AUTO_REAL)
      set(DINEROD_BINARY_NEEDS_REFRESH ON)
    endif()
  endif()

  if(DINEROD_BINARY_NEEDS_REFRESH)
    set(DINEROD_BINARY "${DINEROD_BINARY_AUTO}" CACHE FILEPATH "Bundled dinerod binary" FORCE)
  endif()
endif()

if(TARGET dinero-cli)
  set(CLI_BINARY "$<TARGET_FILE:dinero-cli>" CACHE FILEPATH "Bundled dinero-cli binary (from in-tree CMake target)" FORCE)
else()
  prefer_first_existing_binary(CLI_BINARY_AUTO
    "${DINERO_SOURCE_ROOT}/build/dinero-cli"
    "${DINERO_SOURCE_ROOT}/build/bin/dinero-cli"
    "${DINERO_SOURCE_ROOT}/build-release/dinero-cli"
  )

  set(CLI_BINARY_NEEDS_REFRESH OFF)
  if(NOT DEFINED CLI_BINARY OR CLI_BINARY STREQUAL "")
    set(CLI_BINARY_NEEDS_REFRESH ON)
  elseif(CLI_BINARY_AUTO)
    file(REAL_PATH "${DINERO_SOURCE_ROOT}" DINERO_SOURCE_ROOT_REAL)
    file(REAL_PATH "${CLI_BINARY}" CLI_BINARY_REAL BASE_DIRECTORY "${CMAKE_BINARY_DIR}")
    file(REAL_PATH "${CLI_BINARY_AUTO}" CLI_BINARY_AUTO_REAL BASE_DIRECTORY "${CMAKE_BINARY_DIR}")

    if(CLI_BINARY_REAL MATCHES "^${DINERO_SOURCE_ROOT_REAL}/build" AND
       NOT CLI_BINARY_REAL STREQUAL CLI_BINARY_AUTO_REAL)
      set(CLI_BINARY_NEEDS_REFRESH ON)
    endif()
  endif()

  if(CLI_BINARY_NEEDS_REFRESH)
    set(CLI_BINARY "${CLI_BINARY_AUTO}" CACHE FILEPATH "Bundled dinero-cli binary" FORCE)
  endif()
endif()

if(TARGET dinero-gpu-miner)
  set(GPU_MINER_BINARY "$<TARGET_FILE:dinero-gpu-miner>" CACHE FILEPATH "Bundled dinero-gpu-miner binary (from in-tree CMake target)" FORCE)
else()
  prefer_first_existing_binary(GPU_MINER_BINARY_AUTO
    "${DINERO_SOURCE_ROOT}/build/dinero-gpu-miner"
    "${DINERO_SOURCE_ROOT}/build/bin/dinero-gpu-miner"
    "${DINERO_SOURCE_ROOT}/build-release/dinero-gpu-miner"
  )

  set(GPU_MINER_BINARY_NEEDS_REFRESH OFF)
  if(NOT DEFINED GPU_MINER_BINARY OR GPU_MINER_BINARY STREQUAL "")
    set(GPU_MINER_BINARY_NEEDS_REFRESH ON)
  elseif(GPU_MINER_BINARY_AUTO)
    file(REAL_PATH "${DINERO_SOURCE_ROOT}" DINERO_SOURCE_ROOT_REAL)
    file(REAL_PATH "${GPU_MINER_BINARY}" GPU_MINER_BINARY_REAL BASE_DIRECTORY "${CMAKE_BINARY_DIR}")
    file(REAL_PATH "${GPU_MINER_BINARY_AUTO}" GPU_MINER_BINARY_AUTO_REAL BASE_DIRECTORY "${CMAKE_BINARY_DIR}")

    if(GPU_MINER_BINARY_REAL MATCHES "^${DINERO_SOURCE_ROOT_REAL}/build" AND
       NOT GPU_MINER_BINARY_REAL STREQUAL GPU_MINER_BINARY_AUTO_REAL)
      set(GPU_MINER_BINARY_NEEDS_REFRESH ON)
    endif()
  endif()

  if(GPU_MINER_BINARY_NEEDS_REFRESH)
    set(GPU_MINER_BINARY "${GPU_MINER_BINARY_AUTO}" CACHE FILEPATH "Bundled dinero-gpu-miner binary" FORCE)
  endif()
endif()

prefer_first_existing_binary(CPU_MINER_BINARY_AUTO
  "${DINERO_MINER_SOURCE_ROOT}/build-msvc-native/bin/Release/dinero-miner${CMAKE_EXECUTABLE_SUFFIX}"
  "${DINERO_MINER_SOURCE_ROOT}/build/bin/Release/dinero-miner${CMAKE_EXECUTABLE_SUFFIX}"
  "${DINERO_MINER_SOURCE_ROOT}/build/bin/dinero-miner${CMAKE_EXECUTABLE_SUFFIX}"
  "${DINERO_MINER_SOURCE_ROOT}/build-msvc-native/bin/Release/dinero-miner"
  "${DINERO_SOURCE_ROOT}/build/dinero-miner"
  "${DINERO_SOURCE_ROOT}/build/bin/dinero-miner"
  "${DINERO_SOURCE_ROOT}/build-release/dinero-miner"
)

set(CPU_MINER_BINARY_NEEDS_REFRESH OFF)
if(NOT DEFINED CPU_MINER_BINARY OR CPU_MINER_BINARY STREQUAL "")
  set(CPU_MINER_BINARY_NEEDS_REFRESH ON)
elseif(CPU_MINER_BINARY_AUTO)
  file(REAL_PATH "${DINERO_SOURCE_ROOT}" DINERO_SOURCE_ROOT_REAL)
  file(REAL_PATH "${CPU_MINER_BINARY}" CPU_MINER_BINARY_REAL BASE_DIRECTORY "${CMAKE_BINARY_DIR}")
  file(REAL_PATH "${CPU_MINER_BINARY_AUTO}" CPU_MINER_BINARY_AUTO_REAL BASE_DIRECTORY "${CMAKE_BINARY_DIR}")

  if(CPU_MINER_BINARY_REAL MATCHES "^${DINERO_SOURCE_ROOT_REAL}/build" AND
     NOT CPU_MINER_BINARY_REAL STREQUAL CPU_MINER_BINARY_AUTO_REAL)
    set(CPU_MINER_BINARY_NEEDS_REFRESH ON)
  endif()
endif()

if(CPU_MINER_BINARY_NEEDS_REFRESH)
  set(CPU_MINER_BINARY "${CPU_MINER_BINARY_AUTO}" CACHE FILEPATH "Bundled dinero-miner binary" FORCE)
endif()

set(CPU_MINER_RUNTIME_DLLS "" CACHE STRING "Semicolon-separated runtime DLLs required by the bundled CPU miner")

prefer_first_existing_binary(STRATUM_WORKER_BINARY_AUTO
  "${DINERO_SOURCE_ROOT}/build/dinero-stratum-worker"
  "${DINERO_SOURCE_ROOT}/build/bin/dinero-stratum-worker"
  "${DINERO_SOURCE_ROOT}/build-release/dinero-stratum-worker"
)

set(STRATUM_WORKER_BINARY_NEEDS_REFRESH OFF)
if(NOT DEFINED STRATUM_WORKER_BINARY OR STRATUM_WORKER_BINARY STREQUAL "")
  set(STRATUM_WORKER_BINARY_NEEDS_REFRESH ON)
elseif(STRATUM_WORKER_BINARY_AUTO)
  file(REAL_PATH "${DINERO_SOURCE_ROOT}" DINERO_SOURCE_ROOT_REAL)
  file(REAL_PATH "${STRATUM_WORKER_BINARY}" STRATUM_WORKER_BINARY_REAL BASE_DIRECTORY "${CMAKE_BINARY_DIR}")
  file(REAL_PATH "${STRATUM_WORKER_BINARY_AUTO}" STRATUM_WORKER_BINARY_AUTO_REAL BASE_DIRECTORY "${CMAKE_BINARY_DIR}")

  if(STRATUM_WORKER_BINARY_REAL MATCHES "^${DINERO_SOURCE_ROOT_REAL}/build" AND
     NOT STRATUM_WORKER_BINARY_REAL STREQUAL STRATUM_WORKER_BINARY_AUTO_REAL)
    set(STRATUM_WORKER_BINARY_NEEDS_REFRESH ON)
  endif()
endif()

if(STRATUM_WORKER_BINARY_NEEDS_REFRESH)
  set(STRATUM_WORKER_BINARY "${STRATUM_WORKER_BINARY_AUTO}" CACHE FILEPATH "Bundled dinero-stratum-worker binary" FORCE)
endif()

prefer_first_existing_binary(STRATUM_SERVER_BINARY_AUTO
  "${DINERO_STRATUM_SOURCE_ROOT}/build/bin/dinero-stratum"
  "${DINERO_STRATUM_SOURCE_ROOT}/build/dinero-stratum"
  "${DINERO_STRATUM_SOURCE_ROOT}/build-release/bin/dinero-stratum"
  "${DINERO_STRATUM_SOURCE_ROOT}/build-release/dinero-stratum"
)

set(STRATUM_SERVER_BINARY_NEEDS_REFRESH OFF)
if(NOT DEFINED STRATUM_SERVER_BINARY OR STRATUM_SERVER_BINARY STREQUAL "")
  set(STRATUM_SERVER_BINARY_NEEDS_REFRESH ON)
elseif(STRATUM_SERVER_BINARY_AUTO)
  file(REAL_PATH "${DINERO_STRATUM_SOURCE_ROOT}" DINERO_STRATUM_SOURCE_ROOT_REAL)
  file(REAL_PATH "${STRATUM_SERVER_BINARY}" STRATUM_SERVER_BINARY_REAL BASE_DIRECTORY "${CMAKE_BINARY_DIR}")
  file(REAL_PATH "${STRATUM_SERVER_BINARY_AUTO}" STRATUM_SERVER_BINARY_AUTO_REAL BASE_DIRECTORY "${CMAKE_BINARY_DIR}")

  if(STRATUM_SERVER_BINARY_REAL MATCHES "^${DINERO_STRATUM_SOURCE_ROOT_REAL}/build" AND
     NOT STRATUM_SERVER_BINARY_REAL STREQUAL STRATUM_SERVER_BINARY_AUTO_REAL)
    set(STRATUM_SERVER_BINARY_NEEDS_REFRESH ON)
  endif()
endif()

if(STRATUM_SERVER_BINARY_NEEDS_REFRESH)
  set(STRATUM_SERVER_BINARY "${STRATUM_SERVER_BINARY_AUTO}" CACHE FILEPATH "Bundled dinero-stratum server binary" FORCE)
endif()

prefer_first_existing_binary(SV2_MINER_BINARY_AUTO
  "${DINERO_SV2_SOURCE_ROOT}/target/release/dinero-sv2-miner"
)

if(NOT DEFINED SV2_MINER_BINARY OR SV2_MINER_BINARY STREQUAL "")
  set(SV2_MINER_BINARY "${SV2_MINER_BINARY_AUTO}" CACHE FILEPATH "Bundled dinero-sv2-miner binary" FORCE)
endif()

prefer_first_existing_binary(SV2_GPU_MINER_BINARY_AUTO
  "${DINERO_SV2_SOURCE_ROOT}/target/release/dinero-sv2-gpu-miner"
)

if(NOT DEFINED SV2_GPU_MINER_BINARY OR SV2_GPU_MINER_BINARY STREQUAL "")
  set(SV2_GPU_MINER_BINARY "${SV2_GPU_MINER_BINARY_AUTO}" CACHE FILEPATH "Bundled dinero-sv2-gpu-miner binary" FORCE)
endif()

resolve_git_identity("${CMAKE_SOURCE_DIR}" DINERO_QT)
resolve_git_identity("${DINERO_SOURCE_ROOT}" DINERO_SOURCE)
resolve_git_identity("${DINERO_SOLO_MINER_SOURCE_ROOT}" DINERO_SOLO_MINER_SOURCE)
resolve_git_identity("${DINERO_STRATUM_SOURCE_ROOT}" DINERO_STRATUM_SOURCE)
if(DINERO_SOLO_MINER_SOURCE_ROOT STREQUAL "${DINERO_SOURCE_ROOT}/miner")
  set(DINERO_SOLO_MINER_SOURCE_GIT_HASH "${DINERO_SOURCE_GIT_HASH}")
  set(DINERO_SOLO_MINER_SOURCE_GIT_FULL "${DINERO_SOURCE_GIT_FULL}")
endif()
string(TIMESTAMP DINERO_QT_BUILD_TIME "%Y-%m-%dT%H:%M:%S+0000" UTC)
# Use CMAKE_CURRENT_SOURCE_DIR (qt/) not CMAKE_SOURCE_DIR. In standalone
# qt builds the two are identical, but in the monorepo CMAKE_SOURCE_DIR
# is the dinero root and this template lives at qt/cmake/.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/release_identity.json.in
               ${CMAKE_BINARY_DIR}/release-identity.json @ONLY)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ═══════════════════════════════════════════════════════════════════════════════
# Options
# ═══════════════════════════════════════════════════════════════════════════════

option(DIN_EXPERIMENTAL_FEATURES "Enable experimental GUI features (NOT FOR PRODUCTION)" OFF)
option(DIN_ENABLE_WEBSOCKETS "Enable WebSocket support for real-time updates" ON)
option(DIN_ENABLE_QRENCODE "Enable QR code generation (requires libqrencode)" OFF)

if(DIN_EXPERIMENTAL_FEATURES)
  message(STATUS "🧪 Experimental features ENABLED (development build)")
else()
  message(STATUS "✅ Experimental features DISABLED (production build)")
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Dependencies
# ═══════════════════════════════════════════════════════════════════════════════

# Find Qt6 (required)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
message(STATUS "Found Qt6: ${Qt6_VERSION}")

if(BUILD_TESTING)
  find_package(Qt6 REQUIRED COMPONENTS Test)
endif()

# WebSockets (optional)
if(DIN_ENABLE_WEBSOCKETS)
  find_package(Qt6 QUIET COMPONENTS WebSockets)
  if(Qt6WebSockets_FOUND)
    message(STATUS "  ✅ Qt6WebSockets found - real-time updates enabled")
    set(HAVE_WEBSOCKETS ON)
  else()
    message(STATUS "  ⚠️  Qt6WebSockets not found - WebSocket support disabled")
    set(HAVE_WEBSOCKETS OFF)
  endif()
endif()

# QuickWidgets (optional for QML-based mining UI)
find_package(Qt6 QUIET COMPONENTS QuickWidgets)
if(Qt6QuickWidgets_FOUND)
  # DISABLED: QML causes segfault - use widgets-based mining tab
  set(HAVE_QT_QUICK OFF)
  message(STATUS "  ⚠️  Qt Quick disabled (stability issues)")
else()
  set(HAVE_QT_QUICK OFF)
endif()

# QR Code generation (optional)
if(DIN_ENABLE_QRENCODE)
  find_path(QRENCODE_INCLUDE_DIR qrencode.h)
  find_library(QRENCODE_LIB qrencode)
  if(QRENCODE_LIB AND QRENCODE_INCLUDE_DIR)
    message(STATUS "  ✅ libqrencode found - QR codes enabled")
    set(HAVE_QRENCODE ON)
    set(DIN_QR_BACKEND "libqrencode")
  else()
    message(STATUS "  ⚠️  libqrencode not found - using native/built-in QR generator")
    set(HAVE_QRENCODE OFF)
  endif()
else()
  set(HAVE_QRENCODE OFF)
endif()

if(NOT DEFINED DIN_QR_BACKEND)
  if(APPLE)
    set(DIN_QR_BACKEND "macOS CoreImage")
  else()
    set(DIN_QR_BACKEND "unavailable")
  endif()
endif()

# Sparkle framework for auto-updates (macOS, disabled for now)
set(HAVE_SPARKLE OFF)

# ═══════════════════════════════════════════════════════════════════════════════
# Solo Miner Library
# ═══════════════════════════════════════════════════════════════════════════════
#
# In the monorepo, miner/ is added by the root CMakeLists.txt
# (add_subdirectory(miner) — Phase 2 Step 3). In standalone Qt builds,
# the miner sources live in a sibling directory and Qt adds them itself.
# The TARGET check makes both modes work without double-adding.

if(APPLE AND NOT DEFINED MINER_ENABLE_METAL)
  set(MINER_ENABLE_METAL ON CACHE BOOL
      "Enable the embedded Metal solo-mining backend for Dinero Qt")
endif()

if(NOT TARGET dinero-solo-miner)
  add_subdirectory(${DINERO_SOLO_MINER_SOURCE_ROOT} ${CMAKE_BINARY_DIR}/solo-miner)
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Source Files
# ═══════════════════════════════════════════════════════════════════════════════

set(GUI_SOURCES
  src/main.cpp
  src/rpcclient.cpp
  src/rpcclient.h
  src/changeaddressmanager.cpp
  src/changeaddressmanager.h
  src/transactiontracker.cpp
  src/transactiontracker.h
  src/walletnameutils.cpp
  src/walletnameutils.h
  src/advisorybanner.cpp
  src/advisorybanner.h
  src/connection_manager.cpp
  src/connection_manager.h
  src/mainwindow.cpp
  src/mainwindow.h
  src/walletwizard.cpp
  src/walletwizard.h
  src/QrUtil.cpp
  src/QrUtil.h
  src/qrcodegen.cpp
  src/qrcodegen.h
  src/debugconsole.cpp
  src/debugconsole.h
  src/hardwarewalletwidget.cpp
  src/hardwarewalletwidget.h
  src/minercontroller.cpp
  src/minercontroller.h
  src/aibackend.h
  src/aitoolexecutor.h
  src/aitoolexecutor.cpp
  src/claudeclibackend.h
  src/claudeclibackend.cpp
  src/httpaibackend.h
  src/httpaibackend.cpp
  src/aipanel.cpp
  src/aipanel.h
  src/scrollsupport.cpp
  src/scrollsupport.h
  src/aistatusstrip.cpp
  src/aistatusstrip.h
  src/dpiwidget.cpp
  src/dpiwidget.h
  # Phase 5 — Shielded pool surface (re-introduced post-v7-cleanup,
  # tied to the new daemon shielded RPCs).
  src/shieldedwidget.cpp
  src/shieldedwidget.h
  # Track C — Liquidity Vault (production, daemon-side custodial vault).
  src/vaultpanel.cpp
  src/vaultpanel.h
  # MyNodeDashboard — Phase 1 MVP (Tasks 1-9).
  src/dashboardtypes.h
  src/nodepoller.cpp
  src/nodepoller.h
  src/identitysection.cpp
  src/identitysection.h
  src/networksection.cpp
  src/networksection.h
  src/overviewconnectivitycard.cpp
  src/overviewconnectivitycard.h
  src/responsiveuipolicy.h
  src/peerssection.cpp
  src/peerssection.h
  src/mynodedashboard.cpp
  src/mynodedashboard.h
  src/cmdkpanel.cpp
  src/cmdkpanel.h
  src/sparklinewidget.cpp
  src/sparklinewidget.h
  src/contributionsection.cpp
  src/contributionsection.h
  src/discoverysection.cpp
  src/discoverysection.h
  src/topologysection.cpp
  src/topologysection.h
  src/dashboardactioncontroller.cpp
  src/dashboardactioncontroller.h
)

if(APPLE)
  list(APPEND GUI_SOURCES src/QrUtil_mac.mm)
endif()

# Experimental features (conditionally included)
if(DIN_EXPERIMENTAL_FEATURES)
  list(APPEND GUI_SOURCES
    src/bridgewidget.cpp
    src/bridgewidget.h
    src/paymentswidget.cpp
    src/paymentswidget.h
    src/escrowwidget.cpp
    src/escrowwidget.h
    src/marketplacewidget.cpp
    src/marketplacewidget.h
    src/websocketclient.cpp
    src/websocketclient.h
  )
endif()

# QML resources (if enabled)
if(HAVE_QT_QUICK)
  qt_add_resources(QML_RESOURCES qml/qml.qrc)
  list(APPEND GUI_SOURCES
    src/rpchelper.cpp
    src/rpchelper.h
    ${QML_RESOURCES}
  )
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Executable
# ═══════════════════════════════════════════════════════════════════════════════

if(APPLE)
  add_executable(dinero-qt MACOSX_BUNDLE ${GUI_SOURCES})

  set_target_properties(dinero-qt PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME "Dinero-Qt"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.dinerocoin.dinero-qt"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_ICON_FILE "Dinero.icns"
  )
elseif(WIN32)
  # Embed icons/dinero-qt.ico into dinero-qt.exe via a Windows resource
  # script. WIN32_EXECUTABLE also marks the binary as a GUI subsystem
  # exe so it doesn't open a console window. This is the Windows-side
  # parity for MACOSX_BUNDLE_ICON_FILE on macOS - same source artwork
  # (Dinero.png 1024x1024) regenerated at standard Windows sizes
  # (16/24/32/48/64/128/256).
  enable_language(RC)

  # Resolve the rc tag (e.g. "8.0.0-rc2") for the user-visible string fields
  # of VS_VERSION_INFO. The top-level CMakeLists.txt sets DINERO_RELEASE_TAG
  # as a CACHE var; if this qt subdir is being built standalone, fall back
  # to PROJECT_VERSION so we never embed an empty string.
  if(NOT DEFINED DINERO_RELEASE_TAG OR "${DINERO_RELEASE_TAG}" STREQUAL "")
    set(DINERO_RELEASE_TAG "${PROJECT_VERSION}")
  endif()
  set(DINERO_VI_FILEVERSION
      "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
  set(DINERO_VI_PRODUCTVERSION "${DINERO_VI_FILEVERSION}")
  set(DINERO_VI_VERSION_STRING "${DINERO_RELEASE_TAG}")
  # The .rc ICON directive needs a path relative to the .rc file's location
  # (or absolute). Since we configure into CMAKE_CURRENT_BINARY_DIR but the
  # .ico still lives in the source tree, pass an absolute path with forward
  # slashes (rc.exe / windres both accept those on Windows).
  file(TO_CMAKE_PATH
       "${CMAKE_CURRENT_SOURCE_DIR}/icons/dinero-qt.ico"
       DINERO_QT_ICO_PATH)

  set(_dinero_qt_rc "${CMAKE_CURRENT_BINARY_DIR}/dinero-qt.rc")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/icons/dinero-qt.rc.in"
    "${_dinero_qt_rc}"
    @ONLY
  )

  add_executable(dinero-qt WIN32 ${GUI_SOURCES} "${_dinero_qt_rc}")
else()
  add_executable(dinero-qt ${GUI_SOURCES})
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Linking
# ═══════════════════════════════════════════════════════════════════════════════

target_link_libraries(dinero-qt PRIVATE
  Qt6::Widgets
  Qt6::Network
  dinero-solo-miner
)

if(APPLE)
  target_link_libraries(dinero-qt PRIVATE
    "-framework AppKit"
    "-framework CoreImage"
    "-framework Foundation"
  )
elseif(WIN32)
  if(MSVC)
    target_link_libraries(dinero-qt PRIVATE ws2_32 crypt32 wldap32 bcrypt)
  else()
    # Static curl needs these system libs; append them as raw linker flags so they
    # appear AFTER all library references (fixes link-order issue with multiple curl copies).
    target_link_options(dinero-qt PRIVATE
      "LINKER:--push-state,--no-as-needed"
      -lws2_32 -lcrypt32 -lwldap32 -lbcrypt
      "LINKER:--pop-state"
    )
  endif()
endif()

# Solo miner includes
target_include_directories(dinero-qt PRIVATE
  ${DINERO_SOLO_MINER_SOURCE_ROOT}/include
)

if(HAVE_WEBSOCKETS)
  target_link_libraries(dinero-qt PRIVATE Qt6::WebSockets)
  target_compile_definitions(dinero-qt PRIVATE HAVE_WEBSOCKETS)
endif()

if(HAVE_QRENCODE)
  target_link_libraries(dinero-qt PRIVATE ${QRENCODE_LIB})
  target_include_directories(dinero-qt PRIVATE ${QRENCODE_INCLUDE_DIR})
  target_compile_definitions(dinero-qt PRIVATE DIN_HAVE_QRENCODE)
endif()

if(HAVE_QT_QUICK)
  target_link_libraries(dinero-qt PRIVATE Qt6::QuickWidgets)
  target_compile_definitions(dinero-qt PRIVATE HAVE_QT_QUICK)
endif()

if(DIN_EXPERIMENTAL_FEATURES)
  target_compile_definitions(dinero-qt PRIVATE DIN_EXPERIMENTAL_FEATURES)
endif()

if(DIN_ENABLE_LIQUIDITY_VAULT_UI)
  target_compile_definitions(dinero-qt PRIVATE DIN_ENABLE_LIQUIDITY_VAULT_UI=1)
endif()

target_compile_definitions(dinero-qt PRIVATE QT_NO_KEYWORDS)
if(WIN32)
  target_compile_definitions(dinero-qt PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
target_compile_definitions(dinero-qt PRIVATE
  DINERO_QT_REPO="${DINERO_QT_RELEASE_REPO}"
  DINERO_QT_COMPONENT="dinero-qt"
  DINERO_QT_VERSION="${PROJECT_VERSION}"
  DINERO_QT_GIT_SHA="${DINERO_QT_GIT_HASH}"
  DINERO_QT_GIT_FULL="${DINERO_QT_GIT_FULL}"
  DINERO_QT_BUILD_TIME="${DINERO_QT_BUILD_TIME}"
)

# Release tag for Help -> About display. Set explicitly per cut so the
# About dialog matches the published rc tag (CMake's PROJECT_VERSION
# only carries semver; rc/preview suffix lives here). Override at
# configure time with -DDINERO_RELEASE_TAG="...".
if(NOT DEFINED DINERO_RELEASE_TAG)
  set(DINERO_RELEASE_TAG "${PROJECT_VERSION}" CACHE STRING "User-visible release tag shown in Help -> About")
endif()
target_compile_definitions(dinero-qt PRIVATE DINERO_RELEASE_TAG="${DINERO_RELEASE_TAG}")
message(STATUS "dinero-qt release tag: ${DINERO_RELEASE_TAG}")

# Newer macOS SDKs removed AGL.framework. Some Qt builds still expose
# "-framework AGL" through WrapOpenGL::WrapOpenGL, which Qt6::Gui depends on.
# Strip AGL from WrapOpenGL to keep linking portable.
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
  execute_process(
    COMMAND xcrun --sdk macosx --show-sdk-path
    OUTPUT_VARIABLE MACOS_SDK_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(MACOS_SDK_PATH AND NOT EXISTS "${MACOS_SDK_PATH}/System/Library/Frameworks/AGL.framework/AGL")
    get_target_property(WRAP_OPENGL_LINK_IFACE WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    if(WRAP_OPENGL_LINK_IFACE)
      set(WRAP_OPENGL_LINK_IFACE_FILTERED "")
      foreach(LINK_ITEM IN LISTS WRAP_OPENGL_LINK_IFACE)
        if(LINK_ITEM STREQUAL "-framework AGL")
          continue()
        endif()
        if(LINK_ITEM MATCHES "AGL\\.framework")
          continue()
        endif()
        list(APPEND WRAP_OPENGL_LINK_IFACE_FILTERED "${LINK_ITEM}")
      endforeach()
      set_property(TARGET WrapOpenGL::WrapOpenGL PROPERTY INTERFACE_LINK_LIBRARIES "${WRAP_OPENGL_LINK_IFACE_FILTERED}")
      message(STATUS "Removed deprecated AGL framework from WrapOpenGL::WrapOpenGL")
    endif()
  endif()
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# macOS Bundle Configuration
# ═══════════════════════════════════════════════════════════════════════════════

if(APPLE)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  # Copy resources to build directory
  configure_file(${CMAKE_SOURCE_DIR}/Dinero-Coin.png
                 ${CMAKE_BINARY_DIR}/Dinero-Coin.png COPYONLY)
  configure_file(${CMAKE_SOURCE_DIR}/Dinero.png
                 ${CMAKE_BINARY_DIR}/Dinero.png COPYONLY)
  configure_file(${CMAKE_SOURCE_DIR}/icons/Dinero.icns
                 ${CMAKE_BINARY_DIR}/Dinero.icns COPYONLY)

  # Resolve macdeployqt now, but run it after bundling the daemon/resources so
  # a stalled packaging step does not leave the bundle with a stale dinerod.
  find_program(MACDEPLOYQT_EXECUTABLE macdeployqt HINTS
    /opt/homebrew/bin
    $ENV{HOME}/Qt/6.9.1/macos/bin
    /usr/local/bin
  )
  if(MACDEPLOYQT_EXECUTABLE)
    message(STATUS "macdeployqt found: ${MACDEPLOYQT_EXECUTABLE}")
  else()
    message(WARNING "macdeployqt not found - .app bundle will not include Qt frameworks")
  endif()

  # Copy app icon to bundle
  add_custom_command(TARGET dinero-qt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinerod"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-cli"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-seeder"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-gpu-miner"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-miner"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-stratum"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-stratum-worker"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-solo-miner"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-sv2-miner"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-sv2-gpu-miner"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-qt.bak"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinerod"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-cli"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-seeder"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-gpu-miner"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-miner"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-stratum"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-stratum-worker"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-solo-miner"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-sv2-miner"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-sv2-gpu-miner"
    COMMENT "Clearing stale embedded binaries..."
  )

  if(DINERO_TOR_BUNDLE_DIR)
    if(NOT EXISTS "${DINERO_TOR_BUNDLE_DIR}/tor/tor" OR
       NOT EXISTS "${DINERO_TOR_BUNDLE_DIR}/DINERO_TOR_VERSION")
      message(FATAL_ERROR "DINERO_TOR_BUNDLE_DIR is not a verified extracted bundle")
    endif()
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/tor"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${DINERO_TOR_BUNDLE_DIR}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/tor"
      COMMAND chmod +x
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/tor/tor/tor"
      COMMENT "Bundling verified Tor Expert Bundle..."
    )
    message(STATUS "Will bundle verified Tor Expert Bundle from ${DINERO_TOR_BUNDLE_DIR}")
  endif()

  add_custom_command(TARGET dinero-qt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
      "${CMAKE_SOURCE_DIR}/icons/Dinero.icns"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/Dinero.icns"
    COMMENT "Installing app icon..."
  )

  # Copy logo for QR codes
  add_custom_command(TARGET dinero-qt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
      "${CMAKE_SOURCE_DIR}/Dinero-Coin.png"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/Dinero-Coin.png"
    COMMENT "Installing logo for QR codes..."
  )

  add_custom_command(TARGET dinero-qt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
      "${CMAKE_BINARY_DIR}/release-identity.json"
      "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/release-identity.json"
    COMMENT "Installing release identity manifest..."
  )

  # Bundle dinerod inside .app/Contents/Resources/.
  #
  # Monorepo (TARGET dinerod exists): use generator expression with explicit
  # target dependency. The configure-time `if(EXISTS ...)` check below can't
  # see $<TARGET_FILE:dinerod> because that resolves at build time, not
  # configure time — without this branch, the bundle step gets silently
  # skipped in clean builds.
  #
  # Standalone (legacy): fall back to the prebuilt-sibling-binary path.
  if(TARGET dinerod)
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinerod>"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinerod"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinerod"
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinerod>"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinerod"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinerod"
      COMMENT "Bundling in-tree dinerod into dinero-qt.app..."
    )
    add_dependencies(dinero-qt dinerod)
    message(STATUS "Will bundle dinerod from in-tree CMake target")
  elseif(DINEROD_BINARY AND EXISTS "${DINEROD_BINARY}")
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "${DINEROD_BINARY}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinerod"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinerod"
      COMMAND ${CMAKE_COMMAND} -E copy
        "${DINEROD_BINARY}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinerod"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinerod"
      COMMENT "Bundling pre-built dinerod daemon..."
    )
    message(STATUS "Will bundle dinerod from: ${DINEROD_BINARY}")
  endif()

  if(TARGET dinero-cli)
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-cli>"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-cli"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-cli"
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-cli>"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-cli"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-cli"
      COMMENT "Bundling in-tree dinero-cli into dinero-qt.app..."
    )
    add_dependencies(dinero-qt dinero-cli)
    message(STATUS "Will bundle dinero-cli from in-tree CMake target")
  elseif(CLI_BINARY AND EXISTS "${CLI_BINARY}")
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "${CLI_BINARY}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-cli"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-cli"
      COMMAND ${CMAKE_COMMAND} -E copy
        "${CLI_BINARY}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-cli"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-cli"
      COMMENT "Bundling dinero-cli..."
    )
    message(STATUS "Will bundle dinero-cli from: ${CLI_BINARY}")
  endif()

  if(TARGET dinero-seeder)
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-seeder>"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-seeder"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-seeder"
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-seeder>"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-seeder"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-seeder"
      COMMENT "Bundling in-tree dinero-seeder into dinero-qt.app..."
    )
    add_dependencies(dinero-qt dinero-seeder)
    message(STATUS "Will bundle dinero-seeder from in-tree CMake target")
  endif()

  # Bundle GPU miner (macOS only - Metal backend)
  if(TARGET dinero-gpu-miner)
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-gpu-miner>"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-gpu-miner"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-gpu-miner"
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-gpu-miner>"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-gpu-miner"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-gpu-miner"
      COMMENT "Bundling in-tree GPU miner..."
    )
    add_dependencies(dinero-qt dinero-gpu-miner)
    message(STATUS "Will bundle GPU miner from in-tree CMake target")
  elseif(GPU_MINER_BINARY AND EXISTS "${GPU_MINER_BINARY}")
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "${GPU_MINER_BINARY}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-gpu-miner"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-gpu-miner"
      COMMAND ${CMAKE_COMMAND} -E copy
        "${GPU_MINER_BINARY}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-gpu-miner"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-gpu-miner"
      COMMENT "Bundling GPU miner..."
    )
    message(STATUS "Will bundle GPU miner from: ${GPU_MINER_BINARY}")
  endif()

  if(TARGET dinero-miner)
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-miner>"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-miner"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-miner"
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-miner>"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-miner"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-miner"
      COMMENT "Bundling in-tree CPU miner..."
    )
    add_dependencies(dinero-qt dinero-miner)
    message(STATUS "Will bundle CPU miner from in-tree CMake target")
  elseif(CPU_MINER_BINARY AND EXISTS "${CPU_MINER_BINARY}")
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "${CPU_MINER_BINARY}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-miner"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-miner"
      COMMAND ${CMAKE_COMMAND} -E copy
        "${CPU_MINER_BINARY}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-miner"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-miner"
      COMMENT "Bundling CPU miner..."
    )
    message(STATUS "Will bundle CPU miner from: ${CPU_MINER_BINARY}")
  endif()

  if(TARGET dinero-stratum-worker)
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-stratum-worker>"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-stratum-worker"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-stratum-worker"
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-stratum-worker>"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-stratum-worker"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-stratum-worker"
      COMMENT "Bundling in-tree Stratum worker..."
    )
    add_dependencies(dinero-qt dinero-stratum-worker)
    message(STATUS "Will bundle Stratum worker from in-tree CMake target")
  elseif(STRATUM_WORKER_BINARY AND EXISTS "${STRATUM_WORKER_BINARY}")
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "${STRATUM_WORKER_BINARY}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-stratum-worker"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-stratum-worker"
      COMMAND ${CMAKE_COMMAND} -E copy
        "${STRATUM_WORKER_BINARY}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-stratum-worker"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-stratum-worker"
      COMMENT "Bundling Stratum worker..."
    )
    message(STATUS "Will bundle Stratum worker from: ${STRATUM_WORKER_BINARY}")
  endif()

  if(STRATUM_SERVER_BINARY AND EXISTS "${STRATUM_SERVER_BINARY}")
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "${STRATUM_SERVER_BINARY}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-stratum"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-stratum"
      COMMAND ${CMAKE_COMMAND} -E copy
        "${STRATUM_SERVER_BINARY}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-stratum"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-stratum"
      COMMENT "Bundling Stratum server..."
    )
    message(STATUS "Will bundle Stratum server from: ${STRATUM_SERVER_BINARY}")
  endif()

  if(TARGET dinero-solo-miner-cli)
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-solo-miner-cli>"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-solo-miner"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-solo-miner"
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-solo-miner-cli>"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-solo-miner"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-solo-miner"
      COMMENT "Bundling solo miner..."
    )
    message(STATUS "Will bundle solo miner from target dinero-solo-miner-cli")
  endif()

  if(SV2_MINER_BINARY AND EXISTS "${SV2_MINER_BINARY}")
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "${SV2_MINER_BINARY}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-sv2-miner"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-sv2-miner"
      COMMAND ${CMAKE_COMMAND} -E copy
        "${SV2_MINER_BINARY}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-sv2-miner"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-sv2-miner"
      COMMENT "Bundling SV2 CPU miner..."
    )
    message(STATUS "Will bundle SV2 CPU miner from: ${SV2_MINER_BINARY}")
  endif()

  if(SV2_GPU_MINER_BINARY AND EXISTS "${SV2_GPU_MINER_BINARY}")
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "${SV2_GPU_MINER_BINARY}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-sv2-gpu-miner"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/MacOS/dinero-sv2-gpu-miner"
      COMMAND ${CMAKE_COMMAND} -E copy
        "${SV2_GPU_MINER_BINARY}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-sv2-gpu-miner"
      COMMAND chmod +x "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Resources/dinero-sv2-gpu-miner"
      COMMENT "Bundling SV2 GPU miner..."
    )
    message(STATUS "Will bundle SV2 GPU miner from: ${SV2_GPU_MINER_BINARY}")
  endif()

  if(MACDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Frameworks"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/PlugIns"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/_CodeSignature"
      COMMENT "Clearing stale Qt frameworks before macdeployqt..."
    )
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_macdeployqt_sanitized.py"
        "${MACDEPLOYQT_EXECUTABLE}"
        "$<TARGET_BUNDLE_DIR:dinero-qt>"
      COMMENT "Running macdeployqt to bundle Qt frameworks..."
    )
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E rm -f
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/PlugIns/imageformats/libqpdf.dylib"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/PlugIns/iconengines/libqsvgicon.dylib"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/PlugIns/platforminputcontexts/libqtvirtualkeyboardplugin.dylib"
      COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Frameworks/QtPdf.framework"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Frameworks/QtSvg.framework"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Frameworks/QtVirtualKeyboard.framework"
        "$<TARGET_BUNDLE_DIR:dinero-qt>/Contents/Frameworks/QtVirtualKeyboardQml.framework"
      COMMENT "Removing unused Qt plugin/framework baggage from app bundle..."
    )
  endif()

  # Code signing — Developer ID for release, ad-hoc for dev builds
  # To sign for distribution: cmake -DRELEASE_SIGN=ON ..  (or pass via env)
  option(RELEASE_SIGN "Sign with Developer ID Application certificate for distribution" OFF)
  set(SIGN_IDENTITY "Developer ID Application: Mirsad Hajdarevic (JXJS6ZA5FJ)"
      CACHE STRING "Code signing identity")
  set(ENTITLEMENTS_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/entitlements.plist"
      CACHE STRING "Path to entitlements plist")

  if(RELEASE_SIGN)
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sign_macos_bundle.py"
              "$<TARGET_BUNDLE_DIR:dinero-qt>"
              --identity "${SIGN_IDENTITY}"
              --entitlements "${ENTITLEMENTS_PLIST}"
              --runtime
      COMMAND codesign --verify --deep --strict --verbose=2 "$<TARGET_BUNDLE_DIR:dinero-qt>"
      VERBATIM
      COMMENT "Code signing app bundle with Developer ID (release)..."
    )
  else()
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sign_macos_bundle.py"
              "$<TARGET_BUNDLE_DIR:dinero-qt>"
              --identity -
      COMMAND codesign --verify --deep --strict --verbose=2 "$<TARGET_BUNDLE_DIR:dinero-qt>"
      VERBATIM
      COMMENT "Code signing app bundle with ad-hoc signature and verifying..."
    )
  endif()
endif()

# Linux/Windows: bundle runtime tools next to dinero-qt
if(NOT APPLE)
  if(TARGET dinerod)
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinerod>"
        "$<TARGET_FILE_DIR:dinero-qt>/dinerod${CMAKE_EXECUTABLE_SUFFIX}"
      COMMENT "Bundling in-tree dinerod..."
    )
    add_dependencies(dinero-qt dinerod)
    message(STATUS "Will bundle dinerod from in-tree CMake target")
  elseif(DINEROD_BINARY AND EXISTS "${DINEROD_BINARY}")
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "${DINEROD_BINARY}"
        "$<TARGET_FILE_DIR:dinero-qt>/dinerod${CMAKE_EXECUTABLE_SUFFIX}"
      COMMENT "Bundling dinerod daemon..."
    )
    message(STATUS "Will bundle dinerod from: ${DINEROD_BINARY}")
  endif()

  if(TARGET dinero-cli)
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-cli>"
        "$<TARGET_FILE_DIR:dinero-qt>/dinero-cli${CMAKE_EXECUTABLE_SUFFIX}"
      COMMENT "Bundling in-tree dinero-cli..."
    )
    add_dependencies(dinero-qt dinero-cli)
    message(STATUS "Will bundle dinero-cli from in-tree CMake target")
  elseif(CLI_BINARY AND EXISTS "${CLI_BINARY}")
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "${CLI_BINARY}"
        "$<TARGET_FILE_DIR:dinero-qt>/dinero-cli${CMAKE_EXECUTABLE_SUFFIX}"
      COMMENT "Bundling dinero-cli..."
    )
    message(STATUS "Will bundle dinero-cli from: ${CLI_BINARY}")
  endif()

  if(TARGET dinero-seeder)
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-seeder>"
        "$<TARGET_FILE_DIR:dinero-qt>/dinero-seeder${CMAKE_EXECUTABLE_SUFFIX}"
      COMMENT "Bundling in-tree dinero-seeder..."
    )
    add_dependencies(dinero-qt dinero-seeder)
    message(STATUS "Will bundle dinero-seeder from in-tree CMake target")
  endif()

  if(TARGET dinero-gpu-miner)
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-gpu-miner>"
        "$<TARGET_FILE_DIR:dinero-qt>/dinero-gpu-miner${CMAKE_EXECUTABLE_SUFFIX}"
      COMMENT "Bundling in-tree dinero-gpu-miner..."
    )
    add_dependencies(dinero-qt dinero-gpu-miner)
    message(STATUS "Will bundle dinero-gpu-miner from in-tree CMake target")
  elseif(GPU_MINER_BINARY AND EXISTS "${GPU_MINER_BINARY}")
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "${GPU_MINER_BINARY}"
        "$<TARGET_FILE_DIR:dinero-qt>/dinero-gpu-miner${CMAKE_EXECUTABLE_SUFFIX}"
      COMMENT "Bundling dinero-gpu-miner..."
    )
    message(STATUS "Will bundle dinero-gpu-miner from: ${GPU_MINER_BINARY}")
  endif()

  if(TARGET dinero-miner)
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-miner>"
        "$<TARGET_FILE_DIR:dinero-qt>/dinero-miner${CMAKE_EXECUTABLE_SUFFIX}"
      COMMENT "Bundling in-tree dinero-miner..."
    )
    add_dependencies(dinero-qt dinero-miner)
    message(STATUS "Will bundle dinero-miner from in-tree CMake target")
  elseif(CPU_MINER_BINARY AND EXISTS "${CPU_MINER_BINARY}")
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "${CPU_MINER_BINARY}"
        "$<TARGET_FILE_DIR:dinero-qt>/dinero-miner${CMAKE_EXECUTABLE_SUFFIX}"
      COMMENT "Bundling dinero-miner..."
    )
    message(STATUS "Will bundle dinero-miner from: ${CPU_MINER_BINARY}")
  endif()

  foreach(CPU_MINER_DLL IN LISTS CPU_MINER_RUNTIME_DLLS)
    if(CPU_MINER_DLL AND EXISTS "${CPU_MINER_DLL}")
      get_filename_component(CPU_MINER_DLL_NAME "${CPU_MINER_DLL}" NAME)
      add_custom_command(TARGET dinero-qt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CPU_MINER_DLL}"
          "$<TARGET_FILE_DIR:dinero-qt>/${CPU_MINER_DLL_NAME}"
        COMMENT "Bundling dinero-miner runtime DLL: ${CPU_MINER_DLL}"
      )
      message(STATUS "Will bundle dinero-miner runtime DLL: ${CPU_MINER_DLL}")
    endif()
  endforeach()

  if(TARGET dinero-stratum-worker)
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-stratum-worker>"
        "$<TARGET_FILE_DIR:dinero-qt>/dinero-stratum-worker${CMAKE_EXECUTABLE_SUFFIX}"
      COMMENT "Bundling in-tree dinero-stratum-worker..."
    )
    add_dependencies(dinero-qt dinero-stratum-worker)
    message(STATUS "Will bundle dinero-stratum-worker from in-tree CMake target")
  elseif(STRATUM_WORKER_BINARY AND EXISTS "${STRATUM_WORKER_BINARY}")
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "${STRATUM_WORKER_BINARY}"
        "$<TARGET_FILE_DIR:dinero-qt>/dinero-stratum-worker${CMAKE_EXECUTABLE_SUFFIX}"
      COMMENT "Bundling dinero-stratum-worker..."
    )
    message(STATUS "Will bundle dinero-stratum-worker from: ${STRATUM_WORKER_BINARY}")
  endif()

  if(STRATUM_SERVER_BINARY AND EXISTS "${STRATUM_SERVER_BINARY}")
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "${STRATUM_SERVER_BINARY}"
        "$<TARGET_FILE_DIR:dinero-qt>/dinero-stratum${CMAKE_EXECUTABLE_SUFFIX}"
      COMMENT "Bundling dinero-stratum..."
    )
    message(STATUS "Will bundle dinero-stratum from: ${STRATUM_SERVER_BINARY}")
  endif()

  if(TARGET dinero-solo-miner-cli)
    add_custom_command(TARGET dinero-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:dinero-solo-miner-cli>"
        "$<TARGET_FILE_DIR:dinero-qt>/dinero-solo-miner${CMAKE_EXECUTABLE_SUFFIX}"
      COMMENT "Bundling dinero-solo-miner..."
    )
    message(STATUS "Will bundle solo miner from target dinero-solo-miner-cli")
  endif()
endif()

if(NOT APPLE)
  add_custom_command(TARGET dinero-qt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
      "${CMAKE_BINARY_DIR}/release-identity.json"
      "$<TARGET_FILE_DIR:dinero-qt>/release-identity.json"
    COMMENT "Copying release identity manifest..."
  )
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Installation
# ═══════════════════════════════════════════════════════════════════════════════

if(APPLE)
  install(TARGETS dinero-qt BUNDLE DESTINATION .)
  install(FILES ${CMAKE_SOURCE_DIR}/Dinero-Coin.png
    DESTINATION dinero-qt.app/Contents/Resources)
else()
  install(TARGETS dinero-qt RUNTIME DESTINATION bin)
  install(FILES ${CMAKE_SOURCE_DIR}/Dinero-Coin.png DESTINATION bin)
endif()

if(BUILD_TESTING)
  add_executable(test_responsive_ui_policy
    tests/test_responsive_ui_policy.cpp
    src/responsiveuipolicy.h
  )

  target_link_libraries(test_responsive_ui_policy PRIVATE
    Qt6::Core
    Qt6::Test
  )

  target_include_directories(test_responsive_ui_policy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_definitions(test_responsive_ui_policy PRIVATE QT_NO_KEYWORDS)
  add_test(NAME ResponsiveUiPolicy COMMAND test_responsive_ui_policy)

  add_executable(test_wallet_name_utils
    tests/test_wallet_name_utils.cpp
    src/walletnameutils.cpp
    src/walletnameutils.h
  )

  target_link_libraries(test_wallet_name_utils PRIVATE
    Qt6::Core
    Qt6::Test
  )

  target_compile_definitions(test_wallet_name_utils PRIVATE QT_NO_KEYWORDS)
  add_test(NAME WalletNameUtils COMMAND test_wallet_name_utils)

  add_executable(test_wallet_scoped_state
    tests/test_wallet_scoped_state.cpp
    src/rpcclient.cpp
    src/rpcclient.h
    src/walletnameutils.cpp
    src/walletnameutils.h
    src/transactiontracker.cpp
    src/transactiontracker.h
    src/advisorybanner.cpp
    src/advisorybanner.h
  )

  target_link_libraries(test_wallet_scoped_state PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Test
  )

  target_compile_definitions(test_wallet_scoped_state PRIVATE QT_NO_KEYWORDS)
  add_test(NAME WalletScopedState COMMAND test_wallet_scoped_state)

  add_executable(test_shielded_transfer_policy
    tests/test_shielded_transfer_policy.cpp
    src/shieldedtransferpolicy.h
  )
  target_link_libraries(test_shielded_transfer_policy PRIVATE
    Qt6::Core
    Qt6::Test
  )
  target_compile_definitions(test_shielded_transfer_policy PRIVATE QT_NO_KEYWORDS)
  add_test(NAME ShieldedTransferPolicy COMMAND test_shielded_transfer_policy)

  add_executable(test_node_poller
    tests/test_node_poller.cpp
    src/nodepoller.cpp
    src/nodepoller.h
    src/dashboardtypes.h
    src/rpcclient.cpp
    src/rpcclient.h
  )

  target_include_directories(test_node_poller PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(test_node_poller PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Test
  )

  target_compile_definitions(test_node_poller PRIVATE QT_NO_KEYWORDS)
  add_test(NAME NodePoller COMMAND test_node_poller)

  add_executable(test_identity_section
    tests/test_identity_section.cpp
    src/identitysection.cpp
    src/identitysection.h
    src/dashboardtypes.h
  )

  target_link_libraries(test_identity_section PRIVATE
    Qt6::Widgets
    Qt6::Test
  )

  target_include_directories(test_identity_section PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_definitions(test_identity_section PRIVATE QT_NO_KEYWORDS)
  add_test(NAME IdentitySection COMMAND test_identity_section)

  add_executable(test_network_section
    tests/test_network_section.cpp
    src/networksection.cpp
    src/networksection.h
    src/dashboardtypes.h
  )

  target_link_libraries(test_network_section PRIVATE
    Qt6::Widgets
    Qt6::Test
  )

  target_include_directories(test_network_section PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_definitions(test_network_section PRIVATE QT_NO_KEYWORDS)
  add_test(NAME NetworkSection COMMAND test_network_section)

  add_executable(test_overview_connectivity_card
    tests/test_overview_connectivity_card.cpp
    src/overviewconnectivitycard.cpp
    src/overviewconnectivitycard.h
  )

  target_link_libraries(test_overview_connectivity_card PRIVATE
    Qt6::Widgets
    Qt6::Test
  )

  target_include_directories(test_overview_connectivity_card PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_definitions(test_overview_connectivity_card PRIVATE QT_NO_KEYWORDS)
  add_test(NAME OverviewConnectivityCard COMMAND test_overview_connectivity_card)

  add_executable(test_peers_section
    tests/test_peers_section.cpp
    src/peerssection.cpp
    src/peerssection.h
    src/dashboardtypes.h
  )

  target_link_libraries(test_peers_section PRIVATE
    Qt6::Widgets
    Qt6::Test
  )

  target_include_directories(test_peers_section PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_definitions(test_peers_section PRIVATE QT_NO_KEYWORDS)
  add_test(NAME PeersSection COMMAND test_peers_section)

  add_executable(test_sparkline_widget
    tests/test_sparkline_widget.cpp
    src/sparklinewidget.cpp
    src/sparklinewidget.h
  )
  target_include_directories(test_sparkline_widget PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(test_sparkline_widget PRIVATE
    Qt6::Widgets
    Qt6::Test
  )
  target_compile_definitions(test_sparkline_widget PRIVATE QT_NO_KEYWORDS)
  add_test(NAME SparklineWidget COMMAND test_sparkline_widget)
  set_tests_properties(SparklineWidget PROPERTIES
    LABELS "qt;dashboard;smoke"
    TIMEOUT 10
  )

  add_executable(test_decentralization_score
    tests/test_decentralization_score.cpp
    src/nodepoller.cpp
    src/nodepoller.h
    src/dashboardtypes.h
    src/rpcclient.cpp
    src/rpcclient.h
  )
  target_include_directories(test_decentralization_score PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(test_decentralization_score PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Test
  )
  target_compile_definitions(test_decentralization_score PRIVATE QT_NO_KEYWORDS)
  add_test(NAME DecentralizationScore COMMAND test_decentralization_score)
  set_tests_properties(DecentralizationScore PROPERTIES
    LABELS "qt;dashboard;smoke"
    TIMEOUT 5
  )

  add_executable(test_relay_hints_parser
    tests/test_relay_hints_parser.cpp
    src/nodepoller.cpp
    src/nodepoller.h
    src/dashboardtypes.h
    src/rpcclient.cpp
    src/rpcclient.h
  )
  target_link_libraries(test_relay_hints_parser PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Test
  )
  target_compile_definitions(test_relay_hints_parser PRIVATE QT_NO_KEYWORDS)
  target_include_directories(test_relay_hints_parser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME RelayHintsParser COMMAND test_relay_hints_parser)
  set_tests_properties(RelayHintsParser PROPERTIES
    LABELS "qt;dashboard;smoke"
    TIMEOUT 5
  )

  add_executable(test_contribution_section
    tests/test_contribution_section.cpp
    src/contributionsection.cpp
    src/contributionsection.h
    src/sparklinewidget.cpp
    src/sparklinewidget.h
    src/dashboardtypes.h
  )
  target_link_libraries(test_contribution_section PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Test
  )
  target_compile_definitions(test_contribution_section PRIVATE QT_NO_KEYWORDS)
  target_include_directories(test_contribution_section PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ContributionSection COMMAND test_contribution_section)
  set_tests_properties(ContributionSection PROPERTIES
    LABELS "qt;dashboard;smoke"
    TIMEOUT 5
  )

  add_executable(test_discovery_section
    tests/test_discovery_section.cpp
    src/discoverysection.cpp
    src/discoverysection.h
    src/dashboardtypes.h
  )
  target_link_libraries(test_discovery_section PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Test
  )
  target_compile_definitions(test_discovery_section PRIVATE QT_NO_KEYWORDS)
  target_include_directories(test_discovery_section PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME DiscoverySection COMMAND test_discovery_section)
  set_tests_properties(DiscoverySection PROPERTIES
    LABELS "qt;dashboard;smoke"
    TIMEOUT 5
  )

  add_executable(test_topology_snapshot
    tests/test_topology_snapshot.cpp
    src/nodepoller.cpp
    src/nodepoller.h
    src/dashboardtypes.h
    src/rpcclient.cpp
    src/rpcclient.h
  )
  target_link_libraries(test_topology_snapshot PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Test
  )
  target_compile_definitions(test_topology_snapshot PRIVATE QT_NO_KEYWORDS)
  target_include_directories(test_topology_snapshot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME TopologySnapshot COMMAND test_topology_snapshot)
  set_tests_properties(TopologySnapshot PROPERTIES
    LABELS "qt;dashboard;smoke"
    TIMEOUT 5
  )

  add_executable(test_topology_section
    tests/test_topology_section.cpp
    src/topologysection.cpp
    src/topologysection.h
    src/dashboardtypes.h
  )
  target_link_libraries(test_topology_section PRIVATE
    Qt6::Widgets
    Qt6::Test
  )
  target_compile_definitions(test_topology_section PRIVATE QT_NO_KEYWORDS)
  target_include_directories(test_topology_section PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME TopologySection COMMAND test_topology_section)
  set_tests_properties(TopologySection PROPERTIES
    LABELS "qt;dashboard;smoke"
    TIMEOUT 5
  )

  add_executable(test_dashboard_actions
    tests/test_dashboard_actions.cpp
    src/dashboardactioncontroller.cpp
    src/dashboardactioncontroller.h
    src/dashboardtypes.h
    src/rpcclient.cpp
    src/rpcclient.h
  )
  target_link_libraries(test_dashboard_actions PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Test
  )
  target_compile_definitions(test_dashboard_actions PRIVATE QT_NO_KEYWORDS)
  target_include_directories(test_dashboard_actions PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME DashboardActions COMMAND test_dashboard_actions)
  set_tests_properties(DashboardActions PROPERTIES
    LABELS "qt;dashboard;smoke"
    TIMEOUT 5
  )

  # ─────────────────────────────────────────────────────────────────────────
  # Qt firewall compile-fail probe (Phase 2 Step 5 of monorepo consolidation)
  # ─────────────────────────────────────────────────────────────────────────
  #
  # This is the load-bearing piece of Phase 2: a probe target that
  # MUST fail to compile, enforcing the Qt-is-RPC-client architectural
  # contract at build time. See qt/tests/qt_firewall_violation.cpp
  # for the full contract.
  #
  # The probe is an OBJECT library (compile only, no link) that
  # inherits dinero-qt's INCLUDE_DIRECTORIES and COMPILE_DEFINITIONS
  # via generator expressions. It tries to #include a consensus
  # header. If Qt's include path can resolve it, the probe compiles
  # — firewall breach. If the include cannot be resolved, the probe
  # fails to compile — firewall holds.
  #
  # CTest entry uses WILL_FAIL TRUE to invert pass/fail: compile
  # failure → test passes. EXCLUDE_FROM_ALL keeps the probe out of
  # the default build so `cmake --build` only invokes it when
  # CTest does.
  #
  # Functional verification (forward + reverse breach test) is
  # deferred to Phase 2 Step 7's full build pass. Until then this
  # commit only wires the probe; it doesn't execute it.
  # ─────────────────────────────────────────────────────────────────────────
  add_library(qt_firewall_violation_probe OBJECT EXCLUDE_FROM_ALL
    tests/qt_firewall_violation.cpp
  )
  target_include_directories(qt_firewall_violation_probe PRIVATE
    $<TARGET_PROPERTY:dinero-qt,INCLUDE_DIRECTORIES>
  )
  target_compile_definitions(qt_firewall_violation_probe PRIVATE
    $<TARGET_PROPERTY:dinero-qt,COMPILE_DEFINITIONS>
  )
  set_target_properties(qt_firewall_violation_probe PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
  )
  add_test(
    NAME QtFirewallProbe
    COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}"
            --target qt_firewall_violation_probe
            --config $<CONFIG>
  )
  set_tests_properties(QtFirewallProbe PROPERTIES
    WILL_FAIL TRUE
    LABELS "qt;firewall;regression"
  )
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Summary
# ═══════════════════════════════════════════════════════════════════════════════

message(STATUS "")
message(STATUS "════════════════════════════════════════════════════")
message(STATUS "  Dinero-Qt ${PROJECT_VERSION}")
message(STATUS "════════════════════════════════════════════════════")
message(STATUS "  Qt Version:    ${Qt6_VERSION}")
message(STATUS "  Solo Miner:    ON (embedded library)")
message(STATUS "  WebSockets:    ${HAVE_WEBSOCKETS}")
message(STATUS "  QR Backend:    ${DIN_QR_BACKEND}")
message(STATUS "  Qt Quick:      ${HAVE_QT_QUICK}")
message(STATUS "  Experimental:  ${DIN_EXPERIMENTAL_FEATURES}")
message(STATUS "  Build Type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "════════════════════════════════════════════════════")
message(STATUS "")
