#include <gtest/gtest.h>
#include "validation_oracle_v47.h"

using namespace dinero;
using namespace dinero::consensus::test;

class ValidationOracleV47Test : public ::testing::Test {};

TEST_F(ValidationOracleV47Test, EmptyTrace_NoViolations) {
    ValidationOracleV47 oracle;
    ValidationTrace trace(42, "empty");
    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0);
}

TEST_F(ValidationOracleV47Test, SingleAddition_NoViolations) {
    ValidationOracleV47 oracle;
    ValidationTrace trace(42, "single_add");

    uint256 hash;
    for (size_t i = 0; i < 32; i++) hash.data[i] = static_cast<uint8_t>(i);

    // Event: UTXO added
    ValidationEvent utxo_add(ValidationEventType::UTXO_ADDED);
    utxo_add.outpoint = OutPoint(TxId(hash), 0);
    trace.events.push_back(utxo_add);

    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0) << "Single addition should have no violations";
}

TEST_F(ValidationOracleV47Test, AddSpendReAdd_NoViolations) {
    ValidationOracleV47 oracle;
    ValidationTrace trace(42, "add_spend_readd");

    uint256 hash;
    for (size_t i = 0; i < 32; i++) hash.data[i] = static_cast<uint8_t>(i);

    // Event: UTXO added
    ValidationEvent utxo_add1(ValidationEventType::UTXO_ADDED);
    utxo_add1.outpoint = OutPoint(TxId(hash), 0);
    trace.events.push_back(utxo_add1);

    // Event: UTXO spent
    ValidationEvent utxo_spent(ValidationEventType::UTXO_SPENT);
    utxo_spent.outpoint = OutPoint(TxId(hash), 0);
    trace.events.push_back(utxo_spent);

    // Event: UTXO re-added (e.g., after reorg)
    ValidationEvent utxo_add2(ValidationEventType::UTXO_ADDED);
    utxo_add2.outpoint = OutPoint(TxId(hash), 0);
    trace.events.push_back(utxo_add2);

    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0) << "Add→Spend→ReAdd should have no violations";
}

TEST_F(ValidationOracleV47Test, DuplicateAddition_DetectsViolation) {
    ValidationOracleV47 oracle;
    ValidationTrace trace(42, "duplicate_add");

    uint256 hash;
    for (size_t i = 0; i < 32; i++) hash.data[i] = static_cast<uint8_t>(i);

    // Event: UTXO added
    ValidationEvent utxo_add1(ValidationEventType::UTXO_ADDED);
    utxo_add1.outpoint = OutPoint(TxId(hash), 0);
    trace.events.push_back(utxo_add1);

    // Event: UTXO added AGAIN (violation - no spend in between!)
    ValidationEvent utxo_add2(ValidationEventType::UTXO_ADDED);
    utxo_add2.outpoint = OutPoint(TxId(hash), 0);
    trace.events.push_back(utxo_add2);

    auto violations = oracle.check(trace);
    EXPECT_GT(violations.size(), 0) << "Duplicate addition should be detected";
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V4.7");
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
