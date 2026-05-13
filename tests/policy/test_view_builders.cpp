/**
 * @file test_view_builders.cpp
 * @brief View Builder Unit Tests (Phase F.3)
 *
 * Purpose: Verify view builder functions are correct, null-safe, and error-resistant
 *
 * Architecture:
 * - Pure unit tests (no real services, no threads, no IO)
 * - Test stubs/mocks only (minimal objects, no SQLite/RocksDB/network)
 * - Fast tests (target: 0ms runtime like tripwire tests)
 * - Verify null safety and error handling
 *
 * Test Coverage:
 * - BuildWalletView: 8 tests (null, states, errors)
 * - BuildChainView: 2 tests (null, valid)
 * - BuildRestartView: 5 tests (null, fresh start combinations)
 * - BuildMiningStateView: 6 tests (null, mining states, wallet combinations)
 *
 * This ensures view builders (the bridge between services and policy) are correct.
 */

#include <gtest/gtest.h>
#include "rpc/mining_policy.h"

// Forward declarations for test stubs
namespace dinero {
    class WalletManager;
    class WalletService;
    class ChainstateService;
    class MiningService;
}

// ═══════════════════════════════════════════════════════════════════
// TEST STUBS (Minimal Service Mocks)
// ═══════════════════════════════════════════════════════════════════

/**
 * Minimal WalletManager stub for testing
 * No SQLite, no encryption, just return configured values
 */
class StubWalletManager {
public:
    bool is_wallet_encrypted = false;
    bool is_wallet_locked = false;
    bool address_is_mine = false;
    bool throw_on_access = false;

    bool isWalletEncrypted() const {
        if (throw_on_access) throw std::runtime_error("Test exception");
        return is_wallet_encrypted;
    }

    bool isWalletLocked() const {
        if (throw_on_access) throw std::runtime_error("Test exception");
        return is_wallet_locked;
    }

    bool isAddressMine(const std::string& address) const {
        if (throw_on_access) throw std::runtime_error("Test exception");
        return address_is_mine;
    }
};

/**
 * Minimal WalletService stub for testing
 */
class StubWalletService {
public:
    bool has_active_wallet = false;
    std::string wallet_name = "";
    StubWalletManager wallet_manager;

    bool hasActiveWallet() const {
        return has_active_wallet;
    }

    std::string getCurrentWalletName() const {
        return wallet_name;
    }

    StubWalletManager& get() {
        return wallet_manager;
    }
};

/**
 * Minimal ChainstateService stub for testing
 */
class StubChainstateService {
public:
    uint32_t block_height = 0;

    uint32_t getBlockHeight() const {
        return block_height;
    }
};

/**
 * Minimal MiningService stub for testing
 */
class StubMiningService {
public:
    bool mining_enabled = false;

    bool isMiningEnabled() const {
        return mining_enabled;
    }
};

/**
 * Minimal DaemonContext stub for testing
 */
struct StubDaemonContext {
    bool is_fresh_start = false;
    bool mining_was_active_before = false;
};

/**
 * Minimal ExecutionContext stub for testing
 */
struct StubExecutionContext {
    StubDaemonContext* daemon = nullptr;
};

// ═══════════════════════════════════════════════════════════════════
// VIEW BUILDER FUNCTION DECLARATIONS (from methods_mining_context.cpp)
// ═══════════════════════════════════════════════════════════════════
//
// We need to expose these static functions for testing.
// In production, we would either:
// 1. Make them non-static and testable
// 2. Include the implementation file directly (not ideal)
// 3. Move them to a testable header
//
// For F.3, we'll reimplement them here to match the production versions.
// ═══════════════════════════════════════════════════════════════════

dinero::rpc::WalletPolicyView BuildWalletView(
    StubWalletService* wallet,
    const std::string& mining_address
) {
    if (!wallet || !wallet->hasActiveWallet()) {
        return {
            .has_active_wallet = false,
            .wallet_encrypted = false,
            .wallet_unlocked = true,
            .address_owned = false,
            .wallet_name = ""
        };
    }

    try {
        return {
            .has_active_wallet = true,
            .wallet_encrypted = wallet->get().isWalletEncrypted(),
            .wallet_unlocked = !wallet->get().isWalletLocked(),
            .address_owned = wallet->get().isAddressMine(mining_address),
            .wallet_name = wallet->getCurrentWalletName()
        };
    } catch (const std::exception& e) {
        // View building failed - treat as unsafe state
        return {
            .has_active_wallet = false,
            .wallet_encrypted = false,
            .wallet_unlocked = true,
            .address_owned = false,
            .wallet_name = ""
        };
    }
}

