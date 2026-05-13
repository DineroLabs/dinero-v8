#pragma once

#include "http_rpc_server.h"
#include "wallet/hd_wallet.h"
#include <json/json.h>
#include <atomic>
#include <mutex>
#include <string>
#include <chrono>
#include <vector>
#include <thread>
#include <memory>
#include <functional>

namespace dinero {
namespace rpc {

// ═══════════════════════════════════════════════════════════
// Mining State Management
// ═══════════════════════════════════════════════════════════

struct MiningState {
    std::atomic<bool> mining_enabled{false};
    std::atomic<bool> shutdown_requested{false};
    std::atomic<uint32_t> num_threads{0};
    std::atomic<uint64_t> total_hashes{0};
    std::atomic<uint32_t> blocks_found{0};
    std::atomic<double> current_hashrate{0.0};
    std::string mining_address;
    std::vector<std::thread> mining_threads;
    std::mutex mining_mutex;
    std::chrono::steady_clock::time_point mining_start_time;
    std::chrono::steady_clock::time_point last_hashrate_update;
};

// ═══════════════════════════════════════════════════════════
// Configuration Helper Struct
// ═══════════════════════════════════════════════════════════

struct MiningConfig {
    int rpc_port;
    std::string datadir;
    bool regtest;
    bool testnet;
};

// ═══════════════════════════════════════════════════════════
// Mining RPC Method Registrations
// ═══════════════════════════════════════════════════════════

/**
 * Register all mining-related RPC methods with the HTTP RPC server.
 *
 * @param server RPC server to register methods with
 * @param mining_state Shared mining state object
 * @param config Mining configuration (rpc_port, datadir, network)
 * @param registerMiningAddressCallback Callback to register mining address with wallet
 * @param g_hd_wallet Shared pointer to HD wallet (for address derivation)
 */
void registerMiningMethods(
    HttpRpcServer* server,
    std::shared_ptr<MiningState> mining_state,
    const MiningConfig& config,
    std::function<bool(const std::string&)> registerMiningAddressCallback,
    std::shared_ptr<std::shared_ptr<HDWallet>> g_hd_wallet
);

/**
 * Register all mining-related RPC methods with vNext architecture (global g_rpcRegistry).
 *
 * @param mining_state Shared mining state object
 * @param config Mining configuration (rpc_port, datadir, network)
 * @param registerMiningAddressCallback Callback to register mining address with wallet
 * @param g_hd_wallet Shared pointer to HD wallet (for address derivation)
 */
void registerMiningMethodsVNext(
    std::shared_ptr<MiningState> mining_state,
    const MiningConfig& config,
    std::function<bool(const std::string&)> registerMiningAddressCallback,
    std::shared_ptr<std::shared_ptr<HDWallet>> g_hd_wallet
);

} // namespace rpc
} // namespace dinero

// vNext registration in din namespace (shorter alias)
namespace din {
namespace rpc {
    void registerMiningMethodsVNext(
        std::shared_ptr<dinero::rpc::MiningState> mining_state,
        const dinero::rpc::MiningConfig& config,
        std::function<bool(const std::string&)> registerMiningAddressCallback,
        std::shared_ptr<std::shared_ptr<HDWallet>> g_hd_wallet
    );

    // Standalone registration for Stratum methods (context-only, no MiningState)
    void registerStratumMethodsContext();
} // namespace rpc
} // namespace din
