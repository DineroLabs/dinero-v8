/**
 * Phase W.2.1: Wallet Sync Status Tests
 *
 * Tests for truthful, phase-aware sync status aggregation.
 */

#include "wallet/wallet_sync_status.h"
#include "p2p/block_download_scheduler.h"
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

void assert_eq(double a, double b, double epsilon, const std::string& msg) {
    if (std::abs(a - b) > epsilon) {
        std::cerr << "FAIL: " << msg << " (expected " << b << ", got " << a << ")" << std::endl;
        std::exit(1);
    }
}

void assert_eq(uint64_t a, uint64_t b, const std::string& msg) {
    if (a != b) {
        std::cerr << "FAIL: " << msg << " (expected " << b << ", got " << a << ")" << std::endl;
        std::exit(1);
    }
}

// ============================================================================
// W.2.1.1: WalletSyncStatus Construction & Validation
// ============================================================================

void test_w2_1_1_default_construction() {
    std::cout << "Running test_w2_1_1_default_construction..." << std::endl;

    WalletSyncStatus status;

    // Test default values
    assert_true(status.phase == SyncPhase::STEADY_STATE,
                "Default phase should be STEADY_STATE");
    assert_eq(status.overall_progress, 0.0, 0.001,
              "Default progress should be 0.0");
    assert_true(!status.eta.has_value(),
                "Default ETA should be nullopt");
    assert_eq(status.headers_synced, 0,
              "Default headers_synced should be 0");
    assert_eq(status.blocks_synced, 0,
              "Default blocks_synced should be 0");
    assert_eq(status.wallet_scan_height, 0,
              "Default wallet_scan_height should be 0");
    assert_true(!status.is_reorg_in_progress,
                "Default should not be in reorg");
    assert_eq(status.last_reorg_depth, 0,
              "Default reorg depth should be 0");

    // Test validation
    assert_true(status.IsValid(),
                "Default status should be valid");
    assert_true(!status.IsFullySynced(),
                "Default status should not be fully synced");

    std::cout << "✅ test_w2_1_1_default_construction PASSED" << std::endl;
}

// ============================================================================
// W.2.1.2: Component Progress Calculations
// ============================================================================

void test_w2_1_2_component_progress() {
    std::cout << "Running test_w2_1_2_component_progress..." << std::endl;

    WalletSyncStatus status;

    // Test 1: Headers progress
    status.headers_synced = 50;
    status.headers_total = 100;
    assert_eq(status.headers_progress(), 0.5, 0.001,
              "Headers progress should be 50%");

    // Test 2: Blocks progress
    status.blocks_synced = 75;
    status.blocks_total = 100;
    assert_eq(status.blocks_progress(), 0.75, 0.001,
              "Blocks progress should be 75%");

    // Test 3: Wallet scan progress
    status.wallet_scan_height = 25;
    status.chain_height = 100;
    assert_eq(status.wallet_scan_progress(), 0.25, 0.001,
              "Wallet scan progress should be 25%");

    // Test 4: Complete progress
    status.headers_synced = 100;
    status.blocks_synced = 100;
    status.wallet_scan_height = 100;
    assert_eq(status.headers_progress(), 1.0, 0.001,
              "Complete headers progress should be 100%");
    assert_eq(status.blocks_progress(), 1.0, 0.001,
              "Complete blocks progress should be 100%");
    assert_eq(status.wallet_scan_progress(), 1.0, 0.001,
              "Complete wallet scan progress should be 100%");

    // Test 5: Zero total (edge case)
    status.headers_total = 0;
    assert_eq(status.headers_progress(), 0.0, 0.001,
              "Progress with zero total should be 0%");

    std::cout << "✅ test_w2_1_2_component_progress PASSED" << std::endl;
}

// ============================================================================
// W.2.1.3: Phase-Aware Overall Progress
// ============================================================================

