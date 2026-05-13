# ============================================================================
# Lightning Core - Unified DineroCoin Lightning Network Dependencies
# ============================================================================
#
# This module conditionally imports and unifies all Lightning Network
# cryptographic libraries into a single `lightning_core_static` target.
#
# Dependencies (Priority-based):
#   [P0] libwally-core   - PSBT, BOLT #3 commitment txs (CRITICAL)
#   [P1] secp256k1-zkp   - MuSig2, BOLT #12 offers (ADVANCED)
#   [P2] blake3          - Fast channel hashing (PERFORMANCE)
#
# Usage:
#   add_subdirectory(third_party/lightning_core)
#   target_link_libraries(dinero_lightning PUBLIC lightning_core_static)
#
# ============================================================================

cmake_minimum_required(VERSION 3.10...3.29)
project(lightning_core VERSION 1.0.0 LANGUAGES CXX)

# ============================================================================
# Configuration Options
# ============================================================================

option(LIGHTNING_REQUIRE_WALLY "Require libwally-core (fail if missing)" OFF)
option(LIGHTNING_REQUIRE_ZKP "Require secp256k1-zkp (fail if missing)" OFF)
option(LIGHTNING_REQUIRE_BLAKE3 "Require blake3 (fail if missing)" OFF)

# ============================================================================
# Helper Variables
# ============================================================================

set(LIGHTNING_LIBS "")
set(LIGHTNING_INCLUDE_DIRS "")
set(LIGHTNING_FEATURES "")

message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
message(STATUS "Lightning Core: Unified BOLT-compliant dependency module")
message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")

# ============================================================================
# [P0] libwally-core - PSBT & BOLT #3 Primitives (CRITICAL)
# ============================================================================

set(LIBWALLY_ROOT ${CMAKE_SOURCE_DIR}/third_party/libwally-core)
set(LIBWALLY_LIB "${LIBWALLY_ROOT}/src/.libs/libwallycore.a")
set(LIBWALLY_INCLUDE "${LIBWALLY_ROOT}/include")

if(EXISTS "${LIBWALLY_LIB}")
    # Found libwally-core
    add_library(lightning_wally STATIC IMPORTED GLOBAL)
    set_target_properties(lightning_wally PROPERTIES
        IMPORTED_LOCATION "${LIBWALLY_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBWALLY_INCLUDE}"
    )

    list(APPEND LIGHTNING_LIBS lightning_wally)
    list(APPEND LIGHTNING_INCLUDE_DIRS "${LIBWALLY_INCLUDE}")
    list(APPEND LIGHTNING_FEATURES "PSBT" "BOLT3" "BIP174")

    message(STATUS "  ✅ libwally-core    (PSBT, BOLT #3 primitives)")
    message(STATUS "     Location: ${LIBWALLY_LIB}")
    message(STATUS "     Features: PSBT, commitment txs, script utilities")
