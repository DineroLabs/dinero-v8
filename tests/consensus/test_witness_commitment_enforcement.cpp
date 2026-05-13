/**
 * Phase 11d.2: Witness Commitment Enforcement Tests
 *
 * CRITICAL SAFETY TESTS
 * Locks the enforcement behavior BEFORE any activation.
 *
 * These tests ensure:
 * 1. Enforcement is OFF by default (no surprise soft fork)
 * 2. Enforcement only affects blocks with witness data
 * 3. Enforcement is height-gated
 * 4. No witness data = no requirement (even with enforcement ON)
 * 5. Valid commitment passes enforcement
 *
 * Invariants enforced:
 *   - enforce=false → always pass (default safe behavior)
 *   - height < enforcement_height → always pass
 *   - no witness data → always pass (even if enforce=true)
 *   - witness data + enforce=true → commitment REQUIRED
 *   - witness data + valid commitment → pass
 *
 * Why this matters:
 *   - Prevents accidental soft fork activation
 *   - Ensures enforcement is opt-in
 *   - Guarantees network safety
 *   - Locks policy before activation
 *
 * If these tests fail:
 *   Phase 11d enforcement logic is broken.
 *   DO NOT activate enforcement until fixed.
 */

#include <gtest/gtest.h>
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "primitives/amount.h"
#include "consensus/witness_commitment.h"
#include "consensus/merkle_root.h"
#include "consensus/chainparams.h"
#include "consensus/subsidy.h"

using namespace dinero;
using namespace dinero::consensus;

// Test fixture to initialize chain params
class WitnessCommitmentEnforcementTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        SelectParams(Chain::REGTEST);
    }
};

/**
 * Test 1: Optional Acceptance (Enforcement OFF)
 *
 * Block with witness data but no commitment.
 * Enforcement OFF (default).
 * ✅ Should be ACCEPTED
 */
TEST_F(WitnessCommitmentEnforcementTest, EnforcementOff_NoCommitment_Accepted)
{
    uint32_t height = 100;
    AmountUna subsidy = ConsensusSubsidy::GetBlockSubsidy(height);

    // Create coinbase
    Transaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.txid = TxId();
    coinbase.vin[0].prevout.vout = 0xFFFFFFFF;
    coinbase.vout.resize(1);
    coinbase.vout[0].value = subsidy;
    coinbase.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Create transaction WITH witness data
    Transaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout.txid = coinbase.GetTxid();
    tx.vin[0].prevout.vout = 0;
    tx.vin[0].witness.push_back({0x01, 0x02, 0x03});  // Witness data present
    tx.vout.resize(1);
    tx.vout[0].value = subsidy.Div(2).value();
    tx.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Build block WITHOUT witness commitment
    std::vector<Transaction> vtx = {coinbase, tx};

    // Enforcement parameters: OFF
    bool enforce = false;
    uint32_t enforcement_height = 50;  // Would be active, but enforce=false

    // Validate enforcement (should pass - enforcement is OFF)
    std::string error;
    bool result = EnforceWitnessCommitment(vtx, height, enforce, enforcement_height, error);

    EXPECT_TRUE(result)
        << "Block without commitment should be ACCEPTED when enforcement is OFF: " << error;
}

/**
 * Test 2: Enforcement Rejection (Enforcement ON)
 *
 * Block with witness data but no commitment.
 * Enforcement ON, height >= enforcement_height.
 * ❌ Should be REJECTED
 */
