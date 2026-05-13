// SPDX-License-Identifier: MIT
// Phase W.4.2: RBF & CPFP Capability Detection Tests

#include "wallet/rbf_cpfp_detector.h"
#include <iostream>
#include <cassert>

using namespace dinero;

// ============================================================================
// Test Utilities
// ============================================================================

#define ASSERT_TRUE(expr, msg) \
    if (!(expr)) { \
        std::cerr << "❌ ASSERTION FAILED: " << msg << std::endl; \
        std::cerr << "   Expression: " << #expr << std::endl; \
        std::cerr << "   File: " << __FILE__ << ":" << __LINE__ << std::endl; \
        return false; \
    }

#define ASSERT_FALSE(expr, msg) \
    if ((expr)) { \
        std::cerr << "❌ ASSERTION FAILED: " << msg << std::endl; \
        std::cerr << "   Expression: !" << #expr << std::endl; \
        std::cerr << "   File: " << __FILE__ << ":" << __LINE__ << std::endl; \
        return false; \
    }

#define ASSERT_EQ(a, b, msg) \
    if ((a) != (b)) { \
        std::cerr << "❌ ASSERTION FAILED: " << msg << std::endl; \
        std::cerr << "   Expected: " << (b) << std::endl; \
        std::cerr << "   Got: " << (a) << std::endl; \
        std::cerr << "   File: " << __FILE__ << ":" << __LINE__ << std::endl; \
        return false; \
    }

#define ASSERT_GT(a, b, msg) \
    if ((a) <= (b)) { \
        std::cerr << "❌ ASSERTION FAILED: " << msg << std::endl; \
        std::cerr << "   Expected: > " << (b) << std::endl; \
        std::cerr << "   Got: " << (a) << std::endl; \
        std::cerr << "   File: " << __FILE__ << ":" << __LINE__ << std::endl; \
        return false; \
    }

// ============================================================================
// Test 1: Struct Constructors
// ============================================================================