else()
    # libwally-core not found
    if(LIGHTNING_REQUIRE_WALLY)
        message(FATAL_ERROR
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "  ❌ libwally-core REQUIRED but not found!\n"
            "\n"
            "  libwally-core provides:\n"
            "    - PSBT (Partially Signed Bitcoin Transactions)\n"
            "    - BOLT #3 commitment transaction utilities\n"
            "    - BIP174 transaction construction\n"
            "\n"
            "  Build it with:\n"
            "    ./scripts/vendor-libwally.sh\n"
            "\n"
            "  Or disable requirement:\n"
            "    cmake -DLIGHTNING_REQUIRE_WALLY=OFF ..\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        )
    else()
        message(WARNING "  ⚠️  libwally-core   (PSBT, BOLT #3) - NOT FOUND")
        message(WARNING "     Build with: ./scripts/vendor-libwally.sh")
        message(WARNING "     Lightning channels will lack PSBT support!")
    endif()
endif()

# ============================================================================
# [P1] secp256k1-zkp - MuSig2 & Advanced Cryptography (ADVANCED)
# ============================================================================

set(SECP256K1_ZKP_ROOT ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp)
set(SECP256K1_ZKP_LIB "${SECP256K1_ZKP_ROOT}/.libs/libsecp256k1.a")
set(SECP256K1_ZKP_INCLUDE "${SECP256K1_ZKP_ROOT}/include")

if(EXISTS "${SECP256K1_ZKP_LIB}")
    # Found secp256k1-zkp
    add_library(lightning_zkp STATIC IMPORTED GLOBAL)
    set_target_properties(lightning_zkp PROPERTIES
        IMPORTED_LOCATION "${SECP256K1_ZKP_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${SECP256K1_ZKP_INCLUDE}"
    )

    list(APPEND LIGHTNING_LIBS lightning_zkp)
    list(APPEND LIGHTNING_INCLUDE_DIRS "${SECP256K1_ZKP_INCLUDE}")
    list(APPEND LIGHTNING_FEATURES "MuSig2" "BOLT12" "Taproot")

    message(STATUS "  ✅ secp256k1-zkp    (MuSig2, BOLT #12 offers)")
    message(STATUS "     Location: ${SECP256K1_ZKP_LIB}")
    message(STATUS "     Features: MuSig2, adaptor sigs, Taproot channels")
else()
    # secp256k1-zkp not found
    if(LIGHTNING_REQUIRE_ZKP)
        message(FATAL_ERROR
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "  ❌ secp256k1-zkp REQUIRED but not found!\n"
            "\n"
            "  secp256k1-zkp provides:\n"
            "    - MuSig2 multi-signature aggregation\n"
            "    - BOLT #12 offers and async payments\n"
            "    - Taproot channel support\n"
            "    - Adaptor signatures\n"
            "\n"
            "  Build it with:\n"
            "    ./scripts/vendor-secp256k1-zkp.sh\n"
            "\n"
            "  Or disable requirement:\n"
            "    cmake -DLIGHTNING_REQUIRE_ZKP=OFF ..\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        )
    else()
        message(STATUS "  ℹ️  secp256k1-zkp   (MuSig2, BOLT #12) - NOT FOUND (optional)")
        message(STATUS "     Build with: ./scripts/vendor-secp256k1-zkp.sh")
        message(STATUS "     Advanced Lightning features disabled")
    endif()
endif()

# ============================================================================
# [P2] blake3 - Fast Channel Hashing (PERFORMANCE)
# ============================================================================

set(BLAKE3_ROOT ${CMAKE_SOURCE_DIR}/third_party/blake3)
set(BLAKE3_LIB "${BLAKE3_ROOT}/c/build/libblake3.a")
set(BLAKE3_INCLUDE "${BLAKE3_ROOT}/c")

if(EXISTS "${BLAKE3_LIB}")
    # Found blake3
    add_library(lightning_blake3 STATIC IMPORTED GLOBAL)
    set_target_properties(lightning_blake3 PROPERTIES
        IMPORTED_LOCATION "${BLAKE3_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${BLAKE3_INCLUDE}"
    )

    list(APPEND LIGHTNING_LIBS lightning_blake3)
    list(APPEND LIGHTNING_INCLUDE_DIRS "${BLAKE3_INCLUDE}")
    list(APPEND LIGHTNING_FEATURES "BLAKE3_HASHING")

    message(STATUS "  ✅ blake3           (10x faster hashing)")
    message(STATUS "     Location: ${BLAKE3_LIB}")
    message(STATUS "     Features: Optimized onion routing, channel updates")
else()
    # blake3 not found
    if(LIGHTNING_REQUIRE_BLAKE3)
        message(FATAL_ERROR
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "  ❌ blake3 REQUIRED but not found!\n"
            "\n"
            "  blake3 provides:\n"
            "    - 10x faster hashing vs SHA256\n"
            "    - Optimized onion routing\n"
            "    - Fast channel state updates\n"
            "\n"
            "  Build it with:\n"
            "    ./scripts/vendor-blake3.sh\n"
            "\n"
            "  Or disable requirement:\n"
            "    cmake -DLIGHTNING_REQUIRE_BLAKE3=OFF ..\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        )
    else()
        message(STATUS "  ℹ️  blake3          (fast hashing) - NOT FOUND (optional)")
        message(STATUS "     Build with: ./scripts/vendor-blake3.sh")
        message(STATUS "     Will use SHA256 (slower) instead")
    endif()
endif()

# ============================================================================
# Create Unified lightning_core_static Target
# ============================================================================

if(LIGHTNING_LIBS)
    # Create interface library that aggregates all Lightning deps
    add_library(lightning_core_static INTERFACE)

    target_link_libraries(lightning_core_static INTERFACE
        ${LIGHTNING_LIBS}
    )

    target_include_directories(lightning_core_static INTERFACE
        ${LIGHTNING_INCLUDE_DIRS}
    )

    # Add compile definitions for enabled features
    foreach(feature ${LIGHTNING_FEATURES})
        target_compile_definitions(lightning_core_static INTERFACE
            HAVE_LIGHTNING_${feature}
        )
    endforeach()

    # Add alias for compatibility
    add_library(lightning::core ALIAS lightning_core_static)

    # Success summary
    message(STATUS "")
    message(STATUS "  🎯 lightning_core_static target created")
    message(STATUS "     Enabled features: ${LIGHTNING_FEATURES}")
    message(STATUS "")
else()
    # No Lightning libraries found at all
    message(WARNING
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        "  ⚠️  WARNING: No Lightning Network libraries found!\n"
        "\n"
        "  DineroCoin Lightning requires at least one of:\n"
        "    [P0] libwally-core   - PSBT, BOLT #3 (CRITICAL)\n"
        "    [P1] secp256k1-zkp   - MuSig2, BOLT #12 (ADVANCED)\n"
        "    [P2] blake3          - Fast hashing (PERFORMANCE)\n"
        "\n"
        "  Quick setup:\n"
        "    ./scripts/vendor-libwally.sh        # Priority 0\n"
        "    ./scripts/vendor-secp256k1-zkp.sh   # Priority 1\n"
        "    ./scripts/vendor-blake3.sh          # Priority 2\n"
        "\n"
        "  Or install all at once:\n"
        "    cmake -DENABLE_LIGHTNING_SUPERBUILD=ON ..\n"
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    )

    # Create dummy target to avoid build errors
    add_library(lightning_core_static INTERFACE)
    message(STATUS "  ℹ️  Created empty lightning_core_static (no libraries)")
endif()

message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")

# ============================================================================
# Installation (Optional)
# ============================================================================

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    include(GNUInstallDirs)

    install(TARGETS lightning_core_static
        EXPORT lightning_coreTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

# ============================================================================
# Status Summary
# ============================================================================

# Export variables for parent CMakeLists.txt
set(LIGHTNING_CORE_FOUND TRUE PARENT_SCOPE)
set(LIGHTNING_CORE_VERSION "${PROJECT_VERSION}" PARENT_SCOPE)
set(LIGHTNING_CORE_FEATURES "${LIGHTNING_FEATURES}" PARENT_SCOPE)
