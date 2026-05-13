/**
 * Phase W.1: Block Assembly Intelligence Tests
 *
 * Tests for context-aware, network-informed mining optimization
 */

#include "mining/block_assembly_context.h"
#include "mining/transaction_scorer.h"
#include "mining/template_delta_tracker.h"
#include "p2p/block_download_scheduler.h"  // For SyncPhase enum
#include "primitives/uint256.h"
#include <cassert>
#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>

using namespace dinero;

// ============================================================================
// Test Utilities
// ============================================================================

void assert_eq(double a, double b, double epsilon, const std::string& msg) {
    if (std::abs(a - b) > epsilon) {
        std::cerr << "FAIL: " << msg << " (expected " << b << ", got " << a << ")" << std::endl;
        std::exit(1);
    }
}

void assert_true(bool condition, const std::string& msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << std::endl;
        std::exit(1);
    }
}

// ============================================================================
// W.1.1: BlockAssemblyContext Foundation Tests
// ============================================================================

void test_w1_1_1_default_context() {
    std::cout << "Running test_w1_1_1_default_context..." << std::endl;

    BlockAssemblyContext ctx;

    // Default values should be safe (synced, empty mempool, neutral compact)
    assert_true(ctx.sync_phase == SyncPhase::STEADY_STATE,
                "Default context should assume STEADY_STATE");
    assert_eq(ctx.mempool_pressure, 0.0, 0.01,
              "Default mempool pressure should be 0.0");
    assert_eq(ctx.compact_success_rate, 0.5, 0.01,
              "Default compact success rate should be 0.5");
    assert_true(ctx.last_template_time_ms == 0,
                "Default last template time should be 0");
    assert_true(ctx.mempool_delta_count == 0,
                "Default mempool delta should be 0");

    std::cout << "✅ test_w1_1_1_default_context PASSED" << std::endl;
}

void test_w1_1_2_refresh_urgency() {
    std::cout << "Running test_w1_1_2_refresh_urgency..." << std::endl;

    // Test 1: Fresh template (0 urgency)
    {
        BlockAssemblyContext ctx;
        ctx.last_template_time_ms = 1000;
        ctx.mempool_delta_count = 0;
        ctx.mempool_pressure = 0.0;

        double urgency = ctx.GetRefreshUrgency(1100);  // 100ms old
        assert_true(urgency < 0.1, "Fresh template should have low urgency");
    }

    // Test 2: Old template (high urgency)
    {
        BlockAssemblyContext ctx;
        ctx.last_template_time_ms = 1000;
        ctx.mempool_delta_count = 50;  // Many changes
        ctx.mempool_pressure = 5.0;     // High pressure

        double urgency = ctx.GetRefreshUrgency(61000);  // 60 seconds old
        assert_true(urgency > 0.8, "Old template with high pressure should have high urgency");
    }

    // Test 3: Moderate template age (moderate urgency)
    {
        BlockAssemblyContext ctx;
        ctx.last_template_time_ms = 1000;
        ctx.mempool_delta_count = 20;
        ctx.mempool_pressure = 2.0;

        double urgency = ctx.GetRefreshUrgency(31000);  // 30 seconds old
        assert_true(urgency > 0.3 && urgency < 0.7,
                    "Moderate conditions should have moderate urgency");
    }

    std::cout << "✅ test_w1_1_2_refresh_urgency PASSED" << std::endl;
}