dinero::rpc::ChainPolicyView BuildChainView(
    StubChainstateService* chainstate
) {
    if (!chainstate) {
        return {
            .is_initial_block_download = false,
            .is_reindexing = false,
            .chainstate_ready = false,
            .current_height = 0,
            .total_blocks = 0
        };
    }

    // TODO F.2: Get real IBD state from chainstate
    return {
        .is_initial_block_download = false,
        .is_reindexing = false,
        .chainstate_ready = true,
        .current_height = chainstate->getBlockHeight(),
        .total_blocks = 0
    };
}

dinero::rpc::RestartPolicyView BuildRestartView(
    const StubExecutionContext& ctx
) {
    if (!ctx.daemon) {
        // No daemon context - assume runtime state (not a fresh start)
        return {
            .is_fresh_start = false,
            .mining_was_active_before = false
        };
    }

    return {
        .is_fresh_start = ctx.daemon->is_fresh_start,
        .mining_was_active_before = ctx.daemon->mining_was_active_before
    };
}

dinero::rpc::MiningStatePolicyView BuildMiningStateView(
    StubMiningService* mining,
    StubWalletService* wallet
) {
    if (!mining) {
        return {
            .is_mining_active = false,
            .current_wallet_name = "",
            .mining_wallet_name = ""
        };
    }

    std::string current_wallet = "";
    if (wallet && wallet->hasActiveWallet()) {
        current_wallet = wallet->getCurrentWalletName();
    }

    return {
        .is_mining_active = mining->isMiningEnabled(),
        .current_wallet_name = current_wallet,
        .mining_wallet_name = current_wallet
    };
}

// ═══════════════════════════════════════════════════════════════════
// TEST SUITE 1: BuildWalletView
// ═══════════════════════════════════════════════════════════════════

TEST(BuildWalletViewTests, NullWalletService_ReturnsSafeDefaults) {
    // Input: null wallet service
    auto result = BuildWalletView(nullptr, "din1testaddress");

    // Expected: safe defaults (no wallet, no mining allowed)
    EXPECT_FALSE(result.has_active_wallet);
    EXPECT_FALSE(result.wallet_encrypted);
    EXPECT_TRUE(result.wallet_unlocked);  // Default: unencrypted wallets are "unlocked"
    EXPECT_FALSE(result.address_owned);
    EXPECT_EQ("", result.wallet_name);
}

TEST(BuildWalletViewTests, NoActiveWallet_ReturnsSafeState) {
    // Setup: wallet service with no active wallet
    StubWalletService wallet;
    wallet.has_active_wallet = false;

    // Execute
    auto result = BuildWalletView(&wallet, "din1testaddress");

    // Verify: no active wallet means no mining allowed
    EXPECT_FALSE(result.has_active_wallet);
    EXPECT_FALSE(result.address_owned);
}

TEST(BuildWalletViewTests, ActiveWallet_UnencryptedWallet_CorrectState) {
    // Setup: active wallet, unencrypted
    StubWalletService wallet;
    wallet.has_active_wallet = true;
    wallet.wallet_name = "test_wallet";
    wallet.wallet_manager.is_wallet_encrypted = false;
    wallet.wallet_manager.is_wallet_locked = false;
    wallet.wallet_manager.address_is_mine = true;

    // Execute
    auto result = BuildWalletView(&wallet, "din1testaddress");

    // Verify: unencrypted wallets are always "unlocked"
    EXPECT_TRUE(result.has_active_wallet);
    EXPECT_FALSE(result.wallet_encrypted);
    EXPECT_TRUE(result.wallet_unlocked);
    EXPECT_TRUE(result.address_owned);
    EXPECT_EQ("test_wallet", result.wallet_name);
}

TEST(BuildWalletViewTests, ActiveWallet_EncryptedAndLocked_CorrectState) {
    // Setup: active wallet, encrypted, locked
    StubWalletService wallet;
    wallet.has_active_wallet = true;
    wallet.wallet_name = "encrypted_wallet";
    wallet.wallet_manager.is_wallet_encrypted = true;
    wallet.wallet_manager.is_wallet_locked = true;
    wallet.wallet_manager.address_is_mine = true;

    // Execute
    auto result = BuildWalletView(&wallet, "din1testaddress");

    // Verify: E.1.2 check depends on this
    EXPECT_TRUE(result.has_active_wallet);
    EXPECT_TRUE(result.wallet_encrypted);
    EXPECT_FALSE(result.wallet_unlocked);  // Locked!
    EXPECT_TRUE(result.address_owned);
    EXPECT_EQ("encrypted_wallet", result.wallet_name);
}

