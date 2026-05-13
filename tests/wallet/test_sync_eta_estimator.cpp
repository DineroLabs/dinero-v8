/**
 * Phase W.2.2: Sync ETA Estimator Tests
 *
 * Tests for honest, stable ETA estimation.
 */

#include "wallet/sync_eta_estimator.h"
#include "wallet/sync_progress_tracker.h"
#include "wallet/wallet_sync_status.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace dinero;

// ============================================================================
// Test Utilities
// ============================================================================

void assert_true(bool condition, const std::string& msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << std::endl;
        std::exit(1);
    }
}

void assert_false(bool condition, const std::string& msg) {
    if (condition) {
        std::cerr << "FAIL: " << msg << std::endl;
        std::exit(1);
    }
}

// ============================================================================
// W.2.2.1: ETA Hidden Before Stability Window
// ============================================================================

void test_w2_2_1_eta_hidden_before_stability() {
    std::cout << "Running test_w2_2_1_eta_hidden_before_stability..." << std::endl;

    SyncETAEstimator estimator(30000);  // 30s window

    // Record only 5 samples over 5 seconds (< 30s window)
    for (int i = 0; i < 5; ++i) {
        estimator.RecordSample(i * 1000, i * 10);  // 10 items/sec
    }

    // Should not be stable yet
    assert_false(estimator.IsStable(5000),
                 "Estimator should not be stable with <30s data");

    // ETA should be nullopt
    auto eta = estimator.CalculateETA(50, 100, 5000);
    assert_false(eta.has_value(),
                 "ETA should be nullopt before stability window");

    std::cout << "✅ test_w2_2_1_eta_hidden_before_stability PASSED" << std::endl;
}

// ============================================================================
// W.2.2.2: ETA Appears After Stability Window
// ============================================================================

void test_w2_2_2_eta_appears_after_stability() {
    std::cout << "Running test_w2_2_2_eta_appears_after_stability..." << std::endl;

    SyncETAEstimator estimator(30000);  // 30s window

    // Record 40 samples over 40 seconds (> 30s window, > 10 samples)
    for (int i = 0; i < 40; ++i) {
        estimator.RecordSample(i * 1000, i * 10);  // 10 items/sec
    }

    // Should be stable now
    assert_true(estimator.IsStable(40000),
                "Estimator should be stable with ≥30s data and ≥10 samples");

    // ETA should be available
    auto eta = estimator.CalculateETA(400, 500, 40000);
    assert_true(eta.has_value(),
                "ETA should be available after stability window");

    // ETA should be reasonable (~10 seconds for 100 items at 10/sec)
    assert_true(eta.value().count() >= 5 && eta.value().count() <= 15,
                "ETA should be ~10 seconds (got " + std::to_string(eta.value().count()) + ")");

    std::cout << "✅ test_w2_2_2_eta_appears_after_stability PASSED" << std::endl;
}

// ============================================================================
// W.2.2.3: ETA Decreases Monotonically
// ============================================================================

void test_w2_2_3_eta_decreases_monotonically() {
    std::cout << "Running test_w2_2_3_eta_decreases_monotonically..." << std::endl;

    SyncETAEstimator estimator(30000);  // 30s window

    // Build up stable history (40s)
    for (int i = 0; i < 40; ++i) {
        estimator.RecordSample(i * 1000, i * 10);  // 10 items/sec
    }

    // Get initial ETA
    auto eta1 = estimator.CalculateETA(400, 500, 40000);
    assert_true(eta1.has_value(), "ETA1 should be available");

    // Progress further (10 more seconds)
    for (int i = 40; i < 50; ++i) {
        estimator.RecordSample(i * 1000, i * 10);
    }

    // Get updated ETA
    auto eta2 = estimator.CalculateETA(500, 500, 50000);
    assert_true(eta2.has_value(), "ETA2 should be available");

    // ETA should decrease (or reach 0 if complete)
    assert_true(eta2.value() <= eta1.value(),
                "ETA should decrease over time (eta1=" +
                std::to_string(eta1.value().count()) + "s, eta2=" +
                std::to_string(eta2.value().count()) + "s)");

    // When complete, ETA should be 0
    assert_true(eta2.value().count() == 0,
                "ETA should be 0 when progress reaches target");

    std::cout << "✅ test_w2_2_3_eta_decreases_monotonically PASSED" << std::endl;
}

// ============================================================================
// W.2.2.4: ETA Frozen During Reorg
// ============================================================================

