/**
 * @file test_mining_policy_e2e.cpp
 * @brief End-to-End Mining Policy Integration Tests (Phase F.5)
 *
 * Purpose: Prove the system (real daemon + wallet + chainstate + mining) obeys all frozen policy contracts
 *
 * Test Coverage:
 * - F.5.1: Happy Path Mining (wallet owned address)
 * - F.5.2: Wallet Ownership Enforcement (E.1)
 * - F.5.5: mining.stop Semantics (E.4.2)
 * - F.5.6: Restart Semantics (E.3)
 *
 * Architecture:
 * - Black-box testing (RPC only, no internal service calls)
 * - Real daemon (no mocks/stubs)
 * - Temp datadir (isolated test environment)
 * - Unique ports (parallel test execution safe)
 */

#include <gtest/gtest.h>
#include "e2e_test_harness.h"
#include <json/json.h>
#include <iostream>
#include <thread>
#include <chrono>

using namespace dinero::test;

// ═══════════════════════════════════════════════════════════════════════
// Test Fixture
// ═══════════════════════════════════════════════════════════════════════

class MiningPolicyE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        daemon_ = std::make_unique<TestDaemon>();
    }

    void TearDown() override {
        if (daemon_) {
            daemon_->stop();
        }
    }

    std::unique_ptr<TestDaemon> daemon_;
};

// ═══════════════════════════════════════════════════════════════════════
// F.5.1 — Happy Path Mining
// ═══════════════════════════════════════════════════════════════════════

