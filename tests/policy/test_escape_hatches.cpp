/**
 * @file test_escape_hatches.cpp
 * @brief Escape Hatch Integration Tests (Phase F.4)
 *
 * Purpose: Verify escape hatch flags allow what policy permits and block correctly
 *
 * Architecture:
 * - Pure integration tests (no real daemon, just policy functions)
 * - Test that flags correctly override policy decisions
 * - Fast tests (0ms runtime, like tripwire tests)
 *
 * Test Coverage:
 * - --allow-external-mining: Allows mining to external addresses
 * - --mine-during-ibd: Allows mining during IBD
 * - Absence of flags: Blocks correctly
 *
 * Contract: Escape hatches bypass safety checks (user accepts risk)
 */

#include <gtest/gtest.h>
#include "rpc/mining_policy.h"

using namespace dinero::rpc;

// ═══════════════════════════════════════════════════════════════════
// TEST SUITE 1: --allow-external-mining Flag
// ═══════════════════════════════════════════════════════════════════

TEST(EscapeHatchTests, AllowExternalMining_EnablesExternalAddressMining) {
    // Setup: Wallet exists but address is NOT owned
    WalletPolicyView wallet{
        .has_active_wallet = true,
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = false,  // ❌ Foreign address
        .wallet_name = "test_wallet"
    };

    ChainPolicyView chain{
        .is_initial_block_download = false,
        .is_reindexing = false,
        .chainstate_ready = true,
        .current_height = 100,
        .total_blocks = 100
    };

    // Config: Escape hatch ENABLED
    MiningPolicyConfig config{
        .allow_external_mining = true,  // ✅ Escape hatch!
        .skip_ibd_check = false
    };

    // Execute policy check
    auto result = CheckMiningStartPolicy(wallet, chain, config);

    // Verify: Policy allows mining (escape hatch overrides E.1.1)
    EXPECT_TRUE(result.allowed)
        << "Escape hatch should allow mining to external address";
    EXPECT_EQ(0, result.error_code);
}

TEST(EscapeHatchTests, AllowExternalMining_EnablesMiningWithoutWallet) {
    // Setup: NO wallet loaded
    WalletPolicyView wallet{
        .has_active_wallet = false,  // ❌ No wallet
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = false,
        .wallet_name = ""
    };

    ChainPolicyView chain{
        .is_initial_block_download = false,
        .is_reindexing = false,
        .chainstate_ready = true,
        .current_height = 100,
        .total_blocks = 100
    };

    // Config: Escape hatch ENABLED
    MiningPolicyConfig config{
        .allow_external_mining = true,  // ✅ Escape hatch!
        .skip_ibd_check = false
    };

    // Execute policy check
    auto result = CheckMiningStartPolicy(wallet, chain, config);

    // Verify: Policy allows mining (escape hatch overrides E.1.3)
    EXPECT_TRUE(result.allowed)
        << "Escape hatch should allow mining without wallet";
    EXPECT_EQ(0, result.error_code);
}

TEST(EscapeHatchTests, WithoutAllowExternalMining_BlocksExternalAddress) {
    // Setup: Wallet exists but address is NOT owned
    WalletPolicyView wallet{
        .has_active_wallet = true,
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = false,  // ❌ Foreign address
        .wallet_name = "test_wallet"
    };

    ChainPolicyView chain{
        .is_initial_block_download = false,
        .is_reindexing = false,
        .chainstate_ready = true,
        .current_height = 100,
        .total_blocks = 100
    };

    // Config: Escape hatch DISABLED (default)
    MiningPolicyConfig config{
        .allow_external_mining = false,  // ❌ No escape hatch
        .skip_ibd_check = false
    };

    // Execute policy check
    auto result = CheckMiningStartPolicy(wallet, chain, config);

    // Verify: Policy blocks mining (E.1.1 enforced)
    EXPECT_FALSE(result.allowed)
        << "Without escape hatch, external address should be blocked";
    EXPECT_EQ(-13, result.error_code);
    EXPECT_NE(std::string::npos, result.error_message.find("not owned"))
        << "Error message should mention address not owned";
}

// ═══════════════════════════════════════════════════════════════════
// TEST SUITE 2: --mine-during-ibd Flag
// ═══════════════════════════════════════════════════════════════════

TEST(EscapeHatchTests, MineDuringIBD_EnablesMiningDuringSync) {
    // Setup: Wallet OK, but chain is in IBD
    WalletPolicyView wallet{
        .has_active_wallet = true,
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = true,
        .wallet_name = "test_wallet"
    };

    ChainPolicyView chain{
        .is_initial_block_download = true,  // ❌ Syncing!
        .is_reindexing = false,
        .chainstate_ready = true,
        .current_height = 12345,
        .total_blocks = 273891
    };

    // Config: Escape hatch ENABLED
    MiningPolicyConfig config{
        .allow_external_mining = false,
        .skip_ibd_check = true  // ✅ Escape hatch!
    };

    // Execute policy check
    auto result = CheckMiningStartPolicy(wallet, chain, config);

    // Verify: Policy allows mining (escape hatch overrides E.2.1)
    EXPECT_TRUE(result.allowed)
        << "Escape hatch should allow mining during IBD";
    EXPECT_EQ(0, result.error_code);
}

