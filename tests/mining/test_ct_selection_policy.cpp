/**
 * @file test_ct_selection_policy.cpp
 * @brief Unit tests for CTSelectionPolicy
 *
 * Tests cover:
 * - CT transaction detection (HasConfidentialOutputs)
 * - Policy enforcement (CheckPolicy)
 * - Weight calculation (CalculateCTWeight, GetWeightInfo)
 * - Fee rate calculation (CalculateAdjustedFeeRate, MeetsMinimumFeeRate)
 * - Batch verification optimization (OptimizeForBatchVerification)
 */

#include <gtest/gtest.h>
#include "mining/ct_selection_policy.h"
#include "primitives/transaction.h"
#include "primitives/amount.h"
#include <vector>
#include <algorithm>

using namespace dinero;
using namespace dinero::mining;

// ============================================================================
// Test Helpers
// ============================================================================

/**
 * Create a transparent transaction for testing
 */
Transaction CreateTransparentTx(size_t num_inputs, size_t num_outputs, uint64_t fee = 1000) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;
    tx.witness_version = 0;

    // Add inputs
    for (size_t i = 0; i < num_inputs; ++i) {
        TxInput input;
        input.prevout.vout = 0;
        input.sequence = 0xfffffffe;
        tx.vin.push_back(input);
    }

    // Add outputs (transparent)
    for (size_t i = 0; i < num_outputs; ++i) {
        TxOutput output;
        output.value = AmountUna::Una(10000);
        output.scriptPubKey = std::vector<uint8_t>(22, 0x00);  // P2WPKH-like
        output.is_confidential = false;
        tx.vout.push_back(output);
    }

    // Set explicit fee for calculation purposes
    tx.explicit_fee = AmountUna::Una(fee);
    tx.has_explicit_fee = true;

    return tx;
}

/**
 * Create a confidential transaction for testing
 */
Transaction CreateConfidentialTx(size_t num_ct_outputs, size_t proof_size_per_output = 700, uint64_t fee = 2000) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;
    tx.witness_version = 0;

    // Add one input
    TxInput input;
    input.prevout.vout = 0;
    input.sequence = 0xfffffffe;
    tx.vin.push_back(input);

    // Add CT outputs
    for (size_t i = 0; i < num_ct_outputs; ++i) {
        TxOutput output;
        output.value = AmountUna::Zero();  // Hidden for CT
        output.scriptPubKey = std::vector<uint8_t>(22, 0x00);
        output.is_confidential = true;
        output.commitment = std::vector<uint8_t>(33, 0xAA);  // 33-byte commitment
        output.range_proof = std::vector<uint8_t>(proof_size_per_output, 0xBB);  // Range proof
        output.nonce = std::vector<uint8_t>(32, 0xCC);  // 32-byte nonce
        tx.vout.push_back(output);
    }

    // Explicit fee required for CT
    tx.explicit_fee = AmountUna::Una(fee);
    tx.has_explicit_fee = true;

    return tx;
}

/**
 * Create a mixed transaction (transparent + CT outputs)
 */
Transaction CreateMixedTx(size_t num_transparent, size_t num_ct, uint64_t fee = 1500) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;
    tx.witness_version = 0;

    // Add input
    TxInput input;
    input.prevout.vout = 0;
    tx.vin.push_back(input);

    // Add transparent outputs
    for (size_t i = 0; i < num_transparent; ++i) {
        TxOutput output;
        output.value = AmountUna::Una(5000);
        output.scriptPubKey = std::vector<uint8_t>(22, 0x00);
        output.is_confidential = false;
        tx.vout.push_back(output);
    }

    // Add CT outputs
    for (size_t i = 0; i < num_ct; ++i) {
        TxOutput output;
        output.value = AmountUna::Zero();
        output.scriptPubKey = std::vector<uint8_t>(22, 0x00);
        output.is_confidential = true;
        output.commitment = std::vector<uint8_t>(33, 0xAA);
        output.range_proof = std::vector<uint8_t>(700, 0xBB);
        output.nonce = std::vector<uint8_t>(32, 0xCC);
        tx.vout.push_back(output);
    }

    tx.explicit_fee = AmountUna::Una(fee);
    tx.has_explicit_fee = true;

    return tx;
}