void test_w2_1_3_phase_aware_progress() {
    std::cout << "Running test_w2_1_3_phase_aware_progress..." << std::endl;

    // Test 1: IBD phase (40% headers, 40% blocks, 20% scan)
    {
        WalletSyncStatus status;
        status.phase = SyncPhase::IBD;
        status.headers_synced = 50;
        status.headers_total = 100;  // 50%
        status.blocks_synced = 100;
        status.blocks_total = 100;   // 100%
        status.wallet_scan_height = 0;
        status.chain_height = 100;   // 0%

        double expected = 0.4 * 0.5 + 0.4 * 1.0 + 0.2 * 0.0;  // = 0.6
        double actual = WalletSyncStatusAggregator::CalculateOverallProgress(status);

        assert_eq(actual, expected, 0.001,
                  "IBD progress should weight headers and blocks equally");
    }

    // Test 2: CATCHING_UP phase (20% headers, 60% blocks, 20% scan)
    {
        WalletSyncStatus status;
        status.phase = SyncPhase::CATCHING_UP;
        status.headers_synced = 100;
        status.headers_total = 100;  // 100%
        status.blocks_synced = 50;
        status.blocks_total = 100;   // 50%
        status.wallet_scan_height = 0;
        status.chain_height = 100;   // 0%

        double expected = 0.2 * 1.0 + 0.6 * 0.5 + 0.2 * 0.0;  // = 0.5
        double actual = WalletSyncStatusAggregator::CalculateOverallProgress(status);

        assert_eq(actual, expected, 0.001,
                  "CATCHING_UP progress should weight blocks dominantly");
    }

    // Test 3: STEADY_STATE phase (10% headers, 10% blocks, 80% scan)
    {
        WalletSyncStatus status;
        status.phase = SyncPhase::STEADY_STATE;
        status.headers_synced = 100;
        status.headers_total = 100;  // 100%
        status.blocks_synced = 100;
        status.blocks_total = 100;   // 100%
        status.wallet_scan_height = 50;
        status.chain_height = 100;   // 50%

        double expected = 0.1 * 1.0 + 0.1 * 1.0 + 0.8 * 0.5;  // = 0.6
        double actual = WalletSyncStatusAggregator::CalculateOverallProgress(status);

        assert_eq(actual, expected, 0.001,
                  "STEADY_STATE progress should weight wallet scan dominantly");
    }

    std::cout << "✅ test_w2_1_3_phase_aware_progress PASSED" << std::endl;
}

// ============================================================================
// W.2.1.4: Never False 100% Rule
// ============================================================================

void test_w2_1_4_no_false_100_percent() {
    std::cout << "Running test_w2_1_4_no_false_100_percent..." << std::endl;

    // Test 1: Incomplete headers → cap at 99.9%
    {
        WalletSyncStatus status;
        status.phase = SyncPhase::IBD;
        status.headers_synced = 99;
        status.headers_total = 100;  // 99%
        status.blocks_synced = 100;
        status.blocks_total = 100;   // 100%
        status.wallet_scan_height = 100;
        status.chain_height = 100;   // 100%

        double progress = WalletSyncStatusAggregator::CalculateOverallProgress(status);

        assert_true(progress < 1.0,
                    "Progress should be < 100% when headers incomplete");
        assert_true(progress <= 0.999,
                    "Progress should be capped at 99.9% when not fully complete");
    }

    // Test 2: Incomplete blocks → cap at 99.9%
    {
        WalletSyncStatus status;
        status.phase = SyncPhase::CATCHING_UP;
        status.headers_synced = 100;
        status.headers_total = 100;  // 100%
        status.blocks_synced = 99;
        status.blocks_total = 100;   // 99%
        status.wallet_scan_height = 100;
        status.chain_height = 100;   // 100%

        double progress = WalletSyncStatusAggregator::CalculateOverallProgress(status);

        assert_true(progress < 1.0,
                    "Progress should be < 100% when blocks incomplete");
    }

    // Test 3: Incomplete wallet scan → cap at 99.9%
    {
        WalletSyncStatus status;
        status.phase = SyncPhase::STEADY_STATE;
        status.headers_synced = 100;
        status.headers_total = 100;  // 100%
        status.blocks_synced = 100;
        status.blocks_total = 100;   // 100%
        status.wallet_scan_height = 99;
        status.chain_height = 100;   // 99%

        double progress = WalletSyncStatusAggregator::CalculateOverallProgress(status);

        assert_true(progress < 1.0,
                    "Progress should be < 100% when wallet scan incomplete");
    }

    // Test 4: Everything complete → allow 100%
    {
        WalletSyncStatus status;
        status.phase = SyncPhase::STEADY_STATE;
        status.headers_synced = 100;
        status.headers_total = 100;
        status.blocks_synced = 100;
        status.blocks_total = 100;
        status.wallet_scan_height = 100;
        status.chain_height = 100;

        double progress = WalletSyncStatusAggregator::CalculateOverallProgress(status);

        assert_eq(progress, 1.0, 0.001,
                  "Progress should be exactly 100% when everything complete");
        assert_true(status.IsFullySynced(),
                    "Status should report as fully synced");
    }

    std::cout << "✅ test_w2_1_4_no_false_100_percent PASSED" << std::endl;
}

