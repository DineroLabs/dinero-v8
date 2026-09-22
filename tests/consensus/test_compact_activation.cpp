#include "consensus/chainparams.h"
#include "consensus/shielded/compact.h"
#include "consensus/shielded/resource_limits.h"
#include "primitives/transaction.h"

#include <gtest/gtest.h>
#include <limits>

namespace {
namespace sh = dinero::consensus::shielded;
using dinero::ChainParams;
using dinero::Transaction;

ChainParams Scheduled(const char* network) {
    ChainParams params{};
    params.name = network;
    params.shielded_activation_height = 1;
    params.shielded_input_binding_activation_height = 2;
    params.shielded_cv_binding_activation_height = 3;
    params.shielded_epoch_reset_height = 3;
    params.shielded_spend_auth_activation_height = 4;
    params.shielded_spend_auth_epoch_reset_height = 4;
    params.shielded_compact_activation_height = 10;
    return params;
}

TEST(CompactActivation, ProductionKeepsTheExistingV6Envelope) {
    EXPECT_TRUE(Transaction::IsShieldedVersion(5));
    EXPECT_TRUE(Transaction::IsShieldedVersion(6));
    EXPECT_TRUE(Transaction::IsShieldedAuthVersion(6));
#ifndef DINERO_ENABLE_COMPACT_REGTEST
    EXPECT_FALSE(Transaction::IsShieldedVersion(0x40000006));
    EXPECT_FALSE(Transaction::IsCompactRegtestVersion(0x40000006));
#endif
}

TEST(CompactActivation, NetworkParametersDriveBoundaryAndReorgWithoutBuildFlag) {
    for (const auto* network : {"mainnet", "testnet", "regtest"}) {
        SCOPED_TRACE(network);
        const auto params = Scheduled(network);
        ASSERT_TRUE(sh::CompactActivationConfigurationValid(params));
        const auto rules = sh::CompactRulesFor(params);
        EXPECT_FALSE(rules.Active(9));
        EXPECT_TRUE(rules.Active(10));
        EXPECT_TRUE(rules.Active(11));
        EXPECT_FALSE(rules.Active(9)); // no sticky process-global activation
        EXPECT_EQ(params.shielded_epoch_reset_height, 3u);
        EXPECT_EQ(params.shielded_spend_auth_epoch_reset_height, 4u);
    }
}

TEST(CompactActivation, DormantSentinelNeverActivatesIncludingAtMaximumHeight) {
    for (const auto* network : {"mainnet", "testnet", "regtest"}) {
        auto params = Scheduled(network);
        params.shielded_compact_activation_height = UINT32_MAX;
        const auto rules = sh::CompactRulesFor(params);
        EXPECT_FALSE(rules.Active(0));
        EXPECT_FALSE(rules.Active(UINT32_MAX));
        EXPECT_FALSE(rules.Active(std::numeric_limits<uint64_t>::max()));
    }
}

TEST(CompactActivation, MissingOrMisorderedCircuitPrerequisitesFailClosed) {
    const auto baseline = Scheduled("mainnet");
    for (auto field : {&ChainParams::shielded_activation_height,
                       &ChainParams::shielded_input_binding_activation_height,
                       &ChainParams::shielded_cv_binding_activation_height,
                       &ChainParams::shielded_spend_auth_activation_height,
                       &ChainParams::shielded_epoch_reset_height,
                       &ChainParams::shielded_spend_auth_epoch_reset_height}) {
        for (uint32_t height : {10u, 11u, UINT32_MAX}) {
            auto params = baseline;
            params.*field = height;
            EXPECT_FALSE(sh::CompactActivationConfigurationValid(params));
            EXPECT_FALSE(sh::CompactRulesFor(params).Active(100));
        }
    }
    auto params = baseline;
    params.shielded_compact_activation_height = 4;
    EXPECT_FALSE(sh::CompactRulesFor(params).Active(4));
    params = baseline;
    params.shielded_input_binding_activation_height = 5;
    EXPECT_FALSE(sh::CompactRulesFor(params).Active(100));
}

TEST(CompactActivation, OrdinaryBuildRejectsCompactBeforeAnyAuthOrBundleBypass) {
    auto params = Scheduled("mainnet");
    Transaction tx;
    tx.version = Transaction::TX_VERSION_SHIELDED_V2;
    tx.SetExplicitFee(0);
    sh::ShieldedBundle bundle;
    bundle.outputs.resize(1);
    bundle.outputs[0].zk_proof = {'D', 'Z', 'E', '1'};
    tx.shielded_bundle_bytes = sh::SerializeShieldedBundle(bundle);
    size_t proofs = 999;
    std::string error;
    const auto rules = sh::CompactRulesFor(params);
    // A compact proof claim cannot bypass activation via the older Auth gate.
    // This cheap gate never substitutes for actual proof verification.
    for (uint64_t height : {0, 3, 9}) {
        EXPECT_FALSE(sh::CheckAuthTransactionResources(tx, height, 4, proofs, error, rules));
        EXPECT_EQ(proofs, 0u);
        EXPECT_FALSE(sh::CheckAuthBlockResources(std::vector<Transaction>{tx}, height, 4, error, rules));
    }
}

TEST(CompactActivation, ScheduledHeightChangesConsensusFingerprint) {
    const auto baseline = Scheduled("mainnet");
    auto other = baseline;
    ++other.shielded_compact_activation_height;
    EXPECT_NE(dinero::ConsensusChecksum(baseline), dinero::ConsensusChecksum(other));
    other.shielded_compact_activation_height = UINT32_MAX;
    EXPECT_NE(dinero::ConsensusChecksum(baseline), dinero::ConsensusChecksum(other));
}
} // namespace
