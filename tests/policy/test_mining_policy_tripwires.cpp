/**
 * @file test_mining_policy_tripwires.cpp
 * @brief Mining Policy Tripwire Tests (Phase E.1 + E.2)
 *
 * Purpose: Ensure mining policy contracts cannot be weakened
 *
 * CRITICAL: These tests MUST NEVER be modified to "make them pass"
 * If a test fails, it means a policy contract was violated.
 * FIX THE CODE, NOT THE TEST.
 *
 * Tripwires for Phase E.1 - Wallet Ownership Enforcement:
 * - E.1.1: Mining MUST reject non-owned addresses
 * - E.1.2: Mining MUST reject locked wallets
 * - E.1.3: Mining MUST reject missing wallets
 *
 * Tripwires for Phase E.2 - IBD & Sync Safety:
 * - E.2.1: Mining MUST be blocked during IBD
 * - E.2.1: Mining MUST be blocked during reindex
 * - E.2.2: Mining MUST reject unavailable chainstate
 *
 * Architecture:
 * - Tests operate on POLICY VIEWS (pure state), not implementations
 * - No WalletManager initialization (no SQLite, no seeds, no encryption)
 * - No ChainstateService (no blocks, no headers, no sync logic)
 * - Tests inject views directly into CheckMiningStartPolicy()
 * - Fast, deterministic, impossible to weaken
 *
 * This is NOT an integration test. Integration tests come later (Phase E.4).
 */

#include <gtest/gtest.h>
#include "rpc/mining_policy.h"

using namespace dinero::rpc;

/**
 * Test Fixture for Mining Policy Tripwires
 *
 * Sets up policy views WITHOUT touching any real services
 */
class MiningPolicyTripwiresTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Default chain view (no IBD, chainstate ready)
        chain_view_ = ChainPolicyView{
            .is_initial_block_download = false,
            .is_reindexing = false,
            .chainstate_ready = true,
            .current_height = 100,
            .total_blocks = 100
        };

        // Default policy config (no escape hatches)
        policy_config_ = MiningPolicyConfig{
            .allow_external_mining = false,
            .skip_ibd_check = false
        };
    }

    ChainPolicyView chain_view_;
    MiningPolicyConfig policy_config_;
};

// ═══════════════════════════════════════════════════════════════════
// TRIPWIRE E.1.1: Mining MUST reject non-owned addresses
// ═══════════════════════════════════════════════════════════════════

/**
 * Tripwire: Mining with foreign address MUST be rejected
 *
 * Contract: E.1.1 - Wallet Ownership Enforcement
 *
 * If this test fails, it means:
 * - The ownership check was removed from CheckMiningStartPolicy()
 * - OR The policy contract was weakened
 *
 * DO NOT modify this test to make it pass - fix the code instead!
 */
TEST_F(MiningPolicyTripwiresTest, MiningWithForeignAddressMustBeRejected) {
    // Setup: Wallet is loaded and unlocked, but address is NOT owned
    WalletPolicyView wallet_view{
        .has_active_wallet = true,
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = false,  // ❌ Foreign address
        .wallet_name = "test_wallet"
    };

    // Execute: Check mining start policy
    auto result = CheckMiningStartPolicy(wallet_view, chain_view_, policy_config_);

    // TRIPWIRE: This MUST return false (not allowed)
    EXPECT_FALSE(result.allowed)
        << "CRITICAL FAILURE: Mining allowed with foreign address!\\n"
        << "This breaks E.1.1 contract: Mining address ownership enforcement.\\n"
        << "DO NOT modify this test - fix CheckMiningStartPolicy() instead!";

    EXPECT_EQ(-13, result.error_code)
        << "Wrong error code for foreign address rejection";

    EXPECT_NE(std::string::npos, result.error_message.find("not owned"))
        << "Error message should mention address ownership";
}

/**
 * Tripwire: Mining with owned address MUST be accepted (positive test)
 *
 * This proves policy correctly allows owned addresses.
 */