void test_w1_1_3_compact_friendly_bias() {
    std::cout << "Running test_w1_1_3_compact_friendly_bias..." << std::endl;

    // Test 1: High compact success (>90%) → moderate bias
    {
        BlockAssemblyContext ctx;
        ctx.compact_success_rate = 0.95;
        double bias = ctx.GetCompactFriendlyBias();
        assert_eq(bias, 0.3, 0.01, "High success should give 0.3 bias");
    }

    // Test 2: Medium compact success (70-90%) → low bias
    {
        BlockAssemblyContext ctx;
        ctx.compact_success_rate = 0.75;
        double bias = ctx.GetCompactFriendlyBias();
        assert_eq(bias, 0.2, 0.01, "Medium success should give 0.2 bias");
    }

    // Test 3: Low compact success (<70%) → no bias
    {
        BlockAssemblyContext ctx;
        ctx.compact_success_rate = 0.5;
        double bias = ctx.GetCompactFriendlyBias();
        assert_eq(bias, 0.0, 0.01, "Low success should give 0.0 bias");
    }

    std::cout << "✅ test_w1_1_3_compact_friendly_bias PASSED" << std::endl;
}

void test_w1_1_4_should_optimize_for_fees() {
    std::cout << "Running test_w1_1_4_should_optimize_for_fees..." << std::endl;

    // Test 1: Synced + high backlog → optimize for fees
    {
        BlockAssemblyContext ctx;
        ctx.sync_phase = SyncPhase::STEADY_STATE;
        ctx.mempool_pressure = 3.0;  // 3x block capacity
        assert_true(ctx.ShouldOptimizeForFees(),
                    "Synced with backlog should optimize for fees");
    }

    // Test 2: Not synced → don't optimize for fees
    {
        BlockAssemblyContext ctx;
        ctx.sync_phase = SyncPhase::IBD;
        ctx.mempool_pressure = 3.0;
        assert_true(!ctx.ShouldOptimizeForFees(),
                    "IBD phase should not optimize for fees");
    }

    // Test 3: Synced but low backlog → don't optimize aggressively
    {
        BlockAssemblyContext ctx;
        ctx.sync_phase = SyncPhase::STEADY_STATE;
        ctx.mempool_pressure = 1.0;  // Only 1 block worth
        assert_true(!ctx.ShouldOptimizeForFees(),
                    "Low mempool pressure should not trigger aggressive optimization");
    }

    std::cout << "✅ test_w1_1_4_should_optimize_for_fees PASSED" << std::endl;
}

void test_w1_1_5_should_refresh_template() {
    std::cout << "Running test_w1_1_5_should_refresh_template..." << std::endl;

    // Test 1: Fresh template, no changes → don't refresh
    {
        BlockAssemblyContext ctx;
        ctx.last_template_time_ms = 1000;
        ctx.mempool_delta_count = 5;  // Few changes
        ctx.mempool_pressure = 1.0;

        bool should_refresh = ctx.ShouldRefreshTemplate(10000);  // 9 seconds old
        assert_true(!should_refresh, "Fresh template with few changes should not refresh");
    }

    // Test 2: Old template (>30s) → refresh
    {
        BlockAssemblyContext ctx;
        ctx.last_template_time_ms = 1000;
        ctx.mempool_delta_count = 5;
        ctx.mempool_pressure = 1.0;

        bool should_refresh = ctx.ShouldRefreshTemplate(35000);  // 34 seconds old
        assert_true(should_refresh, "Old template (>30s) should refresh");
    }

    // Test 3: Many mempool changes (>10 txs) → refresh
    {
        BlockAssemblyContext ctx;
        ctx.last_template_time_ms = 1000;
        ctx.mempool_delta_count = 15;  // 15 changes
        ctx.mempool_pressure = 1.0;

        bool should_refresh = ctx.ShouldRefreshTemplate(10000);  // 9 seconds old
        assert_true(should_refresh, "Many mempool changes should trigger refresh");
    }

    // Test 4: High pressure + recent template → refresh
    {
        BlockAssemblyContext ctx;
        ctx.last_template_time_ms = 1000;
        ctx.mempool_delta_count = 5;
        ctx.mempool_pressure = 3.0;  // High pressure

        bool should_refresh = ctx.ShouldRefreshTemplate(20000);  // 19 seconds old
        assert_true(should_refresh,
                    "High mempool pressure should trigger frequent refresh (>15s)");
    }

    std::cout << "✅ test_w1_1_5_should_refresh_template PASSED" << std::endl;
}