// ============================================================================
// W.2.1.5: Status Validation Invariants
// ============================================================================

void test_w2_1_5_validation_invariants() {
    std::cout << "Running test_w2_1_5_validation_invariants..." << std::endl;

    // Test 1: Valid status
    {
        WalletSyncStatus status;
        status.headers_synced = 50;
        status.headers_total = 100;
        status.blocks_synced = 50;
        status.blocks_total = 100;
        status.wallet_scan_height = 50;
        status.chain_height = 100;
        status.overall_progress = 0.5;

        assert_true(status.IsValid(),
                    "Valid status should pass validation");
    }

    // Test 2: Invalid - headers_synced > headers_total
    {
        WalletSyncStatus status;
        status.headers_synced = 101;
        status.headers_total = 100;

        assert_true(!status.IsValid(),
                    "Status with headers_synced > headers_total should be invalid");
    }

    // Test 3: Invalid - blocks_synced > blocks_total
    {
        WalletSyncStatus status;
        status.blocks_synced = 101;
        status.blocks_total = 100;

        assert_true(!status.IsValid(),
                    "Status with blocks_synced > blocks_total should be invalid");
    }

    // Test 4: Invalid - wallet_scan_height > chain_height
    {
        WalletSyncStatus status;
        status.wallet_scan_height = 101;
        status.chain_height = 100;

        assert_true(!status.IsValid(),
                    "Status with wallet_scan_height > chain_height should be invalid");
    }

    // Test 5: Invalid - overall_progress out of range
    {
        WalletSyncStatus status;
        status.overall_progress = 1.5;

        assert_true(!status.IsValid(),
                    "Status with progress > 1.0 should be invalid");
    }

    // Test 6: Invalid - claiming 100% but incomplete
    {
        WalletSyncStatus status;
        status.headers_synced = 99;
        status.headers_total = 100;
        status.blocks_synced = 100;
        status.blocks_total = 100;
        status.wallet_scan_height = 100;
        status.chain_height = 100;
        status.overall_progress = 1.0;  // Claiming 100%

        assert_true(!status.IsValid(),
                    "Status claiming 100% with incomplete headers should be invalid");
    }

    std::cout << "✅ test_w2_1_5_validation_invariants PASSED" << std::endl;
}

// ============================================================================
// W.2.1.6: Human-Readable Status Descriptions
// ============================================================================

