/**
 * Phase 11a Consensus Lock: Merkle Invariants
 *
 * CRITICAL REGRESSION TEST
 * This test locks the single-TX merkle invariant that was the source of
 * the Phase 11a merkle root mismatch bug.
 *
 * Invariant:
 *   For a block with exactly one transaction:
 *   merkle_root == txid
 *
 * Why this matters:
 *   - Prevents serialization bugs in merkle computation
 *   - Catches endianness regressions
 *   - Detects "optimized" merkle rewrites that break consensus
 *   - Prevents witness data leaking into merkle logic
 *
 * If this test fails:
 *   STOP IMMEDIATELY. Do not merge. Do not refactor.
 *   Phase 11a consensus is broken.
 */

#include <gtest/gtest.h>
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "primitives/amount.h"
#include "consensus/merkle_root.h"  // Phase 11a.2: Canonical merkle computation
#include "consensus/chainparams.h"
#include "consensus/subsidy.h"

using namespace dinero;

// Test fixture to initialize chain params
class MerkleInvariantsTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        SelectParams(Chain::REGTEST);
    }
};

/**
 * Test: Single-transaction merkle root == txid (canonical consensus invariant)
 */
TEST_F(MerkleInvariantsTest, SingleTransactionMerkleEqualsTxid)
{
    // Use height 2 to avoid genesis/premine special cases
    uint32_t height = 2;
    AmountUna subsidy = ConsensusSubsidy::GetBlockSubsidy(height);

    // Create minimal coinbase transaction with correct subsidy
    Transaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.txid = TxId();  // Null for coinbase
    coinbase.vin[0].prevout.vout = 0xFFFFFFFF;

    coinbase.vout.resize(1);
    coinbase.vout[0].value = subsidy;
    coinbase.vout[0].scriptPubKey = {0x76, 0xa9, 0x14};  // Dummy P2PKH

    // Build block with single transaction
    Block block;
    block.vtx.push_back(coinbase);

    // Compute merkle root via CANONICAL consensus API (Phase 11a.2)
    uint256 merkle_root = consensus::ComputeMerkleRoot(block.vtx);

    // Get transaction ID
    uint256 txid = block.vtx[0].GetTxid().AsUint256();

    // CRITICAL CONSENSUS INVARIANT: For single-TX blocks, merkle_root == txid
    // Compare internal uint256 objects, NOT hex strings
    ASSERT_EQ(merkle_root, txid)
        << "CONSENSUS FAILURE: Single-TX merkle_root != txid\n"
        << "   Merkle: " << merkle_root.GetHex() << "\n"
        << "   Txid:   " << txid.GetHex() << "\n"
        << "   This indicates a regression in merkle computation\n"
        << "   Phase 11a consensus is BROKEN";
}

/**
 * Test: Multi-transaction merkle tree sanity check
 */
TEST_F(MerkleInvariantsTest, MultiTransactionMerkleTree)
{
    // Use height 2 to avoid genesis/premine special cases
    uint32_t height = 2;
    AmountUna subsidy = ConsensusSubsidy::GetBlockSubsidy(height);

    // Create coinbase
    Transaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.txid = TxId();
    coinbase.vin[0].prevout.vout = 0xFFFFFFFF;
    coinbase.vout.resize(1);
    coinbase.vout[0].value = subsidy;

    // Create regular transaction
    Transaction tx1;
    tx1.vin.resize(1);
    tx1.vin[0].prevout.txid = coinbase.GetTxid();
    tx1.vin[0].prevout.vout = 0;
    tx1.vout.resize(1);
    tx1.vout[0].value = subsidy.Div(2).value();  // Spend half

    // Build block with multiple transactions
    Block block;
    block.vtx.push_back(coinbase);
    block.vtx.push_back(tx1);

    // Compute merkle root via CANONICAL consensus API (Phase 11a.2)
    uint256 merkle_root = consensus::ComputeMerkleRoot(block.vtx);

    // For multi-TX, root should NOT equal any single txid
    TxId coinbase_txid = coinbase.GetTxid();
    TxId tx1_txid = tx1.GetTxid();

    ASSERT_NE(merkle_root, coinbase_txid.AsUint256())
        << "SANITY FAILURE: Multi-TX merkle_root equals coinbase txid";
    ASSERT_NE(merkle_root, tx1_txid.AsUint256())
        << "SANITY FAILURE: Multi-TX merkle_root equals tx1 txid";
}

/**
 * CVE-2012-2459: a block that duplicates a transaction to forge another (valid)
 * block's merkle root — and thus its block hash — must be flagged as `mutated`.
 * The malleated block's root is IDENTICAL to the valid block's by construction
 * (that IS the attack); the valid block is never flagged.
 */
TEST_F(MerkleInvariantsTest, DetectsCve20122459Mutation)
{
    uint32_t height = 2;
    AmountUna subsidy = ConsensusSubsidy::GetBlockSubsidy(height);

    Transaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.txid = TxId();
    coinbase.vin[0].prevout.vout = 0xFFFFFFFF;
    coinbase.vout.resize(1);
    coinbase.vout[0].value = subsidy;

    Transaction tx1;
    tx1.vin.resize(1);
    tx1.vin[0].prevout.txid = coinbase.GetTxid();
    tx1.vin[0].prevout.vout = 0;
    tx1.vout.resize(1);
    tx1.vout[0].value = subsidy.Div(2).value();

    // Distinct from tx1 only by prevout vout -> different txid.
    Transaction tx2;
    tx2.vin.resize(1);
    tx2.vin[0].prevout.txid = coinbase.GetTxid();
    tx2.vin[0].prevout.vout = 1;
    tx2.vout.resize(1);
    tx2.vout[0].value = subsidy.Div(2).value();

    ASSERT_NE(tx1.GetTxid(), tx2.GetTxid());

    // Valid: 3 distinct txs (odd -> tx2 is self-duplicated inside the tree, which
    // is legitimate and must NOT be flagged).
    std::vector<Transaction> valid = {coinbase, tx1, tx2};
    // Malleated: tx2 duplicated as a REAL adjacent pair -> identical root.
    std::vector<Transaction> malleated = {coinbase, tx1, tx2, tx2};

    bool valid_mutated = true;       // seed opposite of expected
    bool malleated_mutated = false;  // seed opposite of expected
    uint256 valid_root = consensus::ComputeMerkleRoot(valid, &valid_mutated);
    uint256 malleated_root = consensus::ComputeMerkleRoot(malleated, &malleated_mutated);

    // The CVE property: the malleated block forges the SAME merkle root/hash.
    // Without this the test would prove nothing about CVE-2012-2459.
    ASSERT_EQ(valid_root, malleated_root)
        << "test is meaningless unless both blocks share a merkle root";
    EXPECT_FALSE(valid_mutated)
        << "valid block (legitimate odd-count self-duplication) must not be flagged";
    EXPECT_TRUE(malleated_mutated)
        << "duplicated-transaction block must be flagged as mutated (CVE-2012-2459)";
}
