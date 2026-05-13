#include <gtest/gtest.h>
#include "validation_oracle_v43.h"
#include "consensus/subsidy.h"
#include <random>

using namespace dinero;
using namespace dinero::consensus::test;

// ═══════════════════════════════════════════════════════════════════════════
// V4.3 Oracle Test - Outputs Are Added To UTXO Set
// ═══════════════════════════════════════════════════════════════════════════

class ValidationOracleV43Test : public ::testing::Test {
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

    Block createBlockWithMultipleOutputs(uint32_t height, size_t num_outputs) {
        Block block = createSimpleBlock(height);

        // Add a transaction with multiple outputs
        Transaction multi_output_tx;
        multi_output_tx.version = 2;
        multi_output_tx.lockTime = 0;
        multi_output_tx.witness_version = 0;

        // Input (spending a dummy UTXO)
        TxInput input;
        input.prevout.txid = TxId(randomHash());
        input.prevout.vout = 0;
        input.sequence = 0xfffffffe;
        multi_output_tx.vin.push_back(input);

        // Multiple outputs
        for (size_t i = 0; i < num_outputs; i++) {
            TxOutput output;
            output.value = AmountUna::Una(1000000 + i);  // Different values
            output.scriptPubKey = {0x00, 0x14};  // P2WPKH prefix
            for (int j = 0; j < 20; j++) {
                output.scriptPubKey.push_back(static_cast<uint8_t>(rng() & 0xFF));
            }
            multi_output_tx.vout.push_back(output);
        }

        block.vtx.push_back(multi_output_tx);

        return block;
    }
};

TEST_F(ValidationOracleV43Test, EmptyTrace_NoViolations) {
    ValidationOracleV43 oracle;
    ValidationTrace trace(42, "empty");

    auto violations = oracle.check(trace);

    EXPECT_EQ(violations.size(), 0) << "Empty trace should have no violations";
}

TEST_F(ValidationOracleV43Test, SingleCoinbaseOutput_Added) {
    ValidationOracleV43 oracle;
    ValidationTrace trace(42, "single_coinbase");

    // Create a simple block with just coinbase
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

    EXPECT_EQ(violations.size(), 0) << "Coinbase output should be added correctly";
}

TEST_F(ValidationOracleV43Test, MissingOutputAddition_DetectsViolation) {
    ValidationOracleV43 oracle;
    ValidationTrace trace(42, "missing_addition");

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

    EXPECT_GT(violations.size(), 0) << "Missing output addition should be detected";
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V4.3");
    }
}

TEST_F(ValidationOracleV43Test, MultipleOutputs_AllAdded) {
    ValidationOracleV43 oracle;
    ValidationTrace trace(42, "multiple_outputs");

    // Create a block with 1 coinbase + 1 tx with 3 outputs = 4 total outputs
    Block block = createBlockWithMultipleOutputs(101, 3);

    // Event: Block connected
    ValidationEvent block_event(ValidationEventType::BLOCK_CONNECTED);
    block_event.block = block;
    block_event.sequence_number = 0;
    trace.events.push_back(block_event);

    // Event: UTXO added (coinbase output)
    ValidationEvent utxo_coinbase(ValidationEventType::UTXO_ADDED);
    TxId coinbase_txid = block.vtx[0].GetTxid();
    utxo_coinbase.outpoint = OutPoint(coinbase_txid, 0);
    utxo_coinbase.sequence_number = 1;
    trace.events.push_back(utxo_coinbase);

    // Events: UTXO added (3 outputs from second transaction)
    TxId tx_txid = block.vtx[1].GetTxid();
    for (uint32_t vout = 0; vout < 3; vout++) {
        ValidationEvent utxo_event(ValidationEventType::UTXO_ADDED);
        utxo_event.outpoint = OutPoint(tx_txid, vout);
        utxo_event.sequence_number = 2 + vout;
        trace.events.push_back(utxo_event);
    }

    // State snapshot
    ValidationState state;
    state.height = 101;
    state.total_utxos = 4;  // 1 coinbase + 3 tx outputs
    trace.snapshots.push_back(state);

    auto violations = oracle.check(trace);

    EXPECT_EQ(violations.size(), 0) << "All outputs should be added correctly";
}

TEST_F(ValidationOracleV43Test, MultipleBlocks_OutputsAccumulate) {
    ValidationOracleV43 oracle;
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

    EXPECT_EQ(violations.size(), 0) << "Multiple blocks should add outputs correctly";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