// ============================================================================
// W.1.2: Transaction Scoring Tests
// ============================================================================

void test_w1_2_1_fee_histogram() {
    std::cout << "Running test_w1_2_1_fee_histogram..." << std::endl;

    FeeHistogram histogram;

    // Add transactions with varying fee rates
    histogram.AddTransaction(5);   // Low fee
    histogram.AddTransaction(10);  // Medium fee
    histogram.AddTransaction(50);  // High fee
    histogram.AddTransaction(100); // Very high fee
    histogram.AddTransaction(10);  // Medium fee (duplicate)

    assert_true(histogram.GetTotalCount() == 5, "Histogram should have 5 transactions");

    // Test percentiles
    double p5 = histogram.GetPercentile(5);    // Lowest fee
    double p10 = histogram.GetPercentile(10);  // Medium fee (2 of them)
    double p50 = histogram.GetPercentile(50);  // High fee
    double p100 = histogram.GetPercentile(100); // Highest fee

    assert_true(p5 < p10, "Lower fee should have lower percentile");
    assert_true(p10 < p50, "Medium fee should be below high fee");
    assert_true(p50 < p100, "High fee should be below very high fee");
    assert_true(p100 >= 0.8, "Highest fee should be in top percentile");

    // Test removal
    histogram.RemoveTransaction(10);
    assert_true(histogram.GetTotalCount() == 4, "Histogram should have 4 transactions after removal");

    std::cout << "✅ test_w1_2_1_fee_histogram PASSED" << std::endl;
}

void test_w1_2_2_mining_probability() {
    std::cout << "Running test_w1_2_2_mining_probability..." << std::endl;

    BlockAssemblyContext context;
    context.sync_phase = SyncPhase::STEADY_STATE;
    context.compact_success_rate = 0.5;

    TransactionScorer scorer(context);

    // Create test transactions with different ages
    uint256 txid1 = uint256::FromHexUnsafe(std::string(64, '1'));
    uint256 txid2 = uint256::FromHexUnsafe(std::string(64, '2'));
    uint256 txid3 = uint256::FromHexUnsafe(std::string(64, '3'));

    uint64_t current_time = 300000;  // 5 minutes in ms

    // Transaction 1: Just entered (0 seconds old)
    auto scored1 = scorer.ScoreTransaction(txid1, 10, current_time, current_time);

    // Transaction 2: 1 minute old (60 seconds)
    auto scored2 = scorer.ScoreTransaction(txid2, 10, current_time - 60000, current_time);

    // Transaction 3: 5 minutes old (300 seconds, fully propagated)
    auto scored3 = scorer.ScoreTransaction(txid3, 10, current_time - 300000, current_time);

    // Mining probability should increase with age
    assert_true(scored1.mining_probability < scored2.mining_probability,
                "Older tx should have higher mining probability");
    assert_true(scored2.mining_probability < scored3.mining_probability,
                "Much older tx should have even higher mining probability");

    // Fully propagated (age_factor=1.0) + median fee (fee_percentile=0.5) = 0.75
    // Formula: 0.5 * age_factor + 0.5 * fee_percentile = 0.5 * 1.0 + 0.5 * 0.5 = 0.75
    assert_true(scored3.mining_probability >= 0.7,
                "Fully propagated tx should have high mining probability (>=0.7)");

    std::cout << "✅ test_w1_2_2_mining_probability PASSED" << std::endl;
}

