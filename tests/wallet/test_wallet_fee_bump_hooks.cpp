// SPDX-License-Identifier: MIT
// Phase W.4.5: Wallet Fee Bump UX Hooks Tests

#include "wallet/wallet_fee_bump_hooks.h"
#include "wallet/wallet_manager.h"
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

// ============================================================================
// Test 1: WalletBumpSummary - Default Constructor
// ============================================================================

bool test_w4_5_bump_summary_default() {
    std::cout << "\n[Test 1] W.4.5: WalletBumpSummary default constructor" << std::endl;

    WalletFeeBumpHooks::WalletBumpSummary summary;

    // Verify default initialization
    ASSERT_EQ(summary.total_unconfirmed, 0U, "total_unconfirmed should be 0");
    ASSERT_EQ(summary.likely_to_confirm, 0U, "likely_to_confirm should be 0");
    ASSERT_EQ(summary.stalled, 0U, "stalled should be 0");
    ASSERT_EQ(summary.blocked, 0U, "blocked should be 0");
    ASSERT_EQ(summary.can_rbf, 0U, "can_rbf should be 0");
    ASSERT_EQ(summary.can_cpfp, 0U, "can_cpfp should be 0");
    ASSERT_TRUE(summary.needs_attention.empty(), "needs_attention should be empty");

    // Verify helper methods
    ASSERT_FALSE(summary.has_stuck_transactions(), "Should not have stuck transactions");
    ASSERT_FALSE(summary.has_bump_options(), "Should not have bump options");

    std::cout << "✅ Test 1 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 2: WalletBumpSummary - Helper Methods
// ============================================================================

bool test_w4_5_bump_summary_helpers() {
    std::cout << "\n[Test 2] W.4.5: WalletBumpSummary helper methods" << std::endl;

    WalletFeeBumpHooks::WalletBumpSummary summary;

    // Test has_stuck_transactions()
    summary.stalled = 3;
    ASSERT_TRUE(summary.has_stuck_transactions(), "Should detect stalled transactions");

    summary.stalled = 0;
    summary.blocked = 2;
    ASSERT_TRUE(summary.has_stuck_transactions(), "Should detect blocked transactions");

    summary.blocked = 0;
    ASSERT_FALSE(summary.has_stuck_transactions(), "Should not detect stuck when none");

    // Test has_bump_options()
    summary.can_rbf = 5;
    ASSERT_TRUE(summary.has_bump_options(), "Should detect RBF options");

    summary.can_rbf = 0;
    summary.can_cpfp = 3;
    ASSERT_TRUE(summary.has_bump_options(), "Should detect CPFP options");

    summary.can_cpfp = 0;
    ASSERT_FALSE(summary.has_bump_options(), "Should not detect options when none");

    std::cout << "✅ Test 2 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 3: WalletBumpSummaryToString
// ============================================================================

bool test_w4_5_bump_summary_to_string() {
    std::cout << "\n[Test 3] W.4.5: WalletBumpSummaryToString" << std::endl;

    WalletFeeBumpHooks::WalletBumpSummary summary;
    summary.total_unconfirmed = 10;
    summary.likely_to_confirm = 6;
    summary.stalled = 3;
    summary.blocked = 1;
    summary.can_rbf = 4;
    summary.can_cpfp = 2;

    std::string str = WalletBumpSummaryToString(summary);

    // Verify string contains key information
    ASSERT_TRUE(str.find("total_unconfirmed=10") != std::string::npos,
                "String should contain total_unconfirmed");
    ASSERT_TRUE(str.find("likely=6") != std::string::npos,
                "String should contain likely");
    ASSERT_TRUE(str.find("stalled=3") != std::string::npos,
                "String should contain stalled");
    ASSERT_TRUE(str.find("blocked=1") != std::string::npos,
                "String should contain blocked");
    ASSERT_TRUE(str.find("can_rbf=4") != std::string::npos,
                "String should contain can_rbf");
    ASSERT_TRUE(str.find("can_cpfp=2") != std::string::npos,
                "String should contain can_cpfp");

    std::cout << "✅ Test 3 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 4: WalletFeeBumpHooks - GetWalletBumpSummary (No Wallet)
// ============================================================================

bool test_w4_5_get_summary_null_wallet() {
    std::cout << "\n[Test 4] W.4.5: GetWalletBumpSummary with null wallet" << std::endl;

    WalletFeeBumpHooks hooks;
    auto summary = hooks.GetWalletBumpSummary(nullptr, nullptr);

    // Should return empty summary without crashing
    ASSERT_EQ(summary.total_unconfirmed, 0U, "Should have 0 unconfirmed");
    ASSERT_FALSE(summary.has_stuck_transactions(), "Should not have stuck");
    ASSERT_FALSE(summary.has_bump_options(), "Should not have options");

    std::cout << "✅ Test 4 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 5: WalletFeeBumpHooks - GetTransactionsNeedingBump (No Wallet)
// ============================================================================

bool test_w4_5_get_needing_bump_null_wallet() {
    std::cout << "\n[Test 5] W.4.5: GetTransactionsNeedingBump with null wallet" << std::endl;

    WalletFeeBumpHooks hooks;
    auto txs = hooks.GetTransactionsNeedingBump(nullptr, nullptr, 1);

    // Should return empty list without crashing
    ASSERT_TRUE(txs.empty(), "Should return empty list for null wallet");

    std::cout << "✅ Test 5 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 6: WalletFeeBumpHooks - CheckTransactionStatus
// ============================================================================

bool test_w4_5_check_transaction_status() {
    std::cout << "\n[Test 6] W.4.5: CheckTransactionStatus" << std::endl;

    WalletFeeBumpHooks hooks;
    uint256 txid = uint256::FromHexUnsafe(
        "0000000000000000000000000000000000000000000000000000000000000001"
    );

    // Call without wallet (should work with placeholder data)
    TxInclusionStatus status = hooks.CheckTransactionStatus(txid, nullptr, nullptr);

    // Verify result structure
    ASSERT_EQ(status.txid.ToString(), txid.ToString(), "TxID should match");
    // State will be determined by analyzer (likely UNKNOWN due to no context)
    // Just verify no crash and result is valid

    std::cout << "✅ Test 6 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 7: WalletFeeBumpHooks - GetBumpRecommendation
// ============================================================================

bool test_w4_5_get_bump_recommendation() {
    std::cout << "\n[Test 7] W.4.5: GetBumpRecommendation" << std::endl;

    WalletFeeBumpHooks hooks;
    uint256 txid = uint256::FromHexUnsafe(
        "0000000000000000000000000000000000000000000000000000000000000001"
    );

    // Call without wallet (should work with placeholder data)
    FeeBumpRecommendation recommendation = hooks.GetBumpRecommendation(
        txid,
        nullptr,
        nullptr,
        6  // target 6 blocks
    );

    // Verify result structure
    ASSERT_EQ(recommendation.txid.ToString(), txid.ToString(), "TxID should match");
    ASSERT_EQ(recommendation.estimated_blocks_target, 6U,
              "Target blocks should match input");
    // Strategy will be determined by engine
    // Just verify no crash and result is valid

    std::cout << "✅ Test 7 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 8: WalletFeeBumpHooks - CanBumpTransaction (No Wallet)
// ============================================================================

bool test_w4_5_can_bump_null_wallet() {
    std::cout << "\n[Test 8] W.4.5: CanBumpTransaction with null wallet" << std::endl;

    WalletFeeBumpHooks hooks;
    uint256 txid = uint256::FromHexUnsafe(
        "0000000000000000000000000000000000000000000000000000000000000001"
    );

    // Should return false for null wallet without crashing
    bool can_bump = hooks.CanBumpTransaction(txid, nullptr, nullptr);
    ASSERT_FALSE(can_bump, "Should return false for null wallet");

    std::cout << "✅ Test 8 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 9: WalletFeeBumpHooks - NeedsAttention (No Wallet)
// ============================================================================

bool test_w4_5_needs_attention_null_wallet() {
    std::cout << "\n[Test 9] W.4.5: NeedsAttention with null wallet" << std::endl;

    WalletFeeBumpHooks hooks;
    uint256 txid = uint256::FromHexUnsafe(
        "0000000000000000000000000000000000000000000000000000000000000001"
    );

    // Should return false for null wallet without crashing
    bool needs_attention = hooks.NeedsAttention(txid, nullptr, nullptr, 6);
    ASSERT_FALSE(needs_attention, "Should return false for null wallet");

    std::cout << "✅ Test 9 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 10: WalletFeeBumpHooks - Integration Test (All Methods)
// ============================================================================

bool test_w4_5_integration() {
    std::cout << "\n[Test 10] W.4.5: Integration test - all methods callable" << std::endl;

    WalletFeeBumpHooks hooks;
    uint256 txid = uint256::FromHexUnsafe(
        "0000000000000000000000000000000000000000000000000000000000000001"
    );

    // Test that all methods are callable and don't crash
    // (even with null wallet/mempool - should handle gracefully)

    auto summary = hooks.GetWalletBumpSummary(nullptr, nullptr);
    ASSERT_EQ(summary.total_unconfirmed, 0U, "Summary should be valid");

    auto txs = hooks.GetTransactionsNeedingBump(nullptr, nullptr, 1);
    ASSERT_TRUE(txs.empty(), "Should return empty list");

    auto status = hooks.CheckTransactionStatus(txid, nullptr, nullptr);
    ASSERT_EQ(status.txid.ToString(), txid.ToString(), "Status should be valid");

    auto recommendation = hooks.GetBumpRecommendation(txid, nullptr, nullptr, 6);
    ASSERT_EQ(recommendation.txid.ToString(), txid.ToString(), "Recommendation should be valid");

    bool can_bump = hooks.CanBumpTransaction(txid, nullptr, nullptr);
    // Result depends on analyzer, just verify no crash

    bool needs_attention = hooks.NeedsAttention(txid, nullptr, nullptr, 6);
    // Result depends on analyzer, just verify no crash

    std::cout << "✅ Test 10 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 11: WalletFeeBumpHooks - Default Target Blocks
// ============================================================================

bool test_w4_5_default_target_blocks() {
    std::cout << "\n[Test 11] W.4.5: Default target_blocks parameters" << std::endl;

    WalletFeeBumpHooks hooks;

    // Test GetTransactionsNeedingBump default (should be 1 block)
    auto txs = hooks.GetTransactionsNeedingBump(nullptr);
    ASSERT_TRUE(txs.empty(), "Should work with default target_blocks");

    // Test NeedsAttention default (should be 6 blocks)
    uint256 txid = uint256::FromHexUnsafe(
        "0000000000000000000000000000000000000000000000000000000000000001"
    );
    bool needs = hooks.NeedsAttention(txid, nullptr);
    // Just verify it runs without crash

    std::cout << "✅ Test 11 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 12: Edge Cases - Multiple Transactions in Summary
// ============================================================================

bool test_w4_5_summary_edge_cases() {
    std::cout << "\n[Test 12] W.4.5: Summary edge cases" << std::endl;

    WalletFeeBumpHooks::WalletBumpSummary summary;

    // Test with all categories populated
    summary.total_unconfirmed = 100;
    summary.likely_to_confirm = 80;
    summary.stalled = 15;
    summary.blocked = 5;
    summary.can_rbf = 20;
    summary.can_cpfp = 10;

    // Add some transaction IDs
    for (int i = 0; i < 20; i++) {
        std::string hex_str = "0000000000000000000000000000000000000000000000000000000000000";
        hex_str += std::to_string(i);
        summary.needs_attention.push_back(uint256::FromHexUnsafe(hex_str));
    }

    ASSERT_EQ(summary.needs_attention.size(), 20U, "Should have 20 transactions");
    ASSERT_TRUE(summary.has_stuck_transactions(), "Should detect stuck");
    ASSERT_TRUE(summary.has_bump_options(), "Should detect bump options");

    // Verify string representation
    std::string str = WalletBumpSummaryToString(summary);
    ASSERT_TRUE(str.find("total_unconfirmed=100") != std::string::npos,
                "Should show correct total");
    ASSERT_TRUE(str.find("needs_attention=20") != std::string::npos,
                "Should show correct needs_attention count");

    std::cout << "✅ Test 12 passed" << std::endl;
    return true;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Phase W.4.5: Wallet Fee Bump UX Hooks Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    bool all_passed = true;

    // Run all tests
    all_passed &= test_w4_5_bump_summary_default();
    all_passed &= test_w4_5_bump_summary_helpers();
    all_passed &= test_w4_5_bump_summary_to_string();
    all_passed &= test_w4_5_get_summary_null_wallet();
    all_passed &= test_w4_5_get_needing_bump_null_wallet();
    all_passed &= test_w4_5_check_transaction_status();
    all_passed &= test_w4_5_get_bump_recommendation();
    all_passed &= test_w4_5_can_bump_null_wallet();
    all_passed &= test_w4_5_needs_attention_null_wallet();
    all_passed &= test_w4_5_integration();
    all_passed &= test_w4_5_default_target_blocks();
    all_passed &= test_w4_5_summary_edge_cases();

    std::cout << "\n========================================" << std::endl;
    if (all_passed) {
        std::cout << "✅ ALL TESTS PASSED" << std::endl;
        return 0;
    } else {
        std::cout << "❌ SOME TESTS FAILED" << std::endl;
        return 1;
    }
}
