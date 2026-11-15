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
#include "rpc/legacy_transparent_freshness.h"
#include "daemon/daemon_context.h"
#include "daemon/services/wallet_service.h"
#include "daemon/services/chainstate_service.h"
#include "common/logger.h"
#include <memory>
#include <chrono>

din::Json rpc_context_wallet_getbalance(const ExecutionContext&, const din::Json&);

namespace {
din::Json RefuseChangedStatusWallet() {
    din::Json result;
    result["error"] = "Wallet selection changed during status read";
    result["balance_error"] = result["error"];
    result["balance_available"] = false;
    result["error_code"] = "wallet_balance_not_current";
    result["retryable"] = true;
    result["code"] = -32091;
    result["rpc_schema"] = "din.wallet.v1";
    return result;
}

bool RefuseStaleLegacyWalletBalance(const ExecutionContext& ctx,
        const std::shared_ptr<dinero::WalletService>& service, din::Json& result,
        bool financial_status = true) {
    din::Json refusal;
    if (!RefuseIfLegacyTransparentTipNotCurrent(ctx, service, refusal)) return false;
    result["balance_available"] = false;
    result["balance_error"] = refusal["error"];
    // A sync-only reply must retain its non-financial progress fields. The
    // HTTP dispatcher promotes any `error` member to a top-level RPC error
    // and otherwise discards the whole result. Financial status keeps its
    // existing refusal; sync progress carries the refusal in balance_error.
    if (financial_status) result["error"] = refusal["error"];
    result["error_code"] = refusal["error_code"];
    result["retryable"] = refusal["retryable"];
    result["code"] = -32091;
    return true;
}
}

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
        // Sync progress remains useful while a legacy wallet is being
        // reconciled. A refused financial read must never appear as zero.
        try {
            if (!RefuseStaleLegacyWalletBalance(ctx, wallet_service, result, false)) {
                const auto balance = wallet_service->get().getBalance();
                if (!RefuseStaleLegacyWalletBalance(ctx, wallet_service, result, false)) {
                    result["utxos_discovered"] = balance.utxo_count;
                    result["balance_discovered"] = balance.total;
                    result["balance_available"] = true;
                }
            }
        } catch (const std::exception& e) {
            result["balance_available"] = false;
            result["balance_error"] = e.what();
            result["code"] = -32091;
        }

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

        auto wallet_use = dinero::WalletService::AcquireWalletUse(wallet_service);
        auto& wallet = wallet_use->Wallet();
        std::string selected_status_wallet;
        uint64_t selected_status_session = 0;
        bool transparent_status = false;
        {
            auto lease = wallet.AcquireDatabaseLease();
            selected_status_wallet = lease->WalletName();
            selected_status_session = lease->Session();
        }
        if (selected_status_wallet.empty() ||
            (!ctx.walletName.empty() && ctx.walletName != selected_status_wallet))
            return RefuseChangedStatusWallet();
        result["wallet_loaded"] = true;

        result["encrypted"] = wallet.isWalletEncrypted();
        result["locked"] = wallet.isWalletLocked();

        const auto selected_status_wallet_current = [&] {
            try {
                auto lease = wallet.AcquireDatabaseLease();
                return lease->WalletName() == selected_status_wallet &&
                       lease->Session() == selected_status_session;
            } catch (...) {
                return false;
            }
        };

        // Get balance via context
        try {
            if (!RefuseStaleLegacyWalletBalance(ctx, wallet_service, result)) {
                bool transparent_only = false;
                {
                    auto lease = wallet.AcquireDatabaseLease();
                    if (lease->WalletName() != selected_status_wallet ||
                        lease->Session() != selected_status_session)
                        return RefuseChangedStatusWallet();
                    transparent_only = lease->LegacyTransparentState() ==
                        dinero::WalletManager::LegacyTransparentState::Complete;
                }
                if (transparent_only) {
                    transparent_status = true;
                    // The v3 spend capability and reservation-aware amount are
                    // certified by the same selected-tip read as getbalance.
                    // Do not rederive either from the old wallet row balance.
                    auto selected_context = ctx;
                    selected_context.walletName = selected_status_wallet;
                    const auto current = rpc_context_wallet_getbalance(selected_context, din::obj());
                    if (!selected_status_wallet_current() ||
                        (current.isMember("walletname") &&
                         current["walletname"].asString() != selected_status_wallet))
                        return RefuseChangedStatusWallet();
                    if (current.isMember("error")) {
                        result["balance_available"] = false;
                        result["balance_error"] = current["error"];
                        result["error"] = current["error"];
                        for (const char* key : {"error_code", "retryable", "code"}) {
                            if (current.isMember(key)) result[key] = current[key];
                        }
                    } else {
                        result["balance"] = current["confirmed"];
                        result["confirmed_balance"] = current["confirmed"];
                        result["unconfirmed_balance"] = current["unconfirmed"];
                        result["immature_balance"] = current["immature"];
                        // An imported-runtime ordinary balance may be readable
                        // without authorizing a v3 transparent send. Require an
                        // explicit send certificate before exposing spendable.
                        const bool send_ready = current.isMember("send_ready") &&
                            current["send_ready"].asBool() &&
                            current["send_scope"].asString() == "transparent_recovered_v3";
                        result["spendable_balance"] = send_ready
                            ? current["spendable"] : din::Json(Json::nullValue);
                        result["total_balance"] = current["total"];
                        result["utxo_count"] = current["utxo_count"];
                        // Keep this status-only diagnostic without extending
                        // getbalance's frozen public RPC schema. Its financial
                        // amounts and send capability come from the certified
                        // getbalance read above.
                        result["immature_utxo_count"] =
                            wallet.getBalance().immature_utxo_count;
                        result["confidential_balance"] = din::Json(Json::nullValue);
                        result["total_with_confidential"] = din::Json(Json::nullValue);
                        result["balance_available"] = true;
                        result["wallet_scope"] = "legacy_transparent_only";
                        result["whole_wallet_ready"] = false;
                        result["send_ready"] = send_ready;
                        if (!send_ready)
                            result["send_unavailable_reason"] =
                                "Transparent wallet send source is not ready";
                        for (const char* key : {"wallet_scope", "balance_scope",
                                                "whole_wallet_ready"}) {
                            if (current.isMember(key)) result[key] = current[key];
                        }
                        if (send_ready) {
                            result["send_scope"] = current["send_scope"];
                            result.removeMember("send_unavailable_reason");
                        }
                    }
                } else {
                    const auto balance = wallet.getBalance();
                    if (!RefuseStaleLegacyWalletBalance(ctx, wallet_service, result)) {
                        // WalletManager::getBalance() already returns DIN.
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
                        result["balance_available"] = true;
                        if (ctx.daemon->chainstate) {
                            auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
                            if (chainstate && chainstate->utxoIndex()) {
                                const int64_t conf_balance_una = chainstate->utxoIndex()->GetConfidentialBalance().GetInt64();
                                const double conf_balance_din = static_cast<double>(conf_balance_una) / 1e8;
                                result["confidential_balance"] = conf_balance_din;
                                result["total_with_confidential"] = balance.total + conf_balance_din;
                            }
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            result["balance_available"] = false;
            result["balance_error"] = e.what();
            result["error"] = e.what();
            result["code"] = -32091;
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

        if (!selected_status_wallet_current())
            return RefuseChangedStatusWallet();
        if (transparent_status) {
            din::Json stale;
            auto selected_context = ctx;
            selected_context.walletName = selected_status_wallet;
            try {
                if (RefuseStaleLegacyWalletBalance(selected_context, wallet_service, stale)) {
                    stale["rpc_schema"] = "din.wallet.v1";
                    return stale;
                }
            } catch (...) {
                return RefuseChangedStatusWallet();
            }
        }
        if (!selected_status_wallet_current())
            return RefuseChangedStatusWallet();

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