TEST_F(MiningPolicyTripwiresTest, MiningWithOwnedAddressShouldSucceed) {
    // Setup: Wallet is loaded, unlocked, and owns the address
    WalletPolicyView wallet_view{
        .has_active_wallet = true,
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = true,  // ✅ Owned address
        .wallet_name = "test_wallet"
    };

    // Execute: Check mining start policy
    auto result = CheckMiningStartPolicy(wallet_view, chain_view_, policy_config_);

    // Verify: Policy allows this
    EXPECT_TRUE(result.allowed)
        << "FAILURE: Policy rejected owned address!\\n"
        << "Error: " << result.error_message;
}

// ═══════════════════════════════════════════════════════════════════
// TRIPWIRE E.1.2: Mining MUST reject locked encrypted wallets
// ═══════════════════════════════════════════════════════════════════

/**
 * Tripwire: Mining with locked wallet MUST be rejected
 *
 * Contract: E.1.3 - Wallet Encryption Safety
 *
 * If this test fails, it means:
 * - The wallet lock check was removed from CheckMiningStartPolicy()
 * - OR The policy contract was weakened
 *
 * DO NOT modify this test to make it pass - fix the code instead!
 */
TEST_F(MiningPolicyTripwiresTest, MiningWithLockedWalletMustBeRejected) {
    // Setup: Wallet is loaded, encrypted, and LOCKED
    WalletPolicyView wallet_view{
        .has_active_wallet = true,
        .wallet_encrypted = true,   // ✅ Encrypted
        .wallet_unlocked = false,   // ❌ Locked
        .address_owned = true,      // Address is owned (but wallet locked)
        .wallet_name = "test_wallet"
    };

    // Execute: Check mining start policy
    auto result = CheckMiningStartPolicy(wallet_view, chain_view_, policy_config_);

    // TRIPWIRE: This MUST return false (not allowed)
    EXPECT_FALSE(result.allowed)
        << "CRITICAL FAILURE: Mining allowed with locked wallet!\\n"
        << "This breaks E.1.3 contract: Wallet encryption safety.\\n"
        << "DO NOT modify this test - fix CheckMiningStartPolicy() instead!";

    EXPECT_EQ(-13, result.error_code)
        << "Wrong error code for locked wallet rejection";

    EXPECT_NE(std::string::npos, result.error_message.find("locked"))
        << "Error message should mention wallet is locked";
}

/**
 * Tripwire: Mining with unlocked encrypted wallet SHOULD succeed
 *
 * This proves wallet unlock is correctly detected.
 */
TEST_F(MiningPolicyTripwiresTest, MiningWithUnlockedEncryptedWalletShouldSucceed) {
    // Setup: Wallet is loaded, encrypted, and UNLOCKED
    WalletPolicyView wallet_view{
        .has_active_wallet = true,
        .wallet_encrypted = true,   // ✅ Encrypted
        .wallet_unlocked = true,    // ✅ Unlocked
        .address_owned = true,      // ✅ Address owned
        .wallet_name = "test_wallet"
    };

    // Execute: Check mining start policy
    auto result = CheckMiningStartPolicy(wallet_view, chain_view_, policy_config_);

    // Verify: Policy allows this
    EXPECT_TRUE(result.allowed)
        << "FAILURE: Policy rejected unlocked encrypted wallet!\\n"
        << "Error: " << result.error_message;
}

// ═══════════════════════════════════════════════════════════════════
// TRIPWIRE E.1.3: Mining MUST reject missing wallet
// ═══════════════════════════════════════════════════════════════════

/**
 * Tripwire: Mining without active wallet MUST be rejected
 *
 * Contract: E.1.2 - Wallet Load State
 *
 * If this test fails, it means:
 * - The wallet presence check was removed from CheckMiningStartPolicy()
 * - OR The policy contract was weakened
 *
 * DO NOT modify this test to make it pass - fix the code instead!
 */
