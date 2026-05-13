/**
 * CRITICAL Activation Boundary Tests
 *
 * These tests LOCK the activation boundary behavior FOREVER.
 * If these tests pass, the decision is final:
 *   - Height 0 (genesis): Utreexo ACTIVE (genesis-era activation)
 *   - Height 1 (premine): Utreexo ACTIVE
 *   - All heights: Full rules enforced from genesis
 *
 * DESIGN RATIONALE:
 * Dinero activates all consensus features from genesis because:
 *   1. Clean slate - no legacy blocks to validate
 *   2. Simpler code - no activation logic needed
 *   3. Consistent behavior across all heights
 *
 * THESE TESTS ARE CONSENSUS-CRITICAL.
 * DO NOT MODIFY without understanding the full implications.
 */

#include <gtest/gtest.h>
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "primitives/amount.h"
#include "consensus/witness_commitment.h"
#include "consensus/utreexo_activation.h"
#include "consensus/chainparams.h"
#include "consensus/subsidy.h"

using namespace dinero;
using namespace dinero::consensus;

// ═══════════════════════════════════════════════════════════════════════════
// Test Fixture
// ═══════════════════════════════════════════════════════════════════════════

class ActivationBoundaryCriticalTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Use mainnet params - these are the production rules
        SelectParams(Chain::MAINNET);
    }

    // Helper: Create a minimal coinbase transaction
    static Transaction CreateCoinbase(uint32_t height) {
        Transaction coinbase;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.txid = TxId();
        coinbase.vin[0].prevout.vout = 0xFFFFFFFF;
        coinbase.vout.resize(1);
        coinbase.vout[0].value = ConsensusSubsidy::GetBlockSubsidy(height);
        coinbase.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};  // P2PKH prefix
        return coinbase;
    }

    // Helper: Create a transaction with witness data
    static Transaction CreateWitnessTx(const TxId& prevTxid) {
        Transaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout.txid = prevTxid;
        tx.vin[0].prevout.vout = 0;
        tx.vin[0].witness.push_back({0x01, 0x02, 0x03});  // Witness data
        tx.vout.resize(1);
        tx.vout[0].value = AmountUna::Una(100000000);  // 1 DIN
        tx.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};
        return tx;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// TEST A: Genesis-Era Activation (Height 0)
// ═══════════════════════════════════════════════════════════════════════════
// All consensus features are active from genesis.
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ActivationBoundaryCriticalTest, GenesisInvariants_UtreexoActiveFromGenesis) {
    constexpr uint32_t GENESIS_HEIGHT = 0;

    SelectParams(Chain::MAINNET);

    // Verify FullRulesActive returns TRUE for genesis (genesis-era activation)
    EXPECT_TRUE(FullRulesActive(GENESIS_HEIGHT))
        << "CRITICAL: FullRulesActive must return TRUE for genesis (height 0)";

    // Verify IsUtreexoActive returns TRUE for genesis on mainnet
    EXPECT_TRUE(IsUtreexoActive(GENESIS_HEIGHT))
        << "CRITICAL: IsUtreexoActive must return TRUE for genesis on mainnet";

    // Verify activation height is 0 (genesis-era activation)
    EXPECT_EQ(GetUtreexoActivationHeight(), 0u)
        << "CRITICAL: Utreexo activation height must be 0 (genesis-era activation)";
}

TEST_F(ActivationBoundaryCriticalTest, GenesisInvariants_NoWitnessCommitmentInCoinbase) {
    constexpr uint32_t GENESIS_HEIGHT = 0;

    // Create genesis coinbase (test helper, not actual genesis)
    Transaction coinbase = CreateCoinbase(GENESIS_HEIGHT);
    std::vector<Transaction> vtx = {coinbase};

    // Simple coinbase should not have witness commitment
    auto commitment_index = FindWitnessCommitmentIndex(coinbase);
    EXPECT_FALSE(commitment_index.has_value())
        << "Genesis coinbase must NOT have witness commitment";
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST B: Premine Invariants (Height 1)
// ═══════════════════════════════════════════════════════════════════════════
// Premine is subject to the same rules as all other blocks.
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ActivationBoundaryCriticalTest, PremineInvariants_UtreexoActive) {
    constexpr uint32_t PREMINE_HEIGHT = 1;

    SelectParams(Chain::MAINNET);

    // Verify FullRulesActive returns TRUE for premine
    EXPECT_TRUE(FullRulesActive(PREMINE_HEIGHT))
        << "CRITICAL: FullRulesActive must return TRUE for premine (height 1)";

    // Utreexo is active from genesis - premine IS under Utreexo rules
    EXPECT_TRUE(IsUtreexoActive(PREMINE_HEIGHT))
        << "CRITICAL: Utreexo MUST be active at premine (height 1) on mainnet";

    // Premine is AFTER activation (activation at height 0)
    EXPECT_GE(PREMINE_HEIGHT, GetUtreexoActivationHeight())
        << "CRITICAL: Premine height must be >= Utreexo activation height";
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST C: Height 2 (Normal Block)
// ═══════════════════════════════════════════════════════════════════════════
// Height 2 follows the same rules as all other blocks.
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ActivationBoundaryCriticalTest, Height2_FullRulesActive) {
    constexpr uint32_t NORMAL_HEIGHT = 2;

    SelectParams(Chain::MAINNET);

    // Verify FullRulesActive returns true for height 2
    EXPECT_TRUE(FullRulesActive(NORMAL_HEIGHT))
        << "CRITICAL: FullRulesActive must return TRUE for height 2";

    // Verify IsUtreexoActive returns true for height 2
    EXPECT_TRUE(IsUtreexoActive(NORMAL_HEIGHT))
        << "CRITICAL: IsUtreexoActive must return TRUE for height 2 on mainnet";
}

