#include <gtest/gtest.h>
#include "validation_oracle_v44.h"
#include "consensus/subsidy.h"
#include <random>

using namespace dinero;
using namespace dinero::consensus::test;

// ═══════════════════════════════════════════════════════════════════════════
// V4.4 Oracle Test - Value Is Conserved (Inputs ≥ Outputs + Fee)
// ═══════════════════════════════════════════════════════════════════════════

class ValidationOracleV44Test : public ::testing::Test {
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

TEST_F(ValidationOracleV44Test, EmptyTrace_NoViolations) {
    ValidationOracleV44 oracle;
    ValidationTrace trace(42, "empty");

    auto violations = oracle.check(trace);

    EXPECT_EQ(violations.size(), 0) << "Empty trace should have no violations";
}

TEST_F(ValidationOracleV44Test, CoinbaseOnly_NoViolations) {
    ValidationOracleV44 oracle;
    ValidationTrace trace(42, "coinbase_only");

    // Create a simple block with only coinbase
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

    dinero::consensus::UTXOEntry coin;
    coin.value = block.vtx[0].vout[0].value;
    coin.scriptPubKey = block.vtx[0].vout[0].scriptPubKey;
    coin.height = 100;
    coin.isCoinbase = true;
    utxo_event.coin = coin;

    utxo_event.sequence_number = 1;
    trace.events.push_back(utxo_event);

    // State snapshot
    ValidationState state;
    state.height = 100;
    trace.snapshots.push_back(state);

    auto violations = oracle.check(trace);

    EXPECT_EQ(violations.size(), 0) << "Coinbase-only block should have no violations";
}

TEST_F(ValidationOracleV44Test, ValidSpend_NoViolations) {
    ValidationOracleV44 oracle;
    ValidationTrace trace(42, "valid_spend");

    // Create initial UTXO
    OutPoint utxo1(TxId(randomHash()), 0);
    uint64_t utxo1_value = 10000000;  // 10 DINERO

    // Create a block that spends utxo1
    Block block = createSimpleBlock(101);

    // Add a transaction that spends utxo1
    Transaction spend_tx;
    spend_tx.version = 2;
    spend_tx.lockTime = 0;
    spend_tx.witness_version = 0;

    TxInput input;
    input.prevout.txid = utxo1.txid;
    input.prevout.vout = utxo1.vout;
    input.sequence = 0xfffffffe;
    spend_tx.vin.push_back(input);

    TxOutput output;
    output.value = AmountUna::Una(9000000ULL);  // 9 DINERO (1 DINERO fee)
    output.scriptPubKey = {0x00, 0x14};
    for (int i = 0; i < 20; i++) {
        output.scriptPubKey.push_back(static_cast<uint8_t>(rng() & 0xFF));
    }
    spend_tx.vout.push_back(output);

    block.vtx.push_back(spend_tx);

    // First, emit UTXO_ADDED for the initial UTXO
    ValidationEvent utxo1_event(ValidationEventType::UTXO_ADDED);
    utxo1_event.outpoint = utxo1;

    dinero::consensus::UTXOEntry coin1;
    coin1.value = AmountUna::Una(utxo1_value);
    coin1.scriptPubKey = {0x00, 0x14};
    coin1.height = 100;
    coin1.isCoinbase = false;
    utxo1_event.coin = coin1;

    trace.events.push_back(utxo1_event);

    // Event: Block connected
    ValidationEvent block_event(ValidationEventType::BLOCK_CONNECTED);
    block_event.block = block;
    block_event.sequence_number = 1;
    trace.events.push_back(block_event);

    // Event: UTXO spent
    ValidationEvent spent_event(ValidationEventType::UTXO_SPENT);
    spent_event.outpoint = utxo1;
    spent_event.sequence_number = 2;
    trace.events.push_back(spent_event);

    // State snapshot
    ValidationState state;
    state.height = 101;
    trace.snapshots.push_back(state);

    auto violations = oracle.check(trace);

    EXPECT_EQ(violations.size(), 0) << "Valid spend (inputs >= outputs) should have no violations";
}

TEST_F(ValidationOracleV44Test, InvalidSpend_DetectsViolation) {
    ValidationOracleV44 oracle;
    ValidationTrace trace(42, "invalid_spend");

    // Create initial UTXO
    OutPoint utxo1(TxId(randomHash()), 0);
    uint64_t utxo1_value = 5000000;  // 5 DINERO

    // Create a block that spends utxo1
    Block block = createSimpleBlock(101);

    // Add a transaction that creates value (outputs > inputs)
    Transaction spend_tx;
    spend_tx.version = 2;
    spend_tx.lockTime = 0;
    spend_tx.witness_version = 0;

    TxInput input;
    input.prevout.txid = utxo1.txid;
    input.prevout.vout = utxo1.vout;
    input.sequence = 0xfffffffe;
    spend_tx.vin.push_back(input);

    TxOutput output;
    output.value = AmountUna::Una(10000000ULL);  // 10 DINERO (5 DINERO created out of thin air!)
    output.scriptPubKey = {0x00, 0x14};
    for (int i = 0; i < 20; i++) {
        output.scriptPubKey.push_back(static_cast<uint8_t>(rng() & 0xFF));
    }
    spend_tx.vout.push_back(output);

    block.vtx.push_back(spend_tx);

    // First, emit UTXO_ADDED for the initial UTXO
    ValidationEvent utxo1_event(ValidationEventType::UTXO_ADDED);
    utxo1_event.outpoint = utxo1;

    dinero::consensus::UTXOEntry coin1;
    coin1.value = AmountUna::Una(utxo1_value);
    coin1.scriptPubKey = {0x00, 0x14};
    coin1.height = 100;
    coin1.isCoinbase = false;
    utxo1_event.coin = coin1;

    trace.events.push_back(utxo1_event);

    // Event: Block connected
    ValidationEvent block_event(ValidationEventType::BLOCK_CONNECTED);
    block_event.block = block;
    block_event.sequence_number = 1;
    trace.events.push_back(block_event);

    // State snapshot
    ValidationState state;
    state.height = 101;
    trace.snapshots.push_back(state);

    auto violations = oracle.check(trace);

    EXPECT_GT(violations.size(), 0) << "Invalid spend (outputs > inputs) should be detected";
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V4.4");
    }
}