TEST_F(MiningPolicyTripwiresTest, MiningWithoutWalletMustBeRejected) {
    // Setup: NO active wallet
    WalletPolicyView wallet_view{
        .has_active_wallet = false,  // ❌ No wallet
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = false,
        .wallet_name = ""
    };

    // Execute: Check mining start policy
    auto result = CheckMiningStartPolicy(wallet_view, chain_view_, policy_config_);

    // TRIPWIRE: This MUST return false (not allowed)
    EXPECT_FALSE(result.allowed)
        << "CRITICAL FAILURE: Mining allowed without active wallet!\\n"
        << "This breaks E.1.2 contract: Wallet load state enforcement.\\n"
        << "DO NOT modify this test - fix CheckMiningStartPolicy() instead!";

    EXPECT_EQ(-13, result.error_code)
        << "Wrong error code for missing wallet rejection";

    EXPECT_NE(std::string::npos, result.error_message.find("active wallet"))
        << "Error message should mention missing active wallet";
}

// ═══════════════════════════════════════════════════════════════════
// TRIPWIRE E.1.3 (escape hatch): --allow-external-mining flag
// ═══════════════════════════════════════════════════════════════════

/**
 * Tripwire: --allow-external-mining MUST bypass ownership check (dangerous)
 *
 * Contract: E.1.1 Exception - External Mining Escape Hatch
 *
 * This proves the escape hatch works when explicitly enabled.
 */
TEST_F(MiningPolicyTripwiresTest, AllowExternalMiningBypassesOwnershipCheck) {
    // Setup: Foreign address, but --allow-external-mining enabled
    WalletPolicyView wallet_view{
        .has_active_wallet = true,
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = false,  // ❌ Foreign address
        .wallet_name = "test_wallet"
    };

    MiningPolicyConfig policy_config{
        .allow_external_mining = true,  // ✅ Escape hatch enabled
        .skip_ibd_check = false
    };

    // Execute: Check mining start policy
    auto result = CheckMiningStartPolicy(wallet_view, chain_view_, policy_config);

    // Verify: Policy allows this (dangerous, but user opted in)
    EXPECT_TRUE(result.allowed)
        << "FAILURE: --allow-external-mining flag not working!\\n"
        << "Error: " << result.error_message;
}

/**
 * Tripwire: --allow-external-mining MUST work without wallet
 *
 * Contract: E.1.1 Exception - External Mining Escape Hatch
 */
TEST_F(MiningPolicyTripwiresTest, AllowExternalMiningWorksWithoutWallet) {
    // Setup: NO wallet, but --allow-external-mining enabled
    WalletPolicyView wallet_view{
        .has_active_wallet = false,  // ❌ No wallet
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = false,
        .wallet_name = ""
    };

    MiningPolicyConfig policy_config{
        .allow_external_mining = true,  // ✅ Escape hatch enabled
        .skip_ibd_check = false
    };

    // Execute: Check mining start policy
    auto result = CheckMiningStartPolicy(wallet_view, chain_view_, policy_config);

    // Verify: Policy allows this (dangerous, but user opted in)
    EXPECT_TRUE(result.allowed)
        << "FAILURE: --allow-external-mining flag not working without wallet!\\n"
        << "Error: " << result.error_message;
}

// ═══════════════════════════════════════════════════════════════════
// TRIPWIRE E.2.1: Mining MUST be blocked during IBD
// ═══════════════════════════════════════════════════════════════════

/**
 * Tripwire: Mining during IBD MUST be rejected
 *
 * Contract: E.2.1 - Initial Block Download Safety
 *
 * If this test fails, it means:
 * - The IBD check was removed from CheckMiningStartPolicy()
 * - OR The policy contract was weakened
 *
 * DO NOT modify this test to make it pass - fix the code instead!
 */
TEST_F(MiningPolicyTripwiresTest, MiningDuringIBDMustBeRejected) {
    // Setup: Normal wallet, but chain is in IBD
    WalletPolicyView wallet_view{
        .has_active_wallet = true,
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = true,
        .wallet_name = "test_wallet"
    };

    ChainPolicyView chain_view{
        .is_initial_block_download = true,  // ❌ Syncing
        .is_reindexing = false,
        .chainstate_ready = true,
        .current_height = 12345,
        .total_blocks = 273891
    };

    // Execute: Check mining start policy
    auto result = CheckMiningStartPolicy(wallet_view, chain_view, policy_config_);

    // TRIPWIRE: This MUST return false (not allowed)
    EXPECT_FALSE(result.allowed)
        << "CRITICAL FAILURE: Mining allowed during IBD!\\n"
        << "This breaks E.2.1 contract: IBD safety enforcement.\\n"
        << "DO NOT modify this test - fix CheckMiningStartPolicy() instead!";

    EXPECT_EQ(-10, result.error_code)
        << "Wrong error code for IBD rejection (should be -10, Bitcoin Core compatible)";

    EXPECT_NE(std::string::npos, result.error_message.find("initial block download"))
        << "Error message should mention IBD";
}

