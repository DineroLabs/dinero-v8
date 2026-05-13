/**
 * Consensus Validation Tripwires
 *
 * These tests ensure that consensus-critical validation cannot be weakened.
 * If any of these tests fail, it means someone removed important validation logic.
 *
 * CRITICAL: These tests must NEVER be modified to "make them pass" - if they fail,
 * it means the validation logic has been dangerously weakened.
 */

#include <gtest/gtest.h>
#include "consensus/pow_consensus_engine.h"
#include "consensus/chainparams.h"
#include "primitives/block.h"
#include "primitives/transaction.h"

using namespace dinero;

// v2.2.0: DaemonContext dependency removed from IConsensusEngine
// ValidateBlock is now stateless and doesn't require daemon infrastructure

// ============================================================================
// TRIPWIRE: Merkle Root Validation Must Be Enforced
// ============================================================================

TEST(ConsensusValidationTripwires, MerkleRootMismatchMustBeRejected) {
    // Create a valid-looking block but with WRONG merkle root
    Block block;

    // Valid header fields
    block.header.version = 1;
    block.header.prev_block_hash = uint256();
    block.header.timestamp = static_cast<uint64_t>(std::time(nullptr));
    block.header.difficulty = 0x1e0ffff0;  // Easy difficulty for testing
    block.header.nonce = 0;
    block.header.utreexo_root = uint256();

    // Add a valid coinbase transaction
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.lockTime = 0;

    // Coinbase input
    TxInput input;
    input.prevout.txid = TxId();  // Null hash for coinbase
    input.prevout.vout = 0xFFFFFFFF;
    input.sequence = 0xFFFFFFFF;
    input.scriptSig = {0x03, 0x01, 0x00, 0x00};  // Height 1 in coinbase
    coinbase.vin.push_back(input);

    // Coinbase output
    TxOutput output;
    output.value = AmountUna::Una(100 * 100000000ULL);  // 100 DIN
    output.scriptPubKey = {0x76, 0xa9, 0x14};  // P2PKH prefix
    output.scriptPubKey.insert(output.scriptPubKey.end(), 20, 0x00);
    output.scriptPubKey.push_back(0x88);  // OP_EQUALVERIFY
    output.scriptPubKey.push_back(0xac);  // OP_CHECKSIG
    coinbase.vout.push_back(output);

    block.vtx.push_back(coinbase);

    // CRITICAL: Set WRONG merkle root (not matching the transaction)
    uint256 wrong_hash;
    std::fill(wrong_hash.begin(), wrong_hash.end(), 0xff);  // All 0xff bytes - definitely wrong
    block.header.merkle_root = wrong_hash;

    // Create consensus engine
    auto engine = CreatePowConsensusEngine(nullptr, nullptr);

    // v2.2.0: ValidateBlock is now stateless (no DaemonContext parameter)
    // Consensus validation should not depend on daemon infrastructure

    // TRIPWIRE: This MUST return false
    // If this returns true, it means merkle root validation was removed/weakened
    bool result = engine->ValidateBlock(block);

    EXPECT_FALSE(result)
        << "CRITICAL FAILURE: ValidateBlock accepted a block with WRONG merkle root!\n"
        << "This means merkle root validation has been removed or weakened.\n"
        << "DO NOT modify this test to make it pass - fix the validation code instead!";
}

// ============================================================================
// TRIPWIRE: Empty Block Must Be Rejected
// ============================================================================

TEST(ConsensusValidationTripwires, EmptyBlockMustBeRejected) {
    // Create a block with no transactions
    Block block;

    // Valid header fields
    block.header.version = 1;
    block.header.prev_block_hash = uint256();
    block.header.merkle_root = uint256();
    block.header.timestamp = static_cast<uint64_t>(std::time(nullptr));
    block.header.difficulty = 0x1e0ffff0;
    block.header.nonce = 0;
    block.header.utreexo_root = uint256();

    // NO TRANSACTIONS - this is invalid
    // block.vtx is empty

    auto engine = CreatePowConsensusEngine(nullptr, nullptr);

    // TRIPWIRE: This MUST return false
    bool result = engine->ValidateBlock(block);

    EXPECT_FALSE(result)
        << "CRITICAL FAILURE: ValidateBlock accepted a block with NO transactions!\n"
        << "This means the coinbase check was removed.\n"
        << "DO NOT modify this test - fix the validation code!";
}

