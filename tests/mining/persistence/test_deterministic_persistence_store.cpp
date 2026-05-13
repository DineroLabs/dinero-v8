/**
 * Ring 4 Phase 4g.1: Persistence Store Self-Tests
 *
 * Purpose: Verify persistence foundation before MR1-MR5 work
 * Tests: Clean persist/recover, fault injection, determinism
 */

#include "deterministic_persistence_store.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace mining_test;

// ============================================================================
// Test Utilities
// ============================================================================

void assert_true(bool condition, const std::string& msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << std::endl;
        assert(false);
    }
    std::cout << "PASS: " << msg << std::endl;
}

void assert_false(bool condition, const std::string& msg) {
    assert_true(!condition, msg);
}

// ============================================================================
// Test 1: Clean Persist and Recover
// ============================================================================

void test_clean_persist_and_recover() {
    DeterministicPersistenceStore store(12345);

    MiningState state;
    state.current_height = 100;
    state.blocks_found = 5;
    state.phase = MiningPhase::MINING;

    // Persist the state
    store.persist(state);

    // Recover should return the same state
    auto recovered = store.recover();
    assert_true(recovered.has_value(), "Clean persist and recover: has value");
    assert_true(*recovered == state, "Clean persist and recover: state matches");
}

// ============================================================================
// Test 2: Partial Write Recovers Safely
// ============================================================================

void test_partial_write_recovers_safely() {
    DeterministicPersistenceStore store(12345);

    MiningState state;
    state.current_height = 100;

    // Persist the state
    store.persist(state);

    // Inject partial write (torn write simulation)
    store.injectPartialWrite();

    // Recovery should fail (conservative recovery)
    auto recovered = store.recover();
    assert_false(recovered.has_value(), "Partial write recovers safely: recovery fails");
}

// ============================================================================
// Test 3: Corruption Recovers Safely
// ============================================================================

void test_corruption_recovers_safely() {
    DeterministicPersistenceStore store(12345);

    MiningState state;
    state.current_height = 100;

    // Persist the state
    store.persist(state);

    // Inject corruption
    store.injectCorruption();

    // Recovery should fail (corrupt data detected)
    auto recovered = store.recover();
    assert_false(recovered.has_value(), "Corruption recovers safely: recovery fails");
}

// ============================================================================
// Test 4: ClearStore Wipes State
// ============================================================================

void test_clear_store_wipes_state() {
    DeterministicPersistenceStore store(12345);

    MiningState state;
    state.current_height = 100;

    // Persist the state
    store.persist(state);
    assert_true(store.hasSnapshot(), "ClearStore wipes state: has snapshot before clear");

    // Clear the store (simulate disk wipe)
    store.clearStore();

    // No snapshot should exist
    assert_false(store.hasSnapshot(), "ClearStore wipes state: no snapshot after clear");

    // Recovery should fail
    auto recovered = store.recover();
    assert_false(recovered.has_value(), "ClearStore wipes state: recovery fails after clear");
}

// ============================================================================
// Test 5: No Snapshot Before First Persist
// ============================================================================

void test_no_snapshot_before_first_persist() {
    DeterministicPersistenceStore store(12345);

    // No snapshot before first persist
    assert_false(store.hasSnapshot(), "No snapshot before first persist: no snapshot");

    // Recovery should fail
    auto recovered = store.recover();
    assert_false(recovered.has_value(), "No snapshot before first persist: recovery fails");
}

// ============================================================================
// Test 6: Version Increments On Persist
// ============================================================================

void test_version_increments_on_persist() {
    DeterministicPersistenceStore store(12345);

    MiningState state;

    // Initial version is 0
    assert_true(store.snapshotVersion() == 0, "Version increments: initial version is 0");

    // First persist → version 1
    store.persist(state);
    assert_true(store.snapshotVersion() == 1, "Version increments: version is 1 after first persist");

    // Second persist → version 2
    store.persist(state);
    assert_true(store.snapshotVersion() == 2, "Version increments: version is 2 after second persist");

    // Clear does NOT reset version
    store.clearStore();
    assert_true(store.snapshotVersion() == 2, "Version increments: version preserved after clear");
}