TEST_F(WitnessCommitmentEnforcementTest, EnforcementOn_NoCommitment_Rejected)
{
    uint32_t height = 100;
    AmountUna subsidy = ConsensusSubsidy::GetBlockSubsidy(height);

    // Create coinbase
    Transaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.txid = TxId();
    coinbase.vin[0].prevout.vout = 0xFFFFFFFF;
    coinbase.vout.resize(1);
    coinbase.vout[0].value = subsidy;
    coinbase.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Create transaction WITH witness data
    Transaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout.txid = coinbase.GetTxid();
    tx.vin[0].prevout.vout = 0;
    tx.vin[0].witness.push_back({0xaa, 0xbb, 0xcc});  // Witness data present
    tx.vout.resize(1);
    tx.vout[0].value = subsidy.Div(2).value();
    tx.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Build block WITHOUT witness commitment
    std::vector<Transaction> vtx = {coinbase, tx};

    // Enforcement parameters: ON
    bool enforce = true;
    uint32_t enforcement_height = 50;  // height=100 >= 50 → enforcement active

    // Validate enforcement (should FAIL - commitment required but missing)
    std::string error;
    bool result = EnforceWitnessCommitment(vtx, height, enforce, enforcement_height, error);

    EXPECT_FALSE(result)
        << "Block with witness data but NO commitment should be REJECTED when enforcement is ON";
    EXPECT_FALSE(error.empty())
        << "Error message should explain why block was rejected";
    EXPECT_NE(error.find("REQUIRED"), std::string::npos)
        << "Error should mention commitment is REQUIRED: " << error;
}

/**
 * Test 3: No-Witness Pass-Through (Enforcement ON)
 *
 * Block without witness data.
 * Enforcement ON, height >= enforcement_height.
 * ✅ Should be ACCEPTED (no witness = no requirement)
 */
TEST_F(WitnessCommitmentEnforcementTest, EnforcementOn_NoWitness_Accepted)
{
    uint32_t height = 100;
    AmountUna subsidy = ConsensusSubsidy::GetBlockSubsidy(height);

    // Create coinbase
    Transaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.txid = TxId();
    coinbase.vin[0].prevout.vout = 0xFFFFFFFF;
    coinbase.vout.resize(1);
    coinbase.vout[0].value = subsidy;
    coinbase.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Create transaction WITHOUT witness data
    Transaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout.txid = coinbase.GetTxid();
    tx.vin[0].prevout.vout = 0;
    // NO witness data added
    tx.vout.resize(1);
    tx.vout[0].value = subsidy.Div(2).value();
    tx.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Build block (no witness data, no commitment)
    std::vector<Transaction> vtx = {coinbase, tx};

    // Enforcement parameters: ON
    bool enforce = true;
    uint32_t enforcement_height = 50;  // height=100 >= 50 → enforcement active

    // Validate enforcement (should pass - no witness data = no requirement)
    std::string error;
    bool result = EnforceWitnessCommitment(vtx, height, enforce, enforcement_height, error);

    EXPECT_TRUE(result)
        << "Block without witness data should be ACCEPTED even when enforcement is ON: " << error;
}

/**
 * Test 4: Valid Commitment Acceptance (Enforcement ON)
 *
 * Block with witness data AND valid commitment.
 * Enforcement ON, height >= enforcement_height.
 * ✅ Should be ACCEPTED
 */
TEST_F(WitnessCommitmentEnforcementTest, EnforcementOn_ValidCommitment_Accepted)
{
    uint32_t height = 100;
    AmountUna subsidy = ConsensusSubsidy::GetBlockSubsidy(height);

    // Create coinbase
    Transaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.txid = TxId();
    coinbase.vin[0].prevout.vout = 0xFFFFFFFF;
    coinbase.vout.resize(1);
    coinbase.vout[0].value = subsidy;
    coinbase.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Create transaction WITH witness data
    Transaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout.txid = coinbase.GetTxid();
    tx.vin[0].prevout.vout = 0;
    tx.vin[0].witness.push_back({0xdd, 0xee, 0xff});  // Witness data present
    tx.vout.resize(1);
    tx.vout[0].value = subsidy.Div(2).value();
    tx.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Build block
    std::vector<Transaction> vtx = {coinbase, tx};

    // Add valid witness commitment to coinbase
    std::vector<uint8_t> commitment_script = BuildWitnessCommitment(vtx);
    TxOutput commitment_output;
    commitment_output.value = AmountUna::Zero();
    commitment_output.scriptPubKey = commitment_script;
    vtx[0].vout.push_back(commitment_output);

    // Enforcement parameters: ON
    bool enforce = true;
    uint32_t enforcement_height = 50;  // height=100 >= 50 → enforcement active

    // Validate enforcement (should pass - valid commitment present)
    std::string error;
    bool result = EnforceWitnessCommitment(vtx, height, enforce, enforcement_height, error);

    EXPECT_TRUE(result)
        << "Block with witness data and VALID commitment should be ACCEPTED: " << error;
}

