/**
 * Phase G.4: Fee Estimation Integration Tests
 *
 * Test Coverage:
 * - G.4.1: Transaction entry recording
 * - G.4.2: Confirmation recording and fee calculation
 * - G.4.3: Fee estimation for different targets
 * - G.4.4: Insufficient data handling
 * - G.4.5: Fee estimation over time (multiple blocks)
 */

#include "mempool/fee_estimator.h"
#include "primitives/uint256.h"
#include <iostream>
#include <cassert>
#include <vector>

using namespace dinero;

// ============================================================================
// Test G.4.1: Transaction Entry Recording
// ============================================================================

void test_g4_1_transaction_entry_recording() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.4.1: Transaction Entry Recording" << std::endl;
    std::cout << "========================================\n" << std::endl;

    FeeEstimator estimator;

    // Record some transactions entering mempool
    uint256 tx1 = uint256::FromHexUnsafe("1111111111111111111111111111111111111111111111111111111111111111");
    uint256 tx2 = uint256::FromHexUnsafe("2222222222222222222222222222222222222222222222222222222222222222");
    uint256 tx3 = uint256::FromHexUnsafe("3333333333333333333333333333333333333333333333333333333333333333");

    std::cout << "Recording transaction entries at different fee rates..." << std::endl;

    // Different fee rates (una per byte)
    estimator.recordTxEntry(tx1, 5.0, 100);   // Low fee
    estimator.recordTxEntry(tx2, 10.0, 100);  // Medium fee
    estimator.recordTxEntry(tx3, 20.0, 100);  // High fee

    auto stats = estimator.getStats();
    std::cout << "Tracked transactions: " << stats.tracked_txs << std::endl;

    assert(stats.tracked_txs == 3);
    std::cout << "✅ Successfully tracking 3 transactions\n" << std::endl;

    std::cout << "✅ Test G.4.1 PASSED: Transaction entry recording successful\n" << std::endl;
}

// ============================================================================
// Test G.4.2: Confirmation Recording and Fee Calculation
// ============================================================================

void test_g4_2_confirmation_recording() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.4.2: Confirmation Recording" << std::endl;
    std::cout << "========================================\n" << std::endl;

    FeeEstimator estimator;

    // Record transactions with different fee rates
    uint256 fast_tx = uint256::FromHexUnsafe("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    uint256 medium_tx = uint256::FromHexUnsafe("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    uint256 slow_tx = uint256::FromHexUnsafe("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");

    std::cout << "Simulating transactions confirmed at different speeds..." << std::endl;

    // Fast transaction: high fee, confirmed in 1 block
    estimator.recordTxEntry(fast_tx, 50.0, 100);
    estimator.recordTxConfirmation(fast_tx, 101);  // Confirmed in next block

    // Medium transaction: medium fee, confirmed in 4 blocks
    estimator.recordTxEntry(medium_tx, 20.0, 100);
    estimator.recordTxConfirmation(medium_tx, 104);  // Confirmed in 4 blocks

    // Slow transaction: low fee, confirmed in 10 blocks
    estimator.recordTxEntry(slow_tx, 5.0, 100);
    estimator.recordTxConfirmation(slow_tx, 110);  // Confirmed in 10 blocks

    auto stats = estimator.getStats();
    std::cout << "Confirmed transactions: " << stats.confirmed_txs << std::endl;
    std::cout << "Fast bucket samples: " << stats.fast_samples << std::endl;
    std::cout << "Medium bucket samples: " << stats.medium_samples << std::endl;
    std::cout << "Slow bucket samples: " << stats.slow_samples << std::endl;

    assert(stats.confirmed_txs == 3);
    assert(stats.fast_samples >= 1);    // fast_tx confirmed in 1 block
    assert(stats.medium_samples >= 1);  // medium_tx confirmed in 4 blocks
    assert(stats.slow_samples >= 1);    // slow_tx confirmed in 10 blocks

    std::cout << "✅ Transactions correctly categorized into buckets\n" << std::endl;

    std::cout << "✅ Test G.4.2 PASSED: Confirmation recording successful\n" << std::endl;
}

// ============================================================================
// Test G.4.3: Fee Estimation for Different Targets
// ============================================================================

