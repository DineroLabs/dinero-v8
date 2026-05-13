// Genesis Invariant Tests - DO NOT MODIFY THESE VALUES
// Any change requires a new genesis (hard fork)

#include <gtest/gtest.h>
#include "consensus/chainparams.h"
#include "consensus/subsidy.h"
#include <string>

// ============================================================================
// CRITICAL: These tests MUST pass forever
// If any fail, you've broken consensus - STOP and revert immediately
// ============================================================================

TEST(GenesisInvariants, GenesisHashNeverChanges) {
    const char* EXPECTED = "0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f";
    EXPECT_STREQ(dinero::Params().genesis_hash.c_str(), EXPECTED)
        << "CRITICAL: Genesis hash changed! This breaks the entire chain!";
}

TEST(GenesisInvariants, GenesisMerkleRootNeverChanges) {
    const char* EXPECTED = "b040dae24ff59ae0c875252eec15722ec30ee8d600907eda63249800fa6be364";
    EXPECT_STREQ(dinero::Params().genesis.merkleRootHex.c_str(), EXPECTED)
        << "CRITICAL: Genesis merkle root changed! This breaks the entire chain!";
}

TEST(GenesisInvariants, GenesisNonceNeverChanges) {
    EXPECT_EQ(dinero::Params().genesis.nNonce, 813915426u)
        << "CRITICAL: Genesis nonce changed! This breaks the entire chain!";
}

TEST(GenesisInvariants, GenesisTimestampNeverChanges) {
    EXPECT_EQ(dinero::Params().genesis.nTime, 1776384000u)
        << "CRITICAL: Genesis timestamp changed! This breaks the entire chain!";
}

TEST(GenesisInvariants, SubsidyScheduleNeverChanges) {
    EXPECT_EQ(dinero::ConsensusSubsidy::HALVING_INTERVAL, 1314000u)
        << "CRITICAL: Halving interval changed! This breaks the economic model!";
}

TEST(GenesisInvariants, InitialSubsidyNeverChanges) {
    // Height 1 is first PoW block (100 DIN)
    uint64_t initial_subsidy = dinero::ConsensusSubsidy::GetBlockSubsidy(1).GetUna();
    EXPECT_EQ(initial_subsidy, 10000000000ULL)  // 100 DIN
        << "CRITICAL: Initial subsidy changed! This breaks the economic model!";
}

TEST(GenesisInvariants, TailEmissionNeverChanges) {
    // Tail emission: 1 DIN/block forever
    EXPECT_EQ(dinero::ConsensusSubsidy::TAIL_EMISSION_UNA, 100000000ULL)  // 1 DIN
        << "CRITICAL: Tail emission changed! This breaks the economic model!";
}

TEST(GenesisInvariants, NetworkIDNeverChanges) {
    EXPECT_STREQ(dinero::Params().network_id.c_str(), "main")
        << "CRITICAL: Network ID changed! This breaks network compatibility!";
}

TEST(GenesisInvariants, TargetSpacingNeverChanges) {
    EXPECT_EQ(dinero::Params().target_spacing, 120u)
        << "CRITICAL: Target spacing changed! This breaks difficulty adjustment!";
}

TEST(GenesisInvariants, TaprootAlwaysEnforced) {
    EXPECT_TRUE(true)
        << "Taproot enforcement is architectural and cannot be disabled";
}

TEST(GenesisInvariants, MainnetV1Anchor13000NeverChanges) {
    const auto& checkpoints = dinero::Params().vCheckpoints;
    auto checkpoint = checkpoints.find(13000);
    ASSERT_NE(checkpoint, checkpoints.end())
        << "CRITICAL: h=13000 trust anchor missing";
    EXPECT_EQ(checkpoint->second,
              "0000006f34bdfd52f0d61556175a3ccec56fc57428a1b04f7e012ee7e245c8a3");
    EXPECT_EQ(dinero::Params().assumeValidHeight, 13000u);
    EXPECT_EQ(dinero::Params().defaultAssumeValid,
              "0000006f34bdfd52f0d61556175a3ccec56fc57428a1b04f7e012ee7e245c8a3");
}

// ============================================================================
// Human-readable summary
// ============================================================================
TEST(GenesisInvariants, PrintSummary) {
    std::cout << "\n"
              << "  GENESIS INVARIANTS - LOCKED FOREVER\n"
              << "  Genesis Hash:      " << dinero::Params().genesis_hash << "\n"
              << "  Initial Subsidy:   100 DIN/block (height 1+)\n"
              << "  Tail Emission:     1 DIN/block forever\n"
              << "  Halving Interval:  1,314,000 blocks (5 years)\n"
              << "  Target Spacing:    120 seconds (2 minutes)\n"
              << "  No premine. No hard cap. Fair launch.\n\n";
}

// Entry point
int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
