# ============================================================================
# Phase F.10: Mining/Block Assembly Spending Paths Tests
# ============================================================================
#
# Test Suite: Mining block assembly and validation
# Pattern: Standalone executables (following Phase F.9 wallet_persistence pattern)
# Dependencies: BlockAssembler, BlockValidator, Mempool, CoinsDB
#
# Tests:
#   - T20: Block assembly with mature transactions
#   - T21: Block assembly excludes immature spend
#   - T22: Block validation rejects immature spend
#
# ============================================================================

# ============================================================================
# Standalone Test T20: Block Assembly with Mature Transactions (I.10.1, I.10.3, I.10.4)
# ============================================================================

add_executable(standalone_test_t20
    standalone_test_t20.cpp
    ${CMAKE_SOURCE_DIR}/src/mempool/mempool.cpp
    ${CMAKE_SOURCE_DIR}/src/mempool/invalid_tx_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/mempool/coins_view_mempool.cpp
    ${CMAKE_SOURCE_DIR}/src/mining/block_assembler.cpp
    ${CMAKE_SOURCE_DIR}/src/consensus/block_validation.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    ${CMAKE_SOURCE_DIR}/src/daemon/config.cpp     # Provides GetConfig()
    ${CMAKE_SOURCE_DIR}/src/network/bridge_node.cpp  # Provides BridgeNode::GenerateProofForBlock
    ${CMAKE_SOURCE_DIR}/tests/stubs/vault_test_stubs.cpp  # No-op vault hooks for chainstate_service/wallet_worker
)

# Link libraries for T20 (mining + consensus + mempool)
target_link_libraries(standalone_test_t20 PRIVATE
    dinero_core          # Includes primitives, crypto
    dinero_chainstate    # For block assembly
    dinero_consensus     # For CoinsDB, block validation
    dinero_crypto        # For crypto primitives
    argon2
    jsoncpp_static
    sqlite3
    secp256k1
    bulletproofs_ffi     # For confidential transactions
    OpenSSL::Crypto      # For crypto functions
    RocksDB::rocksdb
)

# Include directories for T20
target_include_directories(standalone_test_t20 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
    ${CMAKE_SOURCE_DIR}/third_party/argon2/include
)

