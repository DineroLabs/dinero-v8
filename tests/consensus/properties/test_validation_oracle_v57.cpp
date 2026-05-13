#include <gtest/gtest.h>
#include "validation_oracle_v57.h"
#include <random>

using namespace dinero;
using namespace dinero::consensus::test;

class ValidationOracleV57Test : public ::testing::Test {
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

TEST_F(ValidationOracleV57Test, EmptyTrace_NoViolations) {
    ValidationOracleV57 oracle;
    ValidationTrace trace(42, "empty");
    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0);
}

TEST_F(ValidationOracleV57Test, ValidPath_NoViolations) {
    ValidationOracleV57 oracle;
    ValidationTrace trace(42, "valid_path");

    Block block = createSimpleBlock();

    ValidationEvent connect(ValidationEventType::BLOCK_CONNECTED);
    connect.block = block;
    connect.success = true;
    trace.events.push_back(connect);

    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0);
}

TEST_F(ValidationOracleV57Test, InvalidInput_TriggersViolation) {
    ValidationOracleV57 oracle;
    ValidationTrace trace(42, "non_deterministic");

    Block block = createSimpleBlock();

    // First rejection
    ValidationEvent fail1(ValidationEventType::BLOCK_CONNECTED);
    fail1.block = block;
    fail1.success = false;
    fail1.error_message = "Invalid merkle root";
    trace.events.push_back(fail1);

    // Same block, different error (non-deterministic!)
    ValidationEvent fail2(ValidationEventType::BLOCK_CONNECTED);
    fail2.block = block;
    fail2.success = false;
    fail2.error_message = "Invalid timestamp";  // Different error!
    trace.events.push_back(fail2);

    auto violations = oracle.check(trace);
    EXPECT_GT(violations.size(), 0);
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V5.7");
    }
}

TEST_F(ValidationOracleV57Test, MixedValidInvalid_DetectsOnlyInvalid) {
    ValidationOracleV57 oracle;
    ValidationTrace trace(42, "mixed");

    Block block1 = createSimpleBlock();
    Block block2 = createSimpleBlock();

    // Block 1: deterministic rejection
    ValidationEvent fail1a(ValidationEventType::BLOCK_CONNECTED);
    fail1a.block = block1;
    fail1a.success = false;
    fail1a.error_message = "Bad proof of work";
    trace.events.push_back(fail1a);

    ValidationEvent fail1b(ValidationEventType::BLOCK_CONNECTED);
    fail1b.block = block1;
    fail1b.success = false;
    fail1b.error_message = "Bad proof of work";  // Same error (OK)
    trace.events.push_back(fail1b);

    // Block 2: non-deterministic rejection
    ValidationEvent fail2a(ValidationEventType::BLOCK_CONNECTED);
    fail2a.block = block2;
    fail2a.success = false;
    fail2a.error_message = "Invalid coinbase";
    trace.events.push_back(fail2a);

    ValidationEvent fail2b(ValidationEventType::BLOCK_CONNECTED);
    fail2b.block = block2;
    fail2b.success = false;
    fail2b.error_message = "Invalid timestamp";  // Different! (violation)
    trace.events.push_back(fail2b);

    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 1);
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V5.7");
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
