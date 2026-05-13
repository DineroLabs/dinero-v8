/**
 * Node Diagnostics RPC Methods - Context-Aware
 *
 * Modernized handlers using DaemonContext (no globals).
 *
 * OLD PATTERN (legacy):
 *   Json::Value rpc_node_info(RPCServer& server, const Json::Value& params)
 *   extern din::p2p::NetworkConfig g_network_config;
 *
 * NEW PATTERN (context-aware):
 *   din::Json rpc_context_node_info(const ExecutionContext& ctx, const din::Json& params)
 *   ctx.daemon->p2p->getProtocolVersion()
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/services/p2p_service.h"
#include "daemon/services/mining_service.h"
#include "daemon/services/wallet_service.h"
#include "daemon/services/config_service.h"
#include "cli/version.h"
#include "common/logger.h"
#include <memory>
#include <chrono>
#include <algorithm>

// Global RPC registry (defined in rpc_registry.cpp)
extern RpcRegistry g_rpcRegistry;

namespace {

struct NetworkInfo {
    std::string name = "mainnet";
    bool is_testnet = false;
    bool is_regtest = false;
};

NetworkInfo ResolveNetworkInfo(const ExecutionContext& ctx) {
    NetworkInfo info;
    if (!ctx.daemon || !ctx.daemon->config) {
        return info;
    }

    auto config = std::dynamic_pointer_cast<dinero::ConfigService>(ctx.daemon->config);
    if (!config) {
        return info;
    }

    if (config->IsRegtest()) {
        info.name = "regtest";
        info.is_regtest = true;
    } else if (config->IsTestnet()) {
        info.name = "testnet";
        info.is_testnet = true;
    }

    return info;
}

} // namespace

// ═══════════════════════════════════════════════════════════════
// CONTEXT-AWARE NODE DIAGNOSTICS (Modern Service-Based)
// ═══════════════════════════════════════════════════════════════

/**
 * node.info - Get comprehensive node information
 *
 * Returns: version, git_commit, build_date, protocol_version,
 *          network info, blockchain state, peer count, etc.
 */
din::Json rpc_context_node_info(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Version information
    auto version_info = dinero::cli::getVersionInfo();
    result["version"] = version_info.version;
    result["git_commit"] = version_info.gitSha;  // Full 40-char SHA
    result["build_date"] = version_info.buildDate;

    // P2P/Network information (service-based, no globals)
    // Protocol constants from P2PService (no hardcoded globals)
    result["protocol_version"] = static_cast<int>(dinero::P2PService::GetProtocolVersion());
    result["user_agent"] = dinero::P2PService::GetUserAgent();

    const NetworkInfo network_info = ResolveNetworkInfo(ctx);
    result["network"] = network_info.name;
    result["testnet"] = network_info.is_testnet;
    result["regtest"] = network_info.is_regtest;
    
    if (ctx.daemon && ctx.daemon->p2p) {
        auto p2p = std::dynamic_pointer_cast<dinero::P2PService>(ctx.daemon->p2p);
        if (p2p) {
            int peer_count = static_cast<int>(p2p->GetPeerCount());
            result["peer_count"] = peer_count;
            result["connections"] = peer_count;  // GUI compatibility
        } else {
            result["peer_count"] = 0;
            result["connections"] = 0;
        }
    } else {
        result["peer_count"] = 0;
        result["connections"] = 0;
    }

    // Network+chain state (service-based)
    if (ctx.daemon && ctx.daemon->chainstate) {
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (chainstate) {
            const uint32_t height = chainstate->getBlockHeight();
            const auto ibd = chainstate->GetIBDProgress();
            uint32_t headers = ibd.network_height > 0 ? ibd.network_height : ibd.local_height;
            if (headers < height) {
                headers = height;
            }

            result["blocks"] = static_cast<int>(height);
            result["headers"] = static_cast<int>(headers);
            // result["best_block_hash"] = chainstate->getBestBlockHash();
            // result["difficulty"] = chainstate->getDifficulty();
        } else {
            result["blocks"] = 0;
            result["headers"] = 0;
        }
    } else {
        result["blocks"] = 0;
        result["headers"] = 0;
    }

    // Mempool information
    if (ctx.daemon && ctx.daemon->mempool) {
        auto mempool = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
        if (mempool) {
            result["mempool_tx_count"] = static_cast<int>(mempool->size());
        } else {
            result["mempool_tx_count"] = 0;
        }
    } else {
        result["mempool_tx_count"] = 0;
    }

    // Wallet information (if available)
    if (ctx.daemon && ctx.daemon->wallet) {
        auto wallet = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
        if (wallet && wallet->hasActiveWallet()) {
            result["wallet_loaded"] = true;
            // Encryption/lock status is not exposed through WalletService yet.
        } else {
            result["wallet_loaded"] = false;
        }
    } else {
        result["wallet_loaded"] = false;
    }

    // Mining information (if available)
    if (ctx.daemon && ctx.daemon->mining) {
        auto mining = std::dynamic_pointer_cast<dinero::MiningService>(ctx.daemon->mining);
        if (mining) {
            result["mining_enabled"] = mining->isMiningEnabled();
            result["hashrate"] = mining->getHashrate();
        } else {
            result["mining_enabled"] = false;
            result["hashrate"] = 0.0;
        }
    } else {
        result["mining_enabled"] = false;
        result["hashrate"] = 0.0;
    }

    // Uptime
    static auto start_time = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time
    ).count();
    result["uptime"] = static_cast<int>(uptime);
    
    // Schema versions (Phase 3 - Nov 2025)
    // Unknown until schema managers are exposed via service interfaces.
    din::Json schema_versions;
    schema_versions["wallet"] = din::null();
    schema_versions["explorer"] = din::null();
    schema_versions["mempool"] = din::null();
    schema_versions["peers"] = din::null();
    result["schema"] = schema_versions;

    return result;
}

/**
 * rpc.methods - List all available RPC methods
 *
 * Returns array of method names with categories
 */
din::Json rpc_context_list_methods(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    std::vector<std::string> method_names = g_rpcRegistry.methodNames();
    std::sort(method_names.begin(), method_names.end());

    din::Json methods(Json::arrayValue);
    for (const auto& method : method_names) {
        methods.append(method);
    }

    result["methods"] = methods;
    result["count"] = static_cast<int>(method_names.size());
    
    return result;
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION
// ═══════════════════════════════════════════════════════════════

void WireDiagnosticsRpcContext() {
    // Register context-aware diagnostic methods
    g_rpcRegistry.registerHandler("node.info",
                                 rpc_context_node_info,
                                 RegisterMode::Overwrite,
                                 "Get comprehensive node information");
    
    // Register getinfo as alias for node.info (GUI compatibility)
    g_rpcRegistry.registerHandler("getinfo",
                                 rpc_context_node_info,
                                 RegisterMode::Overwrite,
                                 "Get comprehensive node information (alias for node.info)");
                                 
    g_rpcRegistry.registerHandler("rpc.methods",
                                 rpc_context_list_methods,
                                 RegisterMode::Overwrite,
                                 "List all available RPC methods");
    
    dinero::g_logger.info("[RPC] Registered context-aware diagnostic methods");
}
