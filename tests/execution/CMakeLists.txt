#=============================================================================
# Ring 7: Execution Semantics Test Suite
#=============================================================================

message(STATUS "")
message(STATUS "Configuring Ring 7 Phase 7a: Execution Trace Framework...")

#=============================================================================
# Framework Library
#=============================================================================

add_library(execution_test_framework STATIC
    framework/execution_types.h
    framework/execution_trace.h
    framework/execution_trace.cpp
    framework/script_executor.h
    framework/script_executor.cpp
    framework/execution_simulator.h
    framework/execution_simulator.cpp
)

target_include_directories(execution_test_framework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)

target_link_libraries(execution_test_framework PUBLIC
    dinero_crypto
    dinero_consensus
)

# Platform-specific settings for framework
if(APPLE)
    target_link_libraries(execution_test_framework PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

message(STATUS "✅ Ring 7 Phase 7a execution framework library configured")
message(STATUS "   Components: ExecutionSimulator, ScriptExecutor, ExecutionTrace")

#=============================================================================
# Phase 7a: Smoke Tests
#=============================================================================

add_executable(test_execution_simulator_smoke
    tests/test_execution_simulator_smoke.cpp
)

target_include_directories(test_execution_simulator_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)

target_link_libraries(test_execution_simulator_smoke
    execution_test_framework
    dinero_crypto
    dinero_consensus
    $<TARGET_FILE:gtest>
)

add_test(
    NAME Execution_SimulatorSmoke_R7_7a
    COMMAND test_execution_simulator_smoke
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

set_tests_properties(Execution_SimulatorSmoke_R7_7a PROPERTIES
    LABELS "execution;ring7;phase7a;smoke"
    TIMEOUT 10
)

message(STATUS "✅ Ring 7 Phase 7a smoke tests configured:")
message(STATUS "   • Execution_SimulatorSmoke_R7_7a (script execution, trace recording, determinism)")
message(STATUS "")
message(STATUS "Ring 7 Phase 7a Configuration Complete!")
message(STATUS "Build: cd build && cmake .. && make test_execution_simulator_smoke")
message(STATUS "Test:  ctest -R Execution_SimulatorSmoke_R7_7a --verbose")
message(STATUS "")

#=============================================================================
# Phase 7b: Script Semantics (S1-S5)
#=============================================================================

message(STATUS "")
message(STATUS "Configuring Ring 7 Phase 7b: Script Semantics (S1-S5)...")

# Oracle Library
add_library(semantic_safety_oracle STATIC
    properties/semantic_safety_oracle.h
    properties/semantic_safety_oracle.cpp
    properties/semantic_safety_oracle_s1.h
    properties/semantic_safety_oracle_s1.cpp
    properties/semantic_safety_oracle_s2.h
    properties/semantic_safety_oracle_s2.cpp
    properties/semantic_safety_oracle_s3.h
    properties/semantic_safety_oracle_s3.cpp
    properties/semantic_safety_oracle_s4.h
    properties/semantic_safety_oracle_s4.cpp
    properties/semantic_safety_oracle_s5.h
    properties/semantic_safety_oracle_s5.cpp
)

target_include_directories(semantic_safety_oracle PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)

target_link_libraries(semantic_safety_oracle PUBLIC
    execution_test_framework
    dinero_crypto
    dinero_consensus
)

message(STATUS "✅ Ring 7 Phase 7b semantic safety oracle library configured")
message(STATUS "   Oracles: S1-S5 (Script Determinism, Witness Equivalence, Taproot Isolation, Path Semantics, Version Strictness)")

# Property Tests
add_executable(test_semantic_safety_oracles
    tests/test_semantic_safety_oracles.cpp
)

target_include_directories(test_semantic_safety_oracles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)

target_link_libraries(test_semantic_safety_oracles
    semantic_safety_oracle
    execution_test_framework
    dinero_crypto
    dinero_consensus
    $<TARGET_FILE:gtest>
)

add_test(
    NAME Execution_SemanticSafety_R7_7b
    COMMAND test_semantic_safety_oracles
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

set_tests_properties(Execution_SemanticSafety_R7_7b PROPERTIES
    LABELS "execution;ring7;phase7b;safety;semantics"
    TIMEOUT 30
)

message(STATUS "✅ Ring 7 Phase 7b semantic safety tests configured:")
message(STATUS "   • Execution_SemanticSafety_R7_7b (S1-S5 oracle tests)")
message(STATUS "")
message(STATUS "Ring 7 Phase 7b Configuration Complete!")
message(STATUS "Build: cd build && cmake .. && make test_semantic_safety_oracles")
message(STATUS "Test:  ctest -R Execution_SemanticSafety_R7_7b --verbose")
message(STATUS "")

#=============================================================================
# Phase 7c: Taproot Path Safety (S6-S10)
#=============================================================================

message(STATUS "")
message(STATUS "Configuring Ring 7 Phase 7c: Taproot Path Safety (S6-S10)...")

# Oracle Library
add_library(taproot_path_oracle STATIC
    properties/taproot_path_oracle.h
    properties/taproot_path_oracle.cpp
    properties/taproot_path_oracle_s6.h
    properties/taproot_path_oracle_s6.cpp
    properties/taproot_path_oracle_s7.h
    properties/taproot_path_oracle_s7.cpp
    properties/taproot_path_oracle_s8.h
    properties/taproot_path_oracle_s8.cpp
    properties/taproot_path_oracle_s9.h
    properties/taproot_path_oracle_s9.cpp
    properties/taproot_path_oracle_s10.h
    properties/taproot_path_oracle_s10.cpp
)

target_include_directories(taproot_path_oracle PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)

target_link_libraries(taproot_path_oracle PUBLIC
    execution_test_framework
    dinero_crypto
    dinero_consensus
)

message(STATUS "✅ Ring 7 Phase 7c taproot path oracle library configured")
message(STATUS "   Oracles: S6-S10 (Hidden Path, Partial Reveal, Leakage, Commitment, Uniqueness)")

# Property Tests
add_executable(test_taproot_path_oracles
    tests/test_taproot_path_oracles.cpp
)

target_include_directories(test_taproot_path_oracles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)

target_link_libraries(test_taproot_path_oracles
    taproot_path_oracle
    execution_test_framework
    dinero_crypto
    dinero_consensus
    $<TARGET_FILE:gtest>
)

add_test(
    NAME Execution_TaprootPath_R7_7c
    COMMAND test_taproot_path_oracles
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

set_tests_properties(Execution_TaprootPath_R7_7c PROPERTIES
    LABELS "execution;ring7;phase7c;taproot;path-safety"
    TIMEOUT 30
)

message(STATUS "✅ Ring 7 Phase 7c taproot path tests configured:")
message(STATUS "   • Execution_TaprootPath_R7_7c (S6-S10 oracle tests)")
message(STATUS "")
message(STATUS "Ring 7 Phase 7c Configuration Complete!")
message(STATUS "Build: cd build && cmake .. && make test_taproot_path_oracles")
message(STATUS "Test:  ctest -R Execution_TaprootPath_R7_7c --verbose")
message(STATUS "")

#=============================================================================
# Phase 7d: Covenant Semantics (S11-S15)
#=============================================================================

message(STATUS "")
message(STATUS "Configuring Ring 7 Phase 7d: Covenant Semantics (S11-S15)...")

# Oracle Library
add_library(covenant_semantic_oracle STATIC
    properties/covenant_semantic_oracle.h
    properties/covenant_semantic_oracle.cpp
    properties/covenant_semantic_oracle_s11.h
    properties/covenant_semantic_oracle_s11.cpp
    properties/covenant_semantic_oracle_s12.h
    properties/covenant_semantic_oracle_s12.cpp
    properties/covenant_semantic_oracle_s13.h
    properties/covenant_semantic_oracle_s13.cpp
    properties/covenant_semantic_oracle_s14.h
    properties/covenant_semantic_oracle_s14.cpp
    properties/covenant_semantic_oracle_s15.h
    properties/covenant_semantic_oracle_s15.cpp
)

target_include_directories(covenant_semantic_oracle PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)

target_link_libraries(covenant_semantic_oracle PUBLIC
    execution_test_framework
    dinero_crypto
    dinero_consensus
)

message(STATUS "✅ Ring 7 Phase 7d covenant semantic oracle library configured")
message(STATUS "   Oracles: S11-S15 (Constraint Enforcement, Introspection, Composition, Recursion, State Transitions)")

# Property Tests
add_executable(test_covenant_semantic_oracles
    tests/test_covenant_semantic_oracles.cpp
)

target_include_directories(test_covenant_semantic_oracles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)

target_link_libraries(test_covenant_semantic_oracles
    covenant_semantic_oracle
    execution_test_framework
    dinero_crypto
    dinero_consensus
    $<TARGET_FILE:gtest>
)

add_test(
    NAME Execution_CovenantSemantics_R7_7d
    COMMAND test_covenant_semantic_oracles
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

set_tests_properties(Execution_CovenantSemantics_R7_7d PROPERTIES
    LABELS "execution;ring7;phase7d;covenant;semantics"
    TIMEOUT 30
)

message(STATUS "✅ Ring 7 Phase 7d covenant semantic tests configured:")
message(STATUS "   • Execution_CovenantSemantics_R7_7d (S11-S15 oracle tests)")
message(STATUS "")
message(STATUS "Ring 7 Phase 7d Configuration Complete!")
message(STATUS "Build: cd build && cmake .. && make test_covenant_semantic_oracles")
message(STATUS "Test:  ctest -R Execution_CovenantSemantics_R7_7d --verbose")
message(STATUS "")

#=============================================================================
# Phase 7e: Composition & State (S16-S20)
#=============================================================================

message(STATUS "")
message(STATUS "Configuring Ring 7 Phase 7e: Composition & State (S16-S20)...")

# Oracle Library
add_library(composition_state_oracle STATIC
    properties/composition_state_oracle.h
    properties/composition_state_oracle.cpp
    properties/composition_state_oracle_s16.h
    properties/composition_state_oracle_s16.cpp
    properties/composition_state_oracle_s17.h
    properties/composition_state_oracle_s17.cpp
    properties/composition_state_oracle_s18.h
    properties/composition_state_oracle_s18.cpp
    properties/composition_state_oracle_s19.h
    properties/composition_state_oracle_s19.cpp
    properties/composition_state_oracle_s20.h
    properties/composition_state_oracle_s20.cpp
)

target_include_directories(composition_state_oracle PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)

target_link_libraries(composition_state_oracle PUBLIC
    execution_test_framework
    dinero_crypto
    dinero_consensus
)

message(STATUS "✅ Ring 7 Phase 7e composition & state oracle library configured")
message(STATUS "   Oracles: S16-S20 (Multi-Input Isolation, Parallel Safety, State Consistency, Cross-Input Invariants, Composition Determinism)")

# Property Tests
add_executable(test_composition_state_oracles
    tests/test_composition_state_oracles.cpp
)

target_include_directories(test_composition_state_oracles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)

target_link_libraries(test_composition_state_oracles
    composition_state_oracle
    execution_test_framework
    dinero_crypto
    dinero_consensus
    $<TARGET_FILE:gtest>
)

add_test(
    NAME Execution_CompositionState_R7_7e
    COMMAND test_composition_state_oracles
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

set_tests_properties(Execution_CompositionState_R7_7e PROPERTIES
    LABELS "execution;ring7;phase7e;composition;state"
    TIMEOUT 30
)

message(STATUS "✅ Ring 7 Phase 7e composition & state tests configured:")
message(STATUS "   • Execution_CompositionState_R7_7e (S16-S20 oracle tests)")
message(STATUS "")
message(STATUS "Ring 7 Phase 7e Configuration Complete!")
message(STATUS "Build: cd build && cmake .. && make test_composition_state_oracles")
message(STATUS "Test:  ctest -R Execution_CompositionState_R7_7e --verbose")
message(STATUS "")

#=============================================================================
# Phase 7f: Semantic Determinism (S21-S25)
#=============================================================================

message(STATUS "")
message(STATUS "Configuring Ring 7 Phase 7f: Semantic Determinism (S21-S25)...")

# Update execution_test_framework to include ScriptCanonicalizer
target_sources(execution_test_framework PRIVATE
    framework/script_canonicalizer.h
    framework/script_canonicalizer.cpp
)

# Oracle Library
add_library(semantic_determinism_oracle STATIC
    properties/semantic_determinism_oracle.h
    properties/semantic_determinism_oracle.cpp
    properties/semantic_determinism_oracle_s21.h
    properties/semantic_determinism_oracle_s21.cpp
    properties/semantic_determinism_oracle_s22.h
    properties/semantic_determinism_oracle_s22.cpp
    properties/semantic_determinism_oracle_s23.h
    properties/semantic_determinism_oracle_s23.cpp
    properties/semantic_determinism_oracle_s24.h
    properties/semantic_determinism_oracle_s24.cpp
    properties/semantic_determinism_oracle_s25.h
    properties/semantic_determinism_oracle_s25.cpp
)

target_include_directories(semantic_determinism_oracle PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)

target_link_libraries(semantic_determinism_oracle PUBLIC
    execution_test_framework
    dinero_crypto
    dinero_consensus
)

message(STATUS "✅ Ring 7 Phase 7f semantic determinism oracle library configured")
message(STATUS "   Oracles: S21-S25 (Evaluation Order, Input Permutation, Strategy Independence, Canonical Equivalence, Full Determinism)")

# Property Tests
add_executable(test_semantic_determinism_oracles
    tests/test_semantic_determinism_oracles.cpp
)

target_include_directories(test_semantic_determinism_oracles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)

target_link_libraries(test_semantic_determinism_oracles
    semantic_determinism_oracle
    execution_test_framework
    dinero_crypto
    dinero_consensus
    $<TARGET_FILE:gtest>
)

add_test(
    NAME Execution_SemanticDeterminism_R7_7f
    COMMAND test_semantic_determinism_oracles
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

set_tests_properties(Execution_SemanticDeterminism_R7_7f PROPERTIES
    LABELS "execution;ring7;phase7f;determinism;closure"
    TIMEOUT 30
)

message(STATUS "✅ Ring 7 Phase 7f semantic determinism tests configured:")
message(STATUS "   • Execution_SemanticDeterminism_R7_7f (S21-S25 oracle tests)")
message(STATUS "")
message(STATUS "Ring 7 Phase 7f Configuration Complete!")
message(STATUS "Build: cd build && cmake .. && make test_semantic_determinism_oracles")
message(STATUS "Test:  ctest -R Execution_SemanticDeterminism_R7_7f --verbose")
message(STATUS "")
message(STATUS "🎉 Ring 7 COMPLETE! All phases (7a-7f) configured. 25 properties (S1-S25) ready for testing.")
message(STATUS "")
