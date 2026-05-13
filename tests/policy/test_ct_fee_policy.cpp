/**
 * Phase 3: CT Fee Policy Configuration Tests
 *
 * Verifies CT (Confidential Transaction) fee policy parameters are configured
 * correctly and estimation functions work as expected.
 *
 * Tests:
 * - CT.1: CT minimum fee rate defaults
 * - CT.2: CT weight multiplier defaults
 * - CT.3: CT proof weight factor defaults
 * - CT.4: CT fee estimation calculation
 * - CT.5: CT fee estimate higher than transparent
 * - CT.6: CT fee estimate respects minimum
 */

#include "policy/fee_estimator.h"
#include "mining/ct_selection_policy.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace dinero::policy;
using namespace dinero::mining;

//=============================================================================
// CT.1: CT Minimum Fee Rate Defaults
//=============================================================================

void testCTMinFeeRateDefaults() {
    std::cout << "\n[Test 1] CT minimum fee rate defaults" << std::endl;

    FeeEstimator estimator;

    uint64_t min_rate = estimator.getMinCTFeeRate();
    std::cout << "  CT min fee rate: " << min_rate << " sat/KB" << std::endl;
    std::cout << "  CT min fee rate: " << (min_rate / 1000.0) << " sat/vB" << std::endl;

    // Default should be 2000 sat/KB (2 sat/vB) - 2x transparent minimum
    assert(min_rate == 2000 && "CT min fee rate must be 2000 sat/KB (2 sat/vB)");

    std::cout << "  [✓] CT minimum fee rate: 2 sat/vB (2x transparent)" << std::endl;
}

//=============================================================================
// CT.2: CT Weight Multiplier Defaults
//=============================================================================

void testCTWeightMultiplierDefaults() {
    std::cout << "\n[Test 2] CT weight multiplier defaults" << std::endl;

    FeeEstimator estimator;

    double multiplier = estimator.getCTWeightMultiplier();
    std::cout << "  CT weight multiplier: " << multiplier << "x" << std::endl;

    // Default should be 1.5x to account for verification overhead
    assert(std::abs(multiplier - 1.5) < 0.001 && "CT weight multiplier must be 1.5x");

    std::cout << "  [✓] CT weight multiplier: 1.5x (verification overhead)" << std::endl;
}

//=============================================================================
// CT.3: CT Proof Weight Factor Defaults
//=============================================================================

void testCTProofWeightFactorDefaults() {
    std::cout << "\n[Test 3] CT proof weight factor defaults" << std::endl;

    FeeEstimator estimator;

    uint32_t factor = estimator.getCTProofWeightFactor();
    std::cout << "  CT proof weight factor: " << factor << "x" << std::endl;

    // Default should be 4 (similar to SegWit witness discount inverse)
    assert(factor == 4 && "CT proof weight factor must be 4");

    std::cout << "  [✓] CT proof weight factor: 4 (SegWit-style)" << std::endl;
}

//=============================================================================
// CT.4: CT Fee Estimation Calculation
//=============================================================================

void testCTFeeEstimationCalculation() {
    std::cout << "\n[Test 4] CT fee estimation calculation" << std::endl;

    FeeEstimator estimator;

    // Test with 5000 bytes of proof (typical for ~7 outputs)
    CTFeeEstimate estimate = estimator.estimateCTFee(FeeTarget::NORMAL, 5000);

    std::cout << "  Base fee rate: " << estimate.base_fee_rate << " sat/KB" << std::endl;
    std::cout << "  CT adjusted rate: " << estimate.ct_adjusted_rate << " sat/KB" << std::endl;
    std::cout << "  CT multiplier: " << estimate.ct_multiplier << "x" << std::endl;
    std::cout << "  Proof weight: " << estimate.ct_proof_weight << std::endl;
    std::cout << "  Estimated proof bytes: " << estimate.estimated_proof_bytes << std::endl;

    // Verify estimate has reasonable values
    assert(estimate.ct_adjusted_rate >= estimate.base_fee_rate &&
           "CT adjusted rate must be >= base rate");
    assert(estimate.ct_multiplier == 1.5 && "CT multiplier must be 1.5");
    assert(estimate.ct_proof_weight == 4 && "CT proof weight must be 4");
    assert(estimate.estimated_proof_bytes == 5000 && "Proof bytes must match input");

    std::cout << "  [✓] CT fee estimation produces valid results" << std::endl;
}