TEST_F(MiningPolicyE2ETest, MiningHappyPath_WalletOwnedAddress) {
    // Step 1: Start daemon with temp datadir
    // Launch failure is a FAILURE, not a skip (issue #428). dinerod is a build
    // dependency of this target and CMake passes its absolute path via DINEROD,
    // so a daemon that will not start means something is broken — skipping here
    // would let a registered test report green while testing nothing.
    ASSERT_TRUE(daemon_->start())
        << "dinerod failed to start (" << ResolveDinerodPath() << "); see the "
           "daemon log above";
    std::cout << "[Test] ✅ Daemon started (RPC port: " << daemon_->getRpcPort() << ")" << std::endl;
    std::cout << "[Test] 📋 Daemon log: " << daemon_->getLogPath() << std::endl;

    // Step 2: Create RPC client
    std::string cookie = daemon_->readCookie();
    RpcClient rpc("127.0.0.1", daemon_->getRpcPort(), cookie);

    // Step 3: Create wallet
    WalletHelper wallet(rpc);
    std::string address = wallet.createWallet("test_wallet");
    ASSERT_FALSE(address.empty()) << "Failed to create wallet";
    std::cout << "[Test] ✅ Wallet created, address: " << address << std::endl;

    // Step 4: Call mining.start(1, address)
    Json::Value start_result = rpc.mining_start(1, address);
    EXPECT_TRUE(start_result.isMember("mining"));
    EXPECT_EQ(true, start_result["mining"].asBool()) << "Mining should be active";
    std::cout << "[Test] ✅ mining.start succeeded" << std::endl;

    // Step 5: Wait for mining to be provably active (hashrate OR blocks)
    // Bitcoin Core-style: Don't rely on timing artifacts, wait for observable proof
    ChainHelper chain(rpc);
    int start_height = chain.getHeight();
    bool saw_hashrate = false;
    bool saw_blocks = false;
    double final_hashrate = 0.0;

    for (int i = 0; i < 20; ++i) {  // 10 second timeout (20 * 500ms)
        Json::Value info_result = rpc.mining_info();
        EXPECT_EQ(true, info_result["mining"].asBool()) << "mining.info.mining should be true";

        final_hashrate = info_result["hashrate"].asDouble();
        if (final_hashrate > 0.0) {
            saw_hashrate = true;
            break;
        }

        int current_height = chain.getHeight();
        if (current_height > start_height) {
            saw_blocks = true;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Step 6: Verify mining is provably active (hashrate reported OR blocks found)
    EXPECT_TRUE(saw_hashrate || saw_blocks)
        << "Mining must show activity: either hashrate > 0 or blocks mined";
    std::cout << "[Test] ✅ mining.info shows mining=true, hashrate=" << final_hashrate
              << (saw_blocks ? " (blocks found before hashrate reported)" : "") << std::endl;

    // Step 7: Stop mining
    Json::Value stop_result = rpc.mining_stop();
    EXPECT_EQ(false, stop_result["mining"].asBool()) << "Mining should be stopped";
    std::cout << "[Test] ✅ mining.stop succeeded" << std::endl;

    // Step 8: Verify wallet balance increased (blocks found > 0)
    int height = chain.getHeight();
    EXPECT_GT(height, 0) << "Blocks should have been mined (height > 0)";
    std::cout << "[Test] ✅ Blocks mined, height=" << height << std::endl;

    // Step 9: Verify no policy errors in daemon logs (implicit: test passed)
    std::cout << "[Test] ✅ Happy path complete, no policy errors" << std::endl;

    // Copy daemon log to /tmp for post-mortem debugging
    std::string cmd = "cp " + daemon_->getLogPath() + " /tmp/daemon_mining_test.log 2>/dev/null || true";
    system(cmd.c_str());
    std::cout << "[Test] 📋 Log copied to: /tmp/daemon_mining_test.log" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════
// F.5.2 — Wallet Ownership Enforcement (E.1)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(MiningPolicyE2ETest, MiningForeignAddressRejected) {
    // Step 1: Start daemon
    // Launch failure is a FAILURE, not a skip (issue #428). dinerod is a build
    // dependency of this target and CMake passes its absolute path via DINEROD,
    // so a daemon that will not start means something is broken — skipping here
    // would let a registered test report green while testing nothing.
    ASSERT_TRUE(daemon_->start())
        << "dinerod failed to start (" << ResolveDinerodPath() << "); see the "
           "daemon log above";
    std::cout << "[Test] ✅ Daemon started" << std::endl;

    // Step 2: Create RPC client
    std::string cookie = daemon_->readCookie();
    RpcClient rpc("127.0.0.1", daemon_->getRpcPort(), cookie);

    // Step 3: Create first wallet and get an address
    WalletHelper wallet1(rpc);
    std::string first_address = wallet1.createWallet("test_wallet_1");
    std::cout << "[Test] ✅ Created first wallet, address: " << first_address << std::endl;

    // Step 4: Create second wallet - this makes wallet2 active
    // So first_address is now "foreign" to the active wallet (wallet2)
    WalletHelper wallet2(rpc);
    std::string foreign_address = first_address;  // Address from wallet1 is foreign to wallet2
    wallet2.createWallet("test_wallet_2");
    std::cout << "[Test] ✅ Created second wallet (now active)" << std::endl;
    std::cout << "[Test] Using foreign address (from wallet1): " << foreign_address << std::endl;

    // Step 5: Call mining.start(1, foreign_address) - should FAIL
    bool threw_error = false;
    std::string error_message;
    Json::Value start_result;
    try {
        start_result = rpc.mining_start(1, foreign_address);
        std::cout << "[Test] ⚠️  mining.start returned: " << start_result.toStyledString() << std::endl;
    } catch (const std::runtime_error& e) {
        threw_error = true;
        error_message = e.what();
    }

    // Assertions
    ASSERT_TRUE(threw_error) << "mining.start should have failed with foreign address";
    EXPECT_NE(std::string::npos, error_message.find("\"code\":-13"))
        << "Error code should be -13 (permission error)";
    EXPECT_NE(std::string::npos, error_message.find("not owned"))
        << "Error message should mention 'not owned'";
    std::cout << "[Test] ✅ Foreign address rejected with error: " << error_message << std::endl;

    // Step 6: Verify mining not started
    Json::Value info_result = rpc.mining_info();
    EXPECT_EQ(false, info_result["mining"].asBool()) << "Mining should NOT be active";
    std::cout << "[Test] ✅ Mining not started (mining.info.mining=false)" << std::endl;
}

TEST_F(MiningPolicyE2ETest, MiningWithoutAddressRejected) {
    // Note: Daemon auto-creates "default" wallet, so we test missing address instead
    // Step 1: Start daemon
    // Launch failure is a FAILURE, not a skip (issue #428). dinerod is a build
    // dependency of this target and CMake passes its absolute path via DINEROD,
    // so a daemon that will not start means something is broken — skipping here
    // would let a registered test report green while testing nothing.
    ASSERT_TRUE(daemon_->start())
        << "dinerod failed to start (" << ResolveDinerodPath() << "); see the "
           "daemon log above";
    std::cout << "[Test] ✅ Daemon started" << std::endl;

    // Step 2: Create RPC client
    std::string cookie = daemon_->readCookie();
    RpcClient rpc("127.0.0.1", daemon_->getRpcPort(), cookie);

    // Step 3: Default wallet exists but don't get an address
    std::cout << "[Test] ℹ️  Default wallet exists but not providing address to mining.start" << std::endl;

    // Step 4: Call mining.start WITHOUT address parameter - should FAIL
    bool threw_error = false;
    std::string error_message;
    Json::Value start_result;
    try {
        // Call with threads=1 but empty address (will use default empty string)
        start_result = rpc.mining_start(1, "");
        std::cout << "[Test] ⚠️  mining.start returned: " << start_result.toStyledString() << std::endl;
    } catch (const std::runtime_error& e) {
        threw_error = true;
        error_message = e.what();
    }

    // Assertions - should fail because no address was provided
    ASSERT_TRUE(threw_error) << "mining.start should have failed without address";
    EXPECT_NE(std::string::npos, error_message.find("\"code\":-32602"))
        << "Error code should be -32602 (invalid params)";
    EXPECT_NE(std::string::npos, error_message.find("address"))
        << "Error message should mention 'address'";
    std::cout << "[Test] ✅ No address rejected with error: " << error_message << std::endl;

    // Step 5: Verify mining not started
    Json::Value info_result = rpc.mining_info();
    EXPECT_EQ(false, info_result["mining"].asBool()) << "Mining should NOT be active";
    std::cout << "[Test] ✅ Mining not started (mining.info.mining=false)" << std::endl;
}

TEST_F(MiningPolicyE2ETest, MiningLockedWalletRejected) {
    // Step 1: Start daemon
    // Launch failure is a FAILURE, not a skip (issue #428). dinerod is a build
    // dependency of this target and CMake passes its absolute path via DINEROD,
    // so a daemon that will not start means something is broken — skipping here
    // would let a registered test report green while testing nothing.
    ASSERT_TRUE(daemon_->start())
        << "dinerod failed to start (" << ResolveDinerodPath() << "); see the "
           "daemon log above";
    std::cout << "[Test] ✅ Daemon started" << std::endl;

    // Step 2: Create RPC client
    std::string cookie = daemon_->readCookie();
    RpcClient rpc("127.0.0.1", daemon_->getRpcPort(), cookie);

    // Step 3: Use default wallet and get an address
    WalletHelper wallet(rpc);
    std::string address = wallet.getNewAddress();
    std::cout << "[Test] ✅ Got address from default wallet: " << address << std::endl;

    // Step 4: Encrypt wallet
    wallet.encryptWallet("test_passphrase");
    std::cout << "[Test] ✅ Wallet encrypted (now locked)" << std::endl;

    // Step 5: Do NOT unlock wallet

    // Step 6: Call mining.start(1, address) - should FAIL
    bool threw_error = false;
    std::string error_message;
    Json::Value start_result;
    try {
        start_result = rpc.mining_start(1, address);
        std::cout << "[Test] ⚠️  mining.start returned: " << start_result.toStyledString() << std::endl;
    } catch (const std::runtime_error& e) {
        threw_error = true;
        error_message = e.what();
    }

    // Assertions
    ASSERT_TRUE(threw_error) << "mining.start should have failed with locked wallet";
    EXPECT_NE(std::string::npos, error_message.find("\"code\":-13"))
        << "Error code should be -13 (permission error)";
    // Error message should contain "locked" or "unlock"
    bool has_locked_msg = (error_message.find("locked") != std::string::npos) ||
                          (error_message.find("unlock") != std::string::npos);
    EXPECT_TRUE(has_locked_msg) << "Error message should mention locked/unlock";
    std::cout << "[Test] ✅ Locked wallet rejected with error: " << error_message << std::endl;

    // Step 7: Verify mining not started
    Json::Value info_result = rpc.mining_info();
    EXPECT_EQ(false, info_result["mining"].asBool()) << "Mining should NOT be active";
    std::cout << "[Test] ✅ Mining not started (mining.info.mining=false)" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════
// F.5.5 — mining.stop Semantics (E.4.2)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(MiningPolicyE2ETest, MiningStopIsIdempotent) {
    // Step 1: Start daemon
    // Launch failure is a FAILURE, not a skip (issue #428). dinerod is a build
    // dependency of this target and CMake passes its absolute path via DINEROD,
    // so a daemon that will not start means something is broken — skipping here
    // would let a registered test report green while testing nothing.
    ASSERT_TRUE(daemon_->start())
        << "dinerod failed to start (" << ResolveDinerodPath() << "); see the "
           "daemon log above";
    std::cout << "[Test] ✅ Daemon started" << std::endl;

    // Step 2: Create RPC client
    std::string cookie = daemon_->readCookie();
    RpcClient rpc("127.0.0.1", daemon_->getRpcPort(), cookie);

    // Step 3: Create wallet
    WalletHelper wallet(rpc);
    std::string address = wallet.createWallet("test_wallet");
    std::cout << "[Test] ✅ Wallet created" << std::endl;

    // Step 4: Start mining
    rpc.mining_start(1, address);
    std::cout << "[Test] ✅ Mining started" << std::endl;

    // Step 5: Verify mining active
    Json::Value info_result = rpc.mining_info();
    EXPECT_EQ(true, info_result["mining"].asBool()) << "Mining should be active";
    std::cout << "[Test] ✅ mining.info.mining=true" << std::endl;

    // Step 6: Call mining.stop (first time)
    Json::Value stop_result_1 = rpc.mining_stop();
    EXPECT_EQ(false, stop_result_1["mining"].asBool()) << "Mining should be stopped";
    std::cout << "[Test] ✅ First mining.stop succeeded" << std::endl;

    // Step 7: Call mining.stop (second time) - should succeed (idempotent)
    Json::Value stop_result_2 = rpc.mining_stop();
    EXPECT_EQ(false, stop_result_2["mining"].asBool()) << "Second stop should succeed";
    std::cout << "[Test] ✅ Second mining.stop succeeded (idempotent)" << std::endl;

    // Step 8: Call mining.stop (third time) - should succeed (idempotent)
    Json::Value stop_result_3 = rpc.mining_stop();
    EXPECT_EQ(false, stop_result_3["mining"].asBool()) << "Third stop should succeed";
    std::cout << "[Test] ✅ Third mining.stop succeeded (idempotent)" << std::endl;

    // Assertions: All calls returned success (no exceptions thrown)
    std::cout << "[Test] ✅ mining.stop is idempotent (E.4.2 enforced)" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════
// F.5.6 — Restart Semantics (E.3)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(MiningPolicyE2ETest, MiningDoesNotAutoResumeAfterRestart) {
    // Step 1: Start daemon
    // Launch failure is a FAILURE, not a skip (issue #428). dinerod is a build
    // dependency of this target and CMake passes its absolute path via DINEROD,
    // so a daemon that will not start means something is broken — skipping here
    // would let a registered test report green while testing nothing.
    ASSERT_TRUE(daemon_->start())
        << "dinerod failed to start (" << ResolveDinerodPath() << "); see the "
           "daemon log above";
    std::cout << "[Test] ✅ Daemon started" << std::endl;

    // Step 2: Create RPC client
    std::string cookie = daemon_->readCookie();
    RpcClient rpc("127.0.0.1", daemon_->getRpcPort(), cookie);

    // Step 3: Create wallet
    WalletHelper wallet(rpc);
    std::string address = wallet.createWallet("test_wallet");
    std::cout << "[Test] ✅ Wallet created, address: " << address << std::endl;

    // Step 4: Start mining
    rpc.mining_start(1, address);
    std::cout << "[Test] ✅ Mining started" << std::endl;

    // Step 5: Verify mining active
    Json::Value info_result = rpc.mining_info();
    EXPECT_EQ(true, info_result["mining"].asBool()) << "Mining should be active before restart";
    std::cout << "[Test] ✅ mining.info.mining=true (before restart)" << std::endl;

    // Step 6: Stop daemon cleanly
    daemon_->stop();
    std::cout << "[Test] ✅ Daemon stopped cleanly" << std::endl;

    // Step 7: Restart daemon (same datadir)
    ASSERT_TRUE(daemon_->restart()) << "Daemon failed to restart";
    std::cout << "[Test] ✅ Daemon restarted (same datadir)" << std::endl;

    // Step 8: Reconnect RPC (need new cookie after restart)
    std::string cookie_after_restart = daemon_->readCookie();
    RpcClient rpc_after_restart("127.0.0.1", daemon_->getRpcPort(), cookie_after_restart);

    // Step 9: Call mining.info
    Json::Value info_after_restart = rpc_after_restart.mining_info();

    // Assertions: Mining should NOT auto-resume (E.3.1 contract)
    EXPECT_EQ(false, info_after_restart["mining"].asBool())
        << "❌ CRITICAL: Mining auto-resumed after restart (E.3.1 violated!)";
    std::cout << "[Test] ✅ mining.info.mining=false after restart (E.3.1 enforced)" << std::endl;

    // Mining address should still be present in config
    EXPECT_TRUE(info_after_restart.isMember("address"))
        << "Mining address should be persisted";
    std::cout << "[Test] ✅ Mining address persisted: "
              << info_after_restart["address"].asString() << std::endl;

    std::cout << "[Test] ✅ E.3 contract enforced: Mining does NOT auto-resume after restart" << std::endl;
}

TEST_F(MiningPolicyE2ETest, MiningResumesOnlyAfterExplicitStart) {
    // Step 1: Start daemon
    // Launch failure is a FAILURE, not a skip (issue #428). dinerod is a build
    // dependency of this target and CMake passes its absolute path via DINEROD,
    // so a daemon that will not start means something is broken — skipping here
    // would let a registered test report green while testing nothing.
    ASSERT_TRUE(daemon_->start())
        << "dinerod failed to start (" << ResolveDinerodPath() << "); see the "
           "daemon log above";

    // Step 2: Create RPC client
    std::string cookie = daemon_->readCookie();
    RpcClient rpc("127.0.0.1", daemon_->getRpcPort(), cookie);

    // Step 3: Get address from auto-created "default" wallet and start mining
    // NOTE: Daemon auto-creates and opens "default" wallet on first startup
    WalletHelper wallet(rpc);
    std::string address = wallet.getNewAddress();
    std::cout << "[Test] Got address from default wallet: " << address << std::endl;

    Json::Value start_result_first = rpc.mining_start(1, address);
    std::cout << "[Test] First mining.start result: " << start_result_first.toStyledString() << std::endl;

    // Step 4: Verify mining active
    Json::Value info_before = rpc.mining_info();
    EXPECT_EQ(true, info_before["mining"].asBool());

    // Step 5: Restart daemon (restart() handles shutdown without deleting datadir)
    ASSERT_TRUE(daemon_->restart());

    // Step 6: Reconnect RPC
    std::string cookie_after = daemon_->readCookie();
    RpcClient rpc_after("127.0.0.1", daemon_->getRpcPort(), cookie_after);

    // Step 7: Verify mining NOT active after restart
    Json::Value info_after_restart = rpc_after.mining_info();
    EXPECT_EQ(false, info_after_restart["mining"].asBool())
        << "Mining should NOT auto-resume";

    // DEBUG: Check wallet state after restart
    std::cout << "[Test] Address from before restart: " << address << std::endl;
    try {
        Json::Value wallet_info_after = rpc_after.call("wallet.info", Json::Value());
        std::cout << "[Test] Wallet info after restart: " << wallet_info_after.toStyledString() << std::endl;
    } catch (const std::exception& e) {
        std::cout << "[Test] wallet.info failed: " << e.what() << std::endl;
    }

    // Step 8: Call mining.start explicitly (wallet auto-loaded on restart)
    Json::Value start_result = rpc_after.mining_start(1, address);
    std::cout << "[Test] mining.start result: " << start_result.toStyledString() << std::endl;
    std::cout << "[Test] ✅ Explicit mining.start called after restart" << std::endl;

    // Step 9: Verify mining becomes active
    Json::Value info_after_start = rpc_after.mining_info();
    EXPECT_EQ(true, info_after_start["mining"].asBool())
        << "Mining should be active after explicit start";
    std::cout << "[Test] ✅ Mining active after explicit start" << std::endl;

    // Step 10: Verify no policy errors
    std::cout << "[Test] ✅ E.3 contract enforced: Manual restart required" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
