#include <gtest/gtest.h>
#include "validation_oracle_v56.h"
#include <random>

using namespace dinero;
using namespace dinero::consensus::test;

class ValidationOracleV56Test : public ::testing::Test {
protected:
    std::mt19937_64 rng{42};

    uint256 randomHash() {
        uint256 hash;
        for (size_t i = 0; i < 32; i++) {
            hash.data[i] = static_cast<uint8_t>(rng() & 0xFF);
        }
        return hash;
    }

    Transaction createSimpleTx() {
        Transaction tx;
        tx.version = 2;
        TxInput input;
        input.prevout.txid = TxId(randomHash());
        input.prevout.vout = 0;
        tx.vin.push_back(input);
        TxOutput output;
        output.value = AmountUna::Una(1000000ULL);
        output.scriptPubKey = {0x00, 0x14};
        for (int i = 0; i < 20; i++) {
            output.scriptPubKey.push_back(static_cast<uint8_t>(rng() & 0xFF));
        }
        tx.vout.push_back(output);
        return tx;
    }

    Block createBlockWithTx(const Transaction& tx) {
        Block block;
        block.header.version = 1;
        block.header.prev_block_hash = randomHash();
        block.header.prev_block_hash = block.header.prev_block_hash;
        block.header.merkle_root = randomHash();  // Add merkle root
        block.header.timestamp = 1700000000;
        block.header.difficulty = 1000;
        block.header.nonce = rng();

        // Coinbase
        Transaction coinbase;
        coinbase.version = 2;
        TxInput input;
        input.prevout.txid = TxId();
        input.prevout.vout = 0xffffffff;
        coinbase.vin.push_back(input);
        TxOutput output;
        output.value = AmountUna::Una(10000000000ULL);  // 100 DIN
        output.scriptPubKey = {0x00, 0x14};
        for (int i = 0; i < 20; i++) {
            output.scriptPubKey.push_back(static_cast<uint8_t>(rng() & 0xFF));
        }
        coinbase.vout.push_back(output);
        block.vtx.push_back(coinbase);

        // Add the transaction
        block.vtx.push_back(tx);

        return block;
    }
};

TEST_F(ValidationOracleV56Test, EmptyTrace_NoViolations) {
    ValidationOracleV56 oracle;
    ValidationTrace trace(42, "empty");
    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0);
}

TEST_F(ValidationOracleV56Test, ValidPath_NoViolations) {
    ValidationOracleV56 oracle;
    ValidationTrace trace(42, "valid_path");

    Transaction tx = createSimpleTx();
    Block block = createBlockWithTx(tx);

    // TX validated successfully
    ValidationEvent tx_valid(ValidationEventType::TX_VALIDATED);
    tx_valid.transaction = tx;
    tx_valid.success = true;
    trace.events.push_back(tx_valid);

    // Block with valid TX
    ValidationEvent block_connect(ValidationEventType::BLOCK_CONNECTED);
    block_connect.block = block;
    block_connect.success = true;
    trace.events.push_back(block_connect);

    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0);
}

TEST_F(ValidationOracleV56Test, InvalidInput_TriggersViolation) {
    ValidationOracleV56 oracle;
    ValidationTrace trace(42, "invalid_input");

    Transaction invalid_tx = createSimpleTx();
    Block block = createBlockWithTx(invalid_tx);

    // TX validation fails
    ValidationEvent tx_fail(ValidationEventType::TX_VALIDATED);
    tx_fail.transaction = invalid_tx;
    tx_fail.success = false;
    tx_fail.error_message = "Missing input";
    trace.events.push_back(tx_fail);

    // But it's included in a block anyway (violation!)
    ValidationEvent block_connect(ValidationEventType::BLOCK_CONNECTED);
    block_connect.block = block;
    block_connect.success = true;
    trace.events.push_back(block_connect);

    auto violations = oracle.check(trace);
    EXPECT_GT(violations.size(), 0);
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V5.6");
    }
}

TEST_F(ValidationOracleV56Test, MixedValidInvalid_DetectsOnlyInvalid) {
    ValidationOracleV56 oracle;
    ValidationTrace trace(42, "mixed");

    Transaction valid_tx = createSimpleTx();
    Transaction invalid_tx = createSimpleTx();

    Block block_valid = createBlockWithTx(valid_tx);
    Block block_invalid = createBlockWithTx(invalid_tx);

    // Valid TX
    ValidationEvent tx_valid(ValidationEventType::TX_VALIDATED);
    tx_valid.transaction = valid_tx;
    tx_valid.success = true;
    trace.events.push_back(tx_valid);

    ValidationEvent block_valid_connect(ValidationEventType::BLOCK_CONNECTED);
    block_valid_connect.block = block_valid;
    block_valid_connect.success = true;
    trace.events.push_back(block_valid_connect);

    // Invalid TX
    ValidationEvent tx_invalid(ValidationEventType::TX_VALIDATED);
    tx_invalid.transaction = invalid_tx;
    tx_invalid.success = false;
    tx_invalid.error_message = "Bad signature";
    trace.events.push_back(tx_invalid);

    ValidationEvent block_invalid_connect(ValidationEventType::BLOCK_CONNECTED);
    block_invalid_connect.block = block_invalid;
    block_invalid_connect.success = true;
    trace.events.push_back(block_invalid_connect);

    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 1);
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V5.6");
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