// ============================================================================
// Test Fixture
// ============================================================================

class CTSelectionPolicyTest : public ::testing::Test {
protected:
    CTSelectionConfig default_config_;

    void SetUp() override {
        // Default configuration
        default_config_.ct_weight_multiplier = 1.5;
        default_config_.ct_min_fee_rate = 2;
        default_config_.max_ct_per_block = 50;
        default_config_.max_ct_proof_data = 50000;
        default_config_.enable_batch_optimization = true;
        default_config_.batch_size_threshold = 5;
        default_config_.ct_proof_weight_factor = 4;
    }
};

// ============================================================================
// Test: HasConfidentialOutputs
// ============================================================================

TEST_F(CTSelectionPolicyTest, HasConfidentialOutputs_TransparentTx) {
    CTSelectionPolicy policy(default_config_);

    Transaction tx = CreateTransparentTx(2, 2);

    EXPECT_FALSE(policy.HasConfidentialOutputs(tx))
        << "Transparent transaction should not be detected as confidential";
}

TEST_F(CTSelectionPolicyTest, HasConfidentialOutputs_ConfidentialTx) {
    CTSelectionPolicy policy(default_config_);

    Transaction tx = CreateConfidentialTx(2);

    EXPECT_TRUE(policy.HasConfidentialOutputs(tx))
        << "CT transaction should be detected as confidential";
}

TEST_F(CTSelectionPolicyTest, HasConfidentialOutputs_MixedTx) {
    CTSelectionPolicy policy(default_config_);

    Transaction tx = CreateMixedTx(2, 1);

    EXPECT_TRUE(policy.HasConfidentialOutputs(tx))
        << "Mixed transaction with CT outputs should be detected";
}

TEST_F(CTSelectionPolicyTest, HasConfidentialOutputs_EmptyTx) {
    CTSelectionPolicy policy(default_config_);

    Transaction tx;
    tx.version = 2;

    EXPECT_FALSE(policy.HasConfidentialOutputs(tx))
        << "Empty transaction should not be confidential";
}

// ============================================================================
// Test: CheckPolicy - Acceptance
// ============================================================================

TEST_F(CTSelectionPolicyTest, CheckPolicy_AcceptsTransparentTx) {
    CTSelectionPolicy policy(default_config_);

    Transaction tx = CreateTransparentTx(2, 2);

    auto result = policy.CheckPolicy(tx, 1000, 0, 0);

    EXPECT_TRUE(result.acceptable)
        << "Transparent transactions should always pass CT policy";
}

TEST_F(CTSelectionPolicyTest, CheckPolicy_AcceptsValidCT) {
    CTSelectionPolicy policy(default_config_);

    // Create CT with good fee rate (2000 sat fee for ~300 bytes base = ~6.6 sat/vB)
    Transaction tx = CreateConfidentialTx(1, 700, 5000);

    auto result = policy.CheckPolicy(tx, 1000, 0, 0);

    EXPECT_TRUE(result.acceptable)
        << "Valid CT transaction should be accepted. Rejection: " << result.rejection_reason;
}

// ============================================================================
// Test: CheckPolicy - Per-Block Limits
// ============================================================================

TEST_F(CTSelectionPolicyTest, CheckPolicy_RejectsExceedingMaxCTPerBlock) {
    CTSelectionConfig config = default_config_;
    config.max_ct_per_block = 5;
    CTSelectionPolicy policy(config);

    Transaction tx = CreateConfidentialTx(1, 700, 5000);

    // Already at limit
    auto result = policy.CheckPolicy(tx, 1000, 5, 0);

    EXPECT_FALSE(result.acceptable)
        << "Should reject CT when max per block exceeded";
    EXPECT_TRUE(result.rejection_reason.find("Max CT per block") != std::string::npos)
        << "Rejection reason should mention max CT per block";
}

TEST_F(CTSelectionPolicyTest, CheckPolicy_AcceptsWithinLimit) {
    CTSelectionConfig config = default_config_;
    config.max_ct_per_block = 10;
    CTSelectionPolicy policy(config);

    Transaction tx = CreateConfidentialTx(1, 700, 5000);

    // Just under limit
    auto result = policy.CheckPolicy(tx, 1000, 9, 0);

    EXPECT_TRUE(result.acceptable)
        << "Should accept CT when under limit. Rejection: " << result.rejection_reason;
}

