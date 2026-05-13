#include <gtest/gtest.h>
#include "validation_oracle_v46.h"

using namespace dinero;
using namespace dinero::consensus::test;

class ValidationOracleV46Test : public ::testing::Test {};

TEST_F(ValidationOracleV46Test, EmptyTrace_NoViolations) {
    ValidationOracleV46 oracle;
    ValidationTrace trace(42, "empty");
    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0);
}

TEST_F(ValidationOracleV46Test, MatureCoinbase_NoViolations) {
    ValidationOracleV46 oracle;
    ValidationTrace trace(42, "mature_coinbase");

    // Add a coinbase UTXO at height 100
    uint256 hash;
    for (size_t i = 0; i < 32; i++) hash.data[i] = static_cast<uint8_t>(i);

    ValidationEvent utxo_add(ValidationEventType::UTXO_ADDED);
    utxo_add.outpoint = OutPoint(TxId(hash), 0);
    dinero::consensus::UTXOEntry coin;
    coin.value = AmountUna::Una(10000000000ULL);  // 100 DIN
    coin.height = 100;
    coin.isCoinbase = true;
    utxo_add.coin = coin;
    trace.events.push_back(utxo_add);

    // Create state at height 200 (100 blocks later - mature!)
    ValidationState state;
    state.height = 200;
    trace.snapshots.push_back(state);

    // Spend the coinbase at height 200
    ValidationEvent utxo_spent(ValidationEventType::UTXO_SPENT);
    utxo_spent.outpoint = OutPoint(TxId(hash), 0);
    trace.events.push_back(utxo_spent);

    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0) << "Mature coinbase spend should have no violations";
}

TEST_F(ValidationOracleV46Test, ImmatureCoinbase_DetectsViolation) {
    ValidationOracleV46 oracle;
    ValidationTrace trace(42, "immature_coinbase");

    // Add a coinbase UTXO at height 100
    uint256 hash;
    for (size_t i = 0; i < 32; i++) hash.data[i] = static_cast<uint8_t>(i);

    ValidationEvent utxo_add(ValidationEventType::UTXO_ADDED);
    utxo_add.outpoint = OutPoint(TxId(hash), 0);
    dinero::consensus::UTXOEntry coin;
    coin.value = AmountUna::Una(10000000000ULL);  // 100 DIN
    coin.height = 100;
    coin.isCoinbase = true;
    utxo_add.coin = coin;
    trace.events.push_back(utxo_add);

    // Create state at height 150 (only 50 blocks later - immature!)
    ValidationState state;
    state.height = 150;
    trace.snapshots.push_back(state);

    // Try to spend the coinbase at height 150 (violation!)
    ValidationEvent utxo_spent(ValidationEventType::UTXO_SPENT);
    utxo_spent.outpoint = OutPoint(TxId(hash), 0);
    trace.events.push_back(utxo_spent);

    auto violations = oracle.check(trace);
    EXPECT_GT(violations.size(), 0) << "Immature coinbase spend should be detected";
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V4.6");
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