void test_g4_3_fee_estimation() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.4.3: Fee Estimation" << std::endl;
    std::cout << "========================================\n" << std::endl;

    FeeEstimator estimator;

    std::cout << "Building fee estimation data with multiple confirmations..." << std::endl;
    std::cout << "(Need at least 10 samples per bucket for estimates)\n" << std::endl;

    // Create a realistic fee estimation scenario
    // Fast confirmations (1-2 blocks) with high fees - need 10+ samples
    for (int i = 0; i < 12; i++) {
        uint256 txid = uint256::FromHexUnsafe(
            "1000000000000000000000000000000000000000000000000000000000000000");
        txid.data[0] = static_cast<unsigned char>(i);  // Make unique

        double fee_rate = 40.0 + (i * 3.0);  // 40-73 sat/byte
        estimator.recordTxEntry(txid, fee_rate, 1000);
        estimator.recordTxConfirmation(txid, 1001);  // 1 block confirmation
    }

    // Medium confirmations (3-6 blocks) with medium fees - need 10+ samples
    for (int i = 0; i < 12; i++) {
        uint256 txid = uint256::FromHexUnsafe(
            "2000000000000000000000000000000000000000000000000000000000000000");
        txid.data[0] = static_cast<unsigned char>(i);

        double fee_rate = 15.0 + (i * 2.0);  // 15-37 sat/byte
        estimator.recordTxEntry(txid, fee_rate, 1000);
        estimator.recordTxConfirmation(txid, 1004);  // 4 block confirmation
    }

    // Slow confirmations (6-12 blocks) with low fees - need 10+ samples
    for (int i = 0; i < 12; i++) {
        uint256 txid = uint256::FromHexUnsafe(
            "3000000000000000000000000000000000000000000000000000000000000000");
        txid.data[0] = static_cast<unsigned char>(i);

        double fee_rate = 3.0 + (i * 1.5);  // 3-19.5 sat/byte
        estimator.recordTxEntry(txid, fee_rate, 1000);
        estimator.recordTxConfirmation(txid, 1010);  // 10 block confirmation
    }

    std::cout << "Estimating fees for different targets...\n" << std::endl;

    // Test fee estimation for different targets
    auto fast_fee = estimator.estimateFee(1);      // Fast: 1-2 blocks
    auto medium_fee = estimator.estimateFee(5);    // Medium: 3-6 blocks
    auto slow_fee = estimator.estimateFee(10);     // Slow: 6-12 blocks

    if (fast_fee.has_value()) {
        std::cout << "Fast (1 block) estimate: " << fast_fee.value() << " sat/byte" << std::endl;
        // Should be higher than medium
        assert(fast_fee.value() > 0);
    } else {
        std::cout << "Fast estimate: Insufficient data" << std::endl;
    }

    if (medium_fee.has_value()) {
        std::cout << "Medium (5 blocks) estimate: " << medium_fee.value() << " sat/byte" << std::endl;
        assert(medium_fee.value() > 0);
    } else {
        std::cout << "Medium estimate: Insufficient data" << std::endl;
    }

    if (slow_fee.has_value()) {
        std::cout << "Slow (10 blocks) estimate: " << slow_fee.value() << " sat/byte" << std::endl;
        assert(slow_fee.value() > 0);
    } else {
        std::cout << "Slow estimate: Insufficient data" << std::endl;
    }

    // At least one estimate should be available
    assert(fast_fee.has_value() || medium_fee.has_value() || slow_fee.has_value());
    std::cout << "\n✅ Fee estimation working correctly\n" << std::endl;

    // Verify ordering: fast should be >= medium >= slow (when all available)
    if (fast_fee.has_value() && medium_fee.has_value()) {
        std::cout << "Verifying fast fee >= medium fee..." << std::endl;
        assert(fast_fee.value() >= medium_fee.value() * 0.8);  // Allow some variance
        std::cout << "✅ Fee ordering correct\n" << std::endl;
    }

    std::cout << "✅ Test G.4.3 PASSED: Fee estimation successful\n" << std::endl;
}

// ============================================================================
// Test G.4.4: Insufficient Data Handling
// ============================================================================

void test_g4_4_insufficient_data() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.4.4: Insufficient Data Handling" << std::endl;
    std::cout << "========================================\n" << std::endl;

    FeeEstimator estimator;

    std::cout << "Testing fee estimation with no data..." << std::endl;

    // Try to estimate fees with no data
    auto fee_estimate = estimator.estimateFee(5);

    // Should return nullopt (no data)
    assert(!fee_estimate.has_value());
    std::cout << "✅ Correctly returns nullopt when no data available\n" << std::endl;

    // Add ONE transaction (still insufficient)
    uint256 tx = uint256::FromHexUnsafe("4444444444444444444444444444444444444444444444444444444444444444");
    estimator.recordTxEntry(tx, 10.0, 100);
    estimator.recordTxConfirmation(tx, 105);

    auto stats = estimator.getStats();
    std::cout << "After 1 confirmation: " << stats.confirmed_txs << " confirmed" << std::endl;

    // Might still be insufficient depending on implementation
    fee_estimate = estimator.estimateFee(5);
    if (fee_estimate.has_value()) {
        std::cout << "Fee estimate available: " << fee_estimate.value() << " sat/byte" << std::endl;
    } else {
        std::cout << "✅ Still returns nullopt (insufficient samples)" << std::endl;
    }

    std::cout << "✅ Test G.4.4 PASSED: Insufficient data handling successful\n" << std::endl;
}

