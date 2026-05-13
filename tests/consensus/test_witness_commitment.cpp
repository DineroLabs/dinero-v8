/**
 * Phase 11c.4: Witness Commitment Tests
 *
 * CRITICAL REGRESSION TESTS
 * Locks the witness commitment behavior before activation.
 *
 * These tests ensure:
 * 1. Mining creates correct witness commitments (DINW magic)
 * 2. Validation accepts valid commitments
 * 3. Validation rejects invalid commitments
 * 4. Blocks without commitments are still valid (optional in Phase 11c)
 * 5. Commitment uses Dinero-specific magic (NOT Bitcoin's 0xaa21a9ed)
 *
 * Invariants enforced:
 *   - Commitment format: OP_RETURN <DINW magic> <version> <32-byte hash>
 *   - Commitment hash = SHA256(witness_merkle_root || witness_nonce)
 *   - Commitment is last output in coinbase
 *   - No commitment = valid (commitment is optional)
 *   - Invalid commitment = block rejected
 *
 * Why this matters:
 *   - Prevents accidental segwit activation via commitment bug
 *   - Ensures Dinero-specific magic is used (NOT Bitcoin's)
 *   - Locks commitment format before enforcement
 *   - Validates mining/validation parity
 *
 * If these tests fail:
 *   Phase 11c witness commitment is broken.
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
class WitnessCommitmentTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        SelectParams(Chain::REGTEST);
    }
};

/**
 * Test 1: Build Witness Commitment
 *
 * Ensures BuildWitnessCommitment() creates correct OP_RETURN script with DINW magic.
 */
TEST_F(WitnessCommitmentTest, BuildWitnessCommitmentCreatesCorrectScript)
{
    // Use height 2 to avoid genesis/premine special cases
    uint32_t height = 2;
    AmountUna subsidy = ConsensusSubsidy::GetBlockSubsidy(height);

    // Create coinbase transaction
    Transaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.txid = TxId();
    coinbase.vin[0].prevout.vout = 0xFFFFFFFF;
    coinbase.vout.resize(1);
    coinbase.vout[0].value = subsidy;
    coinbase.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Create transaction with witness data
    Transaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout.txid = coinbase.GetTxid();
    tx.vin[0].prevout.vout = 0;
    tx.vin[0].witness.push_back({0x01, 0x02, 0x03});  // Witness data
    tx.vout.resize(1);
    tx.vout[0].value = subsidy.Div(2).value();
    tx.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Build block
    std::vector<Transaction> vtx = {coinbase, tx};

    // Build witness commitment
    std::vector<uint8_t> commitment_script = BuildWitnessCommitment(vtx);

    // Verify script format
    ASSERT_FALSE(commitment_script.empty()) << "Commitment script should not be empty";
    ASSERT_GE(commitment_script.size(), 39u)
        << "Commitment script too small (expected: 1 OP_RETURN + 1 size + 37 data)";

    // Check OP_RETURN
    EXPECT_EQ(commitment_script[0], 0x6a) << "First byte should be OP_RETURN (0x6a)";

    // Check data size (37 bytes = 4 magic + 1 version + 32 hash)
    EXPECT_EQ(commitment_script[1], WitnessCommitment::SIZE)
        << "Data size should be 37 bytes";

    // Check Dinero witness magic: DINW (0x444E5257)
    EXPECT_EQ(commitment_script[2], 0x44) << "Magic byte 1 should be 'D' (0x44)";
    EXPECT_EQ(commitment_script[3], 0x4E) << "Magic byte 2 should be 'N' (0x4E)";
    EXPECT_EQ(commitment_script[4], 0x52) << "Magic byte 3 should be 'R' (0x52)";
    EXPECT_EQ(commitment_script[5], 0x57) << "Magic byte 4 should be 'W' (0x57)";

    // Check version byte (0x01 for Phase 11c)
    EXPECT_EQ(commitment_script[6], WitnessCommitment::VERSION)
        << "Version byte should be 0x01";

    // Remaining 32 bytes should be the commitment hash (verified in other tests)
    EXPECT_EQ(commitment_script.size(), 39u)
        << "Total script size should be 39 bytes";
}

