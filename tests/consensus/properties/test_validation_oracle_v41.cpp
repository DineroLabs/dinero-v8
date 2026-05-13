#include <gtest/gtest.h>
#include "validation_oracle_v41.h"
#include "consensus/subsidy.h"
#include <random>

using namespace dinero;
using namespace dinero::consensus::test;

// ═══════════════════════════════════════════════════════════════════════════
// V4.1 Oracle Test - Applying Valid Block Creates Correct UTXO Set
// ═══════════════════════════════════════════════════════════════════════════

class ValidationOracleV41Test : public ::testing::Test {
protected:
    std::mt19937_64 rng{42};  // Deterministic RNG

    uint256 randomHash() {
        uint256 hash;
        for (size_t i = 0; i < 32; i++) {
            hash.data[i] = static_cast<uint8_t>(rng() & 0xFF);
        }
        return hash;
    }

    Block createSimpleBlock(uint32_t height) {
        Block block;

        // Header
        block.header.version = 1;
        block.header.prev_block_hash = randomHash();
        block.header.prev_block_hash = block.header.prev_block_hash;
        block.header.timestamp = 1700000000 + height;
        block.header.difficulty = 1000;
        block.header.nonce = rng();

        // Coinbase transaction
        Transaction coinbase;
        coinbase.version = 2;
        coinbase.lockTime = 0;
        coinbase.witness_version = 0;

        TxInput coinbase_input;
        coinbase_input.prevout.txid = TxId();  // Null
        coinbase_input.prevout.vout = 0xffffffff;
        coinbase_input.sequence = 0xffffffff;
        coinbase.vin.push_back(coinbase_input);

        TxOutput coinbase_output;
        coinbase_output.value = ConsensusSubsidy::GetBlockSubsidy(height);
        coinbase_output.scriptPubKey = {0x00, 0x14};  // P2WPKH prefix
        for (int i = 0; i < 20; i++) {
            coinbase_output.scriptPubKey.push_back(static_cast<uint8_t>(rng() & 0xFF));
        }
        coinbase.vout.push_back(coinbase_output);

        block.vtx.push_back(coinbase);

        return block;
    }
};

TEST_F(ValidationOracleV41Test, EmptyTrace_NoViolations) {
    ValidationOracleV41 oracle;
    ValidationTrace trace(42, "empty");

    auto violations = oracle.check(trace);

    EXPECT_EQ(violations.size(), 0) << "Empty trace should have no violations";
}

TEST_F(ValidationOracleV41Test, SingleBlockConnect_NoViolations) {
    ValidationOracleV41 oracle;
    ValidationTrace trace(42, "single_block");

    // Create a simple block
    Block block = createSimpleBlock(100);

    // Event: Block connected
    ValidationEvent block_event(ValidationEventType::BLOCK_CONNECTED);
    block_event.block = block;
    block_event.sequence_number = 0;
    trace.events.push_back(block_event);

    // Event: UTXO added (coinbase output)
    ValidationEvent utxo_event(ValidationEventType::UTXO_ADDED);
    TxId coinbase_txid = block.vtx[0].GetTxid();
    utxo_event.outpoint = OutPoint(coinbase_txid, 0);
    utxo_event.sequence_number = 1;
    trace.events.push_back(utxo_event);

    // State snapshot
    ValidationState state;
    state.height = 100;
    state.total_utxos = 1;
    trace.snapshots.push_back(state);

    auto violations = oracle.check(trace);

    EXPECT_EQ(violations.size(), 0) << "Valid block connection should have no violations";
}

TEST_F(ValidationOracleV41Test, MissingUTXOAddition_DetectsViolation) {
    ValidationOracleV41 oracle;
    ValidationTrace trace(42, "missing_utxo");

    // Create a block
    Block block = createSimpleBlock(100);

    // Event: Block connected
    ValidationEvent block_event(ValidationEventType::BLOCK_CONNECTED);
    block_event.block = block;
    trace.events.push_back(block_event);

    // NO UTXO_ADDED event (violation!)

    // State snapshot
    ValidationState state;
    state.height = 100;
    trace.snapshots.push_back(state);

    auto violations = oracle.check(trace);

    EXPECT_GT(violations.size(), 0) << "Missing UTXO addition should be detected";
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V4.1");
    }
}

TEST_F(ValidationOracleV41Test, MultipleBlocks_NoViolations) {
    ValidationOracleV41 oracle;
    ValidationTrace trace(42, "multiple_blocks");

    for (uint32_t height = 100; height < 103; height++) {
        Block block = createSimpleBlock(height);

        // Block connected event
        ValidationEvent block_event(ValidationEventType::BLOCK_CONNECTED);
        block_event.block = block;
        block_event.sequence_number = (height - 100) * 2;
        trace.events.push_back(block_event);

        // UTXO added event
        ValidationEvent utxo_event(ValidationEventType::UTXO_ADDED);
        TxId coinbase_txid = block.vtx[0].GetTxid();
        utxo_event.outpoint = OutPoint(coinbase_txid, 0);
        utxo_event.sequence_number = (height - 100) * 2 + 1;
        trace.events.push_back(utxo_event);

        // State snapshot
        ValidationState state;
        state.height = height;
        state.total_utxos = height - 99;
        trace.snapshots.push_back(state);
    }

    auto violations = oracle.check(trace);

    EXPECT_EQ(violations.size(), 0) << "Multiple valid blocks should have no violations";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
