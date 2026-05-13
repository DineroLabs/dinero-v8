cmake_minimum_required(VERSION 3.17)
# VERSION 8.0.0 — Phase E of v8 peer-discovery. The seeder is a small
# standalone binary; intentionally light on dependencies so it builds
# fast and operators can run it without dragging in the daemon's full
# library graph.
project(dinero-seeder VERSION 8.0.0 LANGUAGES CXX)

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

target_include_directories(dinero-seeder-lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(dinero-seeder-lib
    PUBLIC
        OpenSSL::Crypto
)

# Compile-time provenance — surfaced via --help/--version if the binary
# gains those flags later.
target_compile_definitions(dinero-seeder-lib PRIVATE
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
