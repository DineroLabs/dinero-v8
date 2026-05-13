#include <gtest/gtest.h>
#include "validation_oracle_v51.h"
#include <random>

using namespace dinero;
using namespace dinero::consensus::test;

class ValidationOracleV51Test : public ::testing::Test {
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
        uint256 prev_hash = randomHash();
        block.header.prev_block_hash = prev_hash;
        block.header.prev_block_hash = block.header.prev_block_hash;
        block.header.merkle_root = randomHash();  // Add merkle root
        block.header.timestamp = 1700000000;
        block.header.difficulty = 1000;
        block.header.nonce = rng();

        // Coinbase
        Transaction coinbase;
        coinbase.version = 2;
        coinbase.lockTime = 0;
        TxInput input;
        input.prevout.txid = TxId();
        input.prevout.vout = 0xffffffff;
        input.sequence = 0xffffffff;
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

TEST_F(ValidationOracleV51Test, EmptyTrace_NoViolations) {
    ValidationOracleV51 oracle;
    ValidationTrace trace(42, "empty");
    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0);
}

TEST_F(ValidationOracleV51Test, ValidPath_NoViolations) {
    ValidationOracleV51 oracle;
    ValidationTrace trace(42, "valid_path");

    Block block = createSimpleBlock();

    // Event: Block validation succeeds
    ValidationEvent validation(ValidationEventType::BLOCK_CONNECTED);
    validation.block = block;
    validation.success = true;
    trace.events.push_back(validation);

    // Event: Block connected
    ValidationEvent connect(ValidationEventType::BLOCK_CONNECTED);
    connect.block = block;
    connect.success = true;
    trace.events.push_back(connect);

    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0) << "Valid block connection should have no violations";
}

TEST_F(ValidationOracleV51Test, InvalidInput_TriggersViolation) {
    ValidationOracleV51 oracle;
    ValidationTrace trace(42, "invalid_input");

    Block block = createSimpleBlock();

    // Event: Block validation FAILS
    ValidationEvent validation_fail(ValidationEventType::BLOCK_CONNECTED);
    validation_fail.block = block;
    validation_fail.success = false;
    validation_fail.error_message = "Invalid merkle root";
    trace.events.push_back(validation_fail);

    // Event: Block connected ANYWAY (violation!)
    ValidationEvent connect(ValidationEventType::BLOCK_CONNECTED);
    connect.block = block;
    connect.success = true;
    trace.events.push_back(connect);

    auto violations = oracle.check(trace);
    EXPECT_GT(violations.size(), 0) << "Connecting invalid block should be detected";
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V5.1");
    }
}

TEST_F(ValidationOracleV51Test, MixedValidInvalid_DetectsOnlyInvalid) {
    ValidationOracleV51 oracle;
    ValidationTrace trace(42, "mixed");

    Block valid_block = createSimpleBlock();
    Block invalid_block = createSimpleBlock();

    // Event: Valid block connected (OK)
    ValidationEvent valid_connect(ValidationEventType::BLOCK_CONNECTED);
    valid_connect.block = valid_block;
    valid_connect.success = true;
    trace.events.push_back(valid_connect);

    // Event: Invalid block validation fails
    ValidationEvent invalid_validation(ValidationEventType::BLOCK_CONNECTED);
    invalid_validation.block = invalid_block;
    invalid_validation.success = false;
    invalid_validation.error_message = "Bad timestamp";
    trace.events.push_back(invalid_validation);

    // Event: Invalid block connected anyway (violation!)
    ValidationEvent invalid_connect(ValidationEventType::BLOCK_CONNECTED);
    invalid_connect.block = invalid_block;
    invalid_connect.success = true;
    trace.events.push_back(invalid_connect);

    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 1) << "Should detect exactly one violation (invalid block)";
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V5.1");
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
