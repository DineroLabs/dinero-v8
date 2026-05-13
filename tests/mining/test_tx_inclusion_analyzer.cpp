// SPDX-License-Identifier: MIT
// Phase W.4.1: Transaction Inclusion Analyzer Tests

#include "mining/tx_inclusion_analyzer.h"
#include "rpc/ergonomics_rpc_handlers.h"
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

#define ASSERT_EQ(a, b, msg) \
    if ((a) != (b)) { \
        std::cerr << "❌ ASSERTION FAILED: " << msg << std::endl; \
        std::cerr << "   Expected: " << (b) << std::endl; \
        std::cerr << "   Got: " << (a) << std::endl; \
        std::cerr << "   File: " << __FILE__ << ":" << __LINE__ << std::endl; \
        return false; \
    }

#define ASSERT_APPROX(a, b, epsilon, msg) \
    if (std::abs((a) - (b)) > (epsilon)) { \
        std::cerr << "❌ ASSERTION FAILED: " << msg << std::endl; \
        std::cerr << "   Expected: " << (b) << " ± " << (epsilon) << std::endl; \
        std::cerr << "   Got: " << (a) << std::endl; \
        std::cerr << "   File: " << __FILE__ << ":" << __LINE__ << std::endl; \
        return false; \
    }

// ============================================================================
// Test 1: Enum String Conversion
// ============================================================================