void test_w2_2_4_eta_frozen_during_reorg() {
    std::cout << "Running test_w2_2_4_eta_frozen_during_reorg..." << std::endl;

    SyncETAEstimator estimator(30000);  // 30s window

    // Build up stable history
    for (int i = 0; i < 40; ++i) {
        estimator.RecordSample(i * 1000, i * 10);
    }

    // ETA should be available
    auto eta_before = estimator.CalculateETA(400, 500, 40000);
    assert_true(eta_before.has_value(), "ETA should be available before freeze");

    // Freeze (simulate reorg)
    estimator.Freeze();
    assert_true(estimator.IsFrozen(), "Estimator should be frozen");

    // ETA should now be nullopt
    auto eta_frozen = estimator.CalculateETA(400, 500, 40000);
    assert_false(eta_frozen.has_value(),
                 "ETA should be nullopt when frozen");

    // Unfreeze
    estimator.Unfreeze();
    assert_false(estimator.IsFrozen(), "Estimator should be unfrozen");

    // ETA should be available again
    auto eta_after = estimator.CalculateETA(400, 500, 40000);
    assert_true(eta_after.has_value(), "ETA should be available after unfreeze");

    std::cout << "✅ test_w2_2_4_eta_frozen_during_reorg PASSED" << std::endl;
}

// ============================================================================
// W.2.2.5: Rate Calculation Accuracy
// ============================================================================

void test_w2_2_5_rate_calculation() {
    std::cout << "Running test_w2_2_5_rate_calculation..." << std::endl;

    SyncETAEstimator estimator(30000);  // 30s window

    // Simulate steady progress: 10 items/second for 40 seconds
    for (int i = 0; i < 40; ++i) {
        estimator.RecordSample(i * 1000, i * 10);
    }

    // Rate should be ~10 items/second
    double rate = estimator.GetCurrentRate();
    assert_true(rate >= 9.0 && rate <= 11.0,
                "Rate should be ~10 items/sec (got " + std::to_string(rate) + ")");

    std::cout << "  ✓ Rate: " << rate << " items/sec" << std::endl;

    std::cout << "✅ test_w2_2_5_rate_calculation PASSED" << std::endl;
}

// ============================================================================
// W.2.2.6: Phase-Based ETA Selection (IBD)
// ============================================================================

void test_w2_2_6_phase_ibd_eta() {
    std::cout << "Running test_w2_2_6_phase_ibd_eta..." << std::endl;

    SyncProgressTracker tracker;

    // Build up stable history
    for (int i = 0; i < 40; ++i) {
        WalletSyncStatus status;
        status.phase = SyncPhase::IBD;
        status.headers_synced = i * 10;    // 10/sec
        status.headers_total = 1000;
        status.blocks_synced = i * 5;      // 5/sec (slower!)
        status.blocks_total = 1000;
        status.wallet_scan_height = 0;
        status.chain_height = 1000;

        tracker.Update(status, i * 1000);
    }

    // Create current status
    WalletSyncStatus status;
    status.phase = SyncPhase::IBD;
    status.headers_synced = 400;
    status.headers_total = 1000;
    status.blocks_synced = 200;  // Slower component
    status.blocks_total = 1000;
    status.wallet_scan_height = 0;
    status.chain_height = 1000;

    // IBD should use max(headers_eta, blocks_eta)
    // Headers: 600 remaining at 10/sec = 60s
    // Blocks: 800 remaining at 5/sec = 160s
    // Should return 160s (wait for slower component)
    auto eta = tracker.CalculateETA(status, 40000);

    assert_true(eta.has_value(), "IBD ETA should be available");
    assert_true(eta.value().count() >= 150 && eta.value().count() <= 170,
                "IBD ETA should be ~160s (wait for blocks), got " +
                std::to_string(eta.value().count()) + "s");

    std::cout << "  ✓ IBD ETA: " << eta.value().count() << "s (waits for slower blocks)" << std::endl;

    std::cout << "✅ test_w2_2_6_phase_ibd_eta PASSED" << std::endl;
}

// ============================================================================
// W.2.2.7: Phase-Based ETA Selection (CATCHING_UP)
// ============================================================================

void test_w2_2_7_phase_catching_up_eta() {
    std::cout << "Running test_w2_2_7_phase_catching_up_eta..." << std::endl;

    SyncProgressTracker tracker;

    // Build up stable history
    for (int i = 0; i < 40; ++i) {
        WalletSyncStatus status;
        status.phase = SyncPhase::CATCHING_UP;
        status.headers_synced = 1000;  // Complete
        status.headers_total = 1000;
        status.blocks_synced = i * 10;  // 10/sec
        status.blocks_total = 1000;
        status.wallet_scan_height = 0;
        status.chain_height = 1000;

        tracker.Update(status, i * 1000);
    }

    // Create current status
    WalletSyncStatus status;
    status.phase = SyncPhase::CATCHING_UP;
    status.headers_synced = 1000;  // Complete
    status.headers_total = 1000;
    status.blocks_synced = 400;
    status.blocks_total = 1000;
    status.wallet_scan_height = 0;
    status.chain_height = 1000;

    // CATCHING_UP should use blocks_eta
    // Blocks: 600 remaining at 10/sec = 60s
    auto eta = tracker.CalculateETA(status, 40000);

    assert_true(eta.has_value(), "CATCHING_UP ETA should be available");
    assert_true(eta.value().count() >= 55 && eta.value().count() <= 65,
                "CATCHING_UP ETA should be ~60s (blocks only), got " +
                std::to_string(eta.value().count()) + "s");

    std::cout << "  ✓ CATCHING_UP ETA: " << eta.value().count() << "s (blocks only)" << std::endl;

    std::cout << "✅ test_w2_2_7_phase_catching_up_eta PASSED" << std::endl;
}