/**
 * Tripwire: Mining after IBD completes SHOULD succeed
 *
 * This proves IBD detection works correctly.
 */
TEST_F(MiningPolicyTripwiresTest, MiningAfterIBDShouldSucceed) {
    // Setup: Normal wallet, chain fully synced
    WalletPolicyView wallet_view{
        .has_active_wallet = true,
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = true,
        .wallet_name = "test_wallet"
    };

    ChainPolicyView chain_view{
        .is_initial_block_download = false,  // ✅ Synced
        .is_reindexing = false,
        .chainstate_ready = true,
        .current_height = 273891,
        .total_blocks = 273891
    };

    // Execute: Check mining start policy
    auto result = CheckMiningStartPolicy(wallet_view, chain_view, policy_config_);

    // Verify: Policy allows this
    EXPECT_TRUE(result.allowed)
        << "FAILURE: Policy rejected mining after IBD completed!\\n"
        << "Error: " << result.error_message;
}

// ═══════════════════════════════════════════════════════════════════
// TRIPWIRE E.2.1: Mining MUST be blocked during reindex
// ═══════════════════════════════════════════════════════════════════

/**
 * Tripwire: Mining during reindex MUST be rejected
 *
 * Contract: E.2.1 - Reindex Safety
 *
 * If this test fails, it means:
 * - The reindex check was removed from CheckMiningStartPolicy()
 * - OR The policy contract was weakened
 *
 * DO NOT modify this test to make it pass - fix the code instead!
 */
TEST_F(MiningPolicyTripwiresTest, MiningDuringReindexMustBeRejected) {
    // Setup: Normal wallet, but chain is reindexing
    WalletPolicyView wallet_view{
        .has_active_wallet = true,
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = true,
        .wallet_name = "test_wallet"
    };

    ChainPolicyView chain_view{
        .is_initial_block_download = false,
        .is_reindexing = true,  // ❌ Reindexing
        .chainstate_ready = true,
        .current_height = 100000,
        .total_blocks = 100000
    };

    // Execute: Check mining start policy
    auto result = CheckMiningStartPolicy(wallet_view, chain_view, policy_config_);

    // TRIPWIRE: This MUST return false (not allowed)
    EXPECT_FALSE(result.allowed)
        << "CRITICAL FAILURE: Mining allowed during reindex!\\n"
        << "This breaks E.2.1 contract: Reindex safety enforcement.\\n"
        << "DO NOT modify this test - fix CheckMiningStartPolicy() instead!";

    EXPECT_EQ(-10, result.error_code)
        << "Wrong error code for reindex rejection (should be -10, Bitcoin Core compatible)";

    EXPECT_NE(std::string::npos, result.error_message.find("reindex"))
        << "Error message should mention reindex";
}

// ═══════════════════════════════════════════════════════════════════
// TRIPWIRE E.2.2: Mining MUST reject unavailable chainstate
// ═══════════════════════════════════════════════════════════════════

/**
 * Tripwire: Mining with unavailable chainstate MUST be rejected
 *
 * Contract: E.2.2 - Chainstate Consistency
 *
 * If this test fails, it means:
 * - The chainstate ready check was removed from CheckMiningStartPolicy()
 * - OR The policy contract was weakened
 *
 * DO NOT modify this test to make it pass - fix the code instead!
 */