TEST(EscapeHatchTests, WithoutMineDuringIBD_BlocksMiningDuringSync) {
    // Setup: Wallet OK, but chain is in IBD
    WalletPolicyView wallet{
        .has_active_wallet = true,
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = true,
        .wallet_name = "test_wallet"
    };

    ChainPolicyView chain{
        .is_initial_block_download = true,  // ❌ Syncing!
        .is_reindexing = false,
        .chainstate_ready = true,
        .current_height = 12345,
        .total_blocks = 273891
    };

    // Config: Escape hatch DISABLED (default)
    MiningPolicyConfig config{
        .allow_external_mining = false,
        .skip_ibd_check = false  // ❌ No escape hatch
    };

    // Execute policy check
    auto result = CheckMiningStartPolicy(wallet, chain, config);

    // Verify: Policy blocks mining (E.2.1 enforced)
    EXPECT_FALSE(result.allowed)
        << "Without escape hatch, mining during IBD should be blocked";
    EXPECT_EQ(-10, result.error_code);
    EXPECT_NE(std::string::npos, result.error_message.find("initial block download"))
        << "Error message should mention IBD";
}

// ═══════════════════════════════════════════════════════════════════
// TEST SUITE 3: Reindex Cannot Be Bypassed (No Escape Hatch)
// ═══════════════════════════════════════════════════════════════════

TEST(EscapeHatchTests, Reindex_CannotBeBypassed_EvenWithFlags) {
    // Setup: Wallet OK, chain is reindexing
    WalletPolicyView wallet{
        .has_active_wallet = true,
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = true,
        .wallet_name = "test_wallet"
    };

    ChainPolicyView chain{
        .is_initial_block_download = false,
        .is_reindexing = true,  // ❌ Reindexing (chainstate inconsistent)
        .chainstate_ready = true,
        .current_height = 100,
        .total_blocks = 100
    };

    // Config: ALL escape hatches ENABLED
    MiningPolicyConfig config{
        .allow_external_mining = true,
        .skip_ibd_check = true  // ✅ All flags enabled
    };

    // Execute policy check
    auto result = CheckMiningStartPolicy(wallet, chain, config);

    // Verify: Policy STILL blocks mining (reindex has no escape hatch)
    EXPECT_FALSE(result.allowed)
        << "Reindex cannot be bypassed, even with escape hatches";
    EXPECT_EQ(-10, result.error_code);
    EXPECT_NE(std::string::npos, result.error_message.find("reindex"))
        << "Error message should mention reindex";
}

// ═══════════════════════════════════════════════════════════════════
// TEST SUITE 4: Combined Escape Hatches
// ═══════════════════════════════════════════════════════════════════

TEST(EscapeHatchTests, BothEscapeHatches_AllowExternalMiningDuringIBD) {
    // Setup: External address, chain in IBD
    WalletPolicyView wallet{
        .has_active_wallet = true,
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = false,  // ❌ Foreign address
        .wallet_name = "test_wallet"
    };

    ChainPolicyView chain{
        .is_initial_block_download = true,  // ❌ Syncing
        .is_reindexing = false,
        .chainstate_ready = true,
        .current_height = 12345,
        .total_blocks = 273891
    };

    // Config: BOTH escape hatches ENABLED
    MiningPolicyConfig config{
        .allow_external_mining = true,  // ✅ Escape hatch 1
        .skip_ibd_check = true          // ✅ Escape hatch 2
    };

    // Execute policy check
    auto result = CheckMiningStartPolicy(wallet, chain, config);

    // Verify: Policy allows mining (both escape hatches active)
    EXPECT_TRUE(result.allowed)
        << "Both escape hatches should allow external mining during IBD";
    EXPECT_EQ(0, result.error_code);
}

TEST(EscapeHatchTests, WithoutEscapeHatches_BlocksExternalMiningDuringIBD) {
    // Setup: External address, chain in IBD
    WalletPolicyView wallet{
        .has_active_wallet = true,
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = false,  // ❌ Foreign address
        .wallet_name = "test_wallet"
    };

    ChainPolicyView chain{
        .is_initial_block_download = true,  // ❌ Syncing
        .is_reindexing = false,
        .chainstate_ready = true,
        .current_height = 12345,
        .total_blocks = 273891
    };

    // Config: NO escape hatches (default)
    MiningPolicyConfig config{
        .allow_external_mining = false,  // ❌ No escape hatch 1
        .skip_ibd_check = false          // ❌ No escape hatch 2
    };

    // Execute policy check
    auto result = CheckMiningStartPolicy(wallet, chain, config);

    // Verify: Policy blocks mining (E.1.1 checked first, blocks before E.2.1)
    EXPECT_FALSE(result.allowed)
        << "Without escape hatches, should block (address ownership checked first)";
    EXPECT_EQ(-13, result.error_code);  // E.1.1 error (-13) comes before E.2.1 error (-10)
}

// ═══════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