// ============================================================================
// Test: CheckPolicy - Proof Data Limits
// ============================================================================

TEST_F(CTSelectionPolicyTest, CheckPolicy_RejectsExceedingMaxProofData) {
    CTSelectionConfig config = default_config_;
    config.max_ct_proof_data = 1000;  // Very low limit
    CTSelectionPolicy policy(config);

    // Transaction with large proof (2000 bytes)
    Transaction tx = CreateConfidentialTx(1, 2000, 10000);

    auto result = policy.CheckPolicy(tx, 1000, 0, 0);

    EXPECT_FALSE(result.acceptable)
        << "Should reject CT when proof data exceeds limit";
    EXPECT_TRUE(result.rejection_reason.find("proof data") != std::string::npos)
        << "Rejection reason should mention proof data";
}

TEST_F(CTSelectionPolicyTest, CheckPolicy_RejectsWhenCumulativeProofDataExceeds) {
    CTSelectionConfig config = default_config_;
    config.max_ct_proof_data = 2000;
    CTSelectionPolicy policy(config);

    Transaction tx = CreateConfidentialTx(1, 700, 5000);

    // Already have 1500 bytes of proof data in block
    auto result = policy.CheckPolicy(tx, 1000, 0, 1500);

    EXPECT_FALSE(result.acceptable)
        << "Should reject when cumulative proof data would exceed limit";
}

// ============================================================================
// Test: Weight Calculation
// ============================================================================

TEST_F(CTSelectionPolicyTest, CalculateCTWeight_TransparentTx) {
    CTSelectionPolicy policy(default_config_);

    Transaction tx = CreateTransparentTx(1, 2);

    uint64_t weight = policy.CalculateCTWeight(tx);

    // Transparent tx: just base weight (size * 4)
    EXPECT_GT(weight, 0) << "Weight should be positive";
}

TEST_F(CTSelectionPolicyTest, CalculateCTWeight_ConfidentialTx) {
    CTSelectionConfig config = default_config_;
    config.ct_weight_multiplier = 2.0;  // 2x multiplier
    config.ct_proof_weight_factor = 4;
    CTSelectionPolicy policy(config);

    // Create CT with known proof size
    Transaction tx = CreateConfidentialTx(1, 1000, 5000);  // 1000 byte proof

    auto info = policy.GetWeightInfo(tx);

    EXPECT_EQ(info.confidential_outputs, 1) << "Should detect 1 CT output";
    EXPECT_EQ(info.proof_bytes, 1000) << "Should count 1000 bytes of proof data";
    EXPECT_EQ(info.proof_weight, 4000) << "Proof weight = proof_bytes * 4";

    // Total = (base * multiplier) + proof_weight
    uint64_t expected_total = static_cast<uint64_t>(info.base_weight * 2.0) + 4000;
    EXPECT_EQ(info.total_weight, expected_total);
}

TEST_F(CTSelectionPolicyTest, GetWeightInfo_MultipleCTOutputs) {
    CTSelectionPolicy policy(default_config_);

    // 3 CT outputs, 500 bytes each
    Transaction tx = CreateConfidentialTx(3, 500, 5000);

    auto info = policy.GetWeightInfo(tx);

    EXPECT_EQ(info.confidential_outputs, 3) << "Should count all CT outputs";
    EXPECT_EQ(info.proof_bytes, 1500) << "Should sum all proof bytes (3 * 500)";
}

// ============================================================================
// Test: Fee Rate Calculation
// ============================================================================

TEST_F(CTSelectionPolicyTest, CalculateAdjustedFeeRate_TransparentTx) {
    CTSelectionPolicy policy(default_config_);

    Transaction tx = CreateTransparentTx(1, 1, 1000);

    double fee_rate = policy.CalculateAdjustedFeeRate(tx);

    EXPECT_GT(fee_rate, 0.0) << "Fee rate should be positive";
}