TEST_F(ActivationBoundaryCriticalTest, Height2_WitnessTxRequiresCommitment) {
    constexpr uint32_t NORMAL_HEIGHT = 2;

    SelectParams(Chain::MAINNET);

    // Create coinbase WITHOUT witness commitment
    Transaction coinbase = CreateCoinbase(NORMAL_HEIGHT);

    // Create transaction WITH witness data
    Transaction witness_tx = CreateWitnessTx(coinbase.GetTxid());

    std::vector<Transaction> vtx = {coinbase, witness_tx};

    // Validate witness commitment - should FAIL (witness tx but no commitment)
    std::string error;
    bool enforce = true;
    uint32_t enforcement_height = 0;  // Enforcement from genesis

    bool result = EnforceWitnessCommitment(vtx, NORMAL_HEIGHT, enforce, enforcement_height, error);

    EXPECT_FALSE(result)
        << "CRITICAL: Block with witness tx but NO commitment must be REJECTED";
    EXPECT_FALSE(error.empty())
        << "CRITICAL: Rejection must include error message";
}

TEST_F(ActivationBoundaryCriticalTest, Height2_ValidCommitmentAccepted) {
    constexpr uint32_t NORMAL_HEIGHT = 2;

    SelectParams(Chain::MAINNET);

    // Create coinbase
    Transaction coinbase = CreateCoinbase(NORMAL_HEIGHT);

    // Create transaction WITH witness data
    Transaction witness_tx = CreateWitnessTx(coinbase.GetTxid());

    std::vector<Transaction> vtx = {coinbase, witness_tx};

    // Build valid witness commitment
    std::vector<uint8_t> commitment_script = BuildWitnessCommitment(vtx);
    ASSERT_FALSE(commitment_script.empty())
        << "BuildWitnessCommitment must return valid script";

    // Add commitment to coinbase
    TxOutput commitment_output;
    commitment_output.value = AmountUna::Zero();
    commitment_output.scriptPubKey = commitment_script;
    vtx[0].vout.push_back(commitment_output);

    // Validate - should PASS
    std::string error;
    bool result = ValidateWitnessCommitment(vtx, error);

    EXPECT_TRUE(result)
        << "CRITICAL: Block with valid witness commitment must be ACCEPTED: " << error;
}

// ═══════════════════════════════════════════════════════════════════════════
// Network Consistency Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ActivationBoundaryCriticalTest, NetworkConsistency_AllNetworksActivateFromGenesis) {
    // All networks now activate Utreexo from genesis

    SelectParams(Chain::MAINNET);
    uint32_t mainnet_activation = GetUtreexoActivationHeight();
    EXPECT_EQ(mainnet_activation, 0u)
        << "CRITICAL: Mainnet must activate from genesis (height 0)";

    SelectParams(Chain::TESTNET);
    uint32_t testnet_activation = GetUtreexoActivationHeight();
    EXPECT_EQ(testnet_activation, 0u)
        << "CRITICAL: Testnet must activate from genesis (height 0)";

    SelectParams(Chain::REGTEST);
    uint32_t regtest_activation = GetUtreexoActivationHeight();
    EXPECT_EQ(regtest_activation, 0u)
        << "CRITICAL: Regtest must activate from genesis (height 0)";

    // All networks should have the same activation height
    EXPECT_EQ(mainnet_activation, testnet_activation)
        << "CRITICAL: Mainnet and testnet must have same activation height";
    EXPECT_EQ(mainnet_activation, regtest_activation)
        << "CRITICAL: All networks must have same activation height";
}

TEST_F(ActivationBoundaryCriticalTest, HighBlock_FullRulesActive) {
    // Verify full rules are active at any high block number
    SelectParams(Chain::MAINNET);

    EXPECT_TRUE(FullRulesActive(100))
        << "Full rules must be active at height 100";
    EXPECT_TRUE(FullRulesActive(1000))
        << "Full rules must be active at height 1000";
    EXPECT_TRUE(FullRulesActive(100000))
        << "Full rules must be active at height 100000";
    EXPECT_TRUE(IsUtreexoActive(100000))
        << "Utreexo must be active at height 100000";
}