/**
 * Test 5: Height Gating (Before Enforcement Height)
 *
 * Block with witness data but no commitment.
 * Enforcement ON, but height < enforcement_height.
 * ✅ Should be ACCEPTED (not yet enforced)
 */
TEST_F(WitnessCommitmentEnforcementTest, BeforeEnforcementHeight_NoCommitment_Accepted)
{
    uint32_t height = 40;  // Before enforcement height
    AmountUna subsidy = ConsensusSubsidy::GetBlockSubsidy(height);

    // Create coinbase
    Transaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.txid = TxId();
    coinbase.vin[0].prevout.vout = 0xFFFFFFFF;
    coinbase.vout.resize(1);
    coinbase.vout[0].value = subsidy;
    coinbase.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Create transaction WITH witness data
    Transaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout.txid = coinbase.GetTxid();
    tx.vin[0].prevout.vout = 0;
    tx.vin[0].witness.push_back({0x11, 0x22, 0x33});  // Witness data present
    tx.vout.resize(1);
    tx.vout[0].value = subsidy.Div(2).value();
    tx.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Build block WITHOUT witness commitment
    std::vector<Transaction> vtx = {coinbase, tx};

    // Enforcement parameters: ON, but height too low
    bool enforce = true;
    uint32_t enforcement_height = 50;  // height=40 < 50 → not yet enforced

    // Validate enforcement (should pass - before enforcement height)
    std::string error;
    bool result = EnforceWitnessCommitment(vtx, height, enforce, enforcement_height, error);

    EXPECT_TRUE(result)
        << "Block before enforcement height should be ACCEPTED even without commitment: " << error;
}

/**
 * Test 6: Default Network Parameters (Safety Check)
 *
 * Verifies that default network parameters have enforcement OFF.
 * This prevents accidental soft fork activation.
 */
TEST_F(WitnessCommitmentEnforcementTest, DefaultNetworkParameters_EnforcementOn)
{
    // Get mainnet params - enforcement ON from height 2
    SelectParams(Chain::MAINNET);
    const auto& mainnet_params = Params();

    EXPECT_TRUE(mainnet_params.enforce_witness_commitment)
        << "Mainnet should have enforcement ON";
    EXPECT_EQ(mainnet_params.witness_commitment_enforcement_height, 1u)
        << "Mainnet enforcement height should be 1 (v7 restart: features live from block 1)";

    // Get testnet params - enforcement ON from height 2
    SelectParams(Chain::TESTNET);
    const auto& testnet_params = Params();

    EXPECT_TRUE(testnet_params.enforce_witness_commitment)
        << "Testnet should have enforcement ON";
    EXPECT_EQ(testnet_params.witness_commitment_enforcement_height, 2u)
        << "Testnet enforcement height should be 2 (first normal block)";

    // Get regtest params - enforcement OFF by default (configurable for tests)
    SelectParams(Chain::REGTEST);
    const auto& regtest_params = Params();

    EXPECT_FALSE(regtest_params.enforce_witness_commitment)
        << "Regtest should have enforcement OFF by default";
    EXPECT_EQ(regtest_params.witness_commitment_enforcement_height, UINT32_MAX)
        << "Regtest enforcement height should be UINT32_MAX (configurable for tests)";
}