// ============================================================================
// W.2.2.8: Phase-Based ETA Selection (STEADY_STATE)
// ============================================================================

void test_w2_2_8_phase_steady_state_eta() {
    std::cout << "Running test_w2_2_8_phase_steady_state_eta..." << std::endl;

    SyncProgressTracker tracker;

    // Build up stable history
    for (int i = 0; i < 40; ++i) {
        WalletSyncStatus status;
        status.phase = SyncPhase::STEADY_STATE;
        status.headers_synced = 1000;  // Complete
        status.headers_total = 1000;
        status.blocks_synced = 1000;   // Complete
        status.blocks_total = 1000;
        status.wallet_scan_height = i * 20;  // 20/sec
        status.chain_height = 1000;

        tracker.Update(status, i * 1000);
    }

    // Create current status
    WalletSyncStatus status;
    status.phase = SyncPhase::STEADY_STATE;
    status.headers_synced = 1000;
    status.headers_total = 1000;
    status.blocks_synced = 1000;
    status.blocks_total = 1000;
    status.wallet_scan_height = 800;
    status.chain_height = 1000;

    // STEADY_STATE should use wallet_scan_eta
    // Wallet scan: 200 remaining at 20/sec = 10s
    auto eta = tracker.CalculateETA(status, 40000);

    assert_true(eta.has_value(), "STEADY_STATE ETA should be available");
    assert_true(eta.value().count() >= 8 && eta.value().count() <= 12,
                "STEADY_STATE ETA should be ~10s (wallet scan only), got " +
                std::to_string(eta.value().count()) + "s");

    std::cout << "  ✓ STEADY_STATE ETA: " << eta.value().count() << "s (wallet scan only)" << std::endl;

    std::cout << "✅ test_w2_2_8_phase_steady_state_eta PASSED" << std::endl;
}

// ============================================================================
// W.2.2.9: Tracker Freeze/Unfreeze
// ============================================================================

void test_w2_2_9_tracker_freeze() {
    std::cout << "Running test_w2_2_9_tracker_freeze..." << std::endl;

    SyncProgressTracker tracker;

    // Build up stable history
    for (int i = 0; i < 40; ++i) {
        WalletSyncStatus status;
        status.phase = SyncPhase::CATCHING_UP;
        status.headers_synced = 1000;
        status.headers_total = 1000;
        status.blocks_synced = i * 10;
        status.blocks_total = 1000;
        status.wallet_scan_height = 0;
        status.chain_height = 1000;

        tracker.Update(status, i * 1000);
    }

    WalletSyncStatus status;
    status.phase = SyncPhase::CATCHING_UP;
    status.headers_synced = 1000;
    status.headers_total = 1000;
    status.blocks_synced = 400;
    status.blocks_total = 1000;
    status.wallet_scan_height = 0;
    status.chain_height = 1000;

    // ETA should be available
    auto eta_before = tracker.CalculateETA(status, 40000);
    assert_true(eta_before.has_value(), "ETA should be available before freeze");

    // Freeze
    tracker.Freeze();
    assert_true(tracker.IsFrozen(), "Tracker should be frozen");

    // ETA should be nullopt
    auto eta_frozen = tracker.CalculateETA(status, 40000);
    assert_false(eta_frozen.has_value(), "ETA should be nullopt when frozen");

    // Unfreeze
    tracker.Unfreeze();
    assert_false(tracker.IsFrozen(), "Tracker should be unfrozen");

    // ETA should be available again
    auto eta_after = tracker.CalculateETA(status, 40000);
    assert_true(eta_after.has_value(), "ETA should be available after unfreeze");

    std::cout << "✅ test_w2_2_9_tracker_freeze PASSED" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Phase W.2.2: Sync ETA Estimator Tests" << std::endl;
    std::cout << "========================================\n" << std::endl;

    test_w2_2_1_eta_hidden_before_stability();
    test_w2_2_2_eta_appears_after_stability();
    test_w2_2_3_eta_decreases_monotonically();
    test_w2_2_4_eta_frozen_during_reorg();
    test_w2_2_5_rate_calculation();
    test_w2_2_6_phase_ibd_eta();
    test_w2_2_7_phase_catching_up_eta();
    test_w2_2_8_phase_steady_state_eta();
    test_w2_2_9_tracker_freeze();

    std::cout << "\n========================================" << std::endl;
    std::cout << "✅ All W.2.2 Tests PASSED (9/9)" << std::endl;
    std::cout << "   ETA Stability Tests (4/4)" << std::endl;
    std::cout << "   Rate Calculation (1/1)" << std::endl;
    std::cout << "   Phase-Based ETA (3/3)" << std::endl;
    std::cout << "   Freeze/Unfreeze (1/1)" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
