#include <gtest/gtest.h>
#include "validation_oracle_v52.h"
#include <random>

using namespace dinero;
using namespace dinero::consensus::test;

class ValidationOracleV52Test : public ::testing::Test {
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
        tx.lockTime = 0;

        TxInput input;
        input.prevout.txid = TxId(randomHash());
        input.prevout.vout = 0;
        input.sequence = 0xfffffffe;
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
};

TEST_F(ValidationOracleV52Test, EmptyTrace_NoViolations) {
    ValidationOracleV52 oracle;
    ValidationTrace trace(42, "empty");
    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0);
}

TEST_F(ValidationOracleV52Test, ValidPath_NoViolations) {
    ValidationOracleV52 oracle;
    ValidationTrace trace(42, "valid_path");

    Transaction tx = createSimpleTx();
    TxId txid = tx.GetTxid();

    // Event: TX validated successfully
    ValidationEvent validation(ValidationEventType::TX_VALIDATED);
    validation.transaction = tx;
    validation.success = true;
    trace.events.push_back(validation);

    // Event: UTXO added (valid)
    ValidationEvent utxo_add(ValidationEventType::UTXO_ADDED);
    utxo_add.outpoint = OutPoint(txid, 0);
    trace.events.push_back(utxo_add);

    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0) << "Valid transaction application should have no violations";
}

TEST_F(ValidationOracleV52Test, InvalidInput_TriggersViolation) {
    ValidationOracleV52 oracle;
    ValidationTrace trace(42, "invalid_input");

    Transaction tx = createSimpleTx();
    TxId txid = tx.GetTxid();

    // Event: TX validation FAILS
    ValidationEvent validation(ValidationEventType::TX_VALIDATED);
    validation.transaction = tx;
    validation.success = false;
    validation.error_message = "Missing input";
    trace.events.push_back(validation);

    // Event: UTXO added ANYWAY (violation!)
    ValidationEvent utxo_add(ValidationEventType::UTXO_ADDED);
    utxo_add.outpoint = OutPoint(txid, 0);
    trace.events.push_back(utxo_add);

    auto violations = oracle.check(trace);
    EXPECT_GT(violations.size(), 0) << "Invalid transaction causing UTXO changes should be detected";
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V5.2");
    }
}

TEST_F(ValidationOracleV52Test, MixedValidInvalid_DetectsOnlyInvalid) {
    ValidationOracleV52 oracle;
    ValidationTrace trace(42, "mixed");

    Transaction valid_tx = createSimpleTx();
    Transaction invalid_tx = createSimpleTx();

    TxId valid_txid = valid_tx.GetTxid();
    TxId invalid_txid = invalid_tx.GetTxid();

    // Valid transaction
    ValidationEvent valid_validation(ValidationEventType::TX_VALIDATED);
    valid_validation.transaction = valid_tx;
    valid_validation.success = true;
    trace.events.push_back(valid_validation);

    ValidationEvent valid_utxo(ValidationEventType::UTXO_ADDED);
    valid_utxo.outpoint = OutPoint(valid_txid, 0);
    trace.events.push_back(valid_utxo);

    // Invalid transaction
    ValidationEvent invalid_validation(ValidationEventType::TX_VALIDATED);
    invalid_validation.transaction = invalid_tx;
    invalid_validation.success = false;
    invalid_validation.error_message = "Signature failed";
    trace.events.push_back(invalid_validation);

    ValidationEvent invalid_utxo(ValidationEventType::UTXO_ADDED);
    invalid_utxo.outpoint = OutPoint(invalid_txid, 0);
    trace.events.push_back(invalid_utxo);

    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 1) << "Should detect exactly one violation (invalid tx)";
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V5.2");
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
