#include <gtest/gtest.h>
#include "validation_oracle_v45.h"

using namespace dinero;
using namespace dinero::consensus::test;

class ValidationOracleV45Test : public ::testing::Test {};

TEST_F(ValidationOracleV45Test, EmptyTrace_NoViolations) {
    ValidationOracleV45 oracle;
    ValidationTrace trace(42, "empty");
    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0);
}

TEST_F(ValidationOracleV45Test, ConnectThenDisconnect_RestoresState) {
    ValidationOracleV45 oracle;
    ValidationTrace trace(42, "connect_disconnect");

    // Event: Block connected
    ValidationEvent connect(ValidationEventType::BLOCK_CONNECTED);
    trace.events.push_back(connect);

    // Event: UTXO added
    uint256 hash;
    for (size_t i = 0; i < 32; i++) hash.data[i] = static_cast<uint8_t>(i);
    ValidationEvent utxo_add(ValidationEventType::UTXO_ADDED);
    utxo_add.outpoint = OutPoint(TxId(hash), 0);
    trace.events.push_back(utxo_add);

    // Event: UTXO removed (rollback happens BEFORE disconnect event)
    ValidationEvent utxo_spent(ValidationEventType::UTXO_SPENT);
    utxo_spent.outpoint = OutPoint(TxId(hash), 0);
    trace.events.push_back(utxo_spent);

    // Event: Block disconnected (reorg) - comes AFTER rollback
    ValidationEvent disconnect(ValidationEventType::BLOCK_DISCONNECTED);
    trace.events.push_back(disconnect);

    auto violations = oracle.check(trace);
    EXPECT_EQ(violations.size(), 0) << "Proper reorg should have no violations";
}

TEST_F(ValidationOracleV45Test, ReorgWithoutProperRollback_DetectsViolation) {
    ValidationOracleV45 oracle;
    ValidationTrace trace(42, "incomplete_rollback");

    // Event: Block connected
    ValidationEvent connect(ValidationEventType::BLOCK_CONNECTED);
    trace.events.push_back(connect);

    // Event: UTXO added
    uint256 hash;
    for (size_t i = 0; i < 32; i++) hash.data[i] = static_cast<uint8_t>(i);
    ValidationEvent utxo_add(ValidationEventType::UTXO_ADDED);
    utxo_add.outpoint = OutPoint(TxId(hash), 0);
    trace.events.push_back(utxo_add);

    // Event: Block disconnected (reorg)
    ValidationEvent disconnect(ValidationEventType::BLOCK_DISCONNECTED);
    trace.events.push_back(disconnect);

    // NO UTXO_SPENT event (violation - UTXO wasn't rolled back!)

    auto violations = oracle.check(trace);
    EXPECT_GT(violations.size(), 0) << "Incomplete rollback should be detected";
    if (!violations.empty()) {
        EXPECT_EQ(violations[0].property, "V4.5");
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