void test_w1_2_3_compact_reconstructability() {
    std::cout << "Running test_w1_2_3_compact_reconstructability..." << std::endl;

    // Test 1: Low compact success (no bias)
    {
        BlockAssemblyContext context;
        context.compact_success_rate = 0.5;  // Low success = 0.0 bias

        TransactionScorer scorer(context);

        uint256 txid = uint256::FromHexUnsafe(std::string(64, '1'));
        uint64_t current_time = 300000;

        // Fresh tx (0 seconds old): base = 0.5 (minimum)
        auto scored_fresh = scorer.ScoreTransaction(txid, 10, current_time, current_time);

        // Old tx (5 minutes, fully propagated): base = 0.5 + 0.5 = 1.0
        auto scored_old = scorer.ScoreTransaction(txid, 10, current_time - 300000, current_time);

        assert_true(scored_fresh.compact_recon < scored_old.compact_recon,
                    "Older tx should have higher reconstructability");
        assert_eq(scored_fresh.compact_recon, 0.5, 0.01,
                  "Fresh tx should have 0.5 reconstructability (minimum)");
        assert_eq(scored_old.compact_recon, 1.0, 0.01,
                  "Fully propagated tx should have 1.0 reconstructability (no bias)");
    }

    // Test 2: High compact success (with bias)
    {
        BlockAssemblyContext context;
        context.compact_success_rate = 0.95;  // High success = 0.3 bias

        TransactionScorer scorer(context);

        uint256 txid = uint256::FromHexUnsafe(std::string(64, '1'));
        uint64_t current_time = 300000;

        // Old tx (5 minutes, fully propagated)
        // base = 1.0, bonus = 0.3 * (1.0 - 0.5) = 0.15, total = 1.15
        auto scored_old = scorer.ScoreTransaction(txid, 10, current_time - 300000, current_time);

        // With bias, reconstructability should exceed 1.0
        assert_true(scored_old.compact_recon > 1.0,
                    "With high compact success, old tx should get bonus (>1.0)");
        assert_eq(scored_old.compact_recon, 1.15, 0.01,
                  "Fully propagated tx with 0.3 bias should be 1.15");
    }

    std::cout << "✅ test_w1_2_3_compact_reconstructability PASSED" << std::endl;
}

void test_w1_2_4_fee_dominance() {
    std::cout << "Running test_w1_2_4_fee_dominance..." << std::endl;

    BlockAssemblyContext context;
    context.sync_phase = SyncPhase::STEADY_STATE;
    context.compact_success_rate = 0.5;

    TransactionScorer scorer(context);

    uint256 high_fee_txid = uint256::FromHexUnsafe(std::string(64, '1'));
    uint256 low_fee_txid = uint256::FromHexUnsafe(std::string(64, '2'));

    uint64_t current_time = 300000;

    // High fee but fresh (just entered)
    auto high_fee = scorer.ScoreTransaction(high_fee_txid, 100, current_time, current_time);

    // Low fee but old (fully propagated)
    auto low_fee = scorer.ScoreTransaction(low_fee_txid, 10, current_time - 300000, current_time);

    // High fee should ALWAYS beat low fee (fee dominance)
    assert_true(high_fee.score > low_fee.score,
                "High fee (100 sat/byte) should beat low fee (10 sat/byte) even when fresh");

    std::cout << "✅ test_w1_2_4_fee_dominance PASSED" << std::endl;
}

void test_w1_2_5_scoring_with_histogram() {
    std::cout << "Running test_w1_2_5_scoring_with_histogram..." << std::endl;

    // Build fee histogram
    FeeHistogram histogram;
    histogram.AddTransaction(1);   // Very low
    histogram.AddTransaction(5);   // Low
    histogram.AddTransaction(10);  // Medium
    histogram.AddTransaction(50);  // High
    histogram.AddTransaction(100); // Very high

    BlockAssemblyContext context;
    context.sync_phase = SyncPhase::STEADY_STATE;
    context.compact_success_rate = 0.8;  // Medium compact success

    TransactionScorer scorer(context);
    scorer.SetFeeHistogram(&histogram);

    // Score transactions at different fee rates
    uint256 txid1 = uint256::FromHexUnsafe(std::string(64, '1'));
    uint256 txid2 = uint256::FromHexUnsafe(std::string(64, '2'));

    uint64_t current_time = 60000;  // 1 minute

    // Both transactions are 30 seconds old, but different fees
    auto low_fee = scorer.ScoreTransaction(txid1, 5, current_time - 30000, current_time);
    auto high_fee = scorer.ScoreTransaction(txid2, 50, current_time - 30000, current_time);

    // High fee should have higher score
    assert_true(high_fee.score > low_fee.score,
                "Higher fee rate should produce higher score");

    // High fee should have higher mining probability (better percentile)
    assert_true(high_fee.mining_probability > low_fee.mining_probability,
                "Higher fee should have higher mining probability");

    std::cout << "✅ test_w1_2_5_scoring_with_histogram PASSED" << std::endl;
}

