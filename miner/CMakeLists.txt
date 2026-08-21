cmake_minimum_required(VERSION 3.17)  # bumped for find_package(CUDAToolkit)
# In a monorepo release the root owns the shared version. Keep a standalone
# fallback for legacy direct builds of miner/.
if(NOT DEFINED DINERO_MONOREPO_VERSION)
  set(DINERO_MONOREPO_VERSION 8.1.7)
endif()
project(dinero-solo-miner VERSION ${DINERO_MONOREPO_VERSION} LANGUAGES CXX)

# C++20 required: src/build_identity.cpp uses designated initializers, which
# are a C++20 feature. Clang/GCC accept them as a C++17 extension, but MSVC
# enforces the standard and rejects them under /std:c++17.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(BUILD_CLI "Build standalone CLI miner" ON)
option(BUILD_TESTS "Build tests" ON)
option(MINER_ENABLE_CUDA "Enable CUDA mining backend (NVRTC at runtime, requires CUDA Toolkit)" OFF)
option(MINER_ENABLE_METAL "Enable Metal mining backend (Apple Silicon, macOS only)" OFF)
option(MINER_ENABLE_OPENCL "Enable OpenCL mining backend (AMD/Intel/NVIDIA via OpenCL ICD)" OFF)

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE DINERO_SOLO_MINER_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE DINERO_SOLO_MINER_GIT_FULL
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
else()
    set(DINERO_SOLO_MINER_GIT_HASH "unknown")
    set(DINERO_SOLO_MINER_GIT_FULL "unknown")
endif()

string(TIMESTAMP DINERO_SOLO_MINER_BUILD_TIME "%Y-%m-%dT%H:%M:%S+0000" UTC)

# Find dependencies
if(NOT TARGET CURL::libcurl)
  find_package(CURL REQUIRED)
endif()

# Homebrew upgrades can leave stale OPENSSL_* cache entries in an existing build dir.
# Clear invalid cached paths before discovery to keep incremental builds working.
if(DEFINED OPENSSL_INCLUDE_DIR AND NOT EXISTS "${OPENSSL_INCLUDE_DIR}/openssl/ssl.h")
    message(WARNING "Clearing stale OPENSSL_* cache entries (missing headers at ${OPENSSL_INCLUDE_DIR})")
    unset(OPENSSL_INCLUDE_DIR CACHE)
    unset(OPENSSL_SSL_LIBRARY CACHE)
    unset(OPENSSL_CRYPTO_LIBRARY CACHE)
endif()

if(DEFINED OPENSSL_SSL_LIBRARY AND NOT EXISTS "${OPENSSL_SSL_LIBRARY}")
    message(WARNING "Clearing stale OPENSSL_SSL_LIBRARY cache entry (${OPENSSL_SSL_LIBRARY})")
    unset(OPENSSL_SSL_LIBRARY CACHE)
endif()

if(DEFINED OPENSSL_CRYPTO_LIBRARY AND NOT EXISTS "${OPENSSL_CRYPTO_LIBRARY}")
    message(WARNING "Clearing stale OPENSSL_CRYPTO_LIBRARY cache entry (${OPENSSL_CRYPTO_LIBRARY})")
    unset(OPENSSL_CRYPTO_LIBRARY CACHE)
endif()

find_package(OpenSSL REQUIRED)

# OpenSSL imported target compatibility:
# - Modern CMake: OpenSSL::Crypto
# - Some environments expose only OPENSSL_CRYPTO_LIBRARY.
set(OPENSSL_CRYPTO_LINK_TARGET "")
if(TARGET OpenSSL::Crypto)
    set(OPENSSL_CRYPTO_LINK_TARGET OpenSSL::Crypto)
elseif(TARGET OpenSSL::SSL::Crypto)
    set(OPENSSL_CRYPTO_LINK_TARGET OpenSSL::SSL::Crypto)
