// SPDX-License-Identifier: MIT
// Phase D.2: Invariant Tests - Consensus Limits
//
// This test verifies that consensus size/weight limits are correctly enforced
// and that compile-time assertions catch violations.
//
// Phase D Ground Rules (In Effect):
// - ❌ NO new features
// - ❌ NO refactors for "cleanliness"
// - ❌ NO performance changes
// - ✅ ONLY: document, isolate, test consensus

#include "consensus/limits.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace dinero::consensus;

int main() {
    std::cout << "Phase D.2: Consensus Limits Invariant Tests\n";
    std::cout << "===========================================\n\n";

    int passed = 0;
    int failed = 0;

    // ========================================================================
    // Test 1: Block size limit validation
    // ========================================================================
    {
        std::cout << "Test 1: Block size validation\n";

        bool test_ok = true;

        // Valid block sizes
        if (!IsValidBlockSize(1)) {
            std::cout << "  ❌ FAIL: Size 1 should be valid\n";
            test_ok = false;
        }

        if (!IsValidBlockSize(MAX_BLOCK_SIZE)) {
            std::cout << "  ❌ FAIL: MAX_BLOCK_SIZE should be valid\n";
            test_ok = false;
        }

        if (!IsValidBlockSize(500000)) {
            std::cout << "  ❌ FAIL: 500KB should be valid\n";
            test_ok = false;
        }

        // Invalid block sizes
        if (IsValidBlockSize(0)) {
            std::cout << "  ❌ FAIL: Size 0 should be invalid\n";
            test_ok = false;
        }

        if (IsValidBlockSize(MAX_BLOCK_SIZE + 1)) {
            std::cout << "  ❌ FAIL: MAX_BLOCK_SIZE + 1 should be invalid\n";
            test_ok = false;
        }

        if (IsValidBlockSize(2000000)) {
            std::cout << "  ❌ FAIL: 2MB should be invalid (exceeds 1MB limit)\n";
            test_ok = false;
        }

        if (test_ok) {
            std::cout << "  ✅ PASS: Block size validation correct\n";
            passed++;
        } else {
            failed++;
        }
    }

    // ========================================================================
    // Test 2: Block weight limit validation
    // ========================================================================
    {
        std::cout << "\nTest 2: Block weight validation\n";

        bool test_ok = true;

        // Valid block weights
        if (!IsValidBlockWeight(1)) {
            std::cout << "  ❌ FAIL: Weight 1 should be valid\n";
            test_ok = false;
        }

        if (!IsValidBlockWeight(MAX_BLOCK_WEIGHT)) {
            std::cout << "  ❌ FAIL: MAX_BLOCK_WEIGHT should be valid\n";
            test_ok = false;
        }

        if (!IsValidBlockWeight(2000000)) {
            std::cout << "  ❌ FAIL: 2M weight units should be valid\n";
            test_ok = false;
        }

        // Invalid block weights
        if (IsValidBlockWeight(0)) {
            std::cout << "  ❌ FAIL: Weight 0 should be invalid\n";
            test_ok = false;
        }

        if (IsValidBlockWeight(MAX_BLOCK_WEIGHT + 1)) {
            std::cout << "  ❌ FAIL: MAX_BLOCK_WEIGHT + 1 should be invalid\n";
            test_ok = false;
        }

        if (test_ok) {
            std::cout << "  ✅ PASS: Block weight validation correct\n";
            passed++;
        } else {
            failed++;
        }
    }

    // ========================================================================
    // Test 3: Transaction size limit validation
    // ========================================================================
    {
        std::cout << "\nTest 3: Transaction size validation\n";

        bool test_ok = true;

        // Valid tx sizes
        if (!IsValidTxSize(1)) {
            std::cout << "  ❌ FAIL: Size 1 should be valid\n";
            test_ok = false;
        }

        if (!IsValidTxSize(MAX_TX_SIZE)) {
            std::cout << "  ❌ FAIL: MAX_TX_SIZE should be valid\n";
            test_ok = false;
        }

        if (!IsValidTxSize(50000)) {
            std::cout << "  ❌ FAIL: 50KB should be valid\n";
            test_ok = false;
        }

        // Invalid tx sizes
        if (IsValidTxSize(0)) {
            std::cout << "  ❌ FAIL: Size 0 should be invalid\n";
            test_ok = false;
        }

        if (IsValidTxSize(MAX_TX_SIZE + 1)) {
            std::cout << "  ❌ FAIL: MAX_TX_SIZE + 1 should be invalid\n";
            test_ok = false;
        }

        if (IsValidTxSize(200000)) {
            std::cout << "  ❌ FAIL: 200KB should be invalid (exceeds 100KB limit)\n";
            test_ok = false;
        }

        if (test_ok) {
            std::cout << "  ✅ PASS: Transaction size validation correct\n";
            passed++;
        } else {
            failed++;
        }
    }

    // ========================================================================
    // Test 4: Transaction weight limit validation
    // ========================================================================
    {
        std::cout << "\nTest 4: Transaction weight validation\n";

        bool test_ok = true;

        // Valid tx weights
        if (!IsValidTxWeight(1)) {
            std::cout << "  ❌ FAIL: Weight 1 should be valid\n";
            test_ok = false;
        }

        if (!IsValidTxWeight(MAX_TX_WEIGHT)) {
            std::cout << "  ❌ FAIL: MAX_TX_WEIGHT should be valid\n";
            test_ok = false;
        }

        if (!IsValidTxWeight(200000)) {
            std::cout << "  ❌ FAIL: 200k weight units should be valid\n";
            test_ok = false;
        }

        // Invalid tx weights
        if (IsValidTxWeight(0)) {
            std::cout << "  ❌ FAIL: Weight 0 should be invalid\n";
            test_ok = false;
        }

        if (IsValidTxWeight(MAX_TX_WEIGHT + 1)) {
            std::cout << "  ❌ FAIL: MAX_TX_WEIGHT + 1 should be invalid\n";
            test_ok = false;
        }

        if (test_ok) {
            std::cout << "  ✅ PASS: Transaction weight validation correct\n";
            passed++;
        } else {
            failed++;
        }
    }

    // ========================================================================
    // Test 5: Script size limit validation
    // ========================================================================
    {
        std::cout << "\nTest 5: Script size validation\n";

        bool test_ok = true;

        // Valid script sizes
        if (!IsValidScriptSize(0)) {
            std::cout << "  ❌ FAIL: Empty script should be valid\n";
            test_ok = false;
        }

        if (!IsValidScriptSize(MAX_SCRIPT_SIZE)) {
            std::cout << "  ❌ FAIL: MAX_SCRIPT_SIZE should be valid\n";
            test_ok = false;
        }

        if (!IsValidScriptSize(5000)) {
            std::cout << "  ❌ FAIL: 5KB script should be valid\n";
            test_ok = false;
        }

        // Invalid script sizes
        if (IsValidScriptSize(MAX_SCRIPT_SIZE + 1)) {
            std::cout << "  ❌ FAIL: MAX_SCRIPT_SIZE + 1 should be invalid\n";
            test_ok = false;
        }

        if (IsValidScriptSize(20000)) {
            std::cout << "  ❌ FAIL: 20KB script should be invalid (exceeds 10KB limit)\n";
            test_ok = false;
        }

        if (test_ok) {
            std::cout << "  ✅ PASS: Script size validation correct\n";
            passed++;
        } else {
            failed++;
        }
    }

    // ========================================================================
    // Test 6: Verify compile-time constants
    // ========================================================================
    {
        std::cout << "\nTest 6: Compile-time constant verification\n";

        bool test_ok = true;

        // Block limits
        if (MAX_BLOCK_SIZE != 1000000) {
            std::cout << "  ❌ FAIL: MAX_BLOCK_SIZE != 1MB\n";
            test_ok = false;
        }

        if (MAX_BLOCK_WEIGHT != 4000000) {
            std::cout << "  ❌ FAIL: MAX_BLOCK_WEIGHT != 4MW\n";
            test_ok = false;
        }

        if (MAX_BLOCK_SIGOPS_COST != 80000) {
            std::cout << "  ❌ FAIL: MAX_BLOCK_SIGOPS_COST != 80k\n";
            test_ok = false;
        }

        // Transaction limits
        if (MAX_TX_SIZE != 100000) {
            std::cout << "  ❌ FAIL: MAX_TX_SIZE != 100KB\n";
            test_ok = false;
        }

        if (MAX_TX_WEIGHT != 400000) {
            std::cout << "  ❌ FAIL: MAX_TX_WEIGHT != 400k\n";
            test_ok = false;
        }

        if (MAX_TX_SIGOPS_COST != 16000) {
            std::cout << "  ❌ FAIL: MAX_TX_SIGOPS_COST != 16k\n";
            test_ok = false;
        }

        // Script limits
        if (MAX_SCRIPT_SIZE != 10000) {
            std::cout << "  ❌ FAIL: MAX_SCRIPT_SIZE != 10KB\n";
            test_ok = false;
        }

        if (MAX_SCRIPT_OPCODES != 201) {
            std::cout << "  ❌ FAIL: MAX_SCRIPT_OPCODES != 201\n";
            test_ok = false;
        }

        if (MAX_SCRIPT_ELEMENT_SIZE != 520) {
            std::cout << "  ❌ FAIL: MAX_SCRIPT_ELEMENT_SIZE != 520\n";
            test_ok = false;
        }

        if (MAX_STACK_SIZE != 1000) {
            std::cout << "  ❌ FAIL: MAX_STACK_SIZE != 1000\n";
            test_ok = false;
        }

        if (test_ok) {
            std::cout << "  ✅ PASS: All compile-time constants verified\n";
            passed++;
        } else {
            failed++;
        }
    }

    // ========================================================================
    // Test 7: Verify sanity check assertions
    // ========================================================================
    {
        std::cout << "\nTest 7: Sanity check assertions\n";

        bool test_ok = true;

        // MAX_TX_SIZE <= MAX_BLOCK_SIZE
        if (MAX_TX_SIZE > MAX_BLOCK_SIZE) {
            std::cout << "  ❌ FAIL: MAX_TX_SIZE > MAX_BLOCK_SIZE (violates assertion)\n";
            test_ok = false;
        }

        // MAX_TX_WEIGHT <= MAX_BLOCK_WEIGHT
        if (MAX_TX_WEIGHT > MAX_BLOCK_WEIGHT) {
            std::cout << "  ❌ FAIL: MAX_TX_WEIGHT > MAX_BLOCK_WEIGHT (violates assertion)\n";
            test_ok = false;
        }

        // MAX_TX_SIGOPS <= MAX_BLOCK_SIGOPS
        if (MAX_TX_SIGOPS_COST > MAX_BLOCK_SIGOPS_COST) {
            std::cout << "  ❌ FAIL: MAX_TX_SIGOPS_COST > MAX_BLOCK_SIGOPS_COST (violates assertion)\n";
            test_ok = false;
        }

        // MAX_SCRIPT_ELEMENT_SIZE < MAX_SCRIPT_SIZE
        if (MAX_SCRIPT_ELEMENT_SIZE >= MAX_SCRIPT_SIZE) {
            std::cout << "  ❌ FAIL: MAX_SCRIPT_ELEMENT_SIZE >= MAX_SCRIPT_SIZE (violates assertion)\n";
            test_ok = false;
        }

        if (test_ok) {
            std::cout << "  ✅ PASS: All sanity checks verified\n";
            passed++;
        } else {
            failed++;
        }
    }

    // ========================================================================
    // Test 8: Boundary condition tests
    // ========================================================================
    {
        std::cout << "\nTest 8: Boundary condition tests\n";

        bool test_ok = true;

        // Test exact boundaries
        if (!IsValidBlockSize(MAX_BLOCK_SIZE)) {
            std::cout << "  ❌ FAIL: Exact MAX_BLOCK_SIZE should be valid\n";
            test_ok = false;
        }

        if (IsValidBlockSize(MAX_BLOCK_SIZE + 1)) {
            std::cout << "  ❌ FAIL: MAX_BLOCK_SIZE + 1 should be invalid\n";
            test_ok = false;
        }

        if (!IsValidTxSize(MAX_TX_SIZE)) {
            std::cout << "  ❌ FAIL: Exact MAX_TX_SIZE should be valid\n";
            test_ok = false;
        }

        if (IsValidTxSize(MAX_TX_SIZE + 1)) {
            std::cout << "  ❌ FAIL: MAX_TX_SIZE + 1 should be invalid\n";
            test_ok = false;
        }

        if (test_ok) {
            std::cout << "  ✅ PASS: Boundary conditions correct\n";
            passed++;
        } else {
            failed++;
        }
    }

    // ========================================================================
    // Summary
    // ========================================================================
    std::cout << "\n===========================================\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed\n";

    if (failed > 0) {
        std::cout << "❌ CRITICAL: Consensus limits tests FAILED!\n";
        std::cout << "   This indicates a consensus bug that MUST be fixed.\n";
        return 1;
    }

    std::cout << "✅ All consensus limits tests PASSED!\n";
    std::cout << "   Size/weight limits are correctly enforced.\n";
    return 0;
}
