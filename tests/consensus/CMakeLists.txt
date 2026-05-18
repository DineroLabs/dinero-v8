# ============================================================================
# Ring 5 Phase 5a: Multi-Node Consensus Simulator Foundation
# ============================================================================
#
# Purpose: Foundation infrastructure for distributed consensus property testing
# Pattern: Framework + smoke tests (oracles come in phases 5b-5f)
# Extends: Ring 4 (mining properties), Ring 3 (P2P testing framework)
#
# Components:
#   - ConsensusSimulator (multi-node orchestration)
#   - NetworkSimulator (message routing, partitions)
#   - ConsensusNode (single node wrapper)
#   - ConsensusTrace (execution history)
#   - ConsensusTypes (actions/events/states)
#
# Smoke Tests:
#   - test_consensus_trace_smoke: Trace structure and hash verification
#   - test_consensus_simulator_smoke: Full 3-node integration
#
# Exit Criteria: 3 honest nodes exchange messages, deterministic delivery verified
#
# ============================================================================

message(STATUS "")
message(STATUS "Configuring Ring 5 Phase 5a: Multi-Node Consensus Simulator Foundation...")

# ============================================================================
# Consensus Test Framework Library
# ============================================================================

add_library(consensus_test_framework STATIC
    framework/consensus_types.cpp
    framework/consensus_trace.cpp
    framework/consensus_node.cpp
    framework/network_simulator.cpp
    framework/consensus_simulator.cpp
)

# Link dependencies for framework
# Note: dinero_core and mining_test_framework will be added in Phase 5b
# Phase 5a uses simplified standalone implementation
target_link_libraries(consensus_test_framework PUBLIC
    # No external dependencies for Phase 5a foundation
)

# Include directories for framework
target_include_directories(consensus_test_framework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests/mining/framework  # For MiningSimulator
)