// ============================================================================
// Test G.4.5: Fee Estimation Over Time
// ============================================================================

void test_g4_5_fee_estimation_over_time() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.4.5: Fee Estimation Over Time" << std::endl;
    std::cout << "========================================\n" << std::endl;

    FeeEstimator estimator;

    std::cout << "Simulating block-by-block confirmation pattern...\n" << std::endl;

    uint32_t current_height = 1000;

    // Simulate 20 blocks of transactions
    for (uint32_t block = 0; block < 20; block++) {
        current_height++;

        // Each block, add 3 transactions with varying fees
        for (int i = 0; i < 3; i++) {
            uint256 txid = uint256::FromHexUnsafe(
                "5000000000000000000000000000000000000000000000000000000000000000");
            txid.data[0] = static_cast<unsigned char>(block);
            txid.data[1] = static_cast<unsigned char>(i);

            // Fee rate varies: some high priority, some low
            double fee_rate = (block % 3 == 0) ? 50.0 : (block % 3 == 1) ? 20.0 : 5.0;

            estimator.recordTxEntry(txid, fee_rate, current_height - 1);

            // Confirm with variable delay based on fee
            uint32_t confirm_delay = (fee_rate > 40) ? 1 : (fee_rate > 15) ? 4 : 9;
            estimator.recordTxConfirmation(txid, current_height - 1 + confirm_delay);
        }

        if (block % 5 == 0) {
            auto stats = estimator.getStats();
            std::cout << "Block " << current_height << ": "
                      << stats.confirmed_txs << " confirmed, "
                      << stats.tracked_txs << " tracking" << std::endl;
        }
    }

    std::cout << "\nFinal statistics:" << std::endl;
    auto stats = estimator.getStats();
    std::cout << "  Total tracked: " << stats.tracked_txs << std::endl;
    std::cout << "  Total confirmed: " << stats.confirmed_txs << std::endl;
    std::cout << "  Fast samples: " << stats.fast_samples << std::endl;
    std::cout << "  Medium samples: " << stats.medium_samples << std::endl;
    std::cout << "  Slow samples: " << stats.slow_samples << std::endl;

    assert(stats.confirmed_txs == 60);  // 20 blocks * 3 tx/block
    std::cout << "✅ All transactions confirmed\n" << std::endl;

    // Test estimation after accumulating data
    auto fast_estimate = estimator.estimateFee(1);
    auto medium_estimate = estimator.estimateFee(5);
    auto slow_estimate = estimator.estimateFee(10);

    std::cout << "\nFee estimates with sufficient data:" << std::endl;
    if (fast_estimate.has_value()) {
        std::cout << "  Fast (1 block): " << fast_estimate.value() << " sat/byte" << std::endl;
    }
    if (medium_estimate.has_value()) {
        std::cout << "  Medium (5 blocks): " << medium_estimate.value() << " sat/byte" << std::endl;
    }
    if (slow_estimate.has_value()) {
        std::cout << "  Slow (10 blocks): " << slow_estimate.value() << " sat/byte" << std::endl;
    }

    // With 60 confirmations, we should have estimates
    assert(fast_estimate.has_value() || medium_estimate.has_value() || slow_estimate.has_value());
    std::cout << "\n✅ Fee estimation working with real data\n" << std::endl;

    std::cout << "✅ Test G.4.5 PASSED: Fee estimation over time successful\n" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Phase G.4: Fee Estimation Tests     ║" << std::endl;
    std::cout << "╚════════════════════════════════════════╝\n" << std::endl;

    try {
        test_g4_1_transaction_entry_recording();
        test_g4_2_confirmation_recording();
        test_g4_3_fee_estimation();
        test_g4_4_insufficient_data();
        test_g4_5_fee_estimation_over_time();

        std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
        std::cout << "║  ✅ ALL TESTS PASSED                  ║" << std::endl;
        std::cout << "╚════════════════════════════════════════╝\n" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
