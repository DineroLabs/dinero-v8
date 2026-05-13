/**
 * Sync RPC Methods - Context-Aware (Week 2 Migration)
 *
 * This file migrates wallet sync RPC methods from global g_sync_state to DaemonContext.
 *
 * OLD PATTERN (legacy):
 *   static WalletSyncState g_sync_state;
 *   extern WalletServices* g_wallet_services;
 *   result["is_syncing"] = g_sync_state.is_syncing.load();
 *
 * NEW PATTERN (context-aware):
 *   auto wallet_service = ctx.daemon->wallet;
 *   auto sync_state = wallet_service->getSyncState();
 *
 * Benefits:
 * - No dependency on global state
 * - Testable with mock sync services
 * - Clear dependency tracking
 * - Thread-safe service access
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/wallet_service.h"
#include "daemon/services/chainstate_service.h"
#include "common/logger.h"
#include <memory>
#include <chrono>

// ═══════════════════════════════════════════════════════════════
// CONTEXT-AWARE SYNC RPC HANDLERS (Week 2 Pattern)
// ═══════════════════════════════════════════════════════════════

/**
 * wallet.getsyncstate - Get wallet sync state
 *
 * OLD: g_sync_state.is_syncing.load()
 * NEW: ctx.daemon->wallet->getSyncState()
 */
din::Json rpc_context_wallet_getsyncstate(const ExecutionContext& ctx, const din::Json& params) {
    (void)params;
    din::Json result;

    try {
        if (!ctx.daemon || !ctx.daemon->wallet) {
            result["error"] = "Wallet service not available";
            result["is_syncing"] = false;
            result["last_scanned_height"] = 0;
            result["target_height"] = 0;
            result["rpc_schema"] = "din.wallet.v1";
            return result;
        }

        auto wallet_service = ctx.daemon->wallet;
        result["rpc_schema"] = "din.wallet.v1";

        if (!wallet_service->hasActiveWallet()) {
            result["wallet_loaded"] = false;
            result["is_syncing"] = false;
            result["last_scanned_height"] = 0;
            result["target_height"] = 0;
            result["progress_percent"] = 100.0;
            result["is_synced"] = true;
            result["blocks_remaining"] = 0;
            return result;
        }

        uint32_t chain_height = 0;
        if (ctx.daemon->chainstate) {
            chain_height = ctx.daemon->chainstate->getBlockHeight();
        } else {
            chain_height = wallet_service->get().getCurrentBlockchainHeight();
        }

        const auto scan_status = wallet_service->get().GetScanStatus(chain_height);
        const auto balance = wallet_service->get().getBalance();

        result["wallet_loaded"] = true;
        result["is_syncing"] = scan_status.is_scanning;
        result["last_scanned_height"] = static_cast<Json::UInt>(scan_status.scan_height);
        result["target_height"] = static_cast<Json::UInt>(scan_status.chain_height);
        result["progress_percent"] = scan_status.progress() * 100.0;
        result["is_synced"] = scan_status.is_synced();
        result["blocks_remaining"] =
            static_cast<Json::UInt>(scan_status.chain_height > scan_status.scan_height
                ? (scan_status.chain_height - scan_status.scan_height)
                : 0);
        result["utxos_discovered"] = balance.utxo_count;
        result["balance_discovered"] = balance.total;

    } catch (const std::exception& e) {
        result["error"] = std::string("getsyncstate error: ") + e.what();
        result["code"] = -1;
    }

    return result;
}

/**
 * wallet.getstatus - Get wallet status including sync state
 *
 * OLD: g_wallet_services->has_hd_wallet()
 * NEW: ctx.daemon->wallet->hasActiveWallet()
 */
din::Json rpc_context_wallet_getstatus(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        if (!ctx.daemon || !ctx.daemon->wallet) {
            result["wallet_loaded"] = false;
            result["encrypted"] = false;
            result["locked"] = false;
            result["balance"] = 0.0;
            result["address_count"] = 0;
            result["rpc_schema"] = "din.wallet.v1";
            return result;
        }

        auto wallet_service = ctx.daemon->wallet;

        // Check if wallet exists
        if (!wallet_service->hasActiveWallet()) {
            result["wallet_loaded"] = false;
            result["encrypted"] = false;
            result["locked"] = false;
            result["balance"] = 0.0;
            result["address_count"] = 0;
            result["rpc_schema"] = "din.wallet.v1";
            return result;
        }

        auto& wallet = wallet_service->get();
        result["wallet_loaded"] = true;

        result["encrypted"] = wallet.isWalletEncrypted();
        result["locked"] = wallet.isWalletLocked();

        // Get balance via context
        try {
            auto balance = wallet.getBalance();
            // WalletManager::getBalance() already returns DIN-denominated values.
            // Re-scaling here underreported balances by 1e8 in wallet.getstatus.
            result["balance"] = balance.confirmed;
            result["confirmed_balance"] = balance.confirmed;
            result["unconfirmed_balance"] = balance.unconfirmed;
            result["immature_balance"] = balance.immature;
            result["spendable_balance"] = balance.spendable;
            result["total_balance"] = balance.total;
            result["utxo_count"] = balance.utxo_count;
            result["immature_utxo_count"] = balance.immature_utxo_count;
            result["confidential_balance"] = 0.0;
            result["total_with_confidential"] = balance.total;

            if (ctx.daemon->chainstate) {
                auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
                if (chainstate && chainstate->utxoIndex()) {
                    const int64_t conf_balance_una = chainstate->utxoIndex()->GetConfidentialBalance().GetInt64();
                    const double conf_balance_din = static_cast<double>(conf_balance_una) / 1e8;
                    result["confidential_balance"] = conf_balance_din;
                    result["total_with_confidential"] = balance.total + conf_balance_din;
                }
            }
        } catch (...) {
            result["balance"] = 0.0;
            result["confirmed_balance"] = 0.0;
            result["unconfirmed_balance"] = 0.0;
            result["immature_balance"] = 0.0;
            result["spendable_balance"] = 0.0;
            result["total_balance"] = 0.0;
            result["utxo_count"] = 0;
            result["immature_utxo_count"] = 0;
            result["confidential_balance"] = 0.0;
            result["total_with_confidential"] = 0.0;
        }

        // Get address count
        try {
            auto addresses = wallet.getWalletAddresses();
            result["address_count"] = static_cast<int>(addresses.size());
        } catch (...) {
            result["address_count"] = 0;
        }

        // Include sync state
        result["sync_state"] = rpc_context_wallet_getsyncstate(ctx, params);

        result["rpc_schema"] = "din.wallet.v1";

    } catch (const std::exception& e) {
        result["error"] = std::string("getwalletstatus error: ") + e.what();
        result["code"] = -1;
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION FUNCTION
// ═══════════════════════════════════════════════════════════════

extern RpcRegistry g_rpcRegistry;

void registerSyncMethodsContext() {
    g_rpcRegistry.registerHandler("wallet.getsyncstate",
                                 rpc_context_wallet_getsyncstate,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.getstatus",
                                 rpc_context_wallet_getstatus,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // Backward-compat aliases used by older clients.
    g_rpcRegistry.registerAlias("wallet.status", "wallet.getstatus");
    g_rpcRegistry.registerAlias("getwalletstatus", "wallet.getstatus");

    dinero::g_logger.info("[RPC Context] ✅ sync context-aware handlers + aliases registered");
}
