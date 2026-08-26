cmake_minimum_required(VERSION 3.17)
# In a monorepo release the root owns the shared version. Keep a standalone
# fallback for legacy direct builds of seeder/.
if(NOT DEFINED DINERO_MONOREPO_VERSION)
  set(DINERO_MONOREPO_VERSION 8.1.9)
endif()
project(dinero-seeder VERSION ${DINERO_MONOREPO_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# OpenSSL — only SHA256 (used by the wire-format checksum). Cheaper to
# pull in than vendoring our own SHA256 implementation, and it lines up
# with what the daemon already links.
find_package(OpenSSL REQUIRED)

# ─────────────────────────────────────────────────────────────────────
# Sources
# ─────────────────────────────────────────────────────────────────────
set(SEEDER_LIB_SOURCES
    src/wire.cpp
    src/connection.cpp
    src/peer_state.cpp
    src/output.cpp
    src/crawler.cpp
)

add_library(dinero-seeder-lib STATIC ${SEEDER_LIB_SOURCES})

# ─────────────────────────────────────────────────────────────────────
# Monorepo: auto-regenerate include/dinero/seeder/network_constants_generated.h
# from src/consensus/chainparams_impl.cpp when it changes. The committed
# file is the source of truth for IDEs / clangd; CMake refreshes it
# whenever the canonical chainparams file moves. Standalone seeder
# builds (no consensus source available) skip the gate and use the
# committed header as-is. Mirrors the miner/chain_identity sync pattern.
# ─────────────────────────────────────────────────────────────────────
set(_DINERO_MONOREPO_NET_MAGIC_SYNC OFF)
if(EXISTS "${CMAKE_SOURCE_DIR}/src/consensus/chainparams_impl.cpp" AND
   EXISTS "${CMAKE_SOURCE_DIR}/tools/sync_network_constants_headers.py")
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_FOUND)
        set(_SEEDER_GEN_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/dinero/seeder/network_constants_generated.h")
        set(_NET_MAGIC_SYNC_SCRIPT "${CMAKE_SOURCE_DIR}/tools/sync_network_constants_headers.py")
        add_custom_command(
            OUTPUT "${_SEEDER_GEN_HEADER}"
            COMMAND ${Python3_EXECUTABLE} "${_NET_MAGIC_SYNC_SCRIPT}"
            DEPENDS
                "${CMAKE_SOURCE_DIR}/src/consensus/chainparams_impl.cpp"
                "${_NET_MAGIC_SYNC_SCRIPT}"
            COMMENT "Regenerating seeder/include/dinero/seeder/network_constants_generated.h from chainparams"
            VERBATIM
        )
        add_custom_target(dinero_seeder_net_magic_gen
            DEPENDS "${_SEEDER_GEN_HEADER}"
        )
        add_dependencies(dinero-seeder-lib dinero_seeder_net_magic_gen)
        set(_DINERO_MONOREPO_NET_MAGIC_SYNC ON)
    endif()
endif()

target_include_directories(dinero-seeder-lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Monorepo: expose dinero root include/ so seeder sources can pull in the
# shared cross-platform compat shim (compat/net_compat.h) that maps POSIX
# socket headers to winsock2 on Windows. Standalone seeder builds outside
# the dinero monorepo (where this header isn't available) fall back to the
# POSIX-only includes, which is fine since the standalone use case is
# Linux-server-only seed crawlers.
if(EXISTS "${CMAKE_SOURCE_DIR}/include/compat/net_compat.h")
    target_include_directories(dinero-seeder-lib PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
    )
endif()

target_link_libraries(dinero-seeder-lib
    PUBLIC
        OpenSSL::Crypto
)

# Compile-time provenance — surfaced via --help/--version if the binary
# gains those flags later.
target_compile_definitions(dinero-seeder-lib PUBLIC
    DINERO_SEEDER_VERSION="${PROJECT_VERSION}"
)

# ─────────────────────────────────────────────────────────────────────
# CLI binary
# ─────────────────────────────────────────────────────────────────────
add_executable(dinero-seeder cli/main.cpp)
target_link_libraries(dinero-seeder PRIVATE dinero-seeder-lib)
set_target_properties(dinero-seeder PROPERTIES
    OUTPUT_NAME "dinero-seeder"
)

# Install at the same destination shape as miner/qt — operators expect
# `dinero-seeder` in bin/ alongside `dinerod`.
include(GNUInstallDirs)
install(TARGETS dinero-seeder
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ─────────────────────────────────────────────────────────────────────
# CI drift detection: confirm the committed seeder network-constants
# header matches what chainparams_impl.cpp would generate, and that no
# source file holds a magic value that disagrees with chainparams.
# Only registered in monorepo mode where the chainparams source + sync
# script are both reachable.
# ─────────────────────────────────────────────────────────────────────
if(_DINERO_MONOREPO_NET_MAGIC_SYNC)
    enable_testing()
    set(_NET_MAGIC_TEST_SCRIPT "${CMAKE_SOURCE_DIR}/tests/integration/test_network_magic_sync.sh")
    if(EXISTS "${_NET_MAGIC_TEST_SCRIPT}")
        add_test(
            NAME NetworkMagicSync
            COMMAND bash "${_NET_MAGIC_TEST_SCRIPT}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
        set_tests_properties(NetworkMagicSync PROPERTIES LABELS "seeder;network-magic")
    endif()
endif()
