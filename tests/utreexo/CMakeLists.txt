# ============================================================================
# Phase U: Utreexo Semantic Correctness Tests
# ============================================================================
#
# CONSENSUS CRITICAL - These tests validate Utreexo at the semantic level.
# Failure of any test indicates a potential network fork bug.
#
# Tests:
#   U.1 - Proof Generation Correctness (INV-1 through INV-5)
#   U.2 - Forest Mutation Correctness (INV-1 through INV-7)
#   U.3 - Spend-Path Validation (THE CONSENSUS GATE)
#
# ============================================================================

message(STATUS "")
message(STATUS "Configuring Phase U: Utreexo Semantic Correctness Tests...")

# ============================================================================
# U.1: Proof Generation Correctness
# ============================================================================

add_executable(test_utreexo_proof_generation
    test_proof_generation_correctness.cpp
)

target_link_libraries(test_utreexo_proof_generation PRIVATE
    dinero_consensus
    dinero_tx_primitives
    dinero_crypto
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
)

if(APPLE)
    target_link_libraries(test_utreexo_proof_generation PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_utreexo_proof_generation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME Utreexo_ProofGeneration_U1 COMMAND test_utreexo_proof_generation)
set_tests_properties(Utreexo_ProofGeneration_U1
    PROPERTIES
    LABELS "utreexo;phase_u;consensus;proof_generation;critical"
    TIMEOUT 60
)

message(STATUS "  U.1: Proof Generation Correctness configured")
message(STATUS "       INV-1: Sibling count == tree height")
message(STATUS "       INV-2: No duplicate siblings")
message(STATUS "       INV-3: Deterministic ordering")
message(STATUS "       INV-4: Round-trip verification")
message(STATUS "       INV-5: Tamper detection")

# ============================================================================
# U.2: Forest Mutation Correctness
# ============================================================================

add_executable(test_utreexo_forest_mutation
    test_forest_mutation_correctness.cpp
)

target_link_libraries(test_utreexo_forest_mutation PRIVATE
    dinero_consensus
    dinero_tx_primitives
    dinero_crypto
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
)

if(APPLE)
    target_link_libraries(test_utreexo_forest_mutation PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_utreexo_forest_mutation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME Utreexo_ForestMutation_U2 COMMAND test_utreexo_forest_mutation)
set_tests_properties(Utreexo_ForestMutation_U2
    PROPERTIES
    LABELS "utreexo;phase_u;consensus;forest_mutation;critical"
    TIMEOUT 60
)

message(STATUS "  U.2: Forest Mutation Correctness configured")
message(STATUS "       INV-1: Root changes after add")
message(STATUS "       INV-2: Root changes after remove")
message(STATUS "       INV-3: Remove requires valid proof")
message(STATUS "       INV-4: Snapshot/restore identity")
message(STATUS "       INV-5: Deterministic roots")
message(STATUS "       INV-6: Leaf count tracking")
message(STATUS "       INV-7: Remove order independence")

# ============================================================================
# U.3: Spend-Path Validation (THE CONSENSUS GATE)
# ============================================================================

add_executable(test_utreexo_spend_path_validation
    test_spend_path_validation.cpp
)

target_link_libraries(test_utreexo_spend_path_validation PRIVATE
    dinero_consensus
    dinero_tx_primitives
    dinero_crypto
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
)

if(APPLE)
    target_link_libraries(test_utreexo_spend_path_validation PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_utreexo_spend_path_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME Utreexo_SpendPathValidation_U3 COMMAND test_utreexo_spend_path_validation)
set_tests_properties(Utreexo_SpendPathValidation_U3
    PROPERTIES
    LABELS "utreexo;phase_u;consensus;spend_path;critical;gate"
    TIMEOUT 120
)

message(STATUS "  U.3: Spend-Path Validation configured (THE CONSENSUS GATE)")
message(STATUS "       - Valid block with proofs passes")
message(STATUS "       - Invalid proof rejected")
message(STATUS "       - Double-spend rejected")
message(STATUS "       - Wrong Utreexo root rejected")
message(STATUS "       - Transaction reordering invariant")

# ============================================================================
# U.4: Fork-Point Root Validation (TIER-0 SAFETY)
# ============================================================================

add_executable(test_utreexo_fork_point_root
    test_fork_point_root_validation.cpp
)

target_link_libraries(test_utreexo_fork_point_root PRIVATE
    dinero_consensus
    dinero_tx_primitives
    dinero_crypto
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
)

if(APPLE)
    target_link_libraries(test_utreexo_fork_point_root PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

target_include_directories(test_utreexo_fork_point_root PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME Utreexo_ForkPointRoot_U4 COMMAND test_utreexo_fork_point_root)
set_tests_properties(Utreexo_ForkPointRoot_U4
    PROPERTIES
    LABELS "utreexo;phase_u;consensus;fork_point;reorg;tier0;critical"
    TIMEOUT 60
)

message(STATUS "  U.4: Fork-Point Root Validation configured (TIER-0 SAFETY)")
message(STATUS "       FIX-1: CSN checkpoint root matches fork-point header")
message(STATUS "       FIX-2: Post-disconnect forest matches fork-point header")
message(STATUS "       FIX-3: Async proof freshness re-check")

# ============================================================================
# Phase U Summary
# ============================================================================

message(STATUS "")
message(STATUS "Phase U: Utreexo Semantic Correctness - CONFIGURED")
message(STATUS "   U.1: Proof Generation (5 invariants)")
message(STATUS "   U.2: Forest Mutation (7 invariants)")
message(STATUS "   U.3: Spend-Path Validation (THE GATE)")
message(STATUS "   U.4: Fork-Point Root Validation (TIER-0)")
message(STATUS "")
message(STATUS "   Status: CONSENSUS CRITICAL")
message(STATUS "   If any test fails --> NETWORK FORK RISK")
message(STATUS "")