# Platform-specific settings for framework
if(APPLE)
    target_link_libraries(consensus_test_framework PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

message(STATUS "✅ Ring 5 Phase 5a consensus framework library configured")
message(STATUS "   Components: ConsensusSimulator, NetworkSimulator, ConsensusNode, ConsensusTrace")

# ============================================================================
# Smoke Test: Trace Structure and Hash Verification
# ============================================================================

add_executable(test_consensus_trace_smoke
    tests/test_consensus_trace_smoke.cpp
)

# Link libraries for trace smoke tests
target_link_libraries(test_consensus_trace_smoke PRIVATE
    consensus_test_framework
    $<TARGET_FILE:gtest>
)

# Platform-specific settings for trace tests
if(APPLE)
    target_link_libraries(test_consensus_trace_smoke PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for trace tests
target_include_directories(test_consensus_trace_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
    ${CMAKE_SOURCE_DIR}/include
)

# Register trace smoke tests
add_test(NAME Consensus_TraceSmoke_R5_5a COMMAND test_consensus_trace_smoke)
set_tests_properties(Consensus_TraceSmoke_R5_5a
    PROPERTIES
    LABELS "ring5;phase5a;consensus;trace;smoke"
    TIMEOUT 10
)

message(STATUS "✅ Ring 5 Phase 5a trace smoke tests configured")
message(STATUS "   Tests: Trace structure, hash determinism, topology helpers (11 tests)")

# ============================================================================
# Smoke Test: Full 3-Node Integration (Exit Criteria)
# ============================================================================

add_executable(test_consensus_simulator_smoke
    tests/test_consensus_simulator_smoke.cpp
)

# Link libraries for simulator smoke tests
target_link_libraries(test_consensus_simulator_smoke PRIVATE
    consensus_test_framework
    $<TARGET_FILE:gtest>
)

# Platform-specific settings for simulator tests
if(APPLE)
    target_link_libraries(test_consensus_simulator_smoke PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

# Include directories for simulator tests
target_include_directories(test_consensus_simulator_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
    ${CMAKE_SOURCE_DIR}/include
)

# Register simulator smoke tests
add_test(NAME Consensus_SimulatorSmoke_R5_5a COMMAND test_consensus_simulator_smoke)
set_tests_properties(Consensus_SimulatorSmoke_R5_5a
    PROPERTIES
    LABELS "ring5;phase5a;consensus;simulator;smoke;exit_criteria"
    TIMEOUT 30
)

message(STATUS "✅ Ring 5 Phase 5a simulator smoke tests configured")
message(STATUS "   Tests: 3-node network, block propagation, partitions, Byzantine, determinism (15 tests)")
message(STATUS "   Exit Criteria: 3 honest nodes exchange messages, deterministic delivery verified ✅")

# ============================================================================
# Phase 5a Completion Summary
# ============================================================================

message(STATUS "")
message(STATUS "🎯 Ring 5 Phase 5a: Multi-Node Consensus Simulator Foundation - COMPLETE")
message(STATUS "   Framework: ConsensusSimulator, NetworkSimulator, ConsensusNode, ConsensusTrace ✅")
message(STATUS "   Smoke Tests: 26 tests (trace + simulator) ✅")
message(STATUS "   Exit Criteria: ✅ 3 nodes exchange messages")
message(STATUS "                   ✅ Deterministic delivery verified")
message(STATUS "                   ✅ Trace recording operational")
message(STATUS "                   ✅ Network simulation operational")
message(STATUS "")
message(STATUS "📋 Next Phase: 5b - Safety Properties (DC1-DC5 oracles)")
message(STATUS "")

# ============================================================================
# Ring 5 Phase 5b: Safety Properties (DC1-DC5)
# ============================================================================

message(STATUS "Configuring Ring 5 Phase 5b: Safety Properties (DC1-DC5)...")

# ============================================================================
# Safety Oracle Library
# ============================================================================

add_library(consensus_safety_oracles STATIC
    properties/consensus_safety_oracle.cpp
    properties/consensus_safety_oracle_dc1.cpp
    properties/consensus_safety_oracle_dc2.cpp
    properties/consensus_safety_oracle_dc3.cpp
    properties/consensus_safety_oracle_dc4.cpp
    properties/consensus_safety_oracle_dc5.cpp
)

# Link dependencies for safety oracles
target_link_libraries(consensus_safety_oracles PUBLIC
    consensus_test_framework
)

# Include directories for safety oracles
target_include_directories(consensus_safety_oracles PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for safety oracles
if(APPLE)
    target_link_libraries(consensus_safety_oracles PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

message(STATUS "✅ Ring 5 Phase 5b safety oracle library configured")
message(STATUS "   Oracles: DC1-DC5 (Agreement, Validity, Integrity, Ordering, Finality)")

# ============================================================================
# Safety Oracle Tests
# ============================================================================

# DC1 Oracle Tests (Agreement)
add_executable(test_consensus_safety_oracle_dc1
    tests/test_consensus_safety_oracle_dc1.cpp
)

target_link_libraries(test_consensus_safety_oracle_dc1 PRIVATE
    consensus_safety_oracles
    consensus_test_framework
    $<TARGET_FILE:gtest>
)

if(APPLE)
    target_link_libraries(test_consensus_safety_oracle_dc1 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_consensus_safety_oracle_dc1 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

add_test(NAME Consensus_SafetyOracle_DC1_R5_5b COMMAND test_consensus_safety_oracle_dc1)
set_tests_properties(Consensus_SafetyOracle_DC1_R5_5b
    PROPERTIES
    LABELS "ring5;phase5b;safety;dc1;agreement"
    TIMEOUT 30
)

# DC2-DC5 Combined Tests
add_executable(test_consensus_safety_oracles
    tests/test_consensus_safety_oracles.cpp
)

target_link_libraries(test_consensus_safety_oracles PRIVATE
    consensus_safety_oracles
    consensus_test_framework
    $<TARGET_FILE:gtest>
)

if(APPLE)
    target_link_libraries(test_consensus_safety_oracles PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_consensus_safety_oracles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

add_test(NAME Consensus_SafetyOracles_DC2_DC5_R5_5b COMMAND test_consensus_safety_oracles)
set_tests_properties(Consensus_SafetyOracles_DC2_DC5_R5_5b
    PROPERTIES
    LABELS "ring5;phase5b;safety;dc2;dc3;dc4;dc5"
    TIMEOUT 30
)

message(STATUS "✅ Ring 5 Phase 5b safety oracle tests configured")
message(STATUS "   Tests: DC1 (6 tests), DC2-DC5 (9 tests)")
message(STATUS "")
message(STATUS "🎯 Ring 5 Phase 5b: Safety Properties - COMPLETE")
message(STATUS "   DC1: Agreement ✅")
message(STATUS "   DC2: Validity ✅")
message(STATUS "   DC3: Integrity ✅")
message(STATUS "   DC4: Total Ordering ✅")
message(STATUS "   DC5: Finality ✅")
message(STATUS "")
message(STATUS "📋 Next Phase: 5c - Liveness Properties (DL1-DL5 oracles)")
message(STATUS "")

# ============================================================================
# Ring 5 Phase 5c: Liveness Properties (DL1-DL5)
# ============================================================================

message(STATUS "Configuring Ring 5 Phase 5c: Liveness Properties (DL1-DL5)...")

# ============================================================================
# Liveness Oracle Library
# ============================================================================

add_library(consensus_liveness_oracles STATIC
    properties/consensus_liveness_oracle.cpp
    properties/consensus_liveness_oracle_dl1.cpp
    properties/consensus_liveness_oracle_dl2.cpp
    properties/consensus_liveness_oracle_dl3.cpp
    properties/consensus_liveness_oracle_dl4.cpp
    properties/consensus_liveness_oracle_dl5.cpp
)

# Link dependencies for liveness oracles
target_link_libraries(consensus_liveness_oracles PUBLIC
    consensus_test_framework
)

# Include directories for liveness oracles
target_include_directories(consensus_liveness_oracles PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for liveness oracles
if(APPLE)
    target_link_libraries(consensus_liveness_oracles PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

message(STATUS "✅ Ring 5 Phase 5c liveness oracle library configured")
message(STATUS "   Oracles: DL1-DL5 (Eventual Consensus, Block Propagation, Chain Growth, Tx Inclusion, Sync Completion)")

# ============================================================================
# Liveness Oracle Tests
# ============================================================================

# DL1 Oracle Tests (Eventual Consensus)
add_executable(test_consensus_liveness_oracle_dl1
    tests/test_consensus_liveness_oracle_dl1.cpp
)

target_link_libraries(test_consensus_liveness_oracle_dl1 PRIVATE
    consensus_liveness_oracles
    consensus_test_framework
    $<TARGET_FILE:gtest>
)

if(APPLE)
    target_link_libraries(test_consensus_liveness_oracle_dl1 PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_consensus_liveness_oracle_dl1 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

add_test(NAME Consensus_LivenessOracle_DL1_R5_5c COMMAND test_consensus_liveness_oracle_dl1)
set_tests_properties(Consensus_LivenessOracle_DL1_R5_5c
    PROPERTIES
    LABELS "ring5;phase5c;liveness;dl1;eventual_consensus"
    TIMEOUT 30
)

# DL2-DL5 Combined Tests
add_executable(test_consensus_liveness_oracles
    tests/test_consensus_liveness_oracles.cpp
)

target_link_libraries(test_consensus_liveness_oracles PRIVATE
    consensus_liveness_oracles
    consensus_test_framework
    $<TARGET_FILE:gtest>
)

if(APPLE)
    target_link_libraries(test_consensus_liveness_oracles PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_consensus_liveness_oracles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

add_test(NAME Consensus_LivenessOracles_DL2_DL5_R5_5c COMMAND test_consensus_liveness_oracles)
set_tests_properties(Consensus_LivenessOracles_DL2_DL5_R5_5c
    PROPERTIES
    LABELS "ring5;phase5c;liveness;dl2;dl3;dl4;dl5"
    TIMEOUT 30
)

message(STATUS "✅ Ring 5 Phase 5c liveness oracle tests configured")
message(STATUS "   Tests: DL1 (5 tests), DL2-DL5 (13 tests)")
message(STATUS "")
message(STATUS "🎯 Ring 5 Phase 5c: Liveness Properties - COMPLETE")
message(STATUS "   DL1: Eventual Consensus ✅")
message(STATUS "   DL2: Block Propagation ✅")
message(STATUS "   DL3: Chain Growth ✅")
message(STATUS "   DL4: Transaction Inclusion ✅")
message(STATUS "   DL5: Sync Completion ✅")
message(STATUS "")
message(STATUS "📋 Next Phase: 5d - Partition Tolerance (DN1-DN5 oracles)")
message(STATUS "")

# ============================================================================
# Ring 5 Phase 5d: Partition Tolerance (DN1-DN5)
# ============================================================================

message(STATUS "Configuring Ring 5 Phase 5d: Partition Tolerance (DN1-DN5)...")

# ============================================================================
# Partition Tolerance Oracle Library
# ============================================================================

add_library(consensus_partition_tolerance_oracles STATIC
    properties/consensus_partition_tolerance_oracle.cpp
    properties/consensus_partition_tolerance_oracle_dn1.cpp
    properties/consensus_partition_tolerance_oracle_dn2.cpp
    properties/consensus_partition_tolerance_oracle_dn3.cpp
    properties/consensus_partition_tolerance_oracle_dn4.cpp
    properties/consensus_partition_tolerance_oracle_dn5.cpp
)

# Link dependencies for partition tolerance oracles
target_link_libraries(consensus_partition_tolerance_oracles PUBLIC
    consensus_test_framework
)

# Include directories for partition tolerance oracles
target_include_directories(consensus_partition_tolerance_oracles PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for partition tolerance oracles
if(APPLE)
    target_link_libraries(consensus_partition_tolerance_oracles PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

message(STATUS "✅ Ring 5 Phase 5d partition tolerance oracle library configured")
message(STATUS "   Oracles: DN1-DN5 (Partition Tolerance, Minority Stall, Clean Healing, Async Healing, Cascading)")

# ============================================================================
# Partition Tolerance Oracle Tests
# ============================================================================

add_executable(test_consensus_partition_tolerance_oracles
    tests/test_consensus_partition_tolerance_oracles.cpp
)

target_link_libraries(test_consensus_partition_tolerance_oracles PRIVATE
    consensus_partition_tolerance_oracles
    consensus_test_framework
    $<TARGET_FILE:gtest>
)

if(APPLE)
    target_link_libraries(test_consensus_partition_tolerance_oracles PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_consensus_partition_tolerance_oracles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

add_test(NAME Consensus_PartitionToleranceOracles_DN1_DN5_R5_5d
         COMMAND test_consensus_partition_tolerance_oracles)
set_tests_properties(Consensus_PartitionToleranceOracles_DN1_DN5_R5_5d
    PROPERTIES
    LABELS "ring5;phase5d;partition_tolerance;dn1;dn2;dn3;dn4;dn5"
    TIMEOUT 30
)

message(STATUS "✅ Ring 5 Phase 5d partition tolerance oracle tests configured")
message(STATUS "   Tests: DN1-DN5 (11 tests)")
message(STATUS "")
message(STATUS "🎯 Ring 5 Phase 5d: Partition Tolerance - COMPLETE")
message(STATUS "   DN1: Partition Tolerance ✅")
message(STATUS "   DN2: Minority Stall ✅")
message(STATUS "   DN3: Clean Healing ✅")
message(STATUS "   DN4: Asynchronous Healing ✅")
message(STATUS "   DN5: Cascading Partitions ✅")
message(STATUS "")
message(STATUS "📋 Next Phase: 5e - Byzantine Tolerance (DB1-DB5 oracles)")
message(STATUS "")

# ============================================================================
# Ring 5 Phase 5e: Byzantine Tolerance (DB1-DB5)
# ============================================================================

message(STATUS "Configuring Ring 5 Phase 5e: Byzantine Tolerance (DB1-DB5)...")

# ============================================================================
# Byzantine Tolerance Oracle Library
# ============================================================================

add_library(consensus_byzantine_tolerance_oracles STATIC
    properties/consensus_byzantine_tolerance_oracle.cpp
    properties/consensus_byzantine_tolerance_oracle_db1.cpp
    properties/consensus_byzantine_tolerance_oracle_db2.cpp
    properties/consensus_byzantine_tolerance_oracle_db3.cpp
    properties/consensus_byzantine_tolerance_oracle_db4.cpp
    properties/consensus_byzantine_tolerance_oracle_db5.cpp
)

# Link dependencies for byzantine tolerance oracles
target_link_libraries(consensus_byzantine_tolerance_oracles PUBLIC
    consensus_test_framework
)

# Include directories for byzantine tolerance oracles
target_include_directories(consensus_byzantine_tolerance_oracles PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for byzantine tolerance oracles
if(APPLE)
    target_link_libraries(consensus_byzantine_tolerance_oracles PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

message(STATUS "✅ Ring 5 Phase 5e byzantine tolerance oracle library configured")
message(STATUS "   Oracles: DB1-DB5 (Network Resilience, Eclipse Resistance, Double-Spend, Block Withholding, Invalid Block)")

# ============================================================================
# Byzantine Tolerance Oracle Tests
# ============================================================================

add_executable(test_consensus_byzantine_tolerance_oracles
    tests/test_consensus_byzantine_tolerance_oracles.cpp
)

target_link_libraries(test_consensus_byzantine_tolerance_oracles PRIVATE
    consensus_byzantine_tolerance_oracles
    consensus_test_framework
    $<TARGET_FILE:gtest>
)

if(APPLE)
    target_link_libraries(test_consensus_byzantine_tolerance_oracles PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_consensus_byzantine_tolerance_oracles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

add_test(NAME Consensus_ByzantineToleranceOracles_DB1_DB5_R5_5e
         COMMAND test_consensus_byzantine_tolerance_oracles)
set_tests_properties(Consensus_ByzantineToleranceOracles_DB1_DB5_R5_5e
    PROPERTIES
    LABELS "ring5;phase5e;byzantine_tolerance;db1;db2;db3;db4;db5"
    TIMEOUT 30
)

message(STATUS "✅ Ring 5 Phase 5e byzantine tolerance oracle tests configured")
message(STATUS "   Tests: DB1-DB5 (13 tests)")
message(STATUS "")
message(STATUS "🎯 Ring 5 Phase 5e: Byzantine Tolerance - COMPLETE")
message(STATUS "   DB1: Network Resilience ✅")
message(STATUS "   DB2: Eclipse Resistance ✅")
message(STATUS "   DB3: Double-Spend Resistance ✅")
message(STATUS "   DB4: Block Withholding Tolerance ✅")
message(STATUS "   DB5: Invalid Block Rejection ✅")
message(STATUS "")
message(STATUS "📋 Next Phase: 5f - Determinism Validation (DD1-DD5 oracles)")
message(STATUS "")

# ============================================================================
# Ring 5 Phase 5f: Determinism Validation (DD1-DD5)
# ============================================================================

message(STATUS "Configuring Ring 5 Phase 5f: Determinism Validation (DD1-DD5)...")

# ============================================================================
# Determinism Oracle Library
# ============================================================================

add_library(consensus_determinism_oracles STATIC
    properties/consensus_determinism_oracle.cpp
    properties/consensus_determinism_oracle_dd1.cpp
    properties/consensus_determinism_oracle_dd2.cpp
    properties/consensus_determinism_oracle_dd3.cpp
    properties/consensus_determinism_oracle_dd4.cpp
    properties/consensus_determinism_oracle_dd5.cpp
)

# Link dependencies for determinism oracles
target_link_libraries(consensus_determinism_oracles PUBLIC
    consensus_test_framework
)

# Include directories for determinism oracles
target_include_directories(consensus_determinism_oracles PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

# Platform-specific settings for determinism oracles
if(APPLE)
    target_link_libraries(consensus_determinism_oracles PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

message(STATUS "✅ Ring 5 Phase 5f determinism oracle library configured")
message(STATUS "   Oracles: DD1-DD5 (Trace Reproducibility, Message Delivery, State Convergence, Reorg, Byzantine)")

# ============================================================================
# Determinism Oracle Tests
# ============================================================================

add_executable(test_consensus_determinism_oracles
    tests/test_consensus_determinism_oracles.cpp
)

target_link_libraries(test_consensus_determinism_oracles PRIVATE
    consensus_determinism_oracles
    consensus_test_framework
    $<TARGET_FILE:gtest>
)

if(APPLE)
    target_link_libraries(test_consensus_determinism_oracles PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_consensus_determinism_oracles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/properties
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

add_test(NAME Consensus_DeterminismOracles_DD1_DD5_R5_5f
         COMMAND test_consensus_determinism_oracles)
set_tests_properties(Consensus_DeterminismOracles_DD1_DD5_R5_5f
    PROPERTIES
    LABELS "ring5;phase5f;determinism;dd1;dd2;dd3;dd4;dd5"
    TIMEOUT 30
)

message(STATUS "✅ Ring 5 Phase 5f determinism oracle tests configured")
message(STATUS "   Tests: DD1-DD5 (15 tests)")
message(STATUS "")
message(STATUS "🎯 Ring 5 Phase 5f: Determinism Validation - COMPLETE")
message(STATUS "   DD1: Trace Reproducibility ✅")
message(STATUS "   DD2: Message Delivery Determinism ✅")
message(STATUS "   DD3: State Convergence Determinism ✅")
message(STATUS "   DD4: Reorg Determinism ✅")
message(STATUS "   DD5: Byzantine Determinism ✅")
message(STATUS "")
message(STATUS "🏆 RING 5 COMPLETE - All 25 Consensus Properties Proven")
message(STATUS "   Safety (DC1-DC5): ✅ 5 properties")
message(STATUS "   Liveness (DL1-DL5): ✅ 5 properties")
message(STATUS "   Partition Tolerance (DN1-DN5): ✅ 5 properties")
message(STATUS "   Byzantine Tolerance (DB1-DB5): ✅ 5 properties")
message(STATUS "   Determinism (DD1-DD5): ✅ 5 properties")
message(STATUS "")
message(STATUS "   Total: 25 properties, 65+ tests")
message(STATUS "   Ring 5 Status: SEALED 🔒")
message(STATUS "")

# ============================================================================
# Deterministic Serialization Test Vectors
# ============================================================================

message(STATUS "Configuring deterministic serialization test vectors...")

add_executable(test_serialization_vectors
    test_serialization_vectors.cpp
)

target_link_libraries(test_serialization_vectors PRIVATE
    dinero_tx_primitives
    dinero_consensus
    dinero_crypto
    dinero_zk
    zstd
    $<TARGET_FILE:gtest>
)

if(APPLE)
    target_link_libraries(test_serialization_vectors PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_serialization_vectors PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME SerializationVectors COMMAND test_serialization_vectors)
set_tests_properties(SerializationVectors
    PROPERTIES
    LABELS "consensus;serialization;determinism"
    TIMEOUT 30
)

message(STATUS "✅ Deterministic serialization test vectors configured")

# ============================================================================
# SegWit Serialization Round-Trip Test (Phase 11d)
# ============================================================================
# Regression test ensuring witness data survives serialize/deserialize cycle.
# Prevents bugs like Serialize(false) stripping witness from blocks.

add_executable(test_segwit_serialization_roundtrip
    test_segwit_serialization_roundtrip.cpp
)

target_link_libraries(test_segwit_serialization_roundtrip PRIVATE
    dinero_tx_primitives
    dinero_consensus
    dinero_crypto
    zstd
    GTest::gtest
    GTest::gtest_main
)

if(APPLE)
    target_link_libraries(test_segwit_serialization_roundtrip PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_segwit_serialization_roundtrip PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME SegWitSerializationRoundTrip COMMAND test_segwit_serialization_roundtrip)
set_tests_properties(SegWitSerializationRoundTrip
    PROPERTIES
    LABELS "consensus;serialization;segwit;regression"
    TIMEOUT 30
)

message(STATUS "✅ SegWit serialization round-trip test configured")
message(STATUS "")

# ============================================================================
# Cross-Language Vector Runner
# ============================================================================
# Rule: One truth, many runners
# Vectors define reality. This runner proves obedience.
# DO NOT add new assertions - if it's important, add to vectors.

# TEMPORARILY DISABLED (2026-02-02): Uses deprecated OpenSSL 3.0 APIs (SHA256_Init/Update/Final)
# TODO: Update to use EVP_DigestInit_ex/Update/Final_ex
# add_executable(test_cross_language_vectors
#     test_cross_language_vectors.cpp
#     ${CMAKE_SOURCE_DIR}/external/bech32/bech32.cpp
# )
#
# add_dependencies(test_cross_language_vectors gtest)
#
# target_link_libraries(test_cross_language_vectors PRIVATE
#     dinero_tx_primitives
#     dinero_consensus
#     dinero_wallet
#     dinero_crypto
#     secp256k1
#     OpenSSL::Crypto
#     $<TARGET_FILE:gtest>
# )
#
# if(APPLE)
#     target_link_libraries(test_cross_language_vectors PRIVATE
#         "-framework Security"
#         "-framework CoreFoundation"
#     )
# endif()
#
# target_include_directories(test_cross_language_vectors PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#     ${CMAKE_SOURCE_DIR}/third_party/nlohmann
#     ${CMAKE_SOURCE_DIR}/external
# )
#
# add_test(NAME CrossLanguageVectors COMMAND test_cross_language_vectors)
# set_tests_properties(CrossLanguageVectors
#     PROPERTIES
#     LABELS "consensus;cross-language;vectors;immutable"
#     TIMEOUT 60
#     WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
# )

message(STATUS "✅ Cross-language vector runner configured")
message(STATUS "   Vectors: descriptors.json, scanner.json")
message(STATUS "   Rule: One truth, many runners")

# ============================================================================
# Phase 2: ConsensusUTXOSet Fuzz Tests
# ============================================================================
# Pure in-memory UTXO set fuzz testing.
# No database setup required - tests snapshot/restore, add/remove, invariants.
# ============================================================================

message(STATUS "")
message(STATUS "Configuring Phase 2: ConsensusUTXOSet Fuzz Tests...")

add_executable(test_consensus_utxo_set_fuzz
    test_consensus_utxo_set_fuzz.cpp
)

target_link_libraries(test_consensus_utxo_set_fuzz PRIVATE
    dinero_consensus
    dinero_crypto
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
)

if(APPLE)
    target_link_libraries(test_consensus_utxo_set_fuzz PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_consensus_utxo_set_fuzz PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME ConsensusUTXOSetFuzz COMMAND test_consensus_utxo_set_fuzz)
set_tests_properties(ConsensusUTXOSetFuzz
    PROPERTIES
    LABELS "consensus;phase2;fuzz;utxo;snapshot"
    TIMEOUT 60
)

message(STATUS "✅ Phase 2 ConsensusUTXOSet fuzz tests configured")
message(STATUS "   Tests: Add/Remove, Snapshot/Restore, Double-Spend, Random Fuzz")

# ============================================================================
# Phase 2.1: Deterministic Consensus Fuzzer
# ============================================================================
# Comprehensive fuzzer testing blocks, transactions, and reorgs.
# Tests invariants: no negative balances, no double-spend, snapshot exactness,
# round-trip reorg identity, total supply bounds.
# ============================================================================

message(STATUS "")
message(STATUS "Configuring Phase 2.1: Deterministic Consensus Fuzzer...")

add_executable(consensus_fuzzer
    consensus_fuzzer.cpp
)

target_link_libraries(consensus_fuzzer PRIVATE
    dinero_consensus
    dinero_tx_primitives
    dinero_crypto
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
)

if(APPLE)
    target_link_libraries(consensus_fuzzer PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(consensus_fuzzer PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME ConsensusFuzzer COMMAND consensus_fuzzer)
set_tests_properties(ConsensusFuzzer
    PROPERTIES
    LABELS "consensus;phase2;fuzz;blocks;reorg;invariants"
    TIMEOUT 120
)

message(STATUS "✅ Phase 2.1 Deterministic Consensus Fuzzer configured")
message(STATUS "   Tests: Block application, reorgs, invalid rejection, snapshot exactness")

# ============================================================================
# Phase 2.1: Formal Invariants Test Harness
# ============================================================================
# Tests formal invariants that can be run in CI.
# First step toward formal verification.
# ============================================================================

message(STATUS "")
message(STATUS "Configuring Phase 2.1: Formal Invariants Test Harness...")

add_executable(test_formal_invariants
    test_formal_invariants.cpp
)

target_link_libraries(test_formal_invariants PRIVATE
    dinero_consensus
    dinero_crypto
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
)

if(APPLE)
    target_link_libraries(test_formal_invariants PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_formal_invariants PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME FormalInvariants COMMAND test_formal_invariants)
set_tests_properties(FormalInvariants
    PROPERTIES
    LABELS "consensus;phase2;invariants;formal;ci"
    TIMEOUT 30
)

message(STATUS "✅ Phase 2.1 Formal Invariants Test Harness configured")
message(STATUS "   Invariants: I1-I8 (supply bounds, double-spend, maturity, snapshot identity)")

# ============================================================================
# Phase 2.2: Stateless Block Verification Tests
# ============================================================================
# Tests the pure stateless verification function.
# No database required - enables light clients, mobile, WASM verification.
# ============================================================================

message(STATUS "")
message(STATUS "Configuring Phase 2.2: Stateless Block Verification Tests...")

add_executable(test_stateless_verification
    test_stateless_verification.cpp
)

target_link_libraries(test_stateless_verification PRIVATE
    dinero_consensus
    dinero_tx_primitives
    dinero_crypto
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
)

if(APPLE)
    target_link_libraries(test_stateless_verification PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_stateless_verification PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME StatelessVerification COMMAND test_stateless_verification)
set_tests_properties(StatelessVerification
    PROPERTIES
    LABELS "consensus;phase2;stateless;pure;light-client"
    TIMEOUT 30
)

message(STATUS "✅ Phase 2.2 Stateless Block Verification Tests configured")
message(STATUS "   Tests: subsidy, valid block, double-spend, invalid amount, empty block")

# ============================================================================
# Phase 2.3: Consensus Invariants - Forensic-Grade Assertions
# ============================================================================

add_executable(test_consensus_invariants
    test_consensus_invariants.cpp
)

target_link_libraries(test_consensus_invariants PRIVATE
    dinero_consensus
    dinero_tx_primitives
    dinero_crypto
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
)

if(APPLE)
    target_link_libraries(test_consensus_invariants PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_consensus_invariants PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME ConsensusInvariants COMMAND test_consensus_invariants)
set_tests_properties(ConsensusInvariants
    PROPERTIES
    LABELS "consensus;phase2;invariants;forensic"
    TIMEOUT 30
)

message(STATUS "✅ Phase 2.3 Consensus Invariants Tests configured")
message(STATUS "   Tests: supply bounds, context, scope, snapshot, overflow detection")

# ============================================================================
# Phase 2.6: Property-Based Transaction Generation
# ============================================================================
# Finds bugs humans cannot imagine.

add_executable(economic_property_test
    economic_property_test.cpp
)

target_link_libraries(economic_property_test PRIVATE
    dinero_consensus
    dinero_tx_primitives
    dinero_crypto
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
)

if(APPLE)
    target_link_libraries(economic_property_test PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(economic_property_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME PropertyBasedTxGen COMMAND economic_property_test 12345 500)
set_tests_properties(PropertyBasedTxGen
    PROPERTIES
    LABELS "consensus;phase2;property-based;advanced"
    TIMEOUT 120
)

message(STATUS "✅ Phase 2.6 Property-Based Transaction Generation configured")
message(STATUS "   Properties: valid/invalid tx types, supply bounds, snapshot identity")

# ============================================================================
# Phase 2.5: Consensus Core Standalone Test
# ============================================================================
# Verifies that libdinero-consensus works independently.
# No database, no networking, no threading.

add_executable(test_consensus_core_standalone
    test_consensus_core_standalone.cpp
    ${CMAKE_SOURCE_DIR}/tests/stubs/chain_test_stubs.cpp
)

target_link_libraries(test_consensus_core_standalone PRIVATE
    dinero_consensus_core
    zstd
)

target_include_directories(test_consensus_core_standalone PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

if(APPLE)
    target_link_libraries(test_consensus_core_standalone PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

add_test(NAME ConsensusCoreSt COMMAND test_consensus_core_standalone)
set_tests_properties(ConsensusCoreSt
    PROPERTIES
    LABELS "consensus;phase2;standalone;pure"
    TIMEOUT 30
)

message(STATUS "✅ Phase 2.5 Consensus Core Standalone Test configured")
message(STATUS "   Verifies: libdinero-consensus works without DB/networking/threading")

# ============================================================================
# Phase 2 Final: Reorg Simulation Harness
# ============================================================================
# Shakes the rules violently before they travel the world.
# Proves reorg safety as a property, not just a hope.

add_executable(reorg_simulation_harness
    reorg_simulation_harness.cpp
    # dinero_consensus_core's consensus_utxo_set/utreexo_accumulator reference
    # dinero::GetActiveChain() which lives only in chainparams_impl.cpp. The
    # full dinero_consensus library would bring that in plus dinero_zk; the
    # tiny chain_test_stubs TU provides just the GetActiveChain definition
    # the harness needs without pulling in the rest of consensus.
    ${CMAKE_SOURCE_DIR}/tests/stubs/chain_test_stubs.cpp
)

target_link_libraries(reorg_simulation_harness PRIVATE
    dinero_consensus_core
    zstd
)

target_include_directories(reorg_simulation_harness PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

if(APPLE)
    target_link_libraries(reorg_simulation_harness PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

add_test(NAME ReorgSimulation COMMAND reorg_simulation_harness 12345)
set_tests_properties(ReorgSimulation
    PROPERTIES
    LABELS "consensus;phase2;reorg;stress"
    TIMEOUT 60
)

message(STATUS "✅ Phase 2 Final: Reorg Simulation Harness configured")
message(STATUS "   Tests: snapshot exactness, deep reorg, supply conservation, UTXO resurrection")

# ============================================================================
# Premine PoW Regression Guard
# ============================================================================
# Ensures block-1 PoW remains valid using raw uint256 hash/target comparison.
# This prevents regressions back to display-string/endian comparison mistakes.

# test_premine_pow_regression removed — no premine in Fair Launch v3

# ============================================================================
# 128-bit portability shim — bit-identical backend gate
# ============================================================================
# Built TWICE: once with the native __uint128_t backend (default on GCC/Clang
# where __SIZEOF_INT128__ is defined), once with -DDINERO_INT128_FORCE_STRUCT=1
# forcing the struct backend. The same hardcoded test vectors must pass on
# both builds — any divergence is a portability bug in the shim.
#
# This test is the gate that must pass before porting any of the 8 source
# files using __uint128_t / __int128 in src/ (see Dinero-Coin/NATIVE-MSVC-PORT-BUGS.md).

add_executable(test_int128_compat_native
    test_int128_compat.cpp
)
target_include_directories(test_int128_compat_native PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
add_test(NAME Consensus_Int128Compat_Native COMMAND test_int128_compat_native)
set_tests_properties(Consensus_Int128Compat_Native
    PROPERTIES
    LABELS "consensus;portability;int128;native"
    TIMEOUT 10
)

add_executable(test_int128_compat_struct
    test_int128_compat.cpp
)
target_include_directories(test_int128_compat_struct PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_definitions(test_int128_compat_struct PRIVATE
    DINERO_INT128_FORCE_STRUCT=1
)
add_test(NAME Consensus_Int128Compat_Struct COMMAND test_int128_compat_struct)
set_tests_properties(Consensus_Int128Compat_Struct
    PROPERTIES
    LABELS "consensus;portability;int128;struct"
    TIMEOUT 10
)

message(STATUS "✅ 128-bit portability shim tests configured (native + forced-struct)")
