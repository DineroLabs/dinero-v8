#include <gtest/gtest.h>
#include "validation_oracle_v42.h"
#include "consensus/subsidy.h"
#include <random>

using namespace dinero;
using namespace dinero::consensus::test;

// ═══════════════════════════════════════════════════════════════════════════
// V4.2 Oracle Test - Inputs Are Removed From UTXO Set
// ═══════════════════════════════════════════════════════════════════════════

class ValidationOracleV42Test : public ::testing::Test {
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

    Block createBlockWithSpend(uint32_t height, const TxOutPoint& input_to_spend) {
        Block block = createSimpleBlock(height);

        // Add a non-coinbase transaction that spends the input
        Transaction spend_tx;
        spend_tx.version = 2;
        spend_tx.lockTime = 0;
        spend_tx.witness_version = 0;

        TxInput spend_input;
        spend_input.prevout.txid = input_to_spend.txid;
        spend_input.prevout.vout = input_to_spend.vout;
        spend_input.sequence = 0xfffffffe;
        spend_tx.vin.push_back(spend_input);

        TxOutput spend_output;
        spend_output.value = AmountUna::Una(1000000ULL);  // 1 DINERO
        spend_output.scriptPubKey = {0x00, 0x14};  // P2WPKH prefix
        for (int i = 0; i < 20; i++) {
            spend_output.scriptPubKey.push_back(static_cast<uint8_t>(rng() & 0xFF));
        }
        spend_tx.vout.push_back(spend_output);

        block.vtx.push_back(spend_tx);

        return block;
    }
};

TEST_F(ValidationOracleV42Test, EmptyTrace_NoViolations) {
    ValidationOracleV42 oracle;
    ValidationTrace trace(42, "empty");

    auto violations = oracle.check(trace);

    EXPECT_EQ(violations.size(), 0) << "Empty trace should have no violations";
}

TEST_F(ValidationOracleV42Test, CoinbaseOnlyBlock_NoRemovals) {
    ValidationOracleV42 oracle;
    ValidationTrace trace(42, "coinbase_only");

    // Create a block with only coinbase (no inputs to remove)
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

    EXPECT_EQ(violations.size(), 0) << "Coinbase-only block should have no input removals";
}

TEST_F(ValidationOracleV42Test, BlockWithSpend_InputRemoved) {
    ValidationOracleV42 oracle;
    ValidationTrace trace(42, "block_with_spend");

    // Create a UTXO to spend
    TxId utxo_txid = TxId(randomHash());
    TxOutPoint utxo_to_spend(utxo_txid, 0);

    // Create a block that spends this UTXO
    Block block = createBlockWithSpend(101, utxo_to_spend);

    // Event: Block connected
    ValidationEvent block_event(ValidationEventType::BLOCK_CONNECTED);
    block_event.block = block;
    block_event.sequence_number = 0;
    trace.events.push_back(block_event);

    // Event: UTXO spent (input removal)
    ValidationEvent spent_event(ValidationEventType::UTXO_SPENT);
    spent_event.outpoint = OutPoint(utxo_to_spend.txid, utxo_to_spend.vout);
    spent_event.sequence_number = 1;
    trace.events.push_back(spent_event);

    // Event: UTXO added (coinbase output)
    ValidationEvent utxo_event1(ValidationEventType::UTXO_ADDED);
    TxId coinbase_txid = block.vtx[0].GetTxid();
    utxo_event1.outpoint = OutPoint(coinbase_txid, 0);
    utxo_event1.sequence_number = 2;
    trace.events.push_back(utxo_event1);

    // Event: UTXO added (spend tx output)
    ValidationEvent utxo_event2(ValidationEventType::UTXO_ADDED);
    TxId spend_txid = block.vtx[1].GetTxid();
    utxo_event2.outpoint = OutPoint(spend_txid, 0);
    utxo_event2.sequence_number = 3;
    trace.events.push_back(utxo_event2);

    // State snapshot
    ValidationState state;
    state.height = 101;
    state.total_utxos = 2;  // Spent 1, added 2, net +1
    trace.snapshots.push_back(state);

    auto violations = oracle.check(trace);

    EXPECT_EQ(violations.size(), 0) << "Valid spend should have no violations";
}