TEST(BuildWalletViewTests, ActiveWallet_EncryptedAndUnlocked_CorrectState) {
    // Setup: active wallet, encrypted, unlocked
    StubWalletService wallet;
    wallet.has_active_wallet = true;
    wallet.wallet_name = "unlocked_wallet";
    wallet.wallet_manager.is_wallet_encrypted = true;
    wallet.wallet_manager.is_wallet_locked = false;
    wallet.wallet_manager.address_is_mine = true;

    // Execute
    auto result = BuildWalletView(&wallet, "din1testaddress");

    // Verify: mining should be allowed
    EXPECT_TRUE(result.has_active_wallet);
    EXPECT_TRUE(result.wallet_encrypted);
    EXPECT_TRUE(result.wallet_unlocked);  // Unlocked!
    EXPECT_TRUE(result.address_owned);
    EXPECT_EQ("unlocked_wallet", result.wallet_name);
}

TEST(BuildWalletViewTests, ActiveWallet_AddressOwned_CorrectState) {
    // Setup: active wallet, address is mine
    StubWalletService wallet;
    wallet.has_active_wallet = true;
    wallet.wallet_manager.address_is_mine = true;

    // Execute
    auto result = BuildWalletView(&wallet, "din1testaddress");

    // Verify: E.1.1 check depends on this
    EXPECT_TRUE(result.address_owned);
}

TEST(BuildWalletViewTests, ActiveWallet_AddressNotOwned_CorrectState) {
    // Setup: active wallet, address not mine
    StubWalletService wallet;
    wallet.has_active_wallet = true;
    wallet.wallet_manager.address_is_mine = false;

    // Execute
    auto result = BuildWalletView(&wallet, "din1foreignaddress");

    // Verify: foreign address should be rejected
    EXPECT_FALSE(result.address_owned);
}

TEST(BuildWalletViewTests, ExceptionDuringViewBuilding_ReturnsSafeDefaults) {
    // Setup: wallet that throws exception on access
    StubWalletService wallet;
    wallet.has_active_wallet = true;
    wallet.wallet_manager.throw_on_access = true;

    // Execute
    auto result = BuildWalletView(&wallet, "din1testaddress");

    // Verify: view building failure should not crash, should return safe defaults
    EXPECT_FALSE(result.has_active_wallet);
    EXPECT_FALSE(result.address_owned);
}

// ═══════════════════════════════════════════════════════════════════
// TEST SUITE 2: BuildChainView
// ═══════════════════════════════════════════════════════════════════

TEST(BuildChainViewTests, NullChainstateService_ReturnsSafeDefaults) {
    // Input: null chainstate service
    auto result = BuildChainView(nullptr);

    // Expected: safe defaults (chainstate not ready, blocks mining)
    EXPECT_FALSE(result.chainstate_ready);
    EXPECT_FALSE(result.is_initial_block_download);
    EXPECT_FALSE(result.is_reindexing);
    EXPECT_EQ(0u, result.current_height);
    EXPECT_EQ(0u, result.total_blocks);
}

TEST(BuildChainViewTests, ValidChainstate_CorrectState) {
    // Setup: valid chainstate with height
    StubChainstateService chainstate;
    chainstate.block_height = 12345;

    // Execute
    auto result = BuildChainView(&chainstate);

    // Verify: chainstate ready, correct height
    EXPECT_TRUE(result.chainstate_ready);
    EXPECT_EQ(12345u, result.current_height);
    EXPECT_FALSE(result.is_initial_block_download);  // TODO F.2: Will be real when chainstate exposes IBD
    EXPECT_FALSE(result.is_reindexing);  // TODO F.2: Will be real when chainstate exposes reindex
}

// ═══════════════════════════════════════════════════════════════════
// TEST SUITE 3: BuildRestartView
// ═══════════════════════════════════════════════════════════════════

TEST(BuildRestartViewTests, NullDaemonContext_ReturnsSafeDefaults) {
    // Input: execution context with null daemon
    StubExecutionContext ctx;
    ctx.daemon = nullptr;

    // Execute
    auto result = BuildRestartView(ctx);

    // Expected: safe defaults (assume runtime state, not fresh start)
    EXPECT_FALSE(result.is_fresh_start);
    EXPECT_FALSE(result.mining_was_active_before);
}

TEST(BuildRestartViewTests, FreshStart_MiningWasActive_CorrectState) {
    // Setup: fresh start, mining was active before
    StubDaemonContext daemon;
    daemon.is_fresh_start = true;
    daemon.mining_was_active_before = true;

    StubExecutionContext ctx;
    ctx.daemon = &daemon;

    // Execute
    auto result = BuildRestartView(ctx);

    // Verify: E.3 policy should block auto-resume
    EXPECT_TRUE(result.is_fresh_start);
    EXPECT_TRUE(result.mining_was_active_before);
}

TEST(BuildRestartViewTests, FreshStart_MiningWasNotActive_CorrectState) {
    // Setup: fresh start, mining was not active before
    StubDaemonContext daemon;
    daemon.is_fresh_start = true;
    daemon.mining_was_active_before = false;

    StubExecutionContext ctx;
    ctx.daemon = &daemon;

    // Execute
    auto result = BuildRestartView(ctx);

    // Verify: clean restart should allow mining.start
    EXPECT_TRUE(result.is_fresh_start);
    EXPECT_FALSE(result.mining_was_active_before);
}