// ============================================================================
// TRIPWIRE: Future Timestamp Must Be Rejected (Beyond Tolerance)
// ============================================================================

TEST(ConsensusValidationTripwires, FarFutureTimestampMustBeRejected) {
    // Create a block with timestamp way in the future (3 hours ahead)
    Block block;

    block.header.version = 1;
    block.header.prev_block_hash = uint256();
    block.header.timestamp = static_cast<uint64_t>(std::time(nullptr)) + (3 * 60 * 60);  // 3 hours ahead
    block.header.difficulty = 0x1e0ffff0;
    block.header.nonce = 0;
    block.header.utreexo_root = uint256();

    // Add coinbase
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.lockTime = 0;

    TxInput input;
    input.prevout.txid = TxId();
    input.prevout.vout = 0xFFFFFFFF;
    input.sequence = 0xFFFFFFFF;
    input.scriptSig = {0x03, 0x01, 0x00, 0x00};
    coinbase.vin.push_back(input);

    TxOutput output;
    output.value = AmountUna::Una(100 * 100000000ULL);
    output.scriptPubKey = {0x76, 0xa9, 0x14};
    output.scriptPubKey.insert(output.scriptPubKey.end(), 20, 0x00);
    output.scriptPubKey.push_back(0x88);
    output.scriptPubKey.push_back(0xac);
    coinbase.vout.push_back(output);

    block.vtx.push_back(coinbase);

    // Set correct merkle root
    block.header.merkle_root = coinbase.GetTxid().AsUint256();

    auto engine = CreatePowConsensusEngine(nullptr, nullptr);

    // TRIPWIRE: This MUST return false (timestamp is beyond 2-hour tolerance)
    bool result = engine->ValidateBlock(block);

    EXPECT_FALSE(result)
        << "CRITICAL FAILURE: ValidateBlock accepted a block with timestamp 3 hours in future!\n"
        << "This means timestamp validation was removed or tolerance was increased unsafely.\n"
        << "DO NOT modify this test - fix the validation code!";
}

// ============================================================================
// TRIPWIRE: Invalid Version Must Be Rejected
// ============================================================================

TEST(ConsensusValidationTripwires, InvalidVersionMustBeRejected) {
    // Create a block with version 0 (invalid)
    Block block;

    block.header.version = 0;  // INVALID
    block.header.prev_block_hash = uint256();
    block.header.timestamp = static_cast<uint64_t>(std::time(nullptr));
    block.header.difficulty = 0x1e0ffff0;
    block.header.nonce = 0;
    block.header.utreexo_root = uint256();

    // Add coinbase
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.lockTime = 0;

    TxInput input;
    input.prevout.txid = TxId();
    input.prevout.vout = 0xFFFFFFFF;
    input.sequence = 0xFFFFFFFF;
    input.scriptSig = {0x03, 0x01, 0x00, 0x00};
    coinbase.vin.push_back(input);

    TxOutput output;
    output.value = AmountUna::Una(100 * 100000000ULL);
    output.scriptPubKey = {0x76, 0xa9, 0x14};
    output.scriptPubKey.insert(output.scriptPubKey.end(), 20, 0x00);
    output.scriptPubKey.push_back(0x88);
    output.scriptPubKey.push_back(0xac);
    coinbase.vout.push_back(output);

    block.vtx.push_back(coinbase);
    block.header.merkle_root = coinbase.GetTxid().AsUint256();

    auto engine = CreatePowConsensusEngine(nullptr, nullptr);

    // TRIPWIRE: This MUST return false
    bool result = engine->ValidateBlock(block);

    EXPECT_FALSE(result)
        << "CRITICAL FAILURE: ValidateBlock accepted a block with version 0!\n"
        << "This means version validation was removed.\n"
        << "DO NOT modify this test - fix the validation code!";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