TEST_F(CTSelectionPolicyTest, CalculateAdjustedFeeRate_ConfidentialTx) {
    CTSelectionPolicy policy(default_config_);

    // CT with 5000 sat fee
    Transaction tx = CreateConfidentialTx(1, 700, 5000);

    double fee_rate = policy.CalculateAdjustedFeeRate(tx);

    // Fee rate is based on CT weight (higher than base weight)
    // So adjusted fee rate should be lower than naive rate
    EXPECT_GT(fee_rate, 0.0) << "Fee rate should be positive";
}

TEST_F(CTSelectionPolicyTest, MeetsMinimumFeeRate_AboveMinimum) {
    CTSelectionConfig config = default_config_;
    config.ct_min_fee_rate = 1;  // Low minimum
    CTSelectionPolicy policy(config);

    // High fee CT
    Transaction tx = CreateConfidentialTx(1, 700, 50000);

    EXPECT_TRUE(policy.MeetsMinimumFeeRate(tx))
        << "High fee transaction should meet minimum";
}

TEST_F(CTSelectionPolicyTest, MeetsMinimumFeeRate_BelowMinimum) {
    CTSelectionConfig config = default_config_;
    config.ct_min_fee_rate = 100;  // Very high minimum
    CTSelectionPolicy policy(config);

    // Low fee CT
    Transaction tx = CreateConfidentialTx(1, 700, 100);

    EXPECT_FALSE(policy.MeetsMinimumFeeRate(tx))
        << "Low fee transaction should not meet high minimum";
}

// ============================================================================
// Test: Batch Verification Optimization
// ============================================================================

TEST_F(CTSelectionPolicyTest, ShouldUseBatchVerification_BelowThreshold) {
    CTSelectionConfig config = default_config_;
    config.batch_size_threshold = 5;
    config.enable_batch_optimization = true;
    CTSelectionPolicy policy(config);

    EXPECT_FALSE(policy.ShouldUseBatchVerification(3))
        << "Should not batch with 3 CTs (below threshold of 5)";
}

TEST_F(CTSelectionPolicyTest, ShouldUseBatchVerification_AtThreshold) {
    CTSelectionConfig config = default_config_;
    config.batch_size_threshold = 5;
    config.enable_batch_optimization = true;
    CTSelectionPolicy policy(config);

    EXPECT_TRUE(policy.ShouldUseBatchVerification(5))
        << "Should batch with 5 CTs (at threshold)";
}

TEST_F(CTSelectionPolicyTest, ShouldUseBatchVerification_Disabled) {
    CTSelectionConfig config = default_config_;
    config.enable_batch_optimization = false;
    CTSelectionPolicy policy(config);

    EXPECT_FALSE(policy.ShouldUseBatchVerification(100))
        << "Should not batch when optimization disabled";
}

TEST_F(CTSelectionPolicyTest, OptimizeForBatchVerification_GroupsCTTogether) {
    CTSelectionPolicy policy(default_config_);

    // Create mix of transactions
    std::vector<Transaction> txs;
    txs.push_back(CreateTransparentTx(1, 1, 1000));  // Transparent (coinbase placeholder)
    txs.push_back(CreateConfidentialTx(1, 700, 3000));  // CT
    txs.push_back(CreateTransparentTx(1, 2, 1500));  // Transparent
    txs.push_back(CreateConfidentialTx(1, 800, 4000));  // CT
    txs.push_back(CreateTransparentTx(2, 1, 2000));  // Transparent

    auto optimized = policy.OptimizeForBatchVerification(txs);

    ASSERT_EQ(optimized.size(), 5) << "Should preserve all transactions";

    // First transaction should still be first (coinbase)
    EXPECT_FALSE(policy.HasConfidentialOutputs(optimized[0]))
        << "First tx (coinbase) should remain first";

    // CT transactions should be grouped together at the end
    bool found_ct = false;
    bool found_transparent_after_ct = false;

    for (size_t i = 1; i < optimized.size(); ++i) {
        bool is_ct = policy.HasConfidentialOutputs(optimized[i]);
        if (is_ct) {
            found_ct = true;
        } else if (found_ct) {
            found_transparent_after_ct = true;
        }
    }

    // After optimization, transparent should come before CT (except coinbase)
    // So once we see CT, we shouldn't see transparent again
    EXPECT_FALSE(found_transparent_after_ct)
        << "CT transactions should be grouped together at end";
}