void test_w1_2_6_batch_scoring() {
    std::cout << "Running test_w1_2_6_batch_scoring..." << std::endl;

    BlockAssemblyContext context;
    context.sync_phase = SyncPhase::STEADY_STATE;
    context.compact_success_rate = 0.5;

    TransactionScorer scorer(context);

    // Create batch of transactions
    std::vector<std::tuple<uint256, uint64_t, uint64_t>> transactions;

    uint64_t current_time = 300000;

    // High fee, fresh
    transactions.push_back({
        uint256::FromHexUnsafe(std::string(64, '1')),
        100,  // 100 sat/byte
        current_time
    });

    // Medium fee, old
    transactions.push_back({
        uint256::FromHexUnsafe(std::string(64, '2')),
        50,   // 50 sat/byte
        current_time - 300000  // 5 minutes old
    });

    // Low fee, old
    transactions.push_back({
        uint256::FromHexUnsafe(std::string(64, '3')),
        10,   // 10 sat/byte
        current_time - 300000  // 5 minutes old
    });

    // Score all transactions
    auto scored = scorer.ScoreTransactions(transactions, current_time);

    // Should be sorted by score (descending)
    assert_true(scored.size() == 3, "Should have 3 scored transactions");

    // Expected ranking: Medium-fee old (37.5) > High-fee fresh (12.5) > Low-fee old (7.5)
    // This demonstrates the trade-off between fee rate and propagation:
    // - Medium fee (50) + fully propagated (×0.75 mining prob, ×1.0 compact) = 37.5
    // - High fee (100) + fresh (×0.25 mining prob, ×0.5 compact) = 12.5
    // Well-propagated transactions get prioritized for compact block efficiency
    assert_true(scored[0].fee_rate == 50, "Highest score should be medium-fee well-propagated (50)");
    assert_true(scored[1].fee_rate == 100, "Second should be high-fee fresh (100)");
    assert_true(scored[2].fee_rate == 10, "Lowest score should be low-fee old (10)");

    // Scores should be descending
    assert_true(scored[0].score > scored[1].score, "Scores should be descending");
    assert_true(scored[1].score > scored[2].score, "Scores should be descending");

    std::cout << "✅ test_w1_2_6_batch_scoring PASSED" << std::endl;
}

// ============================================================================
// W.1.4: Incremental Template Refresh Tests
// ============================================================================