elseif(OPENSSL_CRYPTO_LIBRARY)
    set(OPENSSL_CRYPTO_LINK_TARGET ${OPENSSL_CRYPTO_LIBRARY})
endif()

if("${OPENSSL_CRYPTO_LINK_TARGET}" STREQUAL "")
    message(FATAL_ERROR "Unable to resolve OpenSSL crypto link target")
endif()

# Try to find nlohmann_json, fall back to FetchContent if not found
find_package(nlohmann_json 3.9 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Monorepo: auto-regenerate include/solo_miner/chain_identity.h from
# the consensus headers when they change. The committed file in this
# subdir stays the source of truth for IDEs / clangd; CMake refreshes
# it whenever ../include/consensus/{chain_identity.h,chain_bundle_generated.h}
# move. The standalone-miner build (when this CMakeLists is the top
# project) doesn't have access to the consensus headers, so the gate
# below is skipped and the committed header is used as-is.
# ─────────────────────────────────────────────────────────────────────────────
set(_DINERO_MONOREPO_CHAIN_ID_SYNC OFF)
if(EXISTS "${CMAKE_SOURCE_DIR}/include/consensus/chain_identity.h" AND
   EXISTS "${CMAKE_SOURCE_DIR}/include/consensus/chain_bundle_generated.h" AND
   EXISTS "${CMAKE_SOURCE_DIR}/tools/sync_chain_identity_headers.py")
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_FOUND)
        set(_MINER_GEN_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/solo_miner/chain_identity.h")
        set(_SYNC_SCRIPT "${CMAKE_SOURCE_DIR}/tools/sync_chain_identity_headers.py")
        add_custom_command(
            OUTPUT "${_MINER_GEN_HEADER}"
            COMMAND ${Python3_EXECUTABLE} "${_SYNC_SCRIPT}"
            DEPENDS
                "${CMAKE_SOURCE_DIR}/include/consensus/chain_identity.h"
                "${CMAKE_SOURCE_DIR}/include/consensus/chain_bundle_generated.h"
                "${_SYNC_SCRIPT}"
            COMMENT "Regenerating miner/include/solo_miner/chain_identity.h from consensus headers"
            VERBATIM
        )
        set(_DINERO_MONOREPO_CHAIN_ID_SYNC ON)
    else()
        message(WARNING "Python3 not found; skipping automatic chain identity regeneration for miner")
    endif()
endif()

# ─────────────────────────────────────────────────────────────────────────────
# GPU mining backends (optional, NVRTC-based — Phase 2 Step 7+ for v8.0.0)
# ─────────────────────────────────────────────────────────────────────────────
# The CUDA backend uses the Driver API + NVRTC at runtime so the kernel is
# compiled to PTX matching the device at miner startup. No nvcc invocation,
# sidestepping the CUDA-12.2 vs recent-MSVC mismatch (NATIVE-MSVC-PORT-BUGS.md
# Bug #4). The Metal backend uses MTLLibrary newLibraryWithSource at startup
# for the same dispatch-at-runtime shape.
#
# Kernel sources (shaders/sha256d.cu, shaders/sha256d.metal) are embedded
# as C++ raw-string literals at configure time via configure_file() with
# @MINER_*_KERNEL_SRC@ substitution — no .cu/.metal files need to ship
# alongside the binary.
#
# OpenCL backend is NOT included here. The MinerBackend::OpenCl enum value
# exists for CLI parity but selection of it errors with
# "OpenCL not yet implemented in dinero-solo-miner v8.0.0". Planned for v8.1.
# ─────────────────────────────────────────────────────────────────────────────
set(GPU_BACKEND_SOURCES
    src/gpu/backend_factory.cpp
)
if(MINER_ENABLE_CUDA)
    find_package(CUDAToolkit REQUIRED)
    message(STATUS "dinero-solo-miner CUDA backend: enabled (CUDAToolkit ${CUDAToolkit_VERSION} at ${CUDAToolkit_LIBRARY_DIR})")

    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/shaders/sha256d.cu MINER_CUDA_KERNEL_SRC)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sha256d_cu_src.cpp.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/sha256d_cu_src.cpp
        @ONLY
    )

    list(APPEND GPU_BACKEND_SOURCES
        src/gpu/cuda_backend.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/generated/sha256d_cu_src.cpp
    )
