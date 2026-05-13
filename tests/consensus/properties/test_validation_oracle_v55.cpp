#include <gtest/gtest.h>
#include "validation_oracle_v55.h"
#include <random>

using namespace dinero;
using namespace dinero::consensus::test;

class ValidationOracleV55Test : public ::testing::Test {
protected:
    std::mt19937_64 rng{42};

    uint256 randomHash() {
        uint256 hash;
        for (size_t i = 0; i < 32; i++) {
            hash.data[i] = static_cast<uint8_t>(rng() & 0xFF);
        }
        return hash;
    }

    Block createSimpleBlock() {
        Block block;
        block.header.version = 1;
        block.header.prev_block_hash = randomHash();
        block.header.prev_block_hash = block.header.prev_block_hash;
        block.header.merkle_root = randomHash();  // Add merkle root
        block.header.timestamp = 1700000000;
        block.header.difficulty = 1000;
        block.header.nonce = rng();

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
        return block;
    }
};

TEST_F(ValidationOracleV55Test, EmptyTrace_NoViolations) {
    ValidationOracleV55 oracle;
    ValidationTrace trace(42, "empty");
    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0);
}

TEST_F(ValidationOracleV55Test, ValidPath_NoViolations) {
    ValidationOracleV55 oracle;
    ValidationTrace trace(42, "valid_path");

    Block block = createSimpleBlock();

    ValidationEvent connect(ValidationEventType::BLOCK_CONNECTED);
    connect.block = block;
    connect.success = true;
    trace.events.push_back(connect);

    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0);
}

TEST_F(ValidationOracleV55Test, InvalidInput_TriggersViolation) {
    ValidationOracleV55 oracle;
    ValidationTrace trace(42, "invalid_reorg");

    Block invalid_block = createSimpleBlock();

    // Block fails validation
    ValidationEvent fail(ValidationEventType::BLOCK_CONNECTED);
    fail.block = invalid_block;
    fail.success = false;
    fail.error_message = "Invalid block";
    trace.events.push_back(fail);

    // But then it's selected as tip (violation!)
    ValidationEvent connect(ValidationEventType::BLOCK_CONNECTED);
    connect.block = invalid_block;
    connect.success = true;
    trace.events.push_back(connect);

    auto violations = oracle.check(trace);
    EXPECT_GT(violations.size(), 0);
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V5.5");
    }
}

TEST_F(ValidationOracleV55Test, MixedValidInvalid_DetectsOnlyInvalid) {
    ValidationOracleV55 oracle;
    ValidationTrace trace(42, "mixed");

    Block valid_block = createSimpleBlock();
    Block invalid_block = createSimpleBlock();

    // Valid block connected
    ValidationEvent valid(ValidationEventType::BLOCK_CONNECTED);
    valid.block = valid_block;
    valid.success = true;
    trace.events.push_back(valid);

    // Invalid block fails
    ValidationEvent invalid_fail(ValidationEventType::BLOCK_CONNECTED);
    invalid_fail.block = invalid_block;
    invalid_fail.success = false;
    invalid_fail.error_message = "Bad proof";
    trace.events.push_back(invalid_fail);

    // Reorg to invalid block (violation!)
    ValidationEvent disconnect(ValidationEventType::BLOCK_DISCONNECTED);
    disconnect.block = valid_block;
    trace.events.push_back(disconnect);

    ValidationEvent invalid_connect(ValidationEventType::BLOCK_CONNECTED);
    invalid_connect.block = invalid_block;
    invalid_connect.success = true;
    trace.events.push_back(invalid_connect);

    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 1);
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V5.5");
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