void test_w1_4_1_delta_tracking() {
    std::cout << "Running test_w1_4_1_delta_tracking..." << std::endl;

    TemplateDeltaTracker tracker;

    // Create initial template
    std::unordered_set<uint256> template_txs;
    template_txs.insert(uint256::FromHexUnsafe(std::string(64, '1')));
    template_txs.insert(uint256::FromHexUnsafe(std::string(64, '2')));
    template_txs.insert(uint256::FromHexUnsafe(std::string(64, '3')));

    uint64_t snapshot_time = 1000;
    tracker.SnapshotMempool(template_txs, snapshot_time);

    assert_true(tracker.GetDeltaCount() == 0, "Initial delta should be 0");
    assert_true(tracker.GetLastSnapshotTime() == snapshot_time, "Snapshot time should be recorded");

    // Add new transaction
    uint256 new_tx = uint256::FromHexUnsafe(std::string(64, '4'));
    tracker.OnTransactionAdded(new_tx, 50, 1100);

    assert_true(tracker.GetDeltaCount() == 1, "Delta should be 1 after addition");
    assert_true(tracker.GetAddedTransactions().count(new_tx) > 0, "New tx should be in added set");

    // Remove template transaction
    uint256 removed_tx = uint256::FromHexUnsafe(std::string(64, '1'));
    tracker.OnTransactionRemoved(removed_tx);

    assert_true(tracker.GetDeltaCount() == 2, "Delta should be 2 after removal");
    assert_true(tracker.GetRemovedTransactions().count(removed_tx) > 0, "Removed tx should be in removed set");

    std::cout << "✅ test_w1_4_1_delta_tracking PASSED" << std::endl;
}

void test_w1_4_2_refresh_decision() {
    std::cout << "Running test_w1_4_2_refresh_decision..." << std::endl;

    TemplateDeltaTracker tracker;

    std::unordered_set<uint256> template_txs;
    for (int i = 0; i < 100; ++i) {
        template_txs.insert(uint256::FromHexUnsafe(std::string(64, '0' + (i % 10))));
    }

    uint64_t snapshot_time = 1000;
    tracker.SnapshotMempool(template_txs, snapshot_time);

    // Test 1: Fresh template, no deltas → no refresh
    assert_true(!tracker.ShouldRefreshTemplate(2000, 10, 30000),
                "Fresh template with no deltas should not refresh");

    // Test 2: Add transactions above delta threshold → refresh
    for (int i = 0; i < 15; ++i) {
        uint256 txid = uint256::FromHexUnsafe(std::string(63, 'a') + static_cast<char>('0' + i));
        tracker.OnTransactionAdded(txid, 10, 2000 + i);
    }

    assert_true(tracker.ShouldRefreshTemplate(3000, 10, 30000),
                "Delta count (15) above threshold (10) should trigger refresh");

    // Test 3: Template age exceeds maximum → refresh
    tracker.Clear();
    tracker.SnapshotMempool(template_txs, 1000);

    assert_true(tracker.ShouldRefreshTemplate(35000, 10, 30000),
                "Template age (34s) exceeding max (30s) should trigger refresh");

    // Test 4: Significant template transactions removed → refresh
    tracker.Clear();
    tracker.SnapshotMempool(template_txs, 1000);

    // Remove 15% of template transactions
    int removal_count = 0;
    for (const auto& txid : template_txs) {
        tracker.OnTransactionRemoved(txid);
        if (++removal_count >= 15) break;
    }

    assert_true(tracker.ShouldRefreshTemplate(2000, 10, 30000),
                "Removing 15% of template txs should trigger refresh");

    std::cout << "✅ test_w1_4_2_refresh_decision PASSED" << std::endl;
}

void test_w1_4_3_high_fee_detection() {
    std::cout << "Running test_w1_4_3_high_fee_detection..." << std::endl;

    TemplateDeltaTracker tracker;

    std::unordered_set<uint256> template_txs;
    template_txs.insert(uint256::FromHexUnsafe(std::string(64, '1')));

    tracker.SnapshotMempool(template_txs, 1000);

    // Add low-fee transaction
    tracker.OnTransactionAdded(uint256::FromHexUnsafe(std::string(64, '2')), 5, 1100);

    // Add high-fee transaction
    tracker.OnTransactionAdded(uint256::FromHexUnsafe(std::string(64, '3')), 100, 1200);

    assert_true(tracker.GetHighestAddedFeeRate() == 100,
                "Highest added fee rate should be 100");

    // Test urgency with high-fee addition
    // With 2 txs, 1 second age: delta=0.016, age=0.005, total=0.021
    // With 10× median fee multiplier (2.0): 0.021 × 2.0 = 0.042
    double urgency = tracker.GetRefreshUrgency(2000, 10);  // median_fee = 10
    assert_true(urgency > 0.04 && urgency < 0.05, "High-fee addition should increase urgency (expected ~0.042)");

    std::cout << "✅ test_w1_4_3_high_fee_detection PASSED" << std::endl;
}