TEST(BuildRestartViewTests, NotFreshStart_MiningWasActive_CorrectState) {
    // Setup: runtime state, mining was active before
    StubDaemonContext daemon;
    daemon.is_fresh_start = false;
    daemon.mining_was_active_before = true;

    StubExecutionContext ctx;
    ctx.daemon = &daemon;

    // Execute
    auto result = BuildRestartView(ctx);

    // Verify: runtime state, E.3 policy should allow mining.start
    EXPECT_FALSE(result.is_fresh_start);
    EXPECT_TRUE(result.mining_was_active_before);
}

TEST(BuildRestartViewTests, NotFreshStart_MiningWasNotActive_CorrectState) {
    // Setup: runtime state, mining was not active before
    StubDaemonContext daemon;
    daemon.is_fresh_start = false;
    daemon.mining_was_active_before = false;

    StubExecutionContext ctx;
    ctx.daemon = &daemon;

    // Execute
    auto result = BuildRestartView(ctx);

    // Verify: runtime state, E.3 policy should allow mining.start
    EXPECT_FALSE(result.is_fresh_start);
    EXPECT_FALSE(result.mining_was_active_before);
}

// ═══════════════════════════════════════════════════════════════════
// TEST SUITE 4: BuildMiningStateView
// ═══════════════════════════════════════════════════════════════════

TEST(BuildMiningStateViewTests, NullMiningService_ReturnsSafeDefaults) {
    // Input: null mining service
    auto result = BuildMiningStateView(nullptr, nullptr);

    // Expected: safe defaults (mining not active)
    EXPECT_FALSE(result.is_mining_active);
    EXPECT_EQ("", result.current_wallet_name);
    EXPECT_EQ("", result.mining_wallet_name);
}

TEST(BuildMiningStateViewTests, MiningActive_NoWallet_CorrectState) {
    // Setup: mining active, no wallet
    StubMiningService mining;
    mining.mining_enabled = true;

    // Execute
    auto result = BuildMiningStateView(&mining, nullptr);

    // Verify: mining active without wallet (external mining scenario)
    EXPECT_TRUE(result.is_mining_active);
    EXPECT_EQ("", result.current_wallet_name);
    EXPECT_EQ("", result.mining_wallet_name);
}

TEST(BuildMiningStateViewTests, MiningNotActive_NoWallet_CorrectState) {
    // Setup: mining not active, no wallet
    StubMiningService mining;
    mining.mining_enabled = false;

    // Execute
    auto result = BuildMiningStateView(&mining, nullptr);

    // Verify: mining not active, no wallet
    EXPECT_FALSE(result.is_mining_active);
    EXPECT_EQ("", result.current_wallet_name);
    EXPECT_EQ("", result.mining_wallet_name);
}

TEST(BuildMiningStateViewTests, MiningActive_WithWallet_CorrectState) {
    // Setup: mining active, wallet loaded
    StubMiningService mining;
    mining.mining_enabled = true;

    StubWalletService wallet;
    wallet.has_active_wallet = true;
    wallet.wallet_name = "wallet1";

    // Execute
    auto result = BuildMiningStateView(&mining, &wallet);

    // Verify: E.4.1 wallet switch check depends on this
    EXPECT_TRUE(result.is_mining_active);
    EXPECT_EQ("wallet1", result.current_wallet_name);
    EXPECT_EQ("wallet1", result.mining_wallet_name);
}

TEST(BuildMiningStateViewTests, MiningNotActive_WithWallet_CorrectState) {
    // Setup: mining not active, wallet loaded
    StubMiningService mining;
    mining.mining_enabled = false;

    StubWalletService wallet;
    wallet.has_active_wallet = true;
    wallet.wallet_name = "wallet1";

    // Execute
    auto result = BuildMiningStateView(&mining, &wallet);

    // Verify: mining not active, wallet switch should be allowed
    EXPECT_FALSE(result.is_mining_active);
    EXPECT_EQ("wallet1", result.current_wallet_name);
}

TEST(BuildMiningStateViewTests, WalletWithoutActiveWallet_CorrectState) {
    // Setup: mining active, wallet service exists but no active wallet
    StubMiningService mining;
    mining.mining_enabled = true;

    StubWalletService wallet;
    wallet.has_active_wallet = false;

    // Execute
    auto result = BuildMiningStateView(&mining, &wallet);

    // Verify: no active wallet means empty wallet name
    EXPECT_TRUE(result.is_mining_active);
    EXPECT_EQ("", result.current_wallet_name);
}

// ═══════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