// ============================================================================
// Test 7: Multiple Persist Operations (Overwrite)
// ============================================================================

void test_multiple_persist_operations() {
    DeterministicPersistenceStore store(12345);

    // First persist
    MiningState state1;
    state1.current_height = 100;
    store.persist(state1);

    // Second persist (overwrites first)
    MiningState state2;
    state2.current_height = 200;
    store.persist(state2);

    // Recovery should return the most recent state
    auto recovered = store.recover();
    assert_true(recovered.has_value(), "Multiple persist operations: has value");
    assert_true(recovered->current_height == 200, "Multiple persist operations: latest state recovered");
}

// ============================================================================
// Test 8: Fault Injection After Persist
// ============================================================================

void test_fault_injection_after_persist() {
    DeterministicPersistenceStore store(12345);

    MiningState state;
    state.current_height = 100;

    // Persist the state
    store.persist(state);

    // First recovery succeeds
    auto recovered1 = store.recover();
    assert_true(recovered1.has_value(), "Fault injection after persist: first recovery succeeds");

    // Inject fault
    store.injectPartialWrite();

    // Second recovery fails
    auto recovered2 = store.recover();
    assert_false(recovered2.has_value(), "Fault injection after persist: second recovery fails");
}

// ============================================================================
// Test 9: Persist Clears Previous Faults
// ============================================================================

void test_persist_clears_previous_faults() {
    DeterministicPersistenceStore store(12345);

    MiningState state1;
    state1.current_height = 100;

    // Persist and inject fault
    store.persist(state1);
    store.injectCorruption();

    // Recovery fails
    auto recovered1 = store.recover();
    assert_false(recovered1.has_value(), "Persist clears faults: recovery fails after corruption");

    // New persist clears fault flags
    MiningState state2;
    state2.current_height = 200;
    store.persist(state2);

    // Recovery now succeeds
    auto recovered2 = store.recover();
    assert_true(recovered2.has_value(), "Persist clears faults: recovery succeeds after new persist");
    assert_true(recovered2->current_height == 200, "Persist clears faults: correct state recovered");
}

// ============================================================================
// Test 10: Determinism With Same Seed
// ============================================================================

void test_determinism_with_same_seed() {
    // Two stores with same seed
    DeterministicPersistenceStore store1(99999);
    DeterministicPersistenceStore store2(99999);

    MiningState state;
    state.current_height = 100;

    // Persist to both
    store1.persist(state);
    store2.persist(state);

    // Both should have same version
    assert_true(store1.snapshotVersion() == store2.snapshotVersion(),
                "Determinism: same version with same seed");

    // Both should recover same state
    auto recovered1 = store1.recover();
    auto recovered2 = store2.recover();
    assert_true(recovered1.has_value() && recovered2.has_value(),
                "Determinism: both recover successfully");
    assert_true(*recovered1 == *recovered2,
                "Determinism: same state recovered");
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4g.1: Persistence Store Self-Tests ===" << std::endl;
    std::cout << std::endl;

    test_clean_persist_and_recover();
    test_partial_write_recovers_safely();
    test_corruption_recovers_safely();
    test_clear_store_wipes_state();
    test_no_snapshot_before_first_persist();
    test_version_increments_on_persist();
    test_multiple_persist_operations();
    test_fault_injection_after_persist();
    test_persist_clears_previous_faults();
    test_determinism_with_same_seed();

    std::cout << std::endl;
    std::cout << "=== All Phase 4g.1 tests passed ===" << std::endl;
    std::cout << std::endl;
    std::cout << "Persistence foundation ready ✅" << std::endl;

    return 0;
}