TEST_F(CTSelectionPolicyTest, OptimizeForBatchVerification_DisabledReturnsOriginal) {
    CTSelectionConfig config = default_config_;
    config.enable_batch_optimization = false;
    CTSelectionPolicy policy(config);

    std::vector<Transaction> txs;
    txs.push_back(CreateTransparentTx(1, 1));
    txs.push_back(CreateConfidentialTx(1));
    txs.push_back(CreateTransparentTx(1, 1));

    auto optimized = policy.OptimizeForBatchVerification(txs);

    ASSERT_EQ(optimized.size(), 3);

    // Order should be preserved when disabled
    EXPECT_FALSE(policy.HasConfidentialOutputs(optimized[0]));
    EXPECT_TRUE(policy.HasConfidentialOutputs(optimized[1]));
    EXPECT_FALSE(policy.HasConfidentialOutputs(optimized[2]));
}

// ============================================================================
// Test: Configuration
// ============================================================================

TEST_F(CTSelectionPolicyTest, SetConfig_UpdatesPolicy) {
    CTSelectionPolicy policy(default_config_);

    CTSelectionConfig new_config;
    new_config.ct_weight_multiplier = 3.0;
    new_config.max_ct_per_block = 100;

    policy.SetConfig(new_config);

    const auto& config = policy.GetConfig();
    EXPECT_DOUBLE_EQ(config.ct_weight_multiplier, 3.0);
    EXPECT_EQ(config.max_ct_per_block, 100);
}

TEST_F(CTSelectionPolicyTest, SetIndividualSettings) {
    CTSelectionPolicy policy(default_config_);

    policy.SetCTWeightMultiplier(2.5);
    policy.SetMaxCTPerBlock(75);
    policy.SetMaxCTProofData(100000);
    policy.SetMinFeeRate(5);
    policy.SetBatchOptimization(false);

    const auto& config = policy.GetConfig();
    EXPECT_DOUBLE_EQ(config.ct_weight_multiplier, 2.5);
    EXPECT_EQ(config.max_ct_per_block, 75);
    EXPECT_EQ(config.max_ct_proof_data, 100000);
    EXPECT_EQ(config.ct_min_fee_rate, 5);
    EXPECT_FALSE(config.enable_batch_optimization);
}

// ============================================================================
// Test: Edge Cases
// ============================================================================

TEST_F(CTSelectionPolicyTest, EdgeCase_ZeroFee) {
    CTSelectionPolicy policy(default_config_);

    Transaction tx = CreateConfidentialTx(1, 700, 0);  // Zero fee

    double fee_rate = policy.CalculateAdjustedFeeRate(tx);

    EXPECT_DOUBLE_EQ(fee_rate, 0.0) << "Zero fee should give zero fee rate";
    EXPECT_FALSE(policy.MeetsMinimumFeeRate(tx))
        << "Zero fee should not meet minimum";
}

TEST_F(CTSelectionPolicyTest, EdgeCase_EmptyProof) {
    CTSelectionPolicy policy(default_config_);

    // Create CT with empty proof (unusual but should handle)
    Transaction tx = CreateConfidentialTx(1, 0, 5000);  // 0-byte proof

    auto info = policy.GetWeightInfo(tx);

    EXPECT_EQ(info.proof_bytes, 0) << "Should handle zero proof bytes";
    EXPECT_TRUE(policy.HasConfidentialOutputs(tx))
        << "Should still detect as confidential";
}

TEST_F(CTSelectionPolicyTest, EdgeCase_LargeProof) {
    CTSelectionPolicy policy(default_config_);

    // Very large proof
    Transaction tx = CreateConfidentialTx(1, 50000, 100000);  // 50KB proof

    auto info = policy.GetWeightInfo(tx);

    EXPECT_EQ(info.proof_bytes, 50000);
    EXPECT_GT(info.total_weight, info.base_weight)
        << "Large proof should increase weight significantly";
}

// ============================================================================
// Test Runner
// ============================================================================

int main(int argc, char** argv) {
    std::cout << "════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "  CT SELECTION POLICY UNIT TESTS" << std::endl;
    std::cout << "════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "\nTesting mining template selection policy for CT transactions\n" << std::endl;

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