TEST_F(ValidationOracleV44Test, MultipleInputs_ValueConserved) {
    ValidationOracleV44 oracle;
    ValidationTrace trace(42, "multiple_inputs");

    // Create 3 UTXOs
    OutPoint utxo1(TxId(randomHash()), 0);
    OutPoint utxo2(TxId(randomHash()), 1);
    OutPoint utxo3(TxId(randomHash()), 2);
    uint64_t utxo1_value = 3000000;
    uint64_t utxo2_value = 4000000;
    uint64_t utxo3_value = 5000000;
    uint64_t total_inputs = utxo1_value + utxo2_value + utxo3_value;  // 12 DINERO

    // Emit UTXO_ADDED events for all 3
    for (size_t i = 0; i < 3; i++) {
        ValidationEvent utxo_event(ValidationEventType::UTXO_ADDED);
        if (i == 0) {
            utxo_event.outpoint = utxo1;
            dinero::consensus::UTXOEntry coin;
            coin.value = AmountUna::Una(utxo1_value);
            coin.scriptPubKey = {0x00, 0x14};
            coin.height = 100;
            coin.isCoinbase = false;
            utxo_event.coin = coin;
        } else if (i == 1) {
            utxo_event.outpoint = utxo2;
            dinero::consensus::UTXOEntry coin;
            coin.value = AmountUna::Una(utxo2_value);
            coin.scriptPubKey = {0x00, 0x14};
            coin.height = 100;
            coin.isCoinbase = false;
            utxo_event.coin = coin;
        } else {
            utxo_event.outpoint = utxo3;
            dinero::consensus::UTXOEntry coin;
            coin.value = AmountUna::Una(utxo3_value);
            coin.scriptPubKey = {0x00, 0x14};
            coin.height = 100;
            coin.isCoinbase = false;
            utxo_event.coin = coin;
        }
        trace.events.push_back(utxo_event);
    }

    // Create a block that spends all 3 UTXOs
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

    // Add 1 output (with fee)
    TxOutput output;
    output.value = AmountUna::Una(total_inputs - 1000000);  // 11 DINERO (1 DINERO fee)
    output.scriptPubKey = {0x00, 0x14};
    for (int i = 0; i < 20; i++) {
        output.scriptPubKey.push_back(static_cast<uint8_t>(rng() & 0xFF));
    }
    multi_input_tx.vout.push_back(output);

    block.vtx.push_back(multi_input_tx);

    // Event: Block connected
    ValidationEvent block_event(ValidationEventType::BLOCK_CONNECTED);
    block_event.block = block;
    block_event.sequence_number = 3;
    trace.events.push_back(block_event);

    // State snapshot
    ValidationState state;
    state.height = 102;
    trace.snapshots.push_back(state);

    auto violations = oracle.check(trace);

    EXPECT_EQ(violations.size(), 0) << "Multiple inputs with value conservation should have no violations";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
