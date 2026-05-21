# Third-party dependency setup for the root build.
# Included from the repository root CMakeLists.txt so legacy targets,
# helper functions, and imported dependency targets remain in root scope.

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
set(DINERO_VENDORED_OPENSSL_VERSION "" CACHE STRING
  "Vendored OpenSSL version directory to use when DINERO_VENDORED_OPENSSL_DIR is unset")
set(DINERO_VENDORED_OPENSSL_DIR "" CACHE PATH
  "Override directory containing prebuilt vendored OpenSSL libraries")
set(DINERO_VENDORED_OPENSSL_SOURCE_DIR "" CACHE PATH
  "Override directory containing vendored OpenSSL source headers")

if(NOT USE_SYSTEM_OPENSSL AND DINERO_VENDORED_OPENSSL_VERSION STREQUAL "")
  if(DINERO_ENABLE_QUIC)
    set(DINERO_VENDORED_OPENSSL_VERSION "3.5.6" CACHE STRING
      "Vendored OpenSSL version directory to use when DINERO_VENDORED_OPENSSL_DIR is unset" FORCE)
  else()
    set(DINERO_VENDORED_OPENSSL_VERSION "3.3.2" CACHE STRING
      "Vendored OpenSSL version directory to use when DINERO_VENDORED_OPENSSL_DIR is unset" FORCE)
  endif()
endif()

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
  set(_base "${CMAKE_SOURCE_DIR}/third_party/openssl-${DINERO_VENDORED_OPENSSL_VERSION}")
  if(WIN32)
    set(_subdir "")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|X86_64)$")
      if(MSVC)
        set(_subdir "prebuilt/windows-x86_64-msvc")
      else()
        set(_subdir "prebuilt/windows-x86_64")
      endif()
    endif()

    if(NOT _subdir STREQUAL "" AND EXISTS "${_base}/${_subdir}")
      set(${out_dir} "${_base}/${_subdir}" PARENT_SCOPE)
    else()
      set(${out_dir} "${_base}" PARENT_SCOPE)
    endif()
    return()
  endif()

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_subdir "")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|X86_64)$")
      set(_subdir "prebuilt/linux-x86_64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
      set(_subdir "prebuilt/linux-aarch64")
    endif()

    if(NOT _subdir STREQUAL "" AND EXISTS "${_base}/${_subdir}")
      set(${out_dir} "${_base}/${_subdir}" PARENT_SCOPE)
    else()
      set(${out_dir} "${_base}" PARENT_SCOPE)
    endif()
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
  set(OPENSSL_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/openssl-${DINERO_VENDORED_OPENSSL_VERSION}")
  if(DINERO_VENDORED_OPENSSL_DIR)
    set(OPENSSL_ROOT_DIR "${DINERO_VENDORED_OPENSSL_DIR}")
    message(STATUS "Using explicit vendored OpenSSL dir: ${OPENSSL_ROOT_DIR}")
  else()
    dinero_default_vendored_openssl_dir(OPENSSL_ROOT_DIR)
  endif()
  if(DINERO_VENDORED_OPENSSL_SOURCE_DIR)
    set(OPENSSL_SOURCE_DIR "${DINERO_VENDORED_OPENSSL_SOURCE_DIR}")
    message(STATUS "Using explicit vendored OpenSSL source dir: ${OPENSSL_SOURCE_DIR}")
  elseif(DINERO_VENDORED_OPENSSL_DIR AND EXISTS "${DINERO_VENDORED_OPENSSL_DIR}/include/openssl/ssl.h")
    set(OPENSSL_SOURCE_DIR "${DINERO_VENDORED_OPENSSL_DIR}")
    message(STATUS "Using explicit vendored OpenSSL dir for headers: ${OPENSSL_SOURCE_DIR}")
  elseif(EXISTS "${OPENSSL_ROOT_DIR}/include/openssl/ssl.h")
    set(OPENSSL_SOURCE_DIR "${OPENSSL_ROOT_DIR}")
    message(STATUS "Using selected vendored OpenSSL dir for headers: ${OPENSSL_SOURCE_DIR}")
  endif()
  set(OPENSSL_USE_STATIC_LIBS TRUE)
  set(OPENSSL_INCLUDE_DIR ${OPENSSL_SOURCE_DIR}/include)
  set(OPENSSL_BUILD_METADATA ${OPENSSL_ROOT_DIR}/.dinero-build-meta)
  if(NOT EXISTS "${OPENSSL_INCLUDE_DIR}/openssl/ssl.h")
    message(FATAL_ERROR
      "Vendored OpenSSL headers not found at ${OPENSSL_INCLUDE_DIR}. "
      "Set DINERO_VENDORED_OPENSSL_SOURCE_DIR to the matching OpenSSL source/install directory.")
  endif()
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

    set(OPENSSL_VENDORED_VERSION "unknown")
    set(OPENSSL_VERSION_HEADER "${OPENSSL_INCLUDE_DIR}/openssl/opensslv.h")
    if(EXISTS "${OPENSSL_VERSION_HEADER}")
      file(STRINGS "${OPENSSL_VERSION_HEADER}" OPENSSL_VERSION_TEXT_LINE
        REGEX "^[ \t]*#[ \t]*define[ \t]+OPENSSL_VERSION_TEXT")
      if(OPENSSL_VERSION_TEXT_LINE)
        string(REGEX REPLACE ".*\"OpenSSL ([^\"]+)\".*" "\\1"
          OPENSSL_VENDORED_VERSION "${OPENSSL_VERSION_TEXT_LINE}")
      endif()
    endif()

    message(STATUS "Using vendored OpenSSL ${OPENSSL_VENDORED_VERSION} (static)")
    message(STATUS "  Headers: ${OPENSSL_INCLUDE_DIR}")
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
