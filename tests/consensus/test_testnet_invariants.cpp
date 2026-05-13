// Testnet Genesis Invariant Tests - Verifies test infrastructure catches changes
//
// Purpose: Demonstrate that the test infrastructure actually works by using
// current mainnet parameters. If someone changes a consensus constant,
// these tests will fail and block the change.

#include <gtest/gtest.h>
#include "consensus/chainparams.h"
#include "consensus/subsidy.h"
#include <string>

// ============================================================================
// INVARIANT VERIFICATION TESTS
// ============================================================================

TEST(TestnetInvariants, GenesisHashVerification) {
    const char* EXPECTED = "0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f";
    EXPECT_STREQ(dinero::Params().genesis_hash.c_str(), EXPECTED)
        << "Genesis hash must match. Any change breaks consensus.";
}

TEST(TestnetInvariants, SubsidyScheduleVerification) {
    uint32_t CURRENT_HALVING = 1314000u;
    EXPECT_EQ(dinero::ConsensusSubsidy::HALVING_INTERVAL, CURRENT_HALVING)
        << "Halving interval must be 1,314,000 blocks.";
}

TEST(TestnetInvariants, TailEmissionVerification) {
    uint64_t EXPECTED_TAIL = 100000000ULL;  // 1 DIN in una
    EXPECT_EQ(dinero::ConsensusSubsidy::TAIL_EMISSION_UNA, EXPECTED_TAIL)
        << "Tail emission must be 1 DIN/block forever.";
}

TEST(TestnetInvariants, NetworkIDVerification) {
    const char* CURRENT_NETWORK = "main";
    EXPECT_STREQ(dinero::Params().network_id.c_str(), CURRENT_NETWORK)
        << "Network ID must be 'main'.";
}

TEST(TestnetInvariants, TargetSpacingVerification) {
    uint32_t CURRENT_SPACING = 120u;
    EXPECT_EQ(dinero::Params().target_spacing, CURRENT_SPACING)
        << "Target spacing must be 120 seconds.";
}

TEST(TestnetInvariants, InitialSubsidyVerification) {
    // Height 1 is first PoW block (100 DIN)
    uint64_t initial_subsidy = dinero::ConsensusSubsidy::GetBlockSubsidy(1).GetUna();
    EXPECT_EQ(initial_subsidy, 10000000000ULL)  // 100 DIN
        << "Initial subsidy must be 100 DIN at height 1.";
}

TEST(TestnetInvariants, NoPremineVerification) {
    // Genesis (height 0) has zero spendable subsidy
    uint64_t genesis_subsidy = dinero::ConsensusSubsidy::GetBlockSubsidy(0).GetUna();
    EXPECT_EQ(genesis_subsidy, 0ULL)
        << "Genesis must have zero spendable subsidy (no premine).";

    // Height 1 is regular PoW (100 DIN), not a premine
    uint64_t h1_subsidy = dinero::ConsensusSubsidy::GetBlockSubsidy(1).GetUna();
    EXPECT_EQ(h1_subsidy, 10000000000ULL)  // 100 DIN
        << "Height 1 must be regular PoW (100 DIN), not a premine.";
}

// ============================================================================
// Verification Summary
// ============================================================================
TEST(TestnetInvariants, PrintVerificationSummary) {
    std::cout << "\n"
              << "  INVARIANT VERIFICATION TESTS\n"
              << "  Purpose: Prove consensus lock enforcement works\n"
              << "  Current values (all tests passing = no drift):\n"
              << "    Genesis Hash:      " << dinero::Params().genesis_hash << "\n"
              << "    Network ID:        " << dinero::Params().network_id << "\n"
              << "    Halving Interval:  " << dinero::ConsensusSubsidy::HALVING_INTERVAL << " blocks\n"
              << "    Target Spacing:    " << dinero::Params().target_spacing << " seconds\n"
              << "    Tail Emission:     1 DIN/block forever\n"
              << "    No premine. Fair launch.\n\n";
}

// Entry point
int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