void test_w2_1_6_status_descriptions() {
    std::cout << "Running test_w2_1_6_status_descriptions..." << std::endl;

    // Test 1: IBD phase name
    {
        WalletSyncStatus status;
        status.phase = SyncPhase::IBD;

        assert_true(status.GetPhaseName() == "Initial Block Download",
                    "IBD phase name should be correct");
        assert_true(status.GetStatusDescription() == "Downloading blockchain",
                    "IBD status description should be correct");
    }

    // Test 2: CATCHING_UP phase name
    {
        WalletSyncStatus status;
        status.phase = SyncPhase::CATCHING_UP;

        assert_true(status.GetPhaseName() == "Catching Up",
                    "CATCHING_UP phase name should be correct");
        assert_true(status.GetStatusDescription() == "Catching up to network",
                    "CATCHING_UP status description should be correct");
    }

    // Test 3: STEADY_STATE phase name
    {
        WalletSyncStatus status;
        status.phase = SyncPhase::STEADY_STATE;

        assert_true(status.GetPhaseName() == "Synced",
                    "STEADY_STATE phase name should be correct");
    }

    // Test 4: Fully synced status
    {
        WalletSyncStatus status;
        status.phase = SyncPhase::STEADY_STATE;
        status.headers_synced = 100;
        status.headers_total = 100;
        status.blocks_synced = 100;
        status.blocks_total = 100;
        status.wallet_scan_height = 100;
        status.chain_height = 100;

        assert_true(status.GetStatusDescription() == "Fully synced",
                    "Fully synced status description should be correct");
    }

    // Test 5: Reorg in progress
    {
        WalletSyncStatus status;
        status.is_reorg_in_progress = true;
        status.last_reorg_depth = 5;

        assert_true(status.GetStatusDescription() == "Reorganization in progress (depth: 5)",
                    "Reorg status description should include depth");
    }

    // Test 6: Wallet scan lagging
    {
        WalletSyncStatus status;
        status.phase = SyncPhase::STEADY_STATE;
        status.headers_synced = 100;
        status.headers_total = 100;
        status.blocks_synced = 100;
        status.blocks_total = 100;
        status.wallet_scan_height = 50;
        status.chain_height = 100;

        assert_true(status.GetStatusDescription() == "Scanning wallet",
                    "Wallet scan status should be shown when lagging");
    }

    std::cout << "✅ test_w2_1_6_status_descriptions PASSED" << std::endl;
}

// ============================================================================
// W.2.1.7: Aggregator Integration (Basic)
// ============================================================================

void test_w2_1_7_aggregator_without_components() {
    std::cout << "Running test_w2_1_7_aggregator_without_components..." << std::endl;

    // Test graceful degradation when no components provided
    auto status = WalletSyncStatusAggregator::CreateFromComponents(
        nullptr,  // No ChainDB
        nullptr,  // No scheduler
        nullptr,  // No header sync
        nullptr   // No wallet
    );

    // Should create valid status with defaults
    assert_true(status.IsValid(),
                "Aggregator should create valid status even without components");
    assert_true(status.phase == SyncPhase::STEADY_STATE,
                "Default phase should be STEADY_STATE");
    assert_eq(status.chain_height, 0,
              "Chain height should be 0 without ChainDB");
    assert_eq(status.overall_progress, 0.0, 0.001,
              "Progress should be 0 without components");

    std::cout << "✅ test_w2_1_7_aggregator_without_components PASSED" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Phase W.2.1: Wallet Sync Status Tests" << std::endl;
    std::cout << "========================================\n" << std::endl;

    test_w2_1_1_default_construction();
    test_w2_1_2_component_progress();
    test_w2_1_3_phase_aware_progress();
    test_w2_1_4_no_false_100_percent();
    test_w2_1_5_validation_invariants();
    test_w2_1_6_status_descriptions();
    test_w2_1_7_aggregator_without_components();

    std::cout << "\n========================================" << std::endl;
    std::cout << "✅ All W.2.1 Tests PASSED (7/7)" << std::endl;
    std::cout << "   Foundation Tests (7/7)" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
