// SPDX-License-Identifier: MIT
// Phase W.4.3: Fee Bump Recommendation Engine Tests

#include "wallet/fee_bump_engine.h"
#include "wallet/rbf_cpfp_detector.h"
#include "mining/tx_inclusion_analyzer.h"
#include <iostream>
#include <cassert>
#include <cmath>

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

#define ASSERT_GE(a, b, msg) \
    if ((a) < (b)) { \
        std::cerr << "❌ ASSERTION FAILED: " << msg << std::endl; \
        std::cerr << "   Expected: >= " << (b) << std::endl; \
        std::cerr << "   Got: " << (a) << std::endl; \
        std::cerr << "   File: " << __FILE__ << ":" << __LINE__ << std::endl; \
        return false; \
    }

// ============================================================================
// Test 1: Enum String Conversion
// ============================================================================

bool test_w4_3_enum_conversion() {
    std::cout << "\n[Test 1] W.4.3: Enum string conversion" << std::endl;

    ASSERT_EQ(BumpStrategyToString(BumpStrategy::NONE), std::string("none"),
              "NONE should convert to 'none'");
    ASSERT_EQ(BumpStrategyToString(BumpStrategy::RBF), std::string("rbf"),
              "RBF should convert to 'rbf'");
    ASSERT_EQ(BumpStrategyToString(BumpStrategy::CPFP), std::string("cpfp"),
              "CPFP should convert to 'cpfp'");
    ASSERT_EQ(BumpStrategyToString(BumpStrategy::BOTH), std::string("both"),
              "BOTH should convert to 'both'");
    ASSERT_EQ(BumpStrategyToString(BumpStrategy::WAIT), std::string("wait"),
              "WAIT should convert to 'wait'");

    std::cout << "✅ Test 1 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 2: Struct Constructors
// ============================================================================

bool test_w4_3_struct_constructors() {
    std::cout << "\n[Test 2] W.4.3: Struct constructors" << std::endl;

    // RbfRecommendation
    RbfRecommendation rbf;
    ASSERT_FALSE(rbf.viable, "Default viable should be false");
    ASSERT_EQ(rbf.original_fee, 0ULL, "Default original_fee should be 0");
    ASSERT_EQ(rbf.recommended_fee, 0ULL, "Default recommended_fee should be 0");

    // CpfpRecommendation
    CpfpRecommendation cpfp;
    ASSERT_FALSE(cpfp.viable, "Default viable should be false");
    ASSERT_EQ(cpfp.output_amount, 0ULL, "Default output_amount should be 0");
    ASSERT_EQ(cpfp.recommended_child_fee, 0ULL, "Default child_fee should be 0");

    // FeeBumpRecommendation
    FeeBumpRecommendation recommendation;
    ASSERT_EQ(BumpStrategyToString(recommendation.strategy), std::string("wait"),
              "Default strategy should be WAIT");
    ASSERT_EQ(recommendation.current_feerate, 0ULL, "Default feerate should be 0");
    ASSERT_TRUE(recommendation.warnings.empty(), "Default warnings should be empty");

    std::cout << "✅ Test 2 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 3: Fee Estimation
// ============================================================================

bool test_w4_3_fee_estimation() {
    std::cout << "\n[Test 3] W.4.3: Fee estimation" << std::endl;

    FeeBumpEngine engine;

    // Test: Estimate fee for next block (target_blocks=1)
    uint64_t fee1 = engine.EstimateFeeRate(1, nullptr);
    ASSERT_GT(fee1, 0ULL, "Next block fee should be > 0");

    // Test: Estimate fee for 3 blocks
    uint64_t fee3 = engine.EstimateFeeRate(3, nullptr);
    ASSERT_GT(fee3, 0ULL, "3-block fee should be > 0");

    // Test: Next block should have higher fee than 3 blocks
    ASSERT_GT(fee1, fee3, "Next block fee should be higher than 3-block fee");

    // Test: Estimate fee for 10 blocks (lower urgency)
    uint64_t fee10 = engine.EstimateFeeRate(10, nullptr);
    ASSERT_GE(fee3, fee10, "3-block fee should be >= 10-block fee");

    std::cout << "✅ Test 3 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 4: Confirmation Block Estimation
// ============================================================================

bool test_w4_3_confirmation_estimation() {
    std::cout << "\n[Test 4] W.4.3: Confirmation block estimation" << std::endl;

    FeeBumpEngine engine;

    // Test: High fee rate should estimate fewer blocks
    uint32_t blocks_high = engine.EstimateConfirmationBlocks(10, nullptr);
    ASSERT_GT(blocks_high, 0U, "Should estimate at least 1 block");

    // Test: Low fee rate should estimate more blocks
    uint32_t blocks_low = engine.EstimateConfirmationBlocks(1, nullptr);
    ASSERT_GE(blocks_low, blocks_high, "Low fee should take more blocks");

    std::cout << "✅ Test 4 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 5: Cost-Benefit Analysis
// ============================================================================

bool test_w4_3_cost_benefit() {
    std::cout << "\n[Test 5] W.4.3: Cost-benefit analysis" << std::endl;

    FeeBumpEngine engine;

    // Test: Calculate cost-benefit ratio
    double ratio = engine.CalculateCostBenefit(1000, 2000, 5);
    ASSERT_EQ(ratio, 200.0, "Ratio should be 1000/5 = 200");

    // Test: Zero blocks saved should return infinity
    double ratio_zero = engine.CalculateCostBenefit(1000, 2000, 0);
    ASSERT_TRUE(std::isinf(ratio_zero), "Zero blocks saved should return infinity");

    // Test: Cost effectiveness check
    bool effective = engine.IsCostEffective(1000, 2000, 10, 200.0);
    ASSERT_TRUE(effective, "100 sats/block should be cost-effective for max 200");

    bool not_effective = engine.IsCostEffective(1000, 5000, 2, 100.0);
    ASSERT_FALSE(not_effective, "2000 sats/block should not be cost-effective for max 100");

    std::cout << "✅ Test 5 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 6: RBF Recommendation (Not Available)
// ============================================================================

bool test_w4_3_rbf_not_available() {
    std::cout << "\n[Test 6] W.4.3: RBF recommendation (not available)" << std::endl;

    FeeBumpEngine engine;
    uint256 txid;

    // Create rescue strategy with RBF not available
    RescueStrategy strategy;
    strategy.rbf_available = false;

    RbfRecommendation rbf = engine.GenerateRbfRecommendation(txid, strategy, 10, nullptr);

    ASSERT_FALSE(rbf.viable, "RBF should not be viable");
    ASSERT_TRUE(rbf.explanation.find("not available") != std::string::npos,
                "Explanation should mention not available");

    std::cout << "✅ Test 6 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 7: RBF Recommendation (Available)
// ============================================================================

bool test_w4_3_rbf_available() {
    std::cout << "\n[Test 7] W.4.3: RBF recommendation (available)" << std::endl;

    FeeBumpEngine engine;
    uint256 txid;

    // Create rescue strategy with RBF available
    RescueStrategy strategy;
    strategy.rbf_available = true;
    strategy.rbf_details.min_replacement_fee = 1500;

    RbfRecommendation rbf = engine.GenerateRbfRecommendation(txid, strategy, 10, nullptr);

    ASSERT_TRUE(rbf.viable, "RBF should be viable");
    ASSERT_GT(rbf.recommended_fee, 0ULL, "Recommended fee should be > 0");
    ASSERT_GE(rbf.recommended_fee, rbf.min_replacement_fee,
              "Recommended fee should meet minimum");
    ASSERT_GT(rbf.additional_cost, 0ULL, "Additional cost should be > 0");
    ASSERT_TRUE(!rbf.explanation.empty(), "Should have explanation");

    std::cout << "✅ Test 7 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 8: CPFP Recommendation (Not Available)
// ============================================================================

bool test_w4_3_cpfp_not_available() {
    std::cout << "\n[Test 8] W.4.3: CPFP recommendation (not available)" << std::endl;

    FeeBumpEngine engine;
    uint256 txid;

    // Create rescue strategy with CPFP not available
    RescueStrategy strategy;
    strategy.cpfp_available = false;

    CpfpRecommendation cpfp = engine.GenerateCpfpRecommendation(
        txid, strategy, 10, nullptr, nullptr
    );

    ASSERT_FALSE(cpfp.viable, "CPFP should not be viable");
    ASSERT_TRUE(cpfp.explanation.find("not available") != std::string::npos,
                "Explanation should mention not available");

    std::cout << "✅ Test 8 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 9: CPFP Recommendation (Available)
// ============================================================================

bool test_w4_3_cpfp_available() {
    std::cout << "\n[Test 9] W.4.3: CPFP recommendation (available)" << std::endl;

    FeeBumpEngine engine;
    uint256 txid;

    // Create rescue strategy with CPFP available
    RescueStrategy strategy;
    strategy.cpfp_available = true;

    // Add spendable output
    SpendableOutput output;
    output.txid = txid;
    output.vout = 1;
    output.amount = 100000;  // 0.001 BTC
    output.is_wallet_controlled = true;
    strategy.cpfp_details.outputs.push_back(output);

    CpfpRecommendation cpfp = engine.GenerateCpfpRecommendation(
        txid, strategy, 10, nullptr, nullptr
    );

    ASSERT_TRUE(cpfp.viable, "CPFP should be viable");
    ASSERT_GT(cpfp.recommended_child_fee, 0ULL, "Child fee should be > 0");
    ASSERT_GT(cpfp.package_feerate, 0ULL, "Package feerate should be > 0");
    ASSERT_EQ(cpfp.output_index, 1U, "Should use output index 1");
    ASSERT_TRUE(!cpfp.explanation.empty(), "Should have explanation");

    std::cout << "✅ Test 9 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 10: Full Recommendation (No Options)
// ============================================================================

bool test_w4_3_full_recommendation_no_options() {
    std::cout << "\n[Test 10] W.4.3: Full recommendation (no options)" << std::endl;

    FeeBumpEngine engine;
    uint256 txid;

    // Create inclusion status
    TxInclusionStatus inclusion_status;
    inclusion_status.state = InclusionState::STALLED;
    inclusion_status.effective_feerate = 5;

    // Create rescue strategy with no options
    RescueStrategy strategy;
    strategy.rbf_available = false;
    strategy.cpfp_available = false;

    FeeBumpRecommendation recommendation = engine.GenerateRecommendation(
        txid, inclusion_status, strategy, nullptr, nullptr, 1
    );

    ASSERT_EQ(BumpStrategyToString(recommendation.strategy), std::string("wait"),
              "Should recommend WAIT when no options");
    ASSERT_TRUE(!recommendation.rationale.empty(), "Should have rationale");
    ASSERT_TRUE(recommendation.timestamp_ms > 0, "Should have timestamp");

    std::cout << "✅ Test 10 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 11: Full Recommendation (RBF Available)
// ============================================================================

bool test_w4_3_full_recommendation_rbf() {
    std::cout << "\n[Test 11] W.4.3: Full recommendation (RBF available)" << std::endl;

    FeeBumpEngine engine;
    uint256 txid;

    // Create inclusion status
    TxInclusionStatus inclusion_status;
    inclusion_status.state = InclusionState::STALLED;
    inclusion_status.effective_feerate = 5;

    // Create rescue strategy with RBF
    RescueStrategy strategy;
    strategy.rbf_available = true;
    strategy.rbf_details.min_replacement_fee = 1500;
    strategy.cpfp_available = false;

    FeeBumpRecommendation recommendation = engine.GenerateRecommendation(
        txid, inclusion_status, strategy, nullptr, nullptr, 1
    );

    ASSERT_EQ(BumpStrategyToString(recommendation.strategy), std::string("rbf"),
              "Should recommend RBF");
    ASSERT_TRUE(recommendation.rbf.has_value(), "Should have RBF details");
    ASSERT_TRUE(recommendation.rbf->viable, "RBF should be viable");
    ASSERT_FALSE(recommendation.cpfp.has_value(), "Should not have CPFP details");

    std::cout << "✅ Test 11 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 12: Full Recommendation (Both Options)
// ============================================================================

bool test_w4_3_full_recommendation_both() {
    std::cout << "\n[Test 12] W.4.3: Full recommendation (both options)" << std::endl;

    FeeBumpEngine engine;
    uint256 txid;

    // Create inclusion status
    TxInclusionStatus inclusion_status;
    inclusion_status.state = InclusionState::STALLED;
    inclusion_status.effective_feerate = 5;

    // Create rescue strategy with both RBF and CPFP
    RescueStrategy strategy;
    strategy.rbf_available = true;
    strategy.rbf_details.min_replacement_fee = 1500;
    strategy.cpfp_available = true;

    // Add spendable output for CPFP
    SpendableOutput output;
    output.txid = txid;
    output.vout = 1;
    output.amount = 100000;
    strategy.cpfp_details.outputs.push_back(output);

    FeeBumpRecommendation recommendation = engine.GenerateRecommendation(
        txid, inclusion_status, strategy, nullptr, nullptr, 1
    );

    ASSERT_EQ(BumpStrategyToString(recommendation.strategy), std::string("both"),
              "Should recommend BOTH when both available");
    ASSERT_TRUE(recommendation.rbf.has_value(), "Should have RBF details");
    ASSERT_TRUE(recommendation.cpfp.has_value(), "Should have CPFP details");
    ASSERT_TRUE(recommendation.rbf->viable, "RBF should be viable");
    ASSERT_TRUE(recommendation.cpfp->viable, "CPFP should be viable");

    std::cout << "✅ Test 12 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 13: Configuration
// ============================================================================

bool test_w4_3_configuration() {
    std::cout << "\n[Test 13] W.4.3: Configuration" << std::endl;

    FeeBumpEngine engine;

    // Test min bump increment
    ASSERT_EQ(engine.GetMinBumpIncrement(), 1ULL, "Default min bump should be 1");
    engine.SetMinBumpIncrement(5);
    ASSERT_EQ(engine.GetMinBumpIncrement(), 5ULL, "Min bump should be 5");

    // Test safety margin
    ASSERT_EQ(engine.GetSafetyMargin(), 1.1, "Default safety margin should be 1.1");
    engine.SetSafetyMargin(1.2);
    ASSERT_EQ(engine.GetSafetyMargin(), 1.2, "Safety margin should be 1.2");

    std::cout << "✅ Test 13 passed" << std::endl;
    return true;
}

// ============================================================================
// Test 14: No Bump Needed (Transaction Already Good)
// ============================================================================

bool test_w4_3_no_bump_needed() {
    std::cout << "\n[Test 14] W.4.3: No bump needed (tx already good)" << std::endl;

    FeeBumpEngine engine;
    uint256 txid;

    // Create inclusion status with LIKELY state
    TxInclusionStatus inclusion_status;
    inclusion_status.state = InclusionState::LIKELY;
    inclusion_status.effective_feerate = 20;

    // Create rescue strategy (doesn't matter, tx is already good)
    RescueStrategy strategy;
    strategy.rbf_available = true;

    FeeBumpRecommendation recommendation = engine.GenerateRecommendation(
        txid, inclusion_status, strategy, nullptr, nullptr, 1
    );

    ASSERT_EQ(BumpStrategyToString(recommendation.strategy), std::string("none"),
              "Should recommend NONE when tx is already likely");
    ASSERT_TRUE(recommendation.rationale.find("No fee bump needed") != std::string::npos ||
                recommendation.rationale.find("sufficient") != std::string::npos,
                "Rationale should mention no bump needed");

    std::cout << "✅ Test 14 passed" << std::endl;
    return true;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Phase W.4.3: FeeBumpEngine Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    bool all_passed = true;

    // Run all tests
    all_passed &= test_w4_3_enum_conversion();
    all_passed &= test_w4_3_struct_constructors();
    all_passed &= test_w4_3_fee_estimation();
    all_passed &= test_w4_3_confirmation_estimation();
    all_passed &= test_w4_3_cost_benefit();
    all_passed &= test_w4_3_rbf_not_available();
    all_passed &= test_w4_3_rbf_available();
    all_passed &= test_w4_3_cpfp_not_available();
    all_passed &= test_w4_3_cpfp_available();
    all_passed &= test_w4_3_full_recommendation_no_options();
    all_passed &= test_w4_3_full_recommendation_rbf();
    all_passed &= test_w4_3_full_recommendation_both();
    all_passed &= test_w4_3_configuration();
    all_passed &= test_w4_3_no_bump_needed();

    std::cout << "\n========================================" << std::endl;
    if (all_passed) {
        std::cout << "✅ ALL TESTS PASSED" << std::endl;
        return 0;
    } else {
        std::cout << "❌ SOME TESTS FAILED" << std::endl;
        return 1;
    }
}