/**
 * Test 2: Find Witness Commitment
 *
 * Ensures FindWitnessCommitmentIndex() correctly locates commitment in coinbase.
 */
TEST_F(WitnessCommitmentTest, FindWitnessCommitmentLocatesCorrectOutput)
{
    // Create coinbase with witness commitment
    Transaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.txid = TxId();
    coinbase.vin[0].prevout.vout = 0xFFFFFFFF;

    // Regular payout output
    coinbase.vout.resize(1);
    coinbase.vout[0].value = AmountUna::Una(5000000000);
    coinbase.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Add witness commitment output (OP_RETURN)
    TxOutput commitment_output;
    commitment_output.value = AmountUna::Zero();
    commitment_output.scriptPubKey = {
        0x6a,  // OP_RETURN
        0x25,  // 37 bytes
        0x44, 0x4E, 0x52, 0x57,  // DINW magic
        0x01,  // Version
        // 32-byte hash (zeros for this test)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    coinbase.vout.push_back(commitment_output);

    // Find witness commitment
    auto index_opt = FindWitnessCommitmentIndex(coinbase);

    ASSERT_TRUE(index_opt.has_value()) << "Should find witness commitment";
    EXPECT_EQ(index_opt.value(), 1u) << "Commitment should be at index 1";
}

/**
 * Test 3: Extract Witness Commitment
 *
 * Ensures ExtractWitnessCommitment() correctly parses commitment hash.
 */
TEST_F(WitnessCommitmentTest, ExtractWitnessCommitmentParsesHash)
{
    // Create coinbase with known commitment hash
    Transaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.txid = TxId();
    coinbase.vin[0].prevout.vout = 0xFFFFFFFF;

    // Create commitment with known hash
    uint256 expected_hash;
    uint256::FromHex("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", expected_hash);

    TxOutput commitment_output;
    commitment_output.value = AmountUna::Zero();
    commitment_output.scriptPubKey.push_back(0x6a);  // OP_RETURN
    commitment_output.scriptPubKey.push_back(0x25);  // 37 bytes
    commitment_output.scriptPubKey.push_back(0x44);  // D
    commitment_output.scriptPubKey.push_back(0x4E);  // N
    commitment_output.scriptPubKey.push_back(0x52);  // R
    commitment_output.scriptPubKey.push_back(0x57);  // W
    commitment_output.scriptPubKey.push_back(0x01);  // Version
    // Add hash bytes (little-endian internal format)
    commitment_output.scriptPubKey.insert(
        commitment_output.scriptPubKey.end(),
        expected_hash.data,
        expected_hash.data + 32
    );

    coinbase.vout.push_back(commitment_output);

    // Extract commitment
    auto commitment_opt = ExtractWitnessCommitment(coinbase, 0);

    ASSERT_TRUE(commitment_opt.has_value()) << "Should extract commitment";
    EXPECT_EQ(commitment_opt.value(), expected_hash) << "Extracted hash should match";
}

/**
 * Test 4: Validate Witness Commitment - Valid
 *
 * Ensures ValidateWitnessCommitment() accepts valid commitments.
 */
TEST_F(WitnessCommitmentTest, ValidateWitnessCommitmentAcceptsValid)
{
    // Use height 2
    uint32_t height = 2;
    AmountUna subsidy = ConsensusSubsidy::GetBlockSubsidy(height);

    // Create coinbase
    Transaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.txid = TxId();
    coinbase.vin[0].prevout.vout = 0xFFFFFFFF;
    coinbase.vout.resize(1);
    coinbase.vout[0].value = subsidy;
    coinbase.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Create transaction with witness
    Transaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout.txid = coinbase.GetTxid();
    tx.vin[0].prevout.vout = 0;
    tx.vin[0].witness.push_back({0xaa, 0xbb, 0xcc});
    tx.vout.resize(1);
    tx.vout[0].value = subsidy.Div(2).value();
    tx.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Build block
    std::vector<Transaction> vtx = {coinbase, tx};

    // Add witness commitment to coinbase
    std::vector<uint8_t> commitment_script = BuildWitnessCommitment(vtx);
    TxOutput commitment_output;
    commitment_output.value = AmountUna::Zero();
    commitment_output.scriptPubKey = commitment_script;
    vtx[0].vout.push_back(commitment_output);

    // Validate commitment
    std::string error;
    bool valid = ValidateWitnessCommitment(vtx, error);

    EXPECT_TRUE(valid) << "Valid commitment should pass validation: " << error;
}

/**
 * Test 5: Validate Witness Commitment - No Commitment
 *
 * Ensures blocks without commitments are still valid (optional in Phase 11c).
 */
TEST_F(WitnessCommitmentTest, ValidateWitnessCommitmentAcceptsNoCommitment)
{
    // Use height 2
    uint32_t height = 2;
    AmountUna subsidy = ConsensusSubsidy::GetBlockSubsidy(height);

    // Create coinbase (no commitment)
    Transaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.txid = TxId();
    coinbase.vin[0].prevout.vout = 0xFFFFFFFF;
    coinbase.vout.resize(1);
    coinbase.vout[0].value = subsidy;
    coinbase.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Create transaction with witness
    Transaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout.txid = coinbase.GetTxid();
    tx.vin[0].prevout.vout = 0;
    tx.vin[0].witness.push_back({0xdd, 0xee, 0xff});
    tx.vout.resize(1);
    tx.vout[0].value = subsidy.Div(2).value();
    tx.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Build block WITHOUT witness commitment
    std::vector<Transaction> vtx = {coinbase, tx};

    // Validate (should pass - commitment is optional)
    std::string error;
    bool valid = ValidateWitnessCommitment(vtx, error);

    EXPECT_TRUE(valid) << "Block without commitment should be valid (optional): " << error;
}

/**
 * Test 6: Validate Witness Commitment - Invalid Hash
 *
 * Ensures blocks with invalid commitment hashes are rejected.
 */
TEST_F(WitnessCommitmentTest, ValidateWitnessCommitmentRejectsInvalidHash)
{
    // Use height 2
    uint32_t height = 2;
    AmountUna subsidy = ConsensusSubsidy::GetBlockSubsidy(height);

    // Create coinbase
    Transaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.txid = TxId();
    coinbase.vin[0].prevout.vout = 0xFFFFFFFF;
    coinbase.vout.resize(1);
    coinbase.vout[0].value = subsidy;
    coinbase.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Create transaction with witness
    Transaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout.txid = coinbase.GetTxid();
    tx.vin[0].prevout.vout = 0;
    tx.vin[0].witness.push_back({0x11, 0x22, 0x33});
    tx.vout.resize(1);
    tx.vout[0].value = subsidy.Div(2).value();
    tx.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};

    // Build block
    std::vector<Transaction> vtx = {coinbase, tx};

    // Add INVALID witness commitment (wrong hash)
    TxOutput commitment_output;
    commitment_output.value = AmountUna::Zero();
    commitment_output.scriptPubKey = {
        0x6a,  // OP_RETURN
        0x25,  // 37 bytes
        0x44, 0x4E, 0x52, 0x57,  // DINW magic
        0x01,  // Version
        // WRONG hash (all zeros)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    vtx[0].vout.push_back(commitment_output);

    // Validate (should fail - invalid hash)
    std::string error;
    bool valid = ValidateWitnessCommitment(vtx, error);

    EXPECT_FALSE(valid) << "Invalid commitment should be rejected";
    EXPECT_FALSE(error.empty()) << "Error message should be provided";
}

/**
 * Test 7: Dinero Magic Isolation
 *
 * Ensures Dinero-specific magic (DINW) is used, NOT Bitcoin's 0xaa21a9ed.
 */
TEST_F(WitnessCommitmentTest, DineroMagicIsolation)
{
    // Verify magic constant
    EXPECT_EQ(WitnessCommitment::MAGIC, 0x444E5257u)
        << "Magic should be DINW (0x444E5257), NOT Bitcoin's 0xaa21a9ed";

    // Verify commitment size (37 bytes: 4 magic + 1 version + 32 hash)
    EXPECT_EQ(WitnessCommitment::SIZE, 37u)
        << "Commitment size should be 37 bytes (Dinero format with version)";

    // Bitcoin uses 36 bytes (4 magic + 32 hash, no version)
    EXPECT_NE(WitnessCommitment::SIZE, 36u)
        << "Commitment size should NOT be 36 bytes (Bitcoin format)";
}