void test_w1_4_4_refresh_urgency() {
    std::cout << "Running test_w1_4_4_refresh_urgency..." << std::endl;

    TemplateDeltaTracker tracker;

    std::unordered_set<uint256> template_txs;
    for (int i = 0; i < 100; ++i) {
        template_txs.insert(uint256::FromHexUnsafe(std::string(63, '0') + static_cast<char>('0' + (i % 10))));
    }

    tracker.SnapshotMempool(template_txs, 1000);

    // Test 1: Low urgency (fresh template, few changes)
    {
        tracker.OnTransactionAdded(uint256::FromHexUnsafe(std::string(64, 'a')), 10, 1100);
        double urgency = tracker.GetRefreshUrgency(2000, 10);
        assert_true(urgency < 0.3, "Fresh template with 1 tx should have low urgency");
    }

    // Test 2: High urgency (old template, many changes, high fees)
    {
        tracker.Clear();
        tracker.SnapshotMempool(template_txs, 1000);

        // Add many high-fee transactions (create unique IDs)
        for (int i = 0; i < 50; ++i) {
            // Create unique hex string by converting i to hex
            std::stringstream ss;
            ss << std::hex << std::setfill('0') << std::setw(2) << i;
            std::string suffix = ss.str();
            std::string txid_str = std::string(64 - suffix.length(), 'b') + suffix;

            tracker.OnTransactionAdded(
                uint256::FromHexUnsafe(txid_str),
                100,  // High fee
                1000 + i
            );
        }

        double urgency = tracker.GetRefreshUrgency(61000, 10);  // 60 seconds later, median=10
        assert_true(urgency > 1.0, "Old template with many high-fee txs should have high urgency");
    }

    std::cout << "✅ test_w1_4_4_refresh_urgency PASSED" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Phase W.1: Block Assembly Intelligence Tests" << std::endl;
    std::cout << "========================================\n" << std::endl;

    try {
        // W.1.1: BlockAssemblyContext
        std::cout << "\n--- W.1.1: BlockAssemblyContext ---\n" << std::endl;
        test_w1_1_1_default_context();
        test_w1_1_2_refresh_urgency();
        test_w1_1_3_compact_friendly_bias();
        test_w1_1_4_should_optimize_for_fees();
        test_w1_1_5_should_refresh_template();

        // W.1.2: Transaction Scoring
        std::cout << "\n--- W.1.2: Transaction Scoring ---\n" << std::endl;
        test_w1_2_1_fee_histogram();
        test_w1_2_2_mining_probability();
        test_w1_2_3_compact_reconstructability();
        test_w1_2_4_fee_dominance();
        test_w1_2_5_scoring_with_histogram();
        test_w1_2_6_batch_scoring();

        // W.1.4: Incremental Template Refresh
        std::cout << "\n--- W.1.4: Incremental Template Refresh ---\n" << std::endl;
        test_w1_4_1_delta_tracking();
        test_w1_4_2_refresh_decision();
        test_w1_4_3_high_fee_detection();
        test_w1_4_4_refresh_urgency();

        std::cout << "\n========================================" << std::endl;
        std::cout << "✅ All W.1 Tests PASSED (15/15)" << std::endl;
        std::cout << "   W.1.1: BlockAssemblyContext (5/5)" << std::endl;
        std::cout << "   W.1.2: Transaction Scoring (6/6)" << std::endl;
        std::cout << "   W.1.4: Incremental Refresh (4/4)" << std::endl;
        std::cout << "========================================\n" << std::endl;

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