TEST_F(ValidationOracleV42Test, MissingInputRemoval_DetectsViolation) {
    ValidationOracleV42 oracle;
    ValidationTrace trace(42, "missing_removal");

    // Create a UTXO to spend
    TxId utxo_txid = TxId(randomHash());
    TxOutPoint utxo_to_spend(utxo_txid, 0);

    // Create a block that spends this UTXO
    Block block = createBlockWithSpend(101, utxo_to_spend);

    // Event: Block connected
    ValidationEvent block_event(ValidationEventType::BLOCK_CONNECTED);
    block_event.block = block;
    trace.events.push_back(block_event);

    // NO UTXO_SPENT event (violation!)

    // State snapshot
    ValidationState state;
    state.height = 101;
    trace.snapshots.push_back(state);

    auto violations = oracle.check(trace);

    EXPECT_GT(violations.size(), 0) << "Missing input removal should be detected";
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V4.2");
    }
}

TEST_F(ValidationOracleV42Test, MultipleInputs_AllRemoved) {
    ValidationOracleV42 oracle;
    ValidationTrace trace(42, "multiple_inputs");

    // Create 3 UTXOs to spend
    TxOutPoint utxo1(TxId(randomHash()), 0);
    TxOutPoint utxo2(TxId(randomHash()), 1);
    TxOutPoint utxo3(TxId(randomHash()), 2);

    // Create a block with a transaction that spends all 3 inputs
    Block block = createSimpleBlock(102);

    Transaction multi_input_tx;
    multi_input_tx.version = 2;
    multi_input_tx.lockTime = 0;
    multi_input_tx.witness_version = 0;

    // Add 3 inputs
    TxInput input1;
    input1.prevout.txid = utxo1.txid;
    input1.prevout.vout = utxo1.vout;
    input1.sequence = 0xfffffffe;
    multi_input_tx.vin.push_back(input1);

    TxInput input2;
    input2.prevout.txid = utxo2.txid;
    input2.prevout.vout = utxo2.vout;
    input2.sequence = 0xfffffffe;
    multi_input_tx.vin.push_back(input2);

    TxInput input3;
    input3.prevout.txid = utxo3.txid;
    input3.prevout.vout = utxo3.vout;
    input3.sequence = 0xfffffffe;
    multi_input_tx.vin.push_back(input3);

    // Add 1 output
    TxOutput output;
    output.value = AmountUna::Una(3000000ULL);  // 3 DINERO
    output.scriptPubKey = {0x00, 0x14};
    for (int i = 0; i < 20; i++) {
        output.scriptPubKey.push_back(static_cast<uint8_t>(rng() & 0xFF));
    }
    multi_input_tx.vout.push_back(output);

    block.vtx.push_back(multi_input_tx);

    // Event: Block connected
    ValidationEvent block_event(ValidationEventType::BLOCK_CONNECTED);
    block_event.block = block;
    block_event.sequence_number = 0;
    trace.events.push_back(block_event);

    // Events: All 3 UTXOs spent
    ValidationEvent spent1(ValidationEventType::UTXO_SPENT);
    spent1.outpoint = OutPoint(utxo1.txid, utxo1.vout);
    spent1.sequence_number = 1;
    trace.events.push_back(spent1);

    ValidationEvent spent2(ValidationEventType::UTXO_SPENT);
    spent2.outpoint = OutPoint(utxo2.txid, utxo2.vout);
    spent2.sequence_number = 2;
    trace.events.push_back(spent2);

    ValidationEvent spent3(ValidationEventType::UTXO_SPENT);
    spent3.outpoint = OutPoint(utxo3.txid, utxo3.vout);
    spent3.sequence_number = 3;
    trace.events.push_back(spent3);

    // Events: UTXOs added
    ValidationEvent utxo_coinbase(ValidationEventType::UTXO_ADDED);
    TxId coinbase_txid = block.vtx[0].GetTxid();
    utxo_coinbase.outpoint = OutPoint(coinbase_txid, 0);
    utxo_coinbase.sequence_number = 4;
    trace.events.push_back(utxo_coinbase);

    ValidationEvent utxo_tx(ValidationEventType::UTXO_ADDED);
    TxId tx_txid = block.vtx[1].GetTxid();
    utxo_tx.outpoint = OutPoint(tx_txid, 0);
    utxo_tx.sequence_number = 5;
    trace.events.push_back(utxo_tx);

    // State snapshot
    ValidationState state;
    state.height = 102;
    state.total_utxos = 2;  // Spent 3, added 2, net -1
    trace.snapshots.push_back(state);

    auto violations = oracle.check(trace);

    EXPECT_EQ(violations.size(), 0) << "All inputs should be removed correctly";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