TEST_F(MiningPolicyTripwiresTest, MiningWithUnavailableChainstateRejected) {
    // Setup: Normal wallet, but chainstate not ready
    WalletPolicyView wallet_view{
        .has_active_wallet = true,
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = true,
        .wallet_name = "test_wallet"
    };

    ChainPolicyView chain_view{
        .is_initial_block_download = false,
        .is_reindexing = false,
        .chainstate_ready = false,  // ❌ Chainstate not ready
        .current_height = 0,
        .total_blocks = 0
    };

    // Execute: Check mining start policy
    auto result = CheckMiningStartPolicy(wallet_view, chain_view, policy_config_);

    // TRIPWIRE: This MUST return false (not allowed)
    EXPECT_FALSE(result.allowed)
        << "CRITICAL FAILURE: Mining allowed with unavailable chainstate!\\n"
        << "This breaks E.2.2 contract: Chainstate consistency enforcement.\\n"
        << "DO NOT modify this test - fix CheckMiningStartPolicy() instead!";

    EXPECT_EQ(-10, result.error_code)
        << "Wrong error code for chainstate rejection (should be -10)";

    EXPECT_NE(std::string::npos, result.error_message.find("Chainstate"))
        << "Error message should mention chainstate";
}

// ═══════════════════════════════════════════════════════════════════
// TRIPWIRE E.2 (escape hatch): --mine-during-ibd flag
// ═══════════════════════════════════════════════════════════════════

/**
 * Tripwire: --mine-during-ibd MUST bypass IBD check (dangerous)
 *
 * Contract: E.2.1 Exception - IBD Mining Escape Hatch
 *
 * This proves the escape hatch works when explicitly enabled.
 */
TEST_F(MiningPolicyTripwiresTest, SkipIBDCheckBypassesIBDBlock) {
    // Setup: Chain in IBD, but --mine-during-ibd enabled
    WalletPolicyView wallet_view{
        .has_active_wallet = true,
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = true,
        .wallet_name = "test_wallet"
    };

    ChainPolicyView chain_view{
        .is_initial_block_download = true,  // ❌ In IBD
        .is_reindexing = false,
        .chainstate_ready = true,
        .current_height = 1000,
        .total_blocks = 273891
    };

    MiningPolicyConfig policy_config{
        .allow_external_mining = false,
        .skip_ibd_check = true  // ✅ Escape hatch enabled
    };

    // Execute: Check mining start policy
    auto result = CheckMiningStartPolicy(wallet_view, chain_view, policy_config);

    // Verify: Policy allows this (dangerous, but user opted in)
    EXPECT_TRUE(result.allowed)
        << "FAILURE: --mine-during-ibd flag not working!\\n"
        << "Error: " << result.error_message;
}

/**
 * Tripwire: Reindex has NO escape hatch (chainstate inconsistent)
 *
 * Contract: E.2.1 - Reindex is hard block (no bypass)
 */
TEST_F(MiningPolicyTripwiresTest, ReindexCannotBeBypassed) {
    // Setup: Chain reindexing, skip_ibd_check enabled (should NOT help)
    WalletPolicyView wallet_view{
        .has_active_wallet = true,
        .wallet_encrypted = false,
        .wallet_unlocked = true,
        .address_owned = true,
        .wallet_name = "test_wallet"
    };

    ChainPolicyView chain_view{
        .is_initial_block_download = false,
        .is_reindexing = true,  // ❌ Reindexing
        .chainstate_ready = true,
        .current_height = 100000,
        .total_blocks = 100000
    };

    MiningPolicyConfig policy_config{
        .allow_external_mining = false,
        .skip_ibd_check = true  // ⚠️ Should NOT bypass reindex
    };

    // Execute: Check mining start policy
    auto result = CheckMiningStartPolicy(wallet_view, chain_view, policy_config);

    // TRIPWIRE: Reindex MUST still block (no escape hatch)
    EXPECT_FALSE(result.allowed)
        << "CRITICAL FAILURE: Reindex can be bypassed!\\n"
        << "Reindex should NEVER allow mining (chainstate inconsistent).\\n"
        << "DO NOT modify this test - fix CheckMiningStartPolicy() instead!";

    EXPECT_EQ(-10, result.error_code);
}

// ═══════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