bool test_w4_1_enum_conversion() {
    std::cout << "\n[Test 1] W.4.1: Enum string conversion" << std::endl;

    // InclusionState
    ASSERT_EQ(InclusionStateToString(InclusionState::LIKELY), std::string("likely"),
             "LIKELY should convert to 'likely'");
    ASSERT_EQ(InclusionStateToString(InclusionState::POSSIBLE), std::string("possible"),
             "POSSIBLE should convert to 'possible'");
    ASSERT_EQ(InclusionStateToString(InclusionState::STALLED), std::string("stalled"),
             "STALLED should convert to 'stalled'");
    ASSERT_EQ(InclusionStateToString(InclusionState::BLOCKED), std::string("blocked"),
             "BLOCKED should convert to 'blocked'");

    // InclusionReason
    ASSERT_EQ(InclusionReasonToString(InclusionReason::NONE), std::string("none"),
             "NONE should convert to 'none'");
    ASSERT_EQ(InclusionReasonToString(InclusionReason::LOW_FEERATE), std::string("low_feerate"),
             "LOW_FEERATE should convert to 'low_feerate'");
    ASSERT_EQ(InclusionReasonToString(InclusionReason::NODE_NOT_READY), std::string("node_not_ready"),
             "NODE_NOT_READY should convert to 'node_not_ready'");

    std::cout << "✅ Test 1 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 2: Determine Inclusion State
// ============================================================================

bool test_w4_1_determine_state() {
    std::cout << "\n[Test 2] W.4.1: Determine inclusion state" << std::endl;

    TxInclusionAnalyzer analyzer;

    // Test: High feerate (1.5x cutoff) → LIKELY
    InclusionState state1 = analyzer.DetermineState(150, 100);
    ASSERT_EQ(state1, InclusionState::LIKELY,
             "1.5x cutoff should be LIKELY");

    // Test: Near cutoff (1.1x) → POSSIBLE
    InclusionState state2 = analyzer.DetermineState(110, 100);
    ASSERT_EQ(state2, InclusionState::POSSIBLE,
             "1.1x cutoff should be POSSIBLE");

    // Test: Below cutoff (0.9x) → STALLED
    InclusionState state3 = analyzer.DetermineState(90, 100);
    ASSERT_EQ(state3, InclusionState::STALLED,
             "0.9x cutoff should be STALLED");

    // Test: Very low (0.5x) → BLOCKED
    InclusionState state4 = analyzer.DetermineState(50, 100);
    ASSERT_EQ(state4, InclusionState::BLOCKED,
             "0.5x cutoff should be BLOCKED");

    // Test: No cutoff (empty mempool) → LIKELY
    InclusionState state5 = analyzer.DetermineState(10, 0);
    ASSERT_EQ(state5, InclusionState::LIKELY,
             "No cutoff should be LIKELY");

    std::cout << "✅ Test 2 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 3: Estimate Inclusion Probability
// ============================================================================

bool test_w4_1_estimate_probability() {
    std::cout << "\n[Test 3] W.4.1: Estimate inclusion probability" << std::endl;

    TxInclusionAnalyzer analyzer;

    // Test: High feerate → high probability
    double prob1 = analyzer.EstimateInclusionProbability(150, 100);
    ASSERT_APPROX(prob1, 0.95, 0.05,
                 "1.5x cutoff should have ~95% probability");

    // Test: At cutoff → moderate probability
    double prob2 = analyzer.EstimateInclusionProbability(100, 100);
    ASSERT_APPROX(prob2, 0.50, 0.15,
                 "1.0x cutoff should have ~50% probability");

    // Test: Low feerate → low probability
    double prob3 = analyzer.EstimateInclusionProbability(50, 100);
    ASSERT_TRUE(prob3 < 0.25,
               "0.5x cutoff should have low probability");

    // Test: No cutoff → 100% probability
    double prob4 = analyzer.EstimateInclusionProbability(10, 0);
    ASSERT_EQ(prob4, 1.0,
             "No cutoff should have 100% probability");

    std::cout << "✅ Test 3 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 4: Suggest Fee Bump
// ============================================================================

bool test_w4_1_suggest_fee_bump() {
    std::cout << "\n[Test 4] W.4.1: Suggest fee bump" << std::endl;

    TxInclusionAnalyzer analyzer;

    // Test: Suggest 1.2x cutoff
    uint64_t bump1 = analyzer.SuggestFeeBump(50, 100);
    ASSERT_EQ(bump1, 120,
             "Should suggest 1.2x cutoff (120 sat/vB)");

    // Test: Ensure bump is higher than current
    uint64_t bump2 = analyzer.SuggestFeeBump(150, 100);
    ASSERT_TRUE(bump2 > 150,
               "Suggested bump should be higher than current feerate");

    // Test: No cutoff
    uint64_t bump3 = analyzer.SuggestFeeBump(10, 0);
    ASSERT_EQ(bump3, 11,
             "No cutoff should suggest minimal bump");

    std::cout << "✅ Test 4 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 5: Node Readiness Check
// ============================================================================

bool test_w4_1_node_readiness() {
    std::cout << "\n[Test 5] W.4.1: Node readiness check" << std::endl;

    TxInclusionAnalyzer analyzer;

    // Test: No health info → assume ready
    bool ready1 = analyzer.IsNodeReady(nullptr);
    ASSERT_TRUE(ready1, "Should assume ready if no health info");

    // Test: RED grade → not ready
    NodeHealth health_red;
    health_red.overall_grade = NodeHealthGrade::RED;
    bool ready2 = analyzer.IsNodeReady(&health_red);
    ASSERT_TRUE(!ready2, "RED grade should be not ready");

    // Test: GREEN grade → ready
    NodeHealth health_green;
    health_green.overall_grade = NodeHealthGrade::GREEN;
    SubsystemHealth sync_green;
    sync_green.name = "sync";
    sync_green.grade = NodeHealthGrade::GREEN;
    health_green.subsystems.push_back(sync_green);
    bool ready3 = analyzer.IsNodeReady(&health_green);
    ASSERT_TRUE(ready3, "GREEN grade should be ready");

    std::cout << "✅ Test 5 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 6: Generate Explanation
// ============================================================================

bool test_w4_1_generate_explanation() {
    std::cout << "\n[Test 6] W.4.1: Generate explanation" << std::endl;

    TxInclusionAnalyzer analyzer;

    // Test: LIKELY state
    TxInclusionStatus status1;
    status1.state = InclusionState::LIKELY;
    status1.effective_feerate = 150;
    status1.cutoff_feerate = 100;
    std::string explanation1 = analyzer.GenerateExplanation(status1);
    ASSERT_TRUE(explanation1.find("likely") != std::string::npos,
               "LIKELY explanation should mention 'likely'");

    // Test: STALLED state with suggestion
    TxInclusionStatus status2;
    status2.state = InclusionState::STALLED;
    status2.effective_feerate = 50;
    status2.cutoff_feerate = 100;
    status2.suggested_bump_feerate = 120;
    std::string explanation2 = analyzer.GenerateExplanation(status2);
    ASSERT_TRUE(explanation2.find("below") != std::string::npos,
               "STALLED explanation should mention 'below'");
    ASSERT_TRUE(explanation2.find("120") != std::string::npos,
               "STALLED explanation should include suggested feerate");

    // Test: BLOCKED state (not in mempool)
    TxInclusionStatus status3;
    status3.state = InclusionState::BLOCKED;
    status3.primary_reason = InclusionReason::NOT_IN_MEMPOOL;
    std::string explanation3 = analyzer.GenerateExplanation(status3);
    ASSERT_TRUE(explanation3.find("not found") != std::string::npos ||
                explanation3.find("not in mempool") != std::string::npos,
               "NOT_IN_MEMPOOL explanation should mention missing tx");

    std::cout << "✅ Test 6 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 7: Full Analysis (No Components)
// ============================================================================

bool test_w4_1_full_analysis_no_components() {
    std::cout << "\n[Test 7] W.4.1: Full analysis without components" << std::endl;

    TxInclusionAnalyzer analyzer;
    uint256 txid;  // Dummy txid

    // Test: No mempool → BLOCKED
    TxInclusionStatus status = analyzer.Analyze(txid, nullptr, nullptr, nullptr);
    ASSERT_EQ(status.state, InclusionState::BLOCKED,
             "No mempool should result in BLOCKED");
    ASSERT_EQ(status.primary_reason, InclusionReason::NOT_IN_MEMPOOL,
             "Should report NOT_IN_MEMPOOL");
    ASSERT_TRUE(!status.explanation.empty(),
               "Should have explanation");
    ASSERT_TRUE(status.timestamp_ms > 0,
               "Should have timestamp");

    std::cout << "✅ Test 7 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 8: Full Analysis (Node Not Ready)
// ============================================================================

bool test_w4_1_full_analysis_node_not_ready() {
    std::cout << "\n[Test 8] W.4.1: Full analysis with node not ready" << std::endl;

    TxInclusionAnalyzer analyzer;
    uint256 txid;

    // Create unhealthy node
    NodeHealth health;
    health.overall_grade = NodeHealthGrade::RED;

    // Test: Node not ready → BLOCKED
    TxInclusionStatus status = analyzer.Analyze(txid, nullptr, nullptr, &health);
    ASSERT_EQ(status.state, InclusionState::BLOCKED,
             "Unhealthy node should result in BLOCKED");
    ASSERT_EQ(status.primary_reason, InclusionReason::NODE_NOT_READY,
             "Should report NODE_NOT_READY");
    ASSERT_TRUE(status.estimated_inclusion_prob == 0.0,
               "Probability should be 0 when node not ready");

    std::cout << "✅ Test 8 passed" << std::endl;
    return true;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Phase W.4.1: TxInclusionAnalyzer Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    bool all_passed = true;

    // Run all tests
    all_passed &= test_w4_1_enum_conversion();
    all_passed &= test_w4_1_determine_state();
    all_passed &= test_w4_1_estimate_probability();
    all_passed &= test_w4_1_suggest_fee_bump();
    all_passed &= test_w4_1_node_readiness();
    all_passed &= test_w4_1_generate_explanation();
    all_passed &= test_w4_1_full_analysis_no_components();
    all_passed &= test_w4_1_full_analysis_node_not_ready();

    std::cout << "\n========================================" << std::endl;
    if (all_passed) {
        std::cout << "✅ ALL TESTS PASSED" << std::endl;
        return 0;
    } else {
        std::cout << "❌ SOME TESTS FAILED" << std::endl;
        return 1;
    }
}
