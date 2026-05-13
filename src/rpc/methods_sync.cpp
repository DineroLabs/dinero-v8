#include "rpc/methods_wallet.h"
#include "rpc/rpc_registry.h"
#include "wallet/wallet_api.h"
#include <iostream>
#include <chrono>
#include <atomic>

extern RpcRegistry g_rpcRegistry;

namespace din {
namespace rpc {

// ═══════════════════════════════════════════════════════════════
// Wallet Sync State Management
// ═══════════════════════════════════════════════════════════════

struct WalletSyncState {
    std::atomic<uint32_t> last_scanned_height{0};
    std::atomic<uint32_t> target_height{0};
    std::atomic<bool> is_syncing{false};
    std::atomic<int64_t> last_sync_time{0};  // Unix timestamp
    std::atomic<uint32_t> utxos_discovered{0};
    std::atomic<uint64_t> balance_discovered{0};  // in una
};

// Global sync state (thread-safe)
static WalletSyncState g_sync_state;

// Cache helpers
void UpdateSyncProgress(uint32_t height, uint32_t target) {
    g_sync_state.last_scanned_height = height;
    g_sync_state.target_height = target;
    g_sync_state.last_sync_time = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;
}

void SetSyncActive(bool active) {
    g_sync_state.is_syncing = active;
    if (active) {
        g_sync_state.utxos_discovered = 0;
        g_sync_state.balance_discovered = 0;
    }
}

void AddDiscoveredUTXO(uint64_t amount) {
    g_sync_state.utxos_discovered++;
    g_sync_state.balance_discovered += amount;
}

// ═══════════════════════════════════════════════════════════════
// Sync State RPC Methods
// ═══════════════════════════════════════════════════════════════

din::Json getsyncstate_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        result["is_syncing"] = g_sync_state.is_syncing.load();
        result["last_scanned_height"] = static_cast<int>(g_sync_state.last_scanned_height.load());
        result["target_height"] = static_cast<int>(g_sync_state.target_height.load());
        result["last_sync_time"] = static_cast<Json::Int64>(g_sync_state.last_sync_time.load());

        if (g_sync_state.is_syncing) {
            uint32_t current = g_sync_state.last_scanned_height.load();
            uint32_t target = g_sync_state.target_height.load();
            if (target > 0) {
                double progress = (static_cast<double>(current) / target) * 100.0;
                result["progress_percent"] = progress;
            }
            result["utxos_discovered"] = static_cast<int>(g_sync_state.utxos_discovered.load());
            result["balance_discovered"] = static_cast<double>(g_sync_state.balance_discovered.load()) / 1e8;
        }

        result["rpc_schema"] = "din.wallet.v1";

    } catch (const std::exception& e) {
        result["error"] = std::string("getsyncstate error: ") + e.what();
        result["code"] = -1;
    }

    return result;
}

din::Json getwalletstatus_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        // Check if wallet exists
        if (!g_wallet_services || !g_wallet_services->has_hd_wallet()) {
            result["wallet_loaded"] = false;
            result["encrypted"] = false;
            result["locked"] = false;
            result["rpc_schema"] = "din.wallet.v1";
            return result;
        }

        auto wallet = g_wallet_services->hd_wallet();
        result["wallet_loaded"] = true;
        result["encrypted"] = wallet->IsEncrypted();
        result["locked"] = g_wallet_services->is_locked();

        // Get balance
        try {
            int64_t balance = walletapi::GetBalance();
            result["balance"] = static_cast<double>(balance) / 1e8;
        } catch (...) {
            result["balance"] = 0.0;
        }

        // Get address count
        try {
            auto addresses = wallet->GetAllAddresses();
            result["address_count"] = static_cast<int>(addresses.size());
        } catch (...) {
            result["address_count"] = 0;
        }

        // Sync state
        result["sync_state"] = getsyncstate_impl(ctx, params);

        result["rpc_schema"] = "din.wallet.v1";

    } catch (const std::exception& e) {
        result["error"] = std::string("getwalletstatus error: ") + e.what();
        result["code"] = -1;
    }

    return result;
}

} // namespace rpc
} // namespace din

// ═══════════════════════════════════════════════════════════════
// Registration Function
// ═══════════════════════════════════════════════════════════════

void registerSyncRPC() {
    std::cout << "[Sync RPC] Registering wallet sync state methods..." << std::endl;

    g_rpcRegistry.registerHandler("wallet.getsyncstate",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return din::rpc::getsyncstate_impl(ctx, params);
        },
        "wallet");

    g_rpcRegistry.registerHandler("wallet.getstatus",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return din::rpc::getwalletstatus_impl(ctx, params);
        },
        "wallet");

    // Backward-compat aliases used by older clients.
    g_rpcRegistry.registerAlias("wallet.status", "wallet.getstatus");
    g_rpcRegistry.registerAlias("getwalletstatus", "wallet.getstatus");

    std::cout << "[Sync RPC] ✅ Registered sync state methods + aliases:" << std::endl;
    std::cout << "[Sync RPC]  - wallet.getsyncstate" << std::endl;
    std::cout << "[Sync RPC]  - wallet.getstatus" << std::endl;
    std::cout << "[Sync RPC]  - wallet.status (alias)" << std::endl;
    std::cout << "[Sync RPC]  - getwalletstatus (alias)" << std::endl;
}

// Export sync state helpers for use by wallet rescan/sync operations
namespace din {
namespace sync {
    void updateProgress(uint32_t height, uint32_t target) {
        din::rpc::UpdateSyncProgress(height, target);
    }

    void setSyncActive(bool active) {
        din::rpc::SetSyncActive(active);
    }

    void addDiscoveredUTXO(uint64_t amount) {
        din::rpc::AddDiscoveredUTXO(amount);
    }
}
}