bool test_w4_2_struct_constructors() {
    std::cout << "\n[Test 1] W.4.2: Struct constructors" << std::endl;

    // RbfCapability
    RbfCapability rbf;
    ASSERT_FALSE(rbf.signals_rbf, "Default signals_rbf should be false");
    ASSERT_FALSE(rbf.can_be_replaced, "Default can_be_replaced should be false");
    ASSERT_EQ(rbf.min_replacement_fee, 0ULL, "Default min_replacement_fee should be 0");
    ASSERT_EQ(rbf.rbf_status, std::string("unknown"), "Default status should be 'unknown'");

    // SpendableOutput
    SpendableOutput output;
    ASSERT_EQ(output.vout, 0U, "Default vout should be 0");
    ASSERT_EQ(output.amount, 0ULL, "Default amount should be 0");
    ASSERT_FALSE(output.is_confirmed, "Default is_confirmed should be false");
    ASSERT_FALSE(output.is_wallet_controlled, "Default is_wallet_controlled should be false");

    // CpfpCapability
    CpfpCapability cpfp;
    ASSERT_FALSE(cpfp.viable, "Default viable should be false");
    ASSERT_TRUE(cpfp.outputs.empty(), "Default outputs should be empty");
    ASSERT_EQ(cpfp.current_ancestor_count, 0U, "Default ancestor count should be 0");
    ASSERT_EQ(cpfp.max_ancestor_count, 25U, "Default max ancestors should be 25");
    ASSERT_FALSE(cpfp.within_package_limits, "Default within_package_limits should be false");
    ASSERT_EQ(cpfp.cpfp_status, std::string("unknown"), "Default status should be 'unknown'");

    // RescueStrategy
    RescueStrategy strategy;
    ASSERT_FALSE(strategy.rbf_available, "Default rbf_available should be false");
    ASSERT_FALSE(strategy.cpfp_available, "Default cpfp_available should be false");
    ASSERT_EQ(strategy.recommended_action, std::string("none"), "Default action should be 'none'");

    std::cout << "✅ Test 1 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 2: RBF Capability Check (No Mempool)
// ============================================================================

bool test_w4_2_rbf_no_mempool() {
    std::cout << "\n[Test 2] W.4.2: RBF capability check without mempool" << std::endl;

    RbfCpfpDetector detector;
    uint256 txid;  // Dummy txid

    RbfCapability capability = detector.CheckRbfCapability(txid, nullptr);

    ASSERT_FALSE(capability.signals_rbf, "Should not signal RBF without mempool");
    ASSERT_FALSE(capability.can_be_replaced, "Should not be replaceable without mempool");
    ASSERT_EQ(capability.min_replacement_fee, 0ULL, "Min fee should be 0 without mempool");
    ASSERT_TRUE(capability.rbf_status.find("unavailable") != std::string::npos,
                "Status should indicate mempool unavailable");

    std::cout << "✅ Test 2 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 3: CanBeReplaced Check
// ============================================================================

bool test_w4_2_can_be_replaced() {
    std::cout << "\n[Test 3] W.4.2: CanBeReplaced check" << std::endl;

    RbfCpfpDetector detector;
    uint256 txid;

    // Without mempool, should return false
    bool can_replace = detector.CanBeReplaced(txid, nullptr);
    ASSERT_FALSE(can_replace, "Should not be replaceable without mempool");

    std::cout << "✅ Test 3 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 4: Calculate Min Replacement Fee
// ============================================================================

bool test_w4_2_min_replacement_fee() {
    std::cout << "\n[Test 4] W.4.2: Calculate minimum replacement fee" << std::endl;

    RbfCpfpDetector detector;
    uint256 txid;

    // Without mempool, should return 0
    uint64_t min_fee = detector.CalculateMinReplacementFee(txid, nullptr);
    ASSERT_EQ(min_fee, 0ULL, "Min fee should be 0 without mempool");

    std::cout << "✅ Test 4 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 5: CPFP Capability Check (No Mempool)
// ============================================================================

bool test_w4_2_cpfp_no_mempool() {
    std::cout << "\n[Test 5] W.4.2: CPFP capability check without mempool" << std::endl;

    RbfCpfpDetector detector;
    uint256 txid;

    CpfpCapability capability = detector.CheckCpfpCapability(txid, nullptr, nullptr);

    ASSERT_FALSE(capability.viable, "CPFP should not be viable without mempool");
    ASSERT_FALSE(capability.within_package_limits, "Should not be within limits without mempool");
    ASSERT_TRUE(capability.cpfp_status.find("unavailable") != std::string::npos,
                "Status should indicate mempool unavailable");

    std::cout << "✅ Test 5 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 6: Find Spendable Outputs (No Wallet)
// ============================================================================

bool test_w4_2_spendable_outputs_no_wallet() {
    std::cout << "\n[Test 6] W.4.2: Find spendable outputs without wallet" << std::endl;

    RbfCpfpDetector detector;
    uint256 txid;

    std::vector<SpendableOutput> outputs = detector.FindSpendableOutputs(txid, nullptr);

    ASSERT_TRUE(outputs.empty(), "Should return empty vector without wallet");

    std::cout << "✅ Test 6 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 7: Package Limits Check (No Mempool)
// ============================================================================

bool test_w4_2_package_limits_no_mempool() {
    std::cout << "\n[Test 7] W.4.2: Package limits check without mempool" << std::endl;

    RbfCpfpDetector detector;
    uint256 txid;

    bool within_limits = detector.IsWithinPackageLimits(txid, nullptr);

    ASSERT_FALSE(within_limits, "Should return false without mempool");

    std::cout << "✅ Test 7 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 8: Determine Rescue Strategy (No Components)
// ============================================================================

bool test_w4_2_rescue_strategy_no_components() {
    std::cout << "\n[Test 8] W.4.2: Determine rescue strategy without components" << std::endl;

    RbfCpfpDetector detector;
    uint256 txid;

    RescueStrategy strategy = detector.DetermineRescueStrategy(txid, nullptr, nullptr);

    ASSERT_FALSE(strategy.rbf_available, "RBF should not be available");
    ASSERT_FALSE(strategy.cpfp_available, "CPFP should not be available");
    ASSERT_EQ(strategy.recommended_action, std::string("wait"),
              "Should recommend wait when no options available");
    ASSERT_TRUE(!strategy.explanation.empty(), "Should have explanation");
    ASSERT_TRUE(strategy.explanation.find("No fee bump") != std::string::npos ||
                strategy.explanation.find("Wait") != std::string::npos,
                "Explanation should mention waiting or no options");

    std::cout << "✅ Test 8 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 9: Configuration - Max Ancestor Count
// ============================================================================

bool test_w4_2_max_ancestor_config() {
    std::cout << "\n[Test 9] W.4.2: Max ancestor count configuration" << std::endl;

    RbfCpfpDetector detector;

    // Default should be 25
    ASSERT_EQ(detector.GetMaxAncestorCount(), 25U, "Default max ancestors should be 25");

    // Set to custom value
    detector.SetMaxAncestorCount(50);
    ASSERT_EQ(detector.GetMaxAncestorCount(), 50U, "Max ancestors should be 50 after setting");

    // Set to 0 (edge case)
    detector.SetMaxAncestorCount(0);
    ASSERT_EQ(detector.GetMaxAncestorCount(), 0U, "Should accept 0 as max ancestors");

    std::cout << "✅ Test 9 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 10: RBF/CPFP Status Strings
// ============================================================================

bool test_w4_2_status_strings() {
    std::cout << "\n[Test 10] W.4.2: RBF/CPFP status strings" << std::endl;

    RbfCpfpDetector detector;
    uint256 txid;

    // Check RBF status
    RbfCapability rbf = detector.CheckRbfCapability(txid, nullptr);
    ASSERT_TRUE(!rbf.rbf_status.empty(), "RBF status should not be empty");

    // Check CPFP status
    CpfpCapability cpfp = detector.CheckCpfpCapability(txid, nullptr, nullptr);
    ASSERT_TRUE(!cpfp.cpfp_status.empty(), "CPFP status should not be empty");

    // Check rescue strategy explanation
    RescueStrategy strategy = detector.DetermineRescueStrategy(txid, nullptr, nullptr);
    ASSERT_TRUE(!strategy.explanation.empty(), "Rescue strategy explanation should not be empty");

    std::cout << "✅ Test 10 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 11: Rescue Strategy Priority (RBF > CPFP > Wait)
// ============================================================================

bool test_w4_2_rescue_priority() {
    std::cout << "\n[Test 11] W.4.2: Rescue strategy priority logic" << std::endl;

    RbfCpfpDetector detector;
    uint256 txid;

    // Without components, should recommend wait
    RescueStrategy strategy = detector.DetermineRescueStrategy(txid, nullptr, nullptr);
    ASSERT_EQ(strategy.recommended_action, std::string("wait"),
              "Should recommend 'wait' when neither RBF nor CPFP available");

    // Verify strategy has both capability details
    ASSERT_FALSE(strategy.rbf_details.can_be_replaced, "RBF details should be populated");
    ASSERT_FALSE(strategy.cpfp_details.viable, "CPFP details should be populated");

    std::cout << "✅ Test 11 passed" << std::endl;
    return true;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Phase W.4.2: RbfCpfpDetector Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    bool all_passed = true;

    // Run all tests
    all_passed &= test_w4_2_struct_constructors();
    all_passed &= test_w4_2_rbf_no_mempool();
    all_passed &= test_w4_2_can_be_replaced();
    all_passed &= test_w4_2_min_replacement_fee();
    all_passed &= test_w4_2_cpfp_no_mempool();
    all_passed &= test_w4_2_spendable_outputs_no_wallet();
    all_passed &= test_w4_2_package_limits_no_mempool();
    all_passed &= test_w4_2_rescue_strategy_no_components();
    all_passed &= test_w4_2_max_ancestor_config();
    all_passed &= test_w4_2_status_strings();
    all_passed &= test_w4_2_rescue_priority();

    std::cout << "\n========================================" << std::endl;
    if (all_passed) {
        std::cout << "✅ ALL TESTS PASSED" << std::endl;
        return 0;
    } else {
        std::cout << "❌ SOME TESTS FAILED" << std::endl;
        return 1;
    }
}