endif()

if(MINER_ENABLE_OPENCL)
    find_package(OpenCL REQUIRED)
    message(STATUS "dinero-solo-miner OpenCL backend: enabled (OpenCL ${OpenCL_VERSION_STRING})")

    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/shaders/sha256d.cl MINER_OPENCL_KERNEL_SRC)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sha256d_cl_src.cpp.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/sha256d_cl_src.cpp
        @ONLY
    )

    list(APPEND GPU_BACKEND_SOURCES
        src/gpu/opencl_backend.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/generated/sha256d_cl_src.cpp
    )
endif()

if(MINER_ENABLE_METAL)
    if(NOT APPLE)
        message(FATAL_ERROR "MINER_ENABLE_METAL is only supported on macOS")
    endif()
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)

    find_library(METAL_FRAMEWORK Metal)
    find_library(FOUNDATION_FRAMEWORK Foundation)
    if(NOT METAL_FRAMEWORK OR NOT FOUNDATION_FRAMEWORK)
        message(FATAL_ERROR "Metal and Foundation frameworks are required for MINER_ENABLE_METAL")
    endif()

    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/shaders/sha256d.metal MINER_METAL_KERNEL_SRC)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sha256d_metal_src.cpp.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/sha256d_metal_src.cpp
        @ONLY
    )

    set_source_files_properties(src/gpu/metal_backend.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )

    list(APPEND GPU_BACKEND_SOURCES
        src/gpu/metal_backend.mm
        ${CMAKE_CURRENT_BINARY_DIR}/generated/sha256d_metal_src.cpp
    )
endif()

# Library target
add_library(dinero-solo-miner
    src/build_identity.cpp
    src/types.cpp
    src/rpc_client.cpp
    src/work_template.cpp
    src/hash_engine_cpu.cpp
    src/miner.cpp
    ${GPU_BACKEND_SOURCES}
)

if(_DINERO_MONOREPO_CHAIN_ID_SYNC)
    add_custom_target(dinero_solo_miner_chain_identity_gen
        DEPENDS "${_MINER_GEN_HEADER}"
    )
    add_dependencies(dinero-solo-miner dinero_solo_miner_chain_identity_gen)
endif()