//=============================================================================
// CT.5: CT Fee Estimate Higher Than Transparent
//=============================================================================

void testCTFeeHigherThanTransparent() {
    std::cout << "\n[Test 5] CT fee estimate >= transparent estimate * multiplier" << std::endl;

    FeeEstimator estimator;

    // Get both estimates for same target
    FeeEstimate transparent = estimator.estimateFee(FeeTarget::NORMAL);
    CTFeeEstimate ct = estimator.estimateCTFee(FeeTarget::NORMAL, 5000);

    std::cout << "  Transparent fee rate: " << transparent.fee_rate << " sat/KB" << std::endl;
    std::cout << "  CT adjusted rate: " << ct.ct_adjusted_rate << " sat/KB" << std::endl;

    // CT rate should be at least base * multiplier
    // (or at least the minimum CT fee rate)
    uint64_t expected_min = static_cast<uint64_t>(ct.base_fee_rate * ct.ct_multiplier);
    expected_min = std::max(expected_min, estimator.getMinCTFeeRate());

    assert(ct.ct_adjusted_rate >= expected_min &&
           "CT adjusted rate must be >= base * multiplier or minimum");

    std::cout << "  [✓] CT fee correctly accounts for verification overhead" << std::endl;
}

//=============================================================================
// CT.6: CT Fee Estimate Respects Minimum
//=============================================================================

void testCTFeeRespectsMinimum() {
    std::cout << "\n[Test 6] CT fee estimate respects minimum fee rate" << std::endl;

    FeeEstimator estimator;

    // Get estimate with minimum proof
    CTFeeEstimate estimate = estimator.estimateCTFee(FeeTarget::ECONOMY, 100);

    std::cout << "  CT adjusted rate: " << estimate.ct_adjusted_rate << " sat/KB" << std::endl;
    std::cout << "  CT minimum rate: " << estimator.getMinCTFeeRate() << " sat/KB" << std::endl;

    // CT rate should never be below minimum
    assert(estimate.ct_adjusted_rate >= estimator.getMinCTFeeRate() &&
           "CT adjusted rate must respect minimum");

    std::cout << "  [✓] CT fee respects minimum fee rate" << std::endl;
}

//=============================================================================
// CT.7: CT Selection Policy Defaults
//=============================================================================

void testCTSelectionPolicyDefaults() {
    std::cout << "\n[Test 7] CT selection policy defaults" << std::endl;

    CTSelectionConfig config;

    std::cout << "  CT min fee rate: " << config.ct_min_fee_rate << " sat/vB" << std::endl;
    std::cout << "  CT weight multiplier: " << config.ct_weight_multiplier << "x" << std::endl;
    std::cout << "  CT proof weight factor: " << config.ct_proof_weight_factor << std::endl;
    std::cout << "  Max CT per block: " << config.max_ct_per_block << std::endl;

    // Verify defaults match expected values
    assert(config.ct_min_fee_rate == 2.0 && "Default CT min fee rate should be 2 sat/vB");
    assert(std::abs(config.ct_weight_multiplier - 1.5) < 0.001 && "Default weight multiplier should be 1.5");
    assert(config.ct_proof_weight_factor == 4 && "Default proof weight factor should be 4");
    assert(config.max_ct_per_block == 50 && "Default max CT per block should be 50");

    std::cout << "  [✓] CT selection policy has correct defaults" << std::endl;
}

//=============================================================================
// CT.8: CT Fee Estimate Helper Function
//=============================================================================

