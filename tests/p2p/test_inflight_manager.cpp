/**
 * Phase G.1.4 Step 2: In-Flight Request Tracking - Pure Unit Tests
 *
 * Test Scope:
 * - Add → Exists
 * - Duplicate add → Rejected
 * - Remove on receipt → No longer exists
 * - Timeout detection (simulated clock)
 * - Global uniqueness (same object can't be requested from 2 peers)
 *
 * Test Constraints:
 * ❌ No sockets
 * ❌ No threads
 * ❌ No timers
 * ✅ Fake clock / injected timestamps
 * ✅ Deterministic
 * ✅ < 100ms total runtime
 */

#include "../../include/p2p/inflight_manager.h"
#include <iostream>
#include <cassert>

using namespace dinero::p2p;

//=============================================================================
// Test 1: Add → Exists
//=============================================================================

void test_add_exists() {
    std::cout << "\n[Test 1] Add → Exists" << std::endl;

    InFlightManager manager;

    // Create test inventory
    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(i);
    }
    InventoryVector inv(MSG_BLOCK, hash);

    // Initially should not exist
    assert(!manager.exists(inv) && "Should not exist initially");

    // Add request
    bool added = manager.add(inv, "peer1");
    assert(added && "Should succeed on first add");

    // Now should exist
    assert(manager.exists(inv) && "Should exist after add");

    std::cout << "  [✓] Add → Exists works!" << std::endl;
}

//=============================================================================
// Test 2: Duplicate Add Rejected
//=============================================================================

void test_duplicate_rejected() {
    std::cout << "\n[Test 2] Duplicate add rejected" << std::endl;

    InFlightManager manager;

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(100 + i);
    }
    InventoryVector inv(MSG_TX, hash);

    // First add succeeds
    bool first = manager.add(inv, "peerA");
    assert(first && "First add should succeed");

    // Second add from different peer fails
    bool second = manager.add(inv, "peerB");
    assert(!second && "Duplicate add should be rejected");

    // Still only exists once
    assert(manager.exists(inv) && "Should still exist");

    std::cout << "  [✓] Duplicate add correctly rejected!" << std::endl;
}

//=============================================================================
// Test 3: Remove on Receipt
//=============================================================================

void test_remove_on_receipt() {
    std::cout << "\n[Test 3] Remove on receipt" << std::endl;

    InFlightManager manager;

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(200 + i);
    }
    InventoryVector inv(MSG_BLOCK, hash);

    // Add
    manager.add(inv, "peer1");
    assert(manager.exists(inv) && "Should exist after add");

    // Remove
    manager.remove(inv);
    assert(!manager.exists(inv) && "Should not exist after remove");

    // Can add again after removal
    bool re_added = manager.add(inv, "peer2");
    assert(re_added && "Should be able to re-add after removal");

    std::cout << "  [✓] Remove on receipt works!" << std::endl;
}

//=============================================================================
// Test 4: Timeout Detection (Simulated Clock)
//=============================================================================

void test_timeout_detection() {
    std::cout << "\n[Test 4] Timeout detection (simulated clock)" << std::endl;

    InFlightManager manager;

    // Create 3 requests at different times
    Hash256 hash1, hash2, hash3;
    for (int i = 0; i < 32; i++) {
        hash1.data[i] = 1;
        hash2.data[i] = 2;
        hash3.data[i] = 3;
    }

    InventoryVector inv1(MSG_BLOCK, hash1);
    InventoryVector inv2(MSG_BLOCK, hash2);
    InventoryVector inv3(MSG_TX, hash3);

    auto t0 = std::chrono::steady_clock::now();
    auto t30 = t0 + std::chrono::seconds(30);
    auto t65 = t0 + std::chrono::seconds(65);

    // Add at different times (simulated)
    manager.add(inv1, "peer1", t0);
    manager.add(inv2, "peer2", t30);
    manager.add(inv3, "peer3", t65);

    // Check expired at t70 (timeout = 60s)
    auto t70 = t0 + std::chrono::seconds(70);
    auto expired = manager.expired(std::chrono::seconds(60), t70);

    // inv1 (age 70s) and inv2 (age 40s → wait, 70-30=40, not expired)
    // Actually: inv1 (t0, age=70) → expired
    //          inv2 (t30, age=40) → NOT expired
    //          inv3 (t65, age=5) → NOT expired

    assert(expired.size() == 1 && "Only inv1 should be expired");
    assert(expired[0] == inv1 && "inv1 should be expired");

    std::cout << "  [✓] Timeout detection works!" << std::endl;
}

//=============================================================================
// Test 5: Global Uniqueness
//=============================================================================

void test_global_uniqueness() {
    std::cout << "\n[Test 5] Global uniqueness (same object can't be requested from 2 peers)" << std::endl;

    InFlightManager manager;

    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(42);
    }

    // Same (type, hash) = same identity
    InventoryVector inv1(MSG_BLOCK, hash);
    InventoryVector inv2(MSG_BLOCK, hash);

    // Add from peer1
    bool first = manager.add(inv1, "peer1");
    assert(first && "First add should succeed");

    // Try to add from peer2 (should fail - already in-flight)
    bool second = manager.add(inv2, "peer2");
    assert(!second && "Second add should fail (global uniqueness)");

    // Try different type, same hash (should succeed - different object)
    InventoryVector inv3(MSG_TX, hash);
    bool third = manager.add(inv3, "peer3");
    assert(third && "Different type should succeed (different object)");

    // Now both exist
    assert(manager.exists(inv1) && "Block should exist");
    assert(manager.exists(inv3) && "TX should exist");

    std::cout << "  [✓] Global uniqueness enforced correctly!" << std::endl;
}

//=============================================================================
// Main Test Runner
//=============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "G.1.4 Step 2: InFlightManager Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\nPure, synchronous, deterministic tests" << std::endl;
    std::cout << "No sockets | No threads | No timers" << std::endl;

    auto start = std::chrono::steady_clock::now();

    try {
        // Test 1: Add → Exists
        test_add_exists();

        // Test 2: Duplicate add rejected
        test_duplicate_rejected();

        // Test 3: Remove on receipt
        test_remove_on_receipt();

        // Test 4: Timeout detection
        test_timeout_detection();

        // Test 5: Global uniqueness
        test_global_uniqueness();

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "\n========================================" << std::endl;
        std::cout << "✅ All InFlightManager Tests Passed!" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "\nRuntime: " << duration.count() << " ms" << std::endl;

        if (duration.count() < 100) {
            std::cout << "[✓] Fast: < 100ms requirement met" << std::endl;
        } else {
            std::cout << "[!] Warning: Exceeded 100ms target" << std::endl;
        }

        std::cout << "\nSummary:" << std::endl;
        std::cout << "  [✓] Add → Exists" << std::endl;
        std::cout << "  [✓] Duplicate add rejected" << std::endl;
        std::cout << "  [✓] Remove on receipt" << std::endl;
        std::cout << "  [✓] Timeout detection" << std::endl;
        std::cout << "  [✓] Global uniqueness" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n❌ Test failed with unknown exception" << std::endl;
        return 1;
    }
}