target_include_directories(dinero-solo-miner
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(dinero-solo-miner
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        CURL::libcurl
        ${OPENSSL_CRYPTO_LINK_TARGET}
)

# GPU backend wiring. v8.0.0 uses dinero-solo-miner's own IGpuBackend
# interface (in include/solo_miner/gpu_backend.h) — not the daemon's
# dinero::gpu::IComputeBackend. The two GPU stacks coexist intentionally:
#   - dinero::solo::IGpuBackend  (this lib, NVRTC for CUDA, MSL for Metal)
#   - dinero::gpu::IComputeBackend  (daemon, src/mining/gpu/ — separate fix)
# Different namespaces, different consumers, no leakage.
if(MINER_ENABLE_CUDA OR MINER_ENABLE_METAL OR MINER_ENABLE_OPENCL)
    target_compile_definitions(dinero-solo-miner PUBLIC DINERO_SOLO_HAS_GPU)
    target_include_directories(dinero-solo-miner PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
endif()
if(MINER_ENABLE_CUDA)
    target_compile_definitions(dinero-solo-miner PRIVATE MINER_ENABLE_CUDA)
    target_link_libraries(dinero-solo-miner PRIVATE
        CUDA::cuda_driver
        CUDA::nvrtc
    )
endif()
if(MINER_ENABLE_METAL)
    target_compile_definitions(dinero-solo-miner PRIVATE MINER_ENABLE_METAL)
    target_link_libraries(dinero-solo-miner PRIVATE
        ${METAL_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
    )
endif()
if(MINER_ENABLE_OPENCL)
    target_compile_definitions(dinero-solo-miner PRIVATE MINER_ENABLE_OPENCL)
    target_link_libraries(dinero-solo-miner PRIVATE
        OpenCL::OpenCL
    )
endif()
if(NOT MINER_ENABLE_CUDA AND NOT MINER_ENABLE_METAL AND NOT MINER_ENABLE_OPENCL)
    message(STATUS "dinero-solo-miner GPU backends: disabled (no MINER_ENABLE_{CUDA,METAL,OPENCL} option set)")
endif()

target_compile_definitions(dinero-solo-miner PRIVATE
    DINERO_SOLO_MINER_REPO="dinero-solo-miner"
    DINERO_SOLO_MINER_COMPONENT="dinero-solo-miner"
    DINERO_SOLO_MINER_VERSION="${PROJECT_VERSION}"
    DINERO_SOLO_MINER_GIT_SHA="${DINERO_SOLO_MINER_GIT_HASH}"
    DINERO_SOLO_MINER_GIT_FULL="${DINERO_SOLO_MINER_GIT_FULL}"
    DINERO_SOLO_MINER_BUILD_TIME="${DINERO_SOLO_MINER_BUILD_TIME}"
)

# Alias for cleaner CMake usage
add_library(dinero::solo-miner ALIAS dinero-solo-miner)

# CLI executable
if(BUILD_CLI)
    add_executable(dinero-solo-miner-cli cli/main.cpp)
    target_link_libraries(dinero-solo-miner-cli PRIVATE dinero-solo-miner)
    set_target_properties(dinero-solo-miner-cli PROPERTIES OUTPUT_NAME "dinero-solo-miner")
    target_compile_definitions(dinero-solo-miner-cli PRIVATE
        DINERO_SOLO_MINER_REPO="dinero-solo-miner"
        DINERO_SOLO_MINER_COMPONENT="dinero-solo-miner"
        DINERO_SOLO_MINER_VERSION="${PROJECT_VERSION}"
        DINERO_SOLO_MINER_GIT_SHA="${DINERO_SOLO_MINER_GIT_HASH}"
        DINERO_SOLO_MINER_GIT_FULL="${DINERO_SOLO_MINER_GIT_FULL}"
        DINERO_SOLO_MINER_BUILD_TIME="${DINERO_SOLO_MINER_BUILD_TIME}"
    )
    # In v8 monorepo mode, the top-level CMakeLists.txt defines the
    # dinero_attach_win32_versioninfo() helper. Skip cleanly when the
    # miner subtree is built standalone (helper undefined).
    if(WIN32 AND COMMAND dinero_attach_win32_versioninfo)
        dinero_attach_win32_versioninfo(dinero-solo-miner-cli
            "Dinero solo miner (mines against a local dinerod)")
    endif()
endif()

# Tests
if(BUILD_TESTS)
    enable_testing()
    add_executable(miner_smoke_test tests/miner_smoke_test.cpp)
    target_link_libraries(miner_smoke_test PRIVATE dinero-solo-miner)
    add_test(NAME miner_smoke_test COMMAND miner_smoke_test)

    # CI drift detection: confirm the committed miner chain identity
    # header matches what the consensus headers would generate. Only
    # registered in monorepo mode where the consensus headers and the
    # sync script are both reachable.
    if(_DINERO_MONOREPO_CHAIN_ID_SYNC)
        add_test(
            NAME ChainIdentitySync
            COMMAND ${Python3_EXECUTABLE} "${_SYNC_SCRIPT}" --check
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(ChainIdentitySync PROPERTIES LABELS "miner;chain-identity")
    endif()
endif()

# Installation
include(GNUInstallDirs)

install(TARGETS dinero-solo-miner
    EXPORT dinero-solo-miner-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/solo_miner
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(BUILD_CLI)
    install(TARGETS dinero-solo-miner-cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
