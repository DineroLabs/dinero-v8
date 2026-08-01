/**
 * @file e2e_test_harness.h
 * @brief End-to-End Test Infrastructure (Phase F.5)
 *
 * Purpose: Provide utilities for testing real daemon + wallet + chainstate + mining
 *
 * Components:
 * - TestDaemon: Manage daemon lifecycle (start/stop/restart)
 * - RpcClient: JSON-RPC communication wrapper
 * - WalletHelper: Wallet operations (create/encrypt/unlock)
 * - ChainHelper: Chain operations (mine blocks, wait for height)
 *
 * Usage:
 *   TestDaemon daemon;
 *   daemon.start();
 *   RpcClient rpc("127.0.0.1", daemon.getRpcPort());
 *   WalletHelper wallet(rpc);
 *   std::string address = wallet.createWallet("test_wallet");
 *   ChainHelper chain(rpc);
 *   chain.mineBlocks(10);
 *   daemon.stop();
 */

#ifndef DINERO_E2E_TEST_HARNESS_H
#define DINERO_E2E_TEST_HARNESS_H

#include <string>
#include <vector>
#include <memory>
#include <json/json.h>

namespace dinero {
namespace test {

// ═══════════════════════════════════════════════════════════════════════
// Daemon path resolution
// ═══════════════════════════════════════════════════════════════════════

// Path to the dinerod binary under test.
//
// CMake sets DINEROD=$<TARGET_FILE:dinerod> on every daemon-spawning test, so
// the returned path is absolute and independent of the process working
// directory (issue #428). Previously these harnesses exec'd the relative
// "./dinerod", which only resolves when the test happens to run from the build
// root — that is what broke ColdStartConsensus (#413).
//
// The "./dinerod" fallback exists ONLY so a developer can still run a test
// binary by hand from the build root. CI always supplies DINEROD.
std::string ResolveDinerodPath();

// True when the resolved daemon path exists and is executable. Callers must
// treat a false result as a test FAILURE, never as a skip: these tests are
// registered unconditionally and dinerod is a build dependency of both test
// targets, so an unusable daemon means the environment is broken, not absent.
bool DinerodAvailable(std::string* resolved_path_out = nullptr,
                      std::string* error_out = nullptr);

// ═══════════════════════════════════════════════════════════════════════
// TestDaemon - Daemon Lifecycle Manager
// ═══════════════════════════════════════════════════════════════════════

class TestDaemon {
public:
    TestDaemon();
    ~TestDaemon();

    // Start daemon with temp datadir + unique ports
    // Returns true if daemon started successfully
    bool start(const std::vector<std::string>& args = {});

    // Graceful shutdown
    void stop();

    // Restart with same config
    bool restart();

    // Get RPC port
    int getRpcPort() const { return rpc_port_; }

    // Get P2P port
    int getP2pPort() const { return p2p_port_; }

    // Get datadir path
    std::string getDataDir() const { return datadir_; }

    // Get daemon log file path
    std::string getLogPath() const { return datadir_ + "/daemon.log"; }

    // Wait for daemon ready (RPC responding)
    bool waitForReady(int timeout_sec = 60);

    // Check if daemon is running
    bool isRunning() const;

    // Read cookie auth (public for test access)
    std::string readCookie() const;

private:
    std::string datadir_;
    int rpc_port_;
    int p2p_port_;
    pid_t daemon_pid_;
    std::vector<std::string> start_args_;  // Save for restart

    // Allocate unique ports
    void allocatePorts();
};

// ═══════════════════════════════════════════════════════════════════════
// RpcClient - JSON-RPC Wrapper
// ═══════════════════════════════════════════════════════════════════════

class RpcClient {
public:
    RpcClient(const std::string& host, int port, const std::string& cookie = "");

    // Generic RPC call
    // Returns JSON result on success, throws std::runtime_error on failure
    Json::Value call(const std::string& method, const Json::Value& params);

    // ─────────────────────────────────────────────────────────────────────
    // Mining RPC Methods
    // ─────────────────────────────────────────────────────────────────────

    Json::Value mining_info();
    Json::Value mining_start(int threads = 1, const std::string& address = "");
    Json::Value mining_stop();
    Json::Value mining_setaddress(const std::string& address);
    Json::Value mining_getaddress();

    // ─────────────────────────────────────────────────────────────────────
    // Wallet RPC Methods
    // ─────────────────────────────────────────────────────────────────────

    Json::Value wallet_createhd(const std::string& name);
    Json::Value wallet_load(const std::string& name);
    Json::Value wallet_encrypt(const std::string& passphrase);
    Json::Value wallet_unlock(const std::string& passphrase, int timeout = 600);
    Json::Value wallet_getnewaddress(const std::string& label = "");
    Json::Value wallet_getbalance();

    // ─────────────────────────────────────────────────────────────────────
    // Chain RPC Methods
    // ─────────────────────────────────────────────────────────────────────

    Json::Value getblockcount();
    Json::Value getbestblockhash();
    Json::Value getblockchaininfo();
    Json::Value generatetoaddress(int nblocks, const std::string& address);

    // Set cookie (for reconnection after daemon restart)
    void setCookie(const std::string& cookie) { cookie_ = cookie; }

private:
    std::string host_;
    int port_;
    std::string cookie_;
    int request_id_;

    // Execute HTTP POST request
    std::string httpPost(const std::string& url, const std::string& data, const std::string& auth);
};

// ═══════════════════════════════════════════════════════════════════════
// WalletHelper - Wallet Operations
// ═══════════════════════════════════════════════════════════════════════

class WalletHelper {
public:
    explicit WalletHelper(RpcClient& rpc) : rpc_(rpc) {}

    // Create and initialize wallet
    // Returns first address in wallet
    std::string createWallet(const std::string& name);

    // Encrypt wallet
    void encryptWallet(const std::string& passphrase);

    // Unlock wallet
    void unlockWallet(const std::string& passphrase, int timeout = 600);

    // Get new address
    std::string getNewAddress(const std::string& label = "");

    // Get balance
    uint64_t getBalance();

private:
    RpcClient& rpc_;
};

// ═══════════════════════════════════════════════════════════════════════
// ChainHelper - Chain Operations
// ═══════════════════════════════════════════════════════════════════════

class ChainHelper {
public:
    explicit ChainHelper(RpcClient& rpc) : rpc_(rpc) {}

    // Mine blocks to address
    void mineBlocks(int count, const std::string& address);

    // Wait for specific height
    bool waitForHeight(int target_height, int timeout_sec = 30);

    // Get current height
    int getHeight();

private:
    RpcClient& rpc_;
};

}  // namespace test
}  // namespace dinero

#endif  // DINERO_E2E_TEST_HARNESS_H