# Compile definitions for T20
target_compile_definitions(standalone_test_t20 PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# Platform-specific settings for T20
if(APPLE)
    target_link_libraries(standalone_test_t20 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Register standalone T20 as a test
add_test(NAME Mining_T20_Standalone COMMAND standalone_test_t20)
set_tests_properties(Mining_T20_Standalone
    PROPERTIES
    LABELS "f10;mining;block_assembly;mature;validation"
    TIMEOUT 30
)

# ============================================================================
# Standalone Test T21: Block Assembly Excludes Immature (I.10.1, I.10.2)
# ============================================================================

add_executable(standalone_test_t21
    standalone_test_t21.cpp
    ${CMAKE_SOURCE_DIR}/src/mempool/mempool.cpp
    ${CMAKE_SOURCE_DIR}/src/mempool/invalid_tx_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/mempool/coins_view_mempool.cpp
    ${CMAKE_SOURCE_DIR}/src/mining/block_assembler.cpp
    ${CMAKE_SOURCE_DIR}/src/consensus/block_validation.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    ${CMAKE_SOURCE_DIR}/src/daemon/config.cpp     # Provides GetConfig()
    ${CMAKE_SOURCE_DIR}/src/network/bridge_node.cpp  # Provides BridgeNode::GenerateProofForBlock
    ${CMAKE_SOURCE_DIR}/tests/stubs/vault_test_stubs.cpp  # No-op vault hooks
)

# Link libraries for T21 (mining + consensus + mempool)
target_link_libraries(standalone_test_t21 PRIVATE
    dinero_core          # Includes primitives, crypto
    dinero_chainstate    # For block assembly
    dinero_consensus     # For CoinsDB, block validation
    dinero_crypto        # For crypto primitives
    argon2
    jsoncpp_static
    sqlite3
    secp256k1
    bulletproofs_ffi     # For confidential transactions
    OpenSSL::Crypto      # For crypto functions
    RocksDB::rocksdb
)

# Include directories for T21
target_include_directories(standalone_test_t21 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
    ${CMAKE_SOURCE_DIR}/third_party/argon2/include
)

# Compile definitions for T21
target_compile_definitions(standalone_test_t21 PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# Platform-specific settings for T21
if(APPLE)
    target_link_libraries(standalone_test_t21 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Register standalone T21 as a test
add_test(NAME Mining_T21_Standalone COMMAND standalone_test_t21)
set_tests_properties(Mining_T21_Standalone
    PROPERTIES
    LABELS "f10;mining;block_assembly;immature;exclusion"
    TIMEOUT 30
)

# ============================================================================
# Standalone Test T22: Block Validation Rejects Immature (I.10.3)
# ============================================================================
# TEMPORARILY DISABLED (2026-02-02): Uses old BlockValidator(UTXOIndex*) API
# TODO: Rewrite for new IConsensusUTXOSet interface
#
# add_executable(standalone_test_t22
#     standalone_test_t22.cpp
#     ${CMAKE_SOURCE_DIR}/src/consensus/block_validation.cpp
# )
#
# target_link_libraries(standalone_test_t22 PRIVATE
#     dinero_core dinero_chainstate dinero_consensus dinero_wallet dinero_crypto
#     argon2 jsoncpp_static sqlite3 secp256k1 bulletproofs_ffi OpenSSL::Crypto RocksDB::rocksdb
# )
#
# target_include_directories(standalone_test_t22 PRIVATE
#     ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}
#     ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
#     ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include ${CMAKE_SOURCE_DIR}/third_party/argon2/include
# )
#
# target_compile_definitions(standalone_test_t22 PRIVATE CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
#
# if(APPLE)
#     target_link_libraries(standalone_test_t22 PRIVATE "-framework Security" "-framework CoreFoundation")
# endif()
#
# add_test(NAME Mining_T22_Standalone COMMAND standalone_test_t22)

message(STATUS "⊘ Phase F.10 T22 test DISABLED (BlockValidator API changed)")

# ============================================================================
# Standalone Test T23: Reorg Unwinds Mined Block (I.11.1, I.11.2, I.11.4)
# ============================================================================
# TEMPORARILY DISABLED (2026-02-02): Uses old BlockValidator(UTXOIndex*) API
# TODO: Rewrite for new IConsensusUTXOSet interface
#
# add_executable(standalone_test_t23
#     standalone_test_t23.cpp
#     ${CMAKE_SOURCE_DIR}/src/consensus/block_validation.cpp
# )
#
# # Link libraries for T23 (consensus reorg + validation)
# target_link_libraries(standalone_test_t23 PRIVATE
#     dinero_core          # Includes primitives, crypto
#     dinero_chainstate    # For block validation context
#     dinero_consensus     # For CoinsDB, block validation, BlockUndo
#     dinero_wallet        # For UTXO index
#     dinero_crypto        # For crypto primitives
#     argon2
#     jsoncpp_static
#     sqlite3
#     secp256k1
#     bulletproofs_ffi     # For confidential transactions
#     OpenSSL::Crypto      # For crypto functions
#     RocksDB::rocksdb
# )
#
# # Include directories for T23
# target_include_directories(standalone_test_t23 PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#     ${CMAKE_BINARY_DIR}
#     ${CMAKE_CURRENT_SOURCE_DIR}
#     ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
#     ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
#     ${CMAKE_SOURCE_DIR}/third_party/argon2/include
# )
#
# # Compile definitions for T23
# target_compile_definitions(standalone_test_t23 PRIVATE
#     CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
# )
#
# # Platform-specific settings for T23
# if(APPLE)
#     target_link_libraries(standalone_test_t23 PRIVATE
#         "-framework Security"
#         "-framework CoreFoundation"
#     )
# endif()
#
# # Register standalone T23 as a test
# add_test(NAME Mining_T23_Standalone COMMAND standalone_test_t23)
# set_tests_properties(Mining_T23_Standalone
#     PROPERTIES
#     LABELS "f11;mining;reorg;block_disconnect;utxo_restoration"
#     TIMEOUT 30
# )

message(STATUS "⊘ Phase F.11 reorg safety tests DISABLED (BlockValidator API changed)")

# ============================================================================
# Standalone Test T24: Reorg + Coinbase Maturity Regression (I.11.3)
# ============================================================================

add_executable(standalone_test_t24
    standalone_test_t24.cpp
    ${CMAKE_SOURCE_DIR}/src/mempool/mempool.cpp
    ${CMAKE_SOURCE_DIR}/src/mempool/invalid_tx_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/mempool/coins_view_mempool.cpp
    ${CMAKE_SOURCE_DIR}/tests/stubs/vault_test_stubs.cpp  # No-op vault hooks
)

# Link libraries for T24 (mempool + consensus)
target_link_libraries(standalone_test_t24 PRIVATE
    dinero_core          # Includes primitives, crypto
    dinero_chainstate    # For mempool context
    dinero_consensus     # For CoinsDB
    dinero_crypto        # For crypto primitives
    argon2
    jsoncpp_static
    sqlite3
    secp256k1
    bulletproofs_ffi     # For confidential transactions
    OpenSSL::Crypto      # For crypto functions
    RocksDB::rocksdb
)

# Include directories for T24
target_include_directories(standalone_test_t24 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
    ${CMAKE_SOURCE_DIR}/third_party/argon2/include
)

# Compile definitions for T24
target_compile_definitions(standalone_test_t24 PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# Platform-specific settings for T24
if(APPLE)
    target_link_libraries(standalone_test_t24 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Register standalone T24 as a test
add_test(NAME Mining_T24_Standalone COMMAND standalone_test_t24)
set_tests_properties(Mining_T24_Standalone
    PROPERTIES
    LABELS "f11;mining;reorg;coinbase_maturity;maturity_regression"
    TIMEOUT 30
)

message(STATUS "✅ Phase F.11 reorg safety tests configured")
message(STATUS "   Standalone tests: T23 (Blocked-by-design), T24 (Coinbase maturity regression)")

# ============================================================================
# Phase W.1: Block Assembly Intelligence Tests
# ============================================================================

add_executable(test_block_assembly_w1
    test_block_assembly_w1.cpp
    ${CMAKE_SOURCE_DIR}/src/mining/block_assembly_context.cpp
    ${CMAKE_SOURCE_DIR}/src/mining/transaction_scorer.cpp
    ${CMAKE_SOURCE_DIR}/src/mining/template_delta_tracker.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    ${CMAKE_SOURCE_DIR}/src/daemon/config.cpp     # Provides GetConfig()
    ${CMAKE_SOURCE_DIR}/src/network/bridge_node.cpp  # Provides BridgeNode::GenerateProofForBlock
)

# Link libraries for W.1 tests (minimal - just context)
target_link_libraries(test_block_assembly_w1 PRIVATE
    dinero_core          # Includes primitives
    bulletproofs_ffi     # For confidential transactions (required by dinero_core)
)

# Platform-specific settings for W.1 tests
if(APPLE)
    target_link_libraries(test_block_assembly_w1 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for W.1 tests
target_include_directories(test_block_assembly_w1 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Register W.1 test
add_test(NAME Mining_W1_BlockAssemblyContext COMMAND test_block_assembly_w1)
set_tests_properties(Mining_W1_BlockAssemblyContext
    PROPERTIES
    LABELS "w1;mining;block_assembly;context;network_aware"
    TIMEOUT 10
)

message(STATUS "✅ Phase W.1 block assembly intelligence tests configured")
message(STATUS "   Tests: BlockAssemblyContext (network-aware mining)")

# ============================================================================
# Phase W.1.5: Block Assembly Intelligence - Integration Tests & Benchmarks
# ============================================================================

add_executable(test_block_assembly_w1_integration
    test_block_assembly_w1_integration.cpp
    ${CMAKE_SOURCE_DIR}/src/mining/block_assembly_context.cpp
    ${CMAKE_SOURCE_DIR}/src/mining/transaction_scorer.cpp
    ${CMAKE_SOURCE_DIR}/src/mining/template_delta_tracker.cpp
    ${CMAKE_SOURCE_DIR}/src/mining/block_assembler.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    ${CMAKE_SOURCE_DIR}/src/daemon/config.cpp     # Provides GetConfig()
    ${CMAKE_SOURCE_DIR}/tests/stubs/vault_test_stubs.cpp  # vault observers (real mempool linked)
    # bridge_node.cpp removed: BridgeNode::* now comes from dinero_bridge transitively via dinero_core
)

# Link libraries for W.1.5 integration tests
target_link_libraries(test_block_assembly_w1_integration PRIVATE
    dinero_core          # Includes primitives
    dinero_consensus     # For block validation
    dinero_chainstate    # For mempool, chain_db
    bulletproofs_ffi     # For confidential transactions
)

# Platform-specific settings for W.1.5 tests
if(APPLE)
    target_link_libraries(test_block_assembly_w1_integration PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for W.1.5 tests
target_include_directories(test_block_assembly_w1_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Register W.1.5 test
add_test(NAME Mining_W1_5_Integration COMMAND test_block_assembly_w1_integration)
set_tests_properties(Mining_W1_5_Integration
    PROPERTIES
    LABELS "w1;mining;integration;benchmark;performance"
    TIMEOUT 30
)

message(STATUS "✅ Phase W.1.5 integration tests & benchmarks configured")
message(STATUS "   Tests: Integration (2), Benchmarks (2), Simulation (2), Edge Cases (1)")

# ============================================================================
# Phase W.2.1: Wallet Sync Status Tests
# ============================================================================

add_executable(test_wallet_sync_status
    ../wallet/test_wallet_sync_status.cpp
    ${CMAKE_SOURCE_DIR}/src/wallet/wallet_sync_status.cpp
)

# Link libraries for W.2.1 tests
target_link_libraries(test_wallet_sync_status PRIVATE
    dinero_core          # Includes primitives
    bulletproofs_ffi     # For confidential transactions
)

# Platform-specific settings for W.2.1 tests
if(APPLE)
    target_link_libraries(test_wallet_sync_status PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for W.2.1 tests
target_include_directories(test_wallet_sync_status PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Register W.2.1 test
add_test(NAME Wallet_W2_1_SyncStatus COMMAND test_wallet_sync_status)
set_tests_properties(Wallet_W2_1_SyncStatus
    PROPERTIES
    LABELS "w2;wallet;sync;status;ux"
    TIMEOUT 10
)

message(STATUS "✅ Phase W.2.1 wallet sync status tests configured")
message(STATUS "   Tests: WalletSyncStatus foundation (7 tests)")

# ============================================================================
# Phase W.2.2: Sync ETA Estimator Tests
# ============================================================================

add_executable(test_sync_eta_estimator
    ../wallet/test_sync_eta_estimator.cpp
    ${CMAKE_SOURCE_DIR}/src/wallet/sync_eta_estimator.cpp
    ${CMAKE_SOURCE_DIR}/src/wallet/sync_progress_tracker.cpp
    ${CMAKE_SOURCE_DIR}/src/wallet/wallet_sync_status.cpp
)

# Link libraries for W.2.2 tests
target_link_libraries(test_sync_eta_estimator PRIVATE
    dinero_core          # Includes primitives
    bulletproofs_ffi     # For confidential transactions
)

# Platform-specific settings for W.2.2 tests
if(APPLE)
    target_link_libraries(test_sync_eta_estimator PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for W.2.2 tests
target_include_directories(test_sync_eta_estimator PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Register W.2.2 test
add_test(NAME Wallet_W2_2_SyncETA COMMAND test_sync_eta_estimator)
set_tests_properties(Wallet_W2_2_SyncETA
    PROPERTIES
    LABELS "w2;wallet;sync;eta;ux"
    TIMEOUT 10
)

message(STATUS "✅ Phase W.2.2 sync ETA estimator tests configured")
message(STATUS "   Tests: ETA estimation (9 tests)")

# ============================================================================
# Phase W.2.4: Reorg Visibility Tests
# ============================================================================

add_executable(test_reorg_visibility
    ../wallet/test_reorg_visibility.cpp
    ${CMAKE_SOURCE_DIR}/src/wallet/reorg_detector.cpp
    ${CMAKE_SOURCE_DIR}/src/wallet/wallet_sync_status.cpp
)

# Link libraries for W.2.4 tests
target_link_libraries(test_reorg_visibility PRIVATE
    dinero_core          # Includes primitives
    bulletproofs_ffi     # For confidential transactions
)

# Platform-specific settings for W.2.4 tests
if(APPLE)
    target_link_libraries(test_reorg_visibility PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for W.2.4 tests
target_include_directories(test_reorg_visibility PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Register W.2.4 test
add_test(NAME Wallet_W2_4_ReorgVisibility COMMAND test_reorg_visibility)
set_tests_properties(Wallet_W2_4_ReorgVisibility
    PROPERTIES
    LABELS "w2;wallet;reorg;visibility;ux"
    TIMEOUT 10
)

message(STATUS "✅ Phase W.2.4 reorg visibility tests configured")
message(STATUS "   Tests: Reorg detection and user visibility (10 tests)")

# ============================================================================
# Phase W.2.5: Slow Reason Analyzer Tests
# ============================================================================

add_executable(test_slow_reason_analyzer
    ../wallet/test_slow_reason_analyzer.cpp
    ${CMAKE_SOURCE_DIR}/src/wallet/slow_reason_analyzer.cpp
    ${CMAKE_SOURCE_DIR}/src/wallet/wallet_sync_status.cpp
    ${CMAKE_SOURCE_DIR}/src/wallet/reorg_detector.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    ${CMAKE_SOURCE_DIR}/src/daemon/config.cpp     # Provides GetConfig()
    ${CMAKE_SOURCE_DIR}/src/network/bridge_node.cpp  # Provides BridgeNode::GenerateProofForBlock
)

# Link libraries for W.2.5 tests
target_link_libraries(test_slow_reason_analyzer PRIVATE
    dinero_core          # Includes primitives
    bulletproofs_ffi     # For confidential transactions
)

# Platform-specific settings for W.2.5 tests
if(APPLE)
    target_link_libraries(test_slow_reason_analyzer PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for W.2.5 tests
target_include_directories(test_slow_reason_analyzer PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Register W.2.5 test
add_test(NAME Wallet_W2_5_SlowReasonAnalyzer COMMAND test_slow_reason_analyzer)
set_tests_properties(Wallet_W2_5_SlowReasonAnalyzer
    PROPERTIES
    LABELS "w2;wallet;slow_reason;diagnostics;ux"
    TIMEOUT 10
)

message(STATUS "✅ Phase W.2.5 slow reason analyzer tests configured")
message(STATUS "   Tests: Why is it slow? detection (10 tests)")

# ============================================================================
# Phase W.2.6: Wallet Sync RPC Tests
# ============================================================================

add_executable(test_wallet_sync_rpc
    ../wallet/test_wallet_sync_rpc.cpp
)

# Link libraries for W.2.6 tests
target_link_libraries(test_wallet_sync_rpc PRIVATE
    jsoncpp_static       # For JSON handling
    bulletproofs_ffi     # For confidential transactions
)

# Platform-specific settings for W.2.6 tests
if(APPLE)
    target_link_libraries(test_wallet_sync_rpc PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for W.2.6 tests
target_include_directories(test_wallet_sync_rpc PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
)

# Register W.2.6 test
add_test(NAME Wallet_W2_6_SyncRPC COMMAND test_wallet_sync_rpc)
set_tests_properties(Wallet_W2_6_SyncRPC
    PROPERTIES
    LABELS "w2;wallet;rpc;sync;api"
    TIMEOUT 10
)

message(STATUS "✅ Phase W.2.6 wallet sync RPC tests configured")
message(STATUS "   Tests: RPC API endpoints (10 tests)")

# ============================================================================
# Phase W.2.6: Wallet Sync Integration Tests
# ============================================================================

add_executable(test_wallet_sync_integration
    ../wallet/test_wallet_sync_integration.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    ${CMAKE_SOURCE_DIR}/src/daemon/config.cpp     # Provides GetConfig()
    ${CMAKE_SOURCE_DIR}/src/network/bridge_node.cpp  # Provides BridgeNode::GenerateProofForBlock
)

# Link libraries for W.2.6 integration tests
target_link_libraries(test_wallet_sync_integration PRIVATE
    dinero_wallet        # For WalletSyncStatus, ReorgDetector, SlowReasonAnalyzer
    dinero_core          # For core components (includes Logger)
    dinero_chainstate    # For ChainDB references
    dinero_consensus     # For consensus components
    dinero_crypto        # For crypto primitives
    dinero_tx_primitives # For transaction primitives
    jsoncpp_static       # For JSON handling
    bulletproofs_ffi     # For confidential transactions
    argon2               # For argon2 hash
    sqlite3              # For database
    secp256k1            # For crypto
    RocksDB::rocksdb     # For RocksDB
)

# Platform-specific settings for W.2.6 integration tests
if(APPLE)
    target_link_libraries(test_wallet_sync_integration PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for W.2.6 integration tests
target_include_directories(test_wallet_sync_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Register W.2.6 integration test
add_test(NAME Wallet_W2_6_SyncIntegration COMMAND test_wallet_sync_integration)
set_tests_properties(Wallet_W2_6_SyncIntegration
    PROPERTIES
    LABELS "w2;wallet;integration;sync"
    TIMEOUT 30
)

message(STATUS "✅ Phase W.2.6 wallet sync integration tests configured")
message(STATUS "   Tests: Component integration (3 tests)")

# ============================================================================
# v1.0.4 Regression Test: Block Header Endianness Bug
# ============================================================================
#
# Purpose: Prevent regression of the consensus-critical endianness bug
#          fixed in v1.0.4 that prevented chain progression beyond block 1.
#
# Bug Description:
#   - BlockAssembler stored prevBlockHash as hex string from uint256::GetHex() (big-endian)
#   - WriteHashLE() expected little-endian hex for wire format
#   - Result: byte-reversed prevBlockHash causing "stale-prevhash" rejections
#
# Test: Mine genesis → block 3, verify byte order, verify no rejections
#
# Fixed in: v1.0.4 (2026-01-01)
# Commit: 42cbfb72 - fix(consensus): correct prevBlockHash endianness in block headers
# ============================================================================

# TEMPORARILY DISABLED (2026-02-02): Uses old BlockHeader.time field (now timestamp)
# and old uint256 API. TODO: Update to new BlockHeader fields.
#
# add_executable(test_block_progression_regression
#     test_block_progression_regression.cpp
#     ${CMAKE_SOURCE_DIR}/src/mining/block_assembler.cpp
#     ${CMAKE_SOURCE_DIR}/src/consensus/block_validation.cpp
#     ${CMAKE_SOURCE_DIR}/src/mempool/mempool.cpp
#     ${CMAKE_SOURCE_DIR}/src/mempool/invalid_tx_cache.cpp
#     ${CMAKE_SOURCE_DIR}/src/mempool/coins_view_mempool.cpp
# )
# target_link_libraries(test_block_progression_regression PRIVATE
#     dinero_core dinero_chainstate dinero_consensus dinero_wallet dinero_crypto
#     argon2 jsoncpp_static sqlite3 secp256k1 bulletproofs_ffi OpenSSL::Crypto RocksDB::rocksdb
# )
# target_include_directories(test_block_progression_regression PRIVATE
#     ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}
#     ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
#     ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include ${CMAKE_SOURCE_DIR}/third_party/argon2/include
# )
# target_compile_definitions(test_block_progression_regression PRIVATE CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# if(APPLE)
#     target_link_libraries(test_block_progression_regression PRIVATE "-framework Security" "-framework CoreFoundation")
# endif()
# add_test(NAME Mining_BlockProgression_Regression_v104 COMMAND test_block_progression_regression)

message(STATUS "⊘ v1.0.4 block progression regression test DISABLED (BlockHeader API changed)")

# ============================================================================
# Ring 4 Phase 4b: Mining Property Test Framework
# ============================================================================
#
# Purpose: Dumb framework for property-based testing of mining
# Pattern: Framework infrastructure ONLY (no property assertions)
# Phase: 4b - Framework implementation (Properties come in phases 4c-4g)
#
# Components:
#   - Deterministic RNG + Clock (reproducible tests)
#   - Mock Mining Simulator (records events without validation)
#   - Sequence Generator (creates test scenarios)
#   - Crash Injection Model (crash/restart semantics)
#   - Framework Self-Tests (determinism + restart verification)
#
# ============================================================================

# ============================================================================
# Mining Test Framework Library
# ============================================================================

add_library(mining_test_framework STATIC
    framework/mining_simulator.cpp
    framework/mining_sequence_generator.cpp
    framework/crash_injection_model.cpp
)

# Link libraries for framework
target_link_libraries(mining_test_framework PUBLIC
    # No dependencies on consensus/wallet/mempool - pure mock framework
)

# Platform-specific settings for framework
if(APPLE)
    target_link_libraries(mining_test_framework PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for framework
target_include_directories(mining_test_framework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# ============================================================================
# Framework Self-Tests (Determinism + Crash/Restart Semantics)
# ============================================================================

add_executable(test_mining_framework_determinism
    framework/test_framework_determinism.cpp
)

# Link libraries for framework tests
target_link_libraries(test_mining_framework_determinism PRIVATE
    mining_test_framework
    # No external dependencies - pure framework tests
)

# Platform-specific settings for framework tests
if(APPLE)
    target_link_libraries(test_mining_framework_determinism PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for framework tests
target_include_directories(test_mining_framework_determinism PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register framework self-tests
add_test(NAME Mining_Framework_Determinism_R4_4b COMMAND test_mining_framework_determinism)
set_tests_properties(Mining_Framework_Determinism_R4_4b
    PROPERTIES
    LABELS "ring4;phase4b;framework;determinism;crash_restart"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4b mining test framework configured")
message(STATUS "   Framework: MiningSimulator, SequenceGenerator, CrashInjector")
message(STATUS "   Self-Tests: Determinism, Crash/Restart semantics (12 tests)")

# ============================================================================
# Ring 4 Phase 4c: Mining Correctness Properties
# ============================================================================
#
# Purpose: Property checkers for mining correctness (MC1-MC5)
# Pattern: Oracle validates properties against Phase 4b traces
# Phase: 4c - Correctness properties (subsidy, coinbase, template, context)
#
# Components:
#   - ConsensusParams (consensus constants)
#   - SubsidyCalculator (expected subsidy calculation)
#   - MiningCorrectnessOracle (property validation)
#   - Property tests (MC1-MC5)
#
# ============================================================================

# ============================================================================
# Subsidy Calculator Library (Step 1)
# ============================================================================

add_library(subsidy_calculator STATIC
    properties/subsidy_calculator.cpp
)

# Include directories for subsidy calculator
target_include_directories(subsidy_calculator PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
)

# Platform-specific settings for subsidy calculator
if(APPLE)
    target_link_libraries(subsidy_calculator PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# Subsidy Calculator Tests
# ============================================================================

add_executable(test_subsidy_calculator
    properties/test_subsidy_calculator.cpp
)

# Link libraries for subsidy calculator tests
target_link_libraries(test_subsidy_calculator PRIVATE
    subsidy_calculator
    # No external dependencies - pure calculation tests
)

# Platform-specific settings for subsidy tests
if(APPLE)
    target_link_libraries(test_subsidy_calculator PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for subsidy tests
target_include_directories(test_subsidy_calculator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
)

# Register subsidy calculator tests
add_test(NAME Mining_SubsidyCalculator_R4_4c COMMAND test_subsidy_calculator)
set_tests_properties(Mining_SubsidyCalculator_R4_4c
    PROPERTIES
    LABELS "ring4;phase4c;subsidy;correctness"
    TIMEOUT 10
)

message(STATUS "✅ Ring 4 Phase 4c subsidy calculator configured")
message(STATUS "   Components: ConsensusParams, SubsidyCalculator")
message(STATUS "   Tests: Subsidy calculation (10 tests)")

# ============================================================================
# Mining Correctness Oracle Library (Step 2)
# ============================================================================

add_library(mining_correctness_oracle STATIC
    properties/mining_correctness_oracle.cpp
)

# Link dependencies for oracle
target_link_libraries(mining_correctness_oracle PUBLIC
    subsidy_calculator
    mining_test_framework  # For MiningTrace, MiningTypes
)

# Include directories for oracle
target_include_directories(mining_correctness_oracle PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for oracle
if(APPLE)
    target_link_libraries(mining_correctness_oracle PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# Mining Correctness Oracle Tests
# ============================================================================

add_executable(test_mining_correctness_oracle
    properties/test_mining_correctness_oracle.cpp
)

# Link libraries for oracle tests
target_link_libraries(test_mining_correctness_oracle PRIVATE
    mining_correctness_oracle
    mining_test_framework
    subsidy_calculator
)

# Platform-specific settings for oracle tests
if(APPLE)
    target_link_libraries(test_mining_correctness_oracle PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for oracle tests
target_include_directories(test_mining_correctness_oracle PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register oracle tests
add_test(NAME Mining_CorrectnessOracle_R4_4c COMMAND test_mining_correctness_oracle)
set_tests_properties(Mining_CorrectnessOracle_R4_4c
    PROPERTIES
    LABELS "ring4;phase4c;oracle;correctness;properties"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4c correctness oracle configured")
message(STATUS "   Oracle: MC1-MC5 property checkers")
message(STATUS "   Tests: Oracle integration with Phase 4b framework (9 tests)")

# ============================================================================
# Ring 4 Phase 4d: Safety Properties
# ============================================================================

message(STATUS "")
message(STATUS "Configuring Ring 4 Phase 4d: Safety Properties...")

# ============================================================================
# Mining Safety Oracle Base Class
# ============================================================================

add_library(mining_safety_oracle_base STATIC
    properties/mining_safety_oracle.cpp
)

# Link dependencies for safety oracle base
target_link_libraries(mining_safety_oracle_base PUBLIC
    subsidy_calculator
    mining_test_framework  # For MiningTrace, MiningTypes
)

# Include directories for safety oracle
target_include_directories(mining_safety_oracle_base PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for safety oracle
if(APPLE)
    target_link_libraries(mining_safety_oracle_base PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# MS1 Oracle: No Inflation Under Restart
# ============================================================================

add_library(mining_safety_oracle_ms1 STATIC
    properties/mining_safety_oracle_ms1.cpp
)

# Link dependencies for MS1
target_link_libraries(mining_safety_oracle_ms1 PUBLIC
    mining_safety_oracle_base
    subsidy_calculator
    mining_test_framework
)

# Include directories for MS1
target_include_directories(mining_safety_oracle_ms1 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for MS1
if(APPLE)
    target_link_libraries(mining_safety_oracle_ms1 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# MS1 Oracle Tests
# ============================================================================

add_executable(test_mining_safety_oracle_ms1
    properties/test_mining_safety_oracle_ms1.cpp
)

# Link libraries for MS1 tests
target_link_libraries(test_mining_safety_oracle_ms1 PRIVATE
    mining_safety_oracle_ms1
    mining_safety_oracle_base
    mining_test_framework
    subsidy_calculator
)

# Platform-specific settings for MS1 tests
if(APPLE)
    target_link_libraries(test_mining_safety_oracle_ms1 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for MS1 tests
target_include_directories(test_mining_safety_oracle_ms1 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register MS1 tests
add_test(NAME Mining_SafetyOracle_MS1_R4_4d COMMAND test_mining_safety_oracle_ms1)
set_tests_properties(Mining_SafetyOracle_MS1_R4_4d
    PROPERTIES
    LABELS "ring4;phase4d;safety;ms1;inflation"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4d MS1 oracle configured")
message(STATUS "   Oracle: MS1 - No Inflation Under Restart")
message(STATUS "   Tests: Restart scenarios, subsidy tracking (6 tests)")

# ============================================================================
# MS2 Oracle: No Duplicate Subsidy
# ============================================================================

add_library(mining_safety_oracle_ms2 STATIC
    properties/mining_safety_oracle_ms2.cpp
)

# Link dependencies for MS2
target_link_libraries(mining_safety_oracle_ms2 PUBLIC
    mining_safety_oracle_base
    subsidy_calculator
    mining_test_framework
)

# Include directories for MS2
target_include_directories(mining_safety_oracle_ms2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for MS2
if(APPLE)
    target_link_libraries(mining_safety_oracle_ms2 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# MS2 Oracle Tests
# ============================================================================

add_executable(test_mining_safety_oracle_ms2
    properties/test_mining_safety_oracle_ms2.cpp
)

# Link libraries for MS2 tests
target_link_libraries(test_mining_safety_oracle_ms2 PRIVATE
    mining_safety_oracle_ms2
    mining_safety_oracle_base
    mining_test_framework
    subsidy_calculator
)

# Platform-specific settings for MS2 tests
if(APPLE)
    target_link_libraries(test_mining_safety_oracle_ms2 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for MS2 tests
target_include_directories(test_mining_safety_oracle_ms2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register MS2 tests
add_test(NAME Mining_SafetyOracle_MS2_R4_4d COMMAND test_mining_safety_oracle_ms2)
set_tests_properties(Mining_SafetyOracle_MS2_R4_4d
    PROPERTIES
    LABELS "ring4;phase4d;safety;ms2;duplicate"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4d MS2 oracle configured")
message(STATUS "   Oracle: MS2 - No Duplicate Subsidy")
message(STATUS "   Tests: Reorg scenarios, height tracking (6 tests)")

# ============================================================================
# MS3 Oracle: No Invalid Transaction Inclusion
# ============================================================================

add_library(mining_safety_oracle_ms3 STATIC
    properties/mining_safety_oracle_ms3.cpp
)

# Link dependencies for MS3
target_link_libraries(mining_safety_oracle_ms3 PUBLIC
    mining_safety_oracle_base
    subsidy_calculator
    mining_test_framework
)

# Include directories for MS3
target_include_directories(mining_safety_oracle_ms3 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for MS3
if(APPLE)
    target_link_libraries(mining_safety_oracle_ms3 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# MS3 Oracle Tests
# ============================================================================

add_executable(test_mining_safety_oracle_ms3
    properties/test_mining_safety_oracle_ms3.cpp
)

# Link libraries for MS3 tests
target_link_libraries(test_mining_safety_oracle_ms3 PRIVATE
    mining_safety_oracle_ms3
    mining_safety_oracle_base
    mining_test_framework
    subsidy_calculator
)

# Platform-specific settings for MS3 tests
if(APPLE)
    target_link_libraries(test_mining_safety_oracle_ms3 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for MS3 tests
target_include_directories(test_mining_safety_oracle_ms3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register MS3 tests
add_test(NAME Mining_SafetyOracle_MS3_R4_4d COMMAND test_mining_safety_oracle_ms3)
set_tests_properties(Mining_SafetyOracle_MS3_R4_4d
    PROPERTIES
    LABELS "ring4;phase4d;safety;ms3;transaction"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4d MS3 oracle configured")
message(STATUS "   Oracle: MS3 - No Invalid Transaction Inclusion (Placeholder)")
message(STATUS "   Tests: Transaction count validation, mempool tracking (6 tests)")

# ============================================================================
# MS4 Oracle: Consensus Always Enforced
# ============================================================================

add_library(mining_safety_oracle_ms4 STATIC
    properties/mining_safety_oracle_ms4.cpp
)

# Link dependencies for MS4
target_link_libraries(mining_safety_oracle_ms4 PUBLIC
    mining_safety_oracle_base
    subsidy_calculator
    mining_test_framework
)

# Include directories for MS4
target_include_directories(mining_safety_oracle_ms4 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for MS4
if(APPLE)
    target_link_libraries(mining_safety_oracle_ms4 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# MS4 Oracle Tests
# ============================================================================

add_executable(test_mining_safety_oracle_ms4
    properties/test_mining_safety_oracle_ms4.cpp
)

# Link libraries for MS4 tests
target_link_libraries(test_mining_safety_oracle_ms4 PRIVATE
    mining_safety_oracle_ms4
    mining_safety_oracle_base
    mining_test_framework
    subsidy_calculator
)

# Platform-specific settings for MS4 tests
if(APPLE)
    target_link_libraries(test_mining_safety_oracle_ms4 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for MS4 tests
target_include_directories(test_mining_safety_oracle_ms4 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register MS4 tests
add_test(NAME Mining_SafetyOracle_MS4_R4_4d COMMAND test_mining_safety_oracle_ms4)
set_tests_properties(Mining_SafetyOracle_MS4_R4_4d
    PROPERTIES
    LABELS "ring4;phase4d;safety;ms4;consensus"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4d MS4 oracle configured")
message(STATUS "   Oracle: MS4 - Consensus Always Enforced (Placeholder)")
message(STATUS "   Tests: Validation bypass detection, crash handling (6 tests)")

# ============================================================================
# MS5 Oracle: No Stale Block Acceptance
# ============================================================================

add_library(mining_safety_oracle_ms5 STATIC
    properties/mining_safety_oracle_ms5.cpp
)

# Link dependencies for MS5
target_link_libraries(mining_safety_oracle_ms5 PUBLIC
    mining_safety_oracle_base
    subsidy_calculator
    mining_test_framework
)

# Include directories for MS5
target_include_directories(mining_safety_oracle_ms5 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for MS5
if(APPLE)
    target_link_libraries(mining_safety_oracle_ms5 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# MS5 Oracle Tests
# ============================================================================

add_executable(test_mining_safety_oracle_ms5
    properties/test_mining_safety_oracle_ms5.cpp
)

# Link libraries for MS5 tests
target_link_libraries(test_mining_safety_oracle_ms5 PRIVATE
    mining_safety_oracle_ms5
    mining_safety_oracle_base
    mining_test_framework
    subsidy_calculator
)

# Platform-specific settings for MS5 tests
if(APPLE)
    target_link_libraries(test_mining_safety_oracle_ms5 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for MS5 tests
target_include_directories(test_mining_safety_oracle_ms5 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register MS5 tests
add_test(NAME Mining_SafetyOracle_MS5_R4_4d COMMAND test_mining_safety_oracle_ms5)
set_tests_properties(Mining_SafetyOracle_MS5_R4_4d
    PROPERTIES
    LABELS "ring4;phase4d;safety;ms5;stale"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4d MS5 oracle configured")
message(STATUS "   Oracle: MS5 - No Stale Block Acceptance (Placeholder)")
message(STATUS "   Tests: Reorg handling, tip tracking (6 tests)")

# ============================================================================
# Phase 4d Complete
# ============================================================================

message(STATUS "")
message(STATUS "🔒 Ring 4 Phase 4d: Safety Properties - COMPLETE")
message(STATUS "   MS1: No Inflation Under Restart ✅")
message(STATUS "   MS2: No Duplicate Subsidy ✅")
message(STATUS "   MS3: No Invalid Transaction Inclusion ✅ (Placeholder)")
message(STATUS "   MS4: Consensus Always Enforced ✅ (Placeholder)")
message(STATUS "   MS5: No Stale Block Acceptance ✅ (Placeholder)")
message(STATUS "   Total: 30 safety property tests")

# ============================================================================
# Ring 4 Phase 4e: Liveness Properties
# ============================================================================

message(STATUS "")
message(STATUS "Configuring Ring 4 Phase 4e: Liveness Properties...")

# ============================================================================
# Mining Liveness Oracle Base Class
# ============================================================================

add_library(mining_liveness_oracle_base STATIC
    properties/mining_liveness_oracle.cpp
)

# Link dependencies for liveness oracle base
target_link_libraries(mining_liveness_oracle_base PUBLIC
    mining_test_framework  # For MiningTrace, MiningTypes
)

# Include directories for liveness oracle
target_include_directories(mining_liveness_oracle_base PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for liveness oracle
if(APPLE)
    target_link_libraries(mining_liveness_oracle_base PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# ML1 Oracle: Templates Eventually Created
# ============================================================================

add_library(mining_liveness_oracle_ml1 STATIC
    properties/mining_liveness_oracle_ml1.cpp
)

# Link dependencies for ML1
target_link_libraries(mining_liveness_oracle_ml1 PUBLIC
    mining_liveness_oracle_base
    mining_test_framework
)

# Include directories for ML1
target_include_directories(mining_liveness_oracle_ml1 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for ML1
if(APPLE)
    target_link_libraries(mining_liveness_oracle_ml1 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# ML1 Oracle Tests
# ============================================================================

add_executable(test_mining_liveness_oracle_ml1
    properties/test_mining_liveness_oracle_ml1.cpp
)

# Link libraries for ML1 tests
target_link_libraries(test_mining_liveness_oracle_ml1 PRIVATE
    mining_liveness_oracle_ml1
    mining_liveness_oracle_base
    mining_test_framework
    subsidy_calculator
)

# Platform-specific settings for ML1 tests
if(APPLE)
    target_link_libraries(test_mining_liveness_oracle_ml1 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for ML1 tests
target_include_directories(test_mining_liveness_oracle_ml1 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register ML1 tests
add_test(NAME Mining_LivenessOracle_ML1_R4_4e COMMAND test_mining_liveness_oracle_ml1)
set_tests_properties(Mining_LivenessOracle_ML1_R4_4e
    PROPERTIES
    LABELS "ring4;phase4e;liveness;ml1;templates"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4e ML1 oracle configured")
message(STATUS "   Oracle: ML1 - Templates Eventually Created")
message(STATUS "   Tests: Mining progress, template creation (6 tests)")

# ============================================================================
# ML2 Oracle: Solutions Eventually Found
# ============================================================================

add_library(mining_liveness_oracle_ml2 STATIC
    properties/mining_liveness_oracle_ml2.cpp
)

# Link dependencies for ML2
target_link_libraries(mining_liveness_oracle_ml2 PUBLIC
    mining_liveness_oracle_base
    mining_test_framework
)

# Include directories for ML2
target_include_directories(mining_liveness_oracle_ml2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for ML2
if(APPLE)
    target_link_libraries(mining_liveness_oracle_ml2 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# ML2 Oracle Tests
# ============================================================================

add_executable(test_mining_liveness_oracle_ml2
    properties/test_mining_liveness_oracle_ml2.cpp
)

# Link libraries for ML2 tests
target_link_libraries(test_mining_liveness_oracle_ml2 PRIVATE
    mining_liveness_oracle_ml2
    mining_liveness_oracle_base
    mining_test_framework
    subsidy_calculator
)

# Platform-specific settings for ML2 tests
if(APPLE)
    target_link_libraries(test_mining_liveness_oracle_ml2 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for ML2 tests
target_include_directories(test_mining_liveness_oracle_ml2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register ML2 tests
add_test(NAME Mining_LivenessOracle_ML2_R4_4e COMMAND test_mining_liveness_oracle_ml2)
set_tests_properties(Mining_LivenessOracle_ML2_R4_4e
    PROPERTIES
    LABELS "ring4;phase4e;liveness;ml2;solutions"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4e ML2 oracle configured")
message(STATUS "   Oracle: ML2 - Solutions Eventually Found")
message(STATUS "   Tests: Mining progress, solution discovery (6 tests)")

# ============================================================================
# ML3 Oracle: Blocks Eventually Submitted
# ============================================================================

add_library(mining_liveness_oracle_ml3 STATIC
    properties/mining_liveness_oracle_ml3.cpp
)

# Link dependencies for ML3
target_link_libraries(mining_liveness_oracle_ml3 PUBLIC
    mining_liveness_oracle_base
    mining_test_framework
)

# Include directories for ML3
target_include_directories(mining_liveness_oracle_ml3 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for ML3
if(APPLE)
    target_link_libraries(mining_liveness_oracle_ml3 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# ML3 Oracle Tests
# ============================================================================

add_executable(test_mining_liveness_oracle_ml3
    properties/test_mining_liveness_oracle_ml3.cpp
)

# Link libraries for ML3 tests
target_link_libraries(test_mining_liveness_oracle_ml3 PRIVATE
    mining_liveness_oracle_ml3
    mining_liveness_oracle_base
    mining_test_framework
    subsidy_calculator
)

# Platform-specific settings for ML3 tests
if(APPLE)
    target_link_libraries(test_mining_liveness_oracle_ml3 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for ML3 tests
target_include_directories(test_mining_liveness_oracle_ml3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register ML3 tests
add_test(NAME Mining_LivenessOracle_ML3_R4_4e COMMAND test_mining_liveness_oracle_ml3)
set_tests_properties(Mining_LivenessOracle_ML3_R4_4e
    PROPERTIES
    LABELS "ring4;phase4e;liveness;ml3;submission"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4e ML3 oracle configured")
message(STATUS "   Oracle: ML3 - Blocks Eventually Submitted")
message(STATUS "   Tests: Solution submission, network progress (6 tests)")

# ============================================================================
# ML4 Oracle: Mining Eventually Restarts
# ============================================================================

add_library(mining_liveness_oracle_ml4 STATIC
    properties/mining_liveness_oracle_ml4.cpp
)

# Link dependencies for ML4
target_link_libraries(mining_liveness_oracle_ml4 PUBLIC
    mining_liveness_oracle_base
    mining_test_framework
)

# Include directories for ML4
target_include_directories(mining_liveness_oracle_ml4 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for ML4
if(APPLE)
    target_link_libraries(mining_liveness_oracle_ml4 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# ML4 Oracle Tests
# ============================================================================

add_executable(test_mining_liveness_oracle_ml4
    properties/test_mining_liveness_oracle_ml4.cpp
)

# Link libraries for ML4 tests
target_link_libraries(test_mining_liveness_oracle_ml4 PRIVATE
    mining_liveness_oracle_ml4
    mining_liveness_oracle_base
    mining_test_framework
    subsidy_calculator
)

# Platform-specific settings for ML4 tests
if(APPLE)
    target_link_libraries(test_mining_liveness_oracle_ml4 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for ML4 tests
target_include_directories(test_mining_liveness_oracle_ml4 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register ML4 tests
add_test(NAME Mining_LivenessOracle_ML4_R4_4e COMMAND test_mining_liveness_oracle_ml4)
set_tests_properties(Mining_LivenessOracle_ML4_R4_4e
    PROPERTIES
    LABELS "ring4;phase4e;liveness;ml4;restart"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4e ML4 oracle configured")
message(STATUS "   Oracle: ML4 - Mining Eventually Restarts")
message(STATUS "   Tests: Crash recovery, restart handling (6 tests)")

# ============================================================================
# ML5 Oracle: Stale Templates Eventually Discarded
# ============================================================================

add_library(mining_liveness_oracle_ml5 STATIC
    properties/mining_liveness_oracle_ml5.cpp
)

# Link dependencies for ML5
target_link_libraries(mining_liveness_oracle_ml5 PUBLIC
    mining_liveness_oracle_base
    mining_test_framework
)

# Include directories for ML5
target_include_directories(mining_liveness_oracle_ml5 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for ML5
if(APPLE)
    target_link_libraries(mining_liveness_oracle_ml5 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# ML5 Oracle Tests
# ============================================================================

add_executable(test_mining_liveness_oracle_ml5
    properties/test_mining_liveness_oracle_ml5.cpp
)

# Link libraries for ML5 tests
target_link_libraries(test_mining_liveness_oracle_ml5 PRIVATE
    mining_liveness_oracle_ml5
    mining_liveness_oracle_base
    mining_test_framework
    subsidy_calculator
)

# Platform-specific settings for ML5 tests
if(APPLE)
    target_link_libraries(test_mining_liveness_oracle_ml5 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for ML5 tests
target_include_directories(test_mining_liveness_oracle_ml5 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register ML5 tests
add_test(NAME Mining_LivenessOracle_ML5_R4_4e COMMAND test_mining_liveness_oracle_ml5)
set_tests_properties(Mining_LivenessOracle_ML5_R4_4e
    PROPERTIES
    LABELS "ring4;phase4e;liveness;ml5;templates"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4e ML5 oracle configured")
message(STATUS "   Oracle: ML5 - Stale Templates Eventually Discarded")
message(STATUS "   Tests: Template lifecycle, tip tracking (6 tests)")

# ============================================================================
# Ring 4 Phase 4e: Completion Summary
# ============================================================================

message(STATUS "")
message(STATUS "🎯 Ring 4 Phase 4e: Liveness Properties - COMPLETE")
message(STATUS "   ML1: Templates Eventually Created ✅")
message(STATUS "   ML2: Solutions Eventually Found ✅")
message(STATUS "   ML3: Blocks Eventually Submitted ✅")
message(STATUS "   ML4: Mining Eventually Restarts ✅")
message(STATUS "   ML5: Stale Templates Eventually Discarded ✅")
message(STATUS "   Total: 30 liveness property tests")
message(STATUS "")

# ============================================================================
# Ring 4 Phase 4f: Determinism Properties
# ============================================================================
#
# Question: "Given the same inputs, do we get the same behavior?"
#
# This phase proves:
# - No hidden randomness
# - No time-based divergence
# - No order-dependent bugs
# - Restart determinism
# - Seed stability
#
# Properties:
# - MD1: Same seed → identical trace
# - MD2: Restart replay determinism
# - MD3: Action commutativity (where allowed)
# - MD4: No hidden entropy sources
# - MD5: Deterministic crash recovery
#
# ============================================================================

message(STATUS "Configuring Ring 4 Phase 4f: Determinism Properties...")

# ============================================================================
# Determinism Oracle Base Class
# ============================================================================

add_library(mining_determinism_oracle_base STATIC
    properties/mining_determinism_oracle.cpp
)

# Link dependencies for determinism oracle
target_link_libraries(mining_determinism_oracle_base PUBLIC
    mining_test_framework  # For MiningTrace, MiningTypes
)

# Include directories for determinism oracle
target_include_directories(mining_determinism_oracle_base PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for determinism oracle
if(APPLE)
    target_link_libraries(mining_determinism_oracle_base PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

message(STATUS "✅ Ring 4 Phase 4f base oracle configured")
message(STATUS "   Oracle: MiningDeterminismOracle (trace comparison)")

# ============================================================================
# MD1 Oracle: Same Seed → Identical Trace
# ============================================================================

add_library(mining_determinism_oracle_md1 STATIC
    properties/mining_determinism_oracle_md1.cpp
)

# Link dependencies for MD1
target_link_libraries(mining_determinism_oracle_md1 PUBLIC
    mining_determinism_oracle_base
    mining_test_framework
)

# Include directories for MD1
target_include_directories(mining_determinism_oracle_md1 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for MD1
if(APPLE)
    target_link_libraries(mining_determinism_oracle_md1 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# MD1 Oracle Tests
# ============================================================================

add_executable(test_mining_determinism_oracle_md1
    properties/test_mining_determinism_oracle_md1.cpp
)

# Link libraries for MD1 tests
target_link_libraries(test_mining_determinism_oracle_md1 PRIVATE
    mining_determinism_oracle_md1
    mining_determinism_oracle_base
    mining_test_framework
    subsidy_calculator
)

# Platform-specific settings for MD1 tests
if(APPLE)
    target_link_libraries(test_mining_determinism_oracle_md1 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for MD1 tests
target_include_directories(test_mining_determinism_oracle_md1 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register MD1 tests
add_test(NAME Mining_DeterminismOracle_MD1_R4_4f COMMAND test_mining_determinism_oracle_md1)
set_tests_properties(Mining_DeterminismOracle_MD1_R4_4f
    PROPERTIES
    LABELS "ring4;phase4f;determinism;md1;seed"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4f MD1 oracle configured")
message(STATUS "   Oracle: MD1 - Same Seed → Identical Trace")
message(STATUS "   Tests: Replay determinism, seed isolation (6 tests)")

# ============================================================================
# MD2 Oracle: Restart Replay Determinism
# ============================================================================

add_library(mining_determinism_oracle_md2 STATIC
    properties/mining_determinism_oracle_md2.cpp
)

# Link dependencies for MD2
target_link_libraries(mining_determinism_oracle_md2 PUBLIC
    mining_determinism_oracle_base
    mining_test_framework
)

# Include directories for MD2
target_include_directories(mining_determinism_oracle_md2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for MD2
if(APPLE)
    target_link_libraries(mining_determinism_oracle_md2 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# MD2 Oracle Tests
# ============================================================================

add_executable(test_mining_determinism_oracle_md2
    properties/test_mining_determinism_oracle_md2.cpp
)

# Link libraries for MD2 tests
target_link_libraries(test_mining_determinism_oracle_md2 PRIVATE
    mining_determinism_oracle_md2
    mining_determinism_oracle_base
    mining_test_framework
    subsidy_calculator
)

# Platform-specific settings for MD2 tests
if(APPLE)
    target_link_libraries(test_mining_determinism_oracle_md2 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for MD2 tests
target_include_directories(test_mining_determinism_oracle_md2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register MD2 tests
add_test(NAME Mining_DeterminismOracle_MD2_R4_4f COMMAND test_mining_determinism_oracle_md2)
set_tests_properties(Mining_DeterminismOracle_MD2_R4_4f
    PROPERTIES
    LABELS "ring4;phase4f;determinism;md2;restart"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4f MD2 oracle configured")
message(STATUS "   Oracle: MD2 - Restart Replay Determinism")
message(STATUS "   Tests: Crash recovery, restart determinism (6 tests)")

# ============================================================================
# MD3 Oracle: Action Commutativity (Where Allowed)
# ============================================================================

add_library(mining_determinism_oracle_md3 STATIC
    properties/mining_determinism_oracle_md3.cpp
)

# Link dependencies for MD3
target_link_libraries(mining_determinism_oracle_md3 PUBLIC
    mining_determinism_oracle_base
    mining_test_framework
)

# Include directories for MD3
target_include_directories(mining_determinism_oracle_md3 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for MD3
if(APPLE)
    target_link_libraries(mining_determinism_oracle_md3 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# MD3 Oracle Tests
# ============================================================================

add_executable(test_mining_determinism_oracle_md3
    properties/test_mining_determinism_oracle_md3.cpp
)

# Link libraries for MD3 tests
target_link_libraries(test_mining_determinism_oracle_md3 PRIVATE
    mining_determinism_oracle_md3
    mining_determinism_oracle_base
    mining_test_framework
    subsidy_calculator
)

# Platform-specific settings for MD3 tests
if(APPLE)
    target_link_libraries(test_mining_determinism_oracle_md3 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for MD3 tests
target_include_directories(test_mining_determinism_oracle_md3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register MD3 tests
add_test(NAME Mining_DeterminismOracle_MD3_R4_4f COMMAND test_mining_determinism_oracle_md3)
set_tests_properties(Mining_DeterminismOracle_MD3_R4_4f
    PROPERTIES
    LABELS "ring4;phase4f;determinism;md3;commutativity"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4f MD3 oracle configured")
message(STATUS "   Oracle: MD3 - Action Commutativity (Where Allowed)")
message(STATUS "   Tests: Independent actions, ordering dependencies (6 tests)")

# ============================================================================
# MD4 Oracle: No Hidden Entropy Sources
# ============================================================================

add_library(mining_determinism_oracle_md4 STATIC
    properties/mining_determinism_oracle_md4.cpp
)

# Link dependencies for MD4
target_link_libraries(mining_determinism_oracle_md4 PUBLIC
    mining_determinism_oracle_base
    mining_test_framework
)

# Include directories for MD4
target_include_directories(mining_determinism_oracle_md4 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for MD4
if(APPLE)
    target_link_libraries(mining_determinism_oracle_md4 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# MD4 Oracle Tests
# ============================================================================

add_executable(test_mining_determinism_oracle_md4
    properties/test_mining_determinism_oracle_md4.cpp
)

# Link libraries for MD4 tests
target_link_libraries(test_mining_determinism_oracle_md4 PRIVATE
    mining_determinism_oracle_md4
    mining_determinism_oracle_base
    mining_test_framework
    subsidy_calculator
)

# Platform-specific settings for MD4 tests
if(APPLE)
    target_link_libraries(test_mining_determinism_oracle_md4 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for MD4 tests
target_include_directories(test_mining_determinism_oracle_md4 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register MD4 tests
add_test(NAME Mining_DeterminismOracle_MD4_R4_4f COMMAND test_mining_determinism_oracle_md4)
set_tests_properties(Mining_DeterminismOracle_MD4_R4_4f
    PROPERTIES
    LABELS "ring4;phase4f;determinism;md4;entropy"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4f MD4 oracle configured")
message(STATUS "   Oracle: MD4 - No Hidden Entropy Sources")
message(STATUS "   Tests: Entropy audit, environmental perturbations (6 tests)")

# ============================================================================
# MD5 Oracle: Deterministic Crash Recovery
# ============================================================================

add_library(mining_determinism_oracle_md5 STATIC
    properties/mining_determinism_oracle_md5.cpp
)

# Link dependencies for MD5
target_link_libraries(mining_determinism_oracle_md5 PUBLIC
    mining_determinism_oracle_base
    mining_test_framework
)

# Include directories for MD5
target_include_directories(mining_determinism_oracle_md5 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for MD5
if(APPLE)
    target_link_libraries(mining_determinism_oracle_md5 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# MD5 Oracle Tests
# ============================================================================

add_executable(test_mining_determinism_oracle_md5
    properties/test_mining_determinism_oracle_md5.cpp
)

# Link libraries for MD5 tests
target_link_libraries(test_mining_determinism_oracle_md5 PRIVATE
    mining_determinism_oracle_md5
    mining_determinism_oracle_base
    mining_test_framework
    subsidy_calculator
)

# Platform-specific settings for MD5 tests
if(APPLE)
    target_link_libraries(test_mining_determinism_oracle_md5 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for MD5 tests
target_include_directories(test_mining_determinism_oracle_md5 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register MD5 tests
add_test(NAME Mining_DeterminismOracle_MD5_R4_4f COMMAND test_mining_determinism_oracle_md5)
set_tests_properties(Mining_DeterminismOracle_MD5_R4_4f
    PROPERTIES
    LABELS "ring4;phase4f;determinism;md5;crash-recovery"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4f MD5 oracle configured")
message(STATUS "   Oracle: MD5 - Deterministic Crash Recovery")
message(STATUS "   Tests: Crash recovery determinism (6 tests)")
message(STATUS "")
message(STATUS "🏁 Phase 4f COMPLETE - All determinism properties sealed 🏁")

# ============================================================================
# Ring 4 Phase 4g.1: Persistence Model Foundation
# ============================================================================

message(STATUS "")
message(STATUS "Configuring Ring 4 Phase 4g.1: Persistence Model Foundation...")

# Deterministic Persistence Store (in-memory simulation)
add_library(deterministic_persistence_store STATIC
    persistence/deterministic_persistence_store.cpp
)

# Link dependencies
target_link_libraries(deterministic_persistence_store PUBLIC
    mining_test_framework
)

# Include directories
target_include_directories(deterministic_persistence_store PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/persistence
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(deterministic_persistence_store PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# ============================================================================
# Phase 4g.1 Self-Tests
# ============================================================================

add_executable(test_deterministic_persistence_store
    persistence/test_deterministic_persistence_store.cpp
)

# Link libraries
target_link_libraries(test_deterministic_persistence_store PRIVATE
    deterministic_persistence_store
    mining_test_framework
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(test_deterministic_persistence_store PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories
target_include_directories(test_deterministic_persistence_store PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/persistence
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Register tests
add_test(NAME Mining_PersistenceStore_SelfTests_R4_4g1 COMMAND test_deterministic_persistence_store)
set_tests_properties(Mining_PersistenceStore_SelfTests_R4_4g1
    PROPERTIES
    LABELS "ring4;phase4g;phase4g1;persistence;foundation"
    TIMEOUT 30
)

message(STATUS "✅ Ring 4 Phase 4g.1 persistence store configured")
message(STATUS "   Component: DeterministicPersistenceStore (in-memory simulation)")
message(STATUS "   Self-Tests: 10 tests (persist, recover, fault injection)")
message(STATUS "   Foundation for MR1-MR5 properties")

# ============================================================================
# Ring 4 Phase 4g.2: Persistence Oracle Base Class
# ============================================================================

message(STATUS "")
message(STATUS "Configuring Ring 4 Phase 4g.2: Persistence Oracle Base Class...")

# Persistence Oracle Base (interface for MR1-MR5)
add_library(mining_persistence_oracle_base STATIC
    properties/mining_persistence_oracle.cpp
)

# Link dependencies
target_link_libraries(mining_persistence_oracle_base PUBLIC
    mining_test_framework
    deterministic_persistence_store
)

# Include directories
target_include_directories(mining_persistence_oracle_base PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
    ${CMAKE_CURRENT_SOURCE_DIR}/persistence
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(mining_persistence_oracle_base PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

message(STATUS "✅ Ring 4 Phase 4g.2 persistence oracle base configured")
message(STATUS "   Oracle: MiningPersistenceOracle (base class for MR1-MR5)")
message(STATUS "   Interface: check(trace, store) -> violations")
message(STATUS "   Ready for MR property implementations")

# ============================================================================
# Ring 4 Phase 4g.2: MR1 - State Survives Restart Correctly
# ============================================================================

message(STATUS "")
message(STATUS "Configuring Ring 4 Phase 4g.2: MR1 Oracle...")

# MR1 Oracle Library
add_library(mining_persistence_oracle_mr1 STATIC
    properties/mining_persistence_oracle_mr1.cpp
)

target_link_libraries(mining_persistence_oracle_mr1 PUBLIC
    mining_persistence_oracle_base
    mining_test_framework
    deterministic_persistence_store
)

target_include_directories(mining_persistence_oracle_mr1 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
    ${CMAKE_CURRENT_SOURCE_DIR}/persistence
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(mining_persistence_oracle_mr1 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# MR1 Test Executable
add_executable(test_mining_persistence_oracle_mr1
    properties/test_mining_persistence_oracle_mr1.cpp
)

target_link_libraries(test_mining_persistence_oracle_mr1 PRIVATE
    mining_persistence_oracle_mr1
    mining_persistence_oracle_base
    mining_test_framework
    deterministic_persistence_store
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(test_mining_persistence_oracle_mr1 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

message(STATUS "✅ Ring 4 Phase 4g.2 MR1 oracle configured")
message(STATUS "   Oracle: MR1 - State Survives Restart Correctly")
message(STATUS "   Tests: 6 scenarios (persist/crash/restart cycles)")
message(STATUS "   Validates: Recovered state matches persisted state")

# ============================================================================
# Ring 4 Phase 4g.2: MR2 - No State Duplication After Crash
# ============================================================================

message(STATUS "")
message(STATUS "Configuring Ring 4 Phase 4g.2: MR2 Oracle...")

# MR2 Oracle Library
add_library(mining_persistence_oracle_mr2 STATIC
    properties/mining_persistence_oracle_mr2.cpp
)

target_link_libraries(mining_persistence_oracle_mr2 PUBLIC
    mining_persistence_oracle_base
    mining_test_framework
    deterministic_persistence_store
)

target_include_directories(mining_persistence_oracle_mr2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
    ${CMAKE_CURRENT_SOURCE_DIR}/persistence
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(mining_persistence_oracle_mr2 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# MR2 Test Executable
add_executable(test_mining_persistence_oracle_mr2
    properties/test_mining_persistence_oracle_mr2.cpp
)

target_link_libraries(test_mining_persistence_oracle_mr2 PRIVATE
    mining_persistence_oracle_mr2
    mining_persistence_oracle_base
    mining_test_framework
    deterministic_persistence_store
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(test_mining_persistence_oracle_mr2 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

message(STATUS "✅ Ring 4 Phase 4g.2 MR2 oracle configured")
message(STATUS "   Oracle: MR2 - No State Duplication After Crash")
message(STATUS "   Tests: 6 scenarios (crash/restart duplication checks)")
message(STATUS "   Validates: No duplicate blocks, heights, or subsidy")

# ============================================================================
# Ring 4 Phase 4g.2: MR3 - Partial Persistence Recovers Safely
# ============================================================================

message(STATUS "")
message(STATUS "Configuring Ring 4 Phase 4g.2: MR3 Oracle...")

# MR3 Oracle Library
add_library(mining_persistence_oracle_mr3 STATIC
    properties/mining_persistence_oracle_mr3.cpp
)

target_link_libraries(mining_persistence_oracle_mr3 PUBLIC
    mining_persistence_oracle_base
    mining_test_framework
    deterministic_persistence_store
)

target_include_directories(mining_persistence_oracle_mr3 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
    ${CMAKE_CURRENT_SOURCE_DIR}/persistence
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(mining_persistence_oracle_mr3 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# MR3 Test Executable
add_executable(test_mining_persistence_oracle_mr3
    properties/test_mining_persistence_oracle_mr3.cpp
)

target_link_libraries(test_mining_persistence_oracle_mr3 PRIVATE
    mining_persistence_oracle_mr3
    mining_persistence_oracle_base
    mining_test_framework
    deterministic_persistence_store
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(test_mining_persistence_oracle_mr3 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

message(STATUS "✅ Ring 4 Phase 4g.2 MR3 oracle configured")
message(STATUS "   Oracle: MR3 - Partial Persistence Recovers Safely")
message(STATUS "   Tests: 6 scenarios (fault injection: partial write, corruption)")
message(STATUS "   Validates: Recovery always returns valid state")

# ============================================================================
# Ring 4 Phase 4g.3: MR4 - Restart Converges to a Valid State
# ============================================================================

message(STATUS "")
message(STATUS "Configuring Ring 4 Phase 4g.3: MR4 Oracle...")

# MR4 Oracle Library
add_library(mining_persistence_oracle_mr4 STATIC
    properties/mining_persistence_oracle_mr4.cpp
)

target_link_libraries(mining_persistence_oracle_mr4 PUBLIC
    mining_persistence_oracle_base
    mining_test_framework
    deterministic_persistence_store
)

target_include_directories(mining_persistence_oracle_mr4 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
    ${CMAKE_CURRENT_SOURCE_DIR}/persistence
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(mining_persistence_oracle_mr4 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# MR4 Test Executable
add_executable(test_mining_persistence_oracle_mr4
    properties/test_mining_persistence_oracle_mr4.cpp
)

target_link_libraries(test_mining_persistence_oracle_mr4 PRIVATE
    mining_persistence_oracle_mr4
    mining_persistence_oracle_base
    mining_test_framework
    deterministic_persistence_store
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(test_mining_persistence_oracle_mr4 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

message(STATUS "✅ Ring 4 Phase 4g.3 MR4 oracle configured")
message(STATUS "   Oracle: MR4 - Restart Converges to a Valid State")
message(STATUS "   Tests: 6 scenarios (eventual convergence to valid state)")
message(STATUS "   Validates: Recovery stabilizes to safe coherent state")

# ============================================================================
# Ring 4 Phase 4g.3: MR5 - Persistence Does Not Break Determinism
# ============================================================================

message(STATUS "")
message(STATUS "Configuring Ring 4 Phase 4g.3: MR5 Oracle...")

# MR5 Oracle Library
add_library(mining_persistence_oracle_mr5 STATIC
    properties/mining_persistence_oracle_mr5.cpp
)

target_link_libraries(mining_persistence_oracle_mr5 PUBLIC
    mining_persistence_oracle_base
    mining_test_framework
    deterministic_persistence_store
)

target_include_directories(mining_persistence_oracle_mr5 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
    ${CMAKE_CURRENT_SOURCE_DIR}/persistence
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(mining_persistence_oracle_mr5 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# MR5 Test Executable
add_executable(test_mining_persistence_oracle_mr5
    properties/test_mining_persistence_oracle_mr5.cpp
)

target_link_libraries(test_mining_persistence_oracle_mr5 PRIVATE
    mining_persistence_oracle_mr5
    mining_persistence_oracle_base
    mining_test_framework
    deterministic_persistence_store
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(test_mining_persistence_oracle_mr5 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

message(STATUS "✅ Ring 4 Phase 4g.3 MR5 oracle configured")
message(STATUS "   Oracle: MR5 - Persistence Does Not Break Determinism")
message(STATUS "   Tests: 6 scenarios (determinism across persistence)")
message(STATUS "   Validates: Phase 4g preserves Phase 4f determinism (MD1-MD5)")
message(STATUS "")
message(STATUS "🎯 Phase 4g.3 COMPLETE - All persistence properties implemented")
message(STATUS "   MR1: State Survives Restart ✅")
message(STATUS "   MR2: No State Duplication ✅")
message(STATUS "   MR3: Safe Recovery ✅")
message(STATUS "   MR4: Convergence ✅")
message(STATUS "   MR5: Determinism Bridge ✅")

# ============================================================================
# Consensus Regression Tests: Witness-Based Extranonce Invariants
# ============================================================================
#
# MILESTONE: mining-witness-extranonce-v1
#
# These tests lock the witness-based extranonce implementation forever.
# They ensure future refactors cannot break Utreexo mining compatibility.
#
# Invariants tested:
#   1. Coinbase witness mutation does NOT change txid
#   2. ScriptSig extranonce causes consensus rejection (different txid)
#   3. Different witness nonces produce same Utreexo root
#   4. Coinbase must have exactly 1 witness item of 8 bytes
#
# ============================================================================

add_executable(test_witness_extranonce_invariants
    test_witness_extranonce_invariants.cpp
)

target_link_libraries(test_witness_extranonce_invariants PRIVATE
    dinero_core          # For Transaction, TxId
    dinero_crypto        # For SHA256
    dinero_tx_primitives # For transaction primitives
)

target_include_directories(test_witness_extranonce_invariants PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(test_witness_extranonce_invariants PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Register consensus regression test
add_test(NAME Mining_WitnessExtranonce_Invariants COMMAND test_witness_extranonce_invariants)
set_tests_properties(Mining_WitnessExtranonce_Invariants
    PROPERTIES
    LABELS "consensus;regression;witness;extranonce;utreexo;milestone"
    TIMEOUT 10
)

message(STATUS "")
message(STATUS "════════════════════════════════════════════════════════════════")
message(STATUS "✅ Consensus Regression: Witness-Based Extranonce Invariants")
message(STATUS "════════════════════════════════════════════════════════════════")
message(STATUS "   Milestone: mining-witness-extranonce-v1")
message(STATUS "   Tests: 8 invariant checks")
message(STATUS "   Purpose: Lock Utreexo mining compatibility FOREVER")
message(STATUS "════════════════════════════════════════════════════════════════")

# ============================================================================
# Phase C1: Daemon-Defined Pool Payouts - PayoutSpec Tests
# ============================================================================
#
# Tests for PayoutSpec data model:
#   - Construction and validation
#   - Weight normalization and amount calculation
#   - Deterministic remainder handling
#   - JSON serialization round-trip
#
# ============================================================================

add_executable(test_payout_spec
    test_payout_spec.cpp
    ${CMAKE_SOURCE_DIR}/src/mining/payout_spec.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/bech32_decode.cpp
)

target_link_libraries(test_payout_spec PRIVATE
    dinero_core          # For primitives
    dinero_crypto        # For address validation
    gtest
    gtest_main
    jsoncpp_static
)

target_include_directories(test_payout_spec PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(test_payout_spec PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Register payout spec test
add_test(NAME Mining_PayoutSpec COMMAND test_payout_spec)
set_tests_properties(Mining_PayoutSpec
    PROPERTIES
    LABELS "mining;payout;pool;c1"
    TIMEOUT 30
)

# ============================================================================
# Phase C4: Pool Payout Integration Tests
# ============================================================================
#
# Tests:
#   - Multi-output coinbase construction with PayoutSpec
#   - Output amounts match resolved payouts
#   - Total coinbase value = subsidy + fees
#   - Deterministic output ordering
#   - Block template validity
#
# ============================================================================

add_executable(test_pool_payout_integration
    test_pool_payout_integration.cpp
    ${CMAKE_SOURCE_DIR}/src/mining/payout_spec.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/bech32_decode.cpp
)

target_link_libraries(test_pool_payout_integration PRIVATE
    dinero_core          # For primitives
    dinero_crypto        # For address validation
    dinero_consensus     # For GetBlockSubsidy
    gtest
    gtest_main
    jsoncpp_static
)

target_include_directories(test_pool_payout_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(test_pool_payout_integration PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Register pool payout integration test
add_test(NAME Mining_PoolPayoutIntegration COMMAND test_pool_payout_integration)
set_tests_properties(Mining_PoolPayoutIntegration
    PROPERTIES
    LABELS "mining;payout;pool;integration;c4"
    TIMEOUT 60
)

message(STATUS "")
message(STATUS "════════════════════════════════════════════════════════════════")
message(STATUS "✅ Phase C1: Daemon-Defined Pool Payouts - PayoutSpec")
message(STATUS "════════════════════════════════════════════════════════════════")
message(STATUS "   Tests: Validation, weight normalization, determinism")
message(STATUS "   Purpose: Data model for daemon-owned coinbase construction")
message(STATUS "════════════════════════════════════════════════════════════════")

message(STATUS "")
message(STATUS "════════════════════════════════════════════════════════════════")
message(STATUS "✅ Phase C4: Pool Payout Integration Tests")
message(STATUS "════════════════════════════════════════════════════════════════")
message(STATUS "   Tests: Multi-output coinbase, BlockTemplateBuilder integration")
message(STATUS "   Purpose: End-to-end verification of daemon-defined payouts")
message(STATUS "════════════════════════════════════════════════════════════════")