void testCTFeeEstimateHelper() {
    std::cout << "\n[Test 8] CTFeeEstimate::estimateFeeForSize() helper" << std::endl;

    FeeEstimator estimator;
    CTFeeEstimate estimate = estimator.estimateCTFee(FeeTarget::NORMAL, 5000);

    // Calculate fee for a 250-byte transaction with 5000 bytes of proofs
    uint64_t fee = estimate.estimateFeeForSize(250, 5000);

    std::cout << "  Transaction size: 250 bytes" << std::endl;
    std::cout << "  Proof size: 5000 bytes" << std::endl;
    std::cout << "  Estimated fee: " << fee << " una" << std::endl;

    // Fee should be positive and reasonable
    assert(fee > 0 && "Fee must be positive");
    assert(fee < 1000000 && "Fee should be reasonable (< 1M una)");

    std::cout << "  [✓] Fee estimation helper works correctly" << std::endl;
}

//=============================================================================
// CT.9: CT Fee Configuration Setters
//=============================================================================

void testCTFeeConfigSetters() {
    std::cout << "\n[Test 9] CT fee configuration setters" << std::endl;

    FeeEstimator estimator;

    // Test setMinCTFeeRate
    estimator.setMinCTFeeRate(5000);
    assert(estimator.getMinCTFeeRate() == 5000 && "setMinCTFeeRate should update value");
    std::cout << "  [✓] setMinCTFeeRate works" << std::endl;

    // Test setCTWeightMultiplier
    estimator.setCTWeightMultiplier(2.0);
    assert(std::abs(estimator.getCTWeightMultiplier() - 2.0) < 0.001 && "setCTWeightMultiplier should update value");
    std::cout << "  [✓] setCTWeightMultiplier works" << std::endl;

    // Test setCTProofWeightFactor
    estimator.setCTProofWeightFactor(8);
    assert(estimator.getCTProofWeightFactor() == 8 && "setCTProofWeightFactor should update value");
    std::cout << "  [✓] setCTProofWeightFactor works" << std::endl;

    // Verify estimate uses updated values
    CTFeeEstimate estimate = estimator.estimateCTFee(FeeTarget::NORMAL, 5000);
    assert(estimate.ct_multiplier == 2.0 && "Estimate should use updated multiplier");
    assert(estimate.ct_proof_weight == 8 && "Estimate should use updated proof weight");
    std::cout << "  [✓] Updated config values are used in estimation" << std::endl;
}

//=============================================================================
// Main Test Runner
//=============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Phase 3: CT Fee Policy Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\nVerifying CT fee policy configuration and estimation." << std::endl;

    try {
        // CT.1-3: Configuration defaults
        testCTMinFeeRateDefaults();
        testCTWeightMultiplierDefaults();
        testCTProofWeightFactorDefaults();

        // CT.4-6: Fee estimation
        testCTFeeEstimationCalculation();
        testCTFeeHigherThanTransparent();
        testCTFeeRespectsMinimum();

        // CT.7-9: Selection policy and helpers
        testCTSelectionPolicyDefaults();
        testCTFeeEstimateHelper();
        testCTFeeConfigSetters();

        std::cout << "\n========================================" << std::endl;
        std::cout << "✅ All CT Fee Policy Tests Passed!" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "\nSummary (CT Fee Policy Verification):" << std::endl;
        std::cout << "  [✓] CT.1: Min CT fee rate = 2 sat/vB (2x transparent)" << std::endl;
        std::cout << "  [✓] CT.2: CT weight multiplier = 1.5x" << std::endl;
        std::cout << "  [✓] CT.3: CT proof weight factor = 4" << std::endl;
        std::cout << "  [✓] CT.4: Fee estimation produces valid results" << std::endl;
        std::cout << "  [✓] CT.5: CT fee >= base * multiplier" << std::endl;
        std::cout << "  [✓] CT.6: CT fee respects minimum" << std::endl;
        std::cout << "  [✓] CT.7: Selection policy defaults correct" << std::endl;
        std::cout << "  [✓] CT.8: Fee helper function works" << std::endl;
        std::cout << "  [✓] CT.9: Configuration setters work" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
