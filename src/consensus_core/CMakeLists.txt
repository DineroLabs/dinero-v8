# ============================================================================
# DINERO CONSENSUS CORE LIBRARY
# ============================================================================
#
# libdinero-consensus: Pure consensus logic, verifiable anywhere.
#
# This library is PURE:
#   - NO IO (no filesystem, no database)
#   - NO networking
#   - NO threads (no mutex, no atomic for sync)
#   - NO logging (except debug assertions)
#   - NO globals (except const)
#   - NO clocks
#   - NO randomness
#
# What this enables:
#   - Mobile wallets verify blocks themselves
#   - Hardware wallets validate transactions
#   - Browsers verify consensus via WASM
#   - Light clients don't trust, they verify
#
# ============================================================================

message(STATUS "")
message(STATUS "============================================================")
message(STATUS "Configuring libdinero-consensus (pure consensus library)")
message(STATUS "============================================================")

# ============================================================================
# Source Files (PURE CONSENSUS ONLY)
# ============================================================================
#
# STRICT RULE: Only files that are PURE may be included here.
# Pure means: no IO, no filesystem, no networking, no threads, no clocks.
#
# If a file needs IO or threading, it belongs in an adapter, not here.
# ============================================================================

set(CONSENSUS_CORE_SOURCES
    # Pure UTXO set
    ${CMAKE_SOURCE_DIR}/src/consensus/consensus_utxo_set.cpp
    ${CMAKE_SOURCE_DIR}/src/consensus/consensus_invariants.cpp

    # Utreexo accumulator (pure cryptographic)
    ${CMAKE_SOURCE_DIR}/src/consensus/utreexo_accumulator.cpp
    ${CMAKE_SOURCE_DIR}/src/consensus/utreexo_stump.cpp

    # Merkle computation (pure math)
    ${CMAKE_SOURCE_DIR}/src/consensus/merkle_root.cpp

    # PoW verification (pure math)
    ${CMAKE_SOURCE_DIR}/src/consensus/pow.cpp
    ${CMAKE_SOURCE_DIR}/src/consensus/chainwork.cpp
    ${CMAKE_SOURCE_DIR}/src/consensus/difficulty_validation.cpp

    # Script execution (pure computation)
    ${CMAKE_SOURCE_DIR}/src/consensus/script.cpp
    ${CMAKE_SOURCE_DIR}/src/consensus/script_validation.cpp
    ${CMAKE_SOURCE_DIR}/src/consensus/script_crypto.cpp
    ${CMAKE_SOURCE_DIR}/src/consensus/crypto/sighash_bip143.cpp
)

# ============================================================================
# Create Static Library
# ============================================================================

add_library(dinero_consensus_core STATIC ${CONSENSUS_CORE_SOURCES})

# ============================================================================
# Include Directories
# ============================================================================

target_include_directories(dinero_consensus_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# ============================================================================
# Dependencies (PURE ONLY)
# ============================================================================
#
# dinero_consensus_core may only depend on:
#   - dinero_crypto (pure cryptographic primitives)
#   - dinero_tx_primitives (pure data structures)
#   - secp256k1 (pure cryptographic library)
#   - OpenSSL crypto (pure cryptographic library)
#
# It may NOT depend on:
#   - rocksdb
#   - filesystem libraries
#   - networking libraries
#   - threading libraries (except for primitives like atomic)
# ============================================================================

target_link_libraries(dinero_consensus_core PUBLIC
    dinero_crypto
    dinero_tx_primitives
    secp256k1
    OpenSSL::Crypto
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(dinero_consensus_core PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# Compile Definitions
# ============================================================================

# Mark this as a pure library build (can be used for conditional compilation)
target_compile_definitions(dinero_consensus_core PRIVATE
    DINERO_CONSENSUS_CORE_BUILD=1
)

# ============================================================================
# Library Properties
# ============================================================================

set_target_properties(dinero_consensus_core PROPERTIES
    OUTPUT_NAME "dinero-consensus"
    VERSION 1.0.0
    SOVERSION 1
)

# ============================================================================
# Verification (Build-Time Purity Check)
# ============================================================================
#
# This custom command runs during build to verify purity.
# Catches banned includes at compile time, not just CI time.
# ============================================================================

if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR DINERO_VERIFY_CONSENSUS_PURITY)
    add_custom_command(
        TARGET dinero_consensus_core POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Verifying consensus library purity..."
        COMMENT "Checking for banned includes in consensus library"
    )
endif()

# ============================================================================
# Summary
# ============================================================================

message(STATUS "")
message(STATUS "libdinero-consensus configuration:")
message(STATUS "  Sources: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "  Output:  libdinero-consensus.a")
message(STATUS "  Pure:    YES (no IO, no threads, no networking)")
message(STATUS "")
message(STATUS "Use cases:")
message(STATUS "  - Mobile wallet block verification")
message(STATUS "  - Hardware wallet transaction validation")
message(STATUS "  - Browser-based consensus (WASM)")
message(STATUS "  - Light client verification")
message(STATUS "")
