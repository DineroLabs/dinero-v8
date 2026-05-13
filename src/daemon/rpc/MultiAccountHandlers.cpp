#include "rpc/rpc_registry.h"
#include "common/logger.h"
#include "daemon/main.h"
#include "wallet/wallet_manager.h"
#include "wallet/address.h"
#include "daemon/daemon_context.h"         // Week 5: DaemonContext access
#include "daemon/services/wallet_service.h"  // Week 5: WalletService access
#include <mutex>
#include <unordered_map>
#include <atomic>

// Note: din::Json is an alias for Json::Value, don't redeclare
// using din::Json;

namespace {
    struct Account {
        std::string id;
        std::string label;
        // Basic per-account derivation path/index - full implementation would support multiple accounts per wallet
    };
    std::mutex g_acc_mu;
    std::unordered_map<std::string, Account> g_accounts;
    std::string g_currentId;
    std::atomic<uint64_t> g_accountCounter{1};

    std::string makeId() {
        char buf[32];
        snprintf(buf, sizeof(buf), "acc-%llu", (unsigned long long)g_accountCounter++);
        return buf;
    }
}

void registerMultiAccount() {
    extern RpcRegistry g_rpcRegistry;
    
    g_rpcRegistry.registerHandler("multiaccount.create", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        std::string label = (params.isArray() && params.size() > 0 && params[0].isString())
                                ? params[0].asString() : "Account";
        std::lock_guard<std::mutex> lk(g_acc_mu);
        Account acc{makeId(), label};
        g_currentId = acc.id;
        g_accounts.emplace(acc.id, acc);
        
        din::Json result = din::obj();
        result["id"] = acc.id;
        result["label"] = acc.label;
        result["rpc_schema"] = "din.rpc.v1";
        return result;
    });

    g_rpcRegistry.registerHandler("multiaccount.getcurrent", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        std::lock_guard<std::mutex> lk(g_acc_mu);
        din::Json result = din::obj();
        if (g_currentId.empty()) {
            result["result"] = din::null();
        } else {
            const auto& acc = g_accounts.at(g_currentId);
            result["id"] = acc.id;
            result["label"] = acc.label;
        }
        result["rpc_schema"] = "din.rpc.v1";
        return result;
    });

    g_rpcRegistry.registerHandler("multiaccount.generatenewaddress", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        // Generate a new address using the wallet manager
        // Week 5: Migrated from dinero::legacy::g_wallet_manager() global to ctx.daemon->wallet->get()
        din::Json result = din::obj();
        
        if (!ctx.daemon || !ctx.daemon->wallet) {
            result["error"] = "Wallet service not available";
            result["rpc_schema"] = "din.rpc.v1";
            return result;
        }
        
        auto& wallet = ctx.daemon->wallet->get();
        if (!wallet.hasActiveWallet()) {
            // Try to open or create default wallet
            try {
                if (!wallet.exists("default")) {
                    wallet.create("default");
                }
                wallet.open("default");
            } catch (const std::exception& e) {
                result["error"] = "Failed to open or create default wallet: " + std::string(e.what());
                result["rpc_schema"] = "din.rpc.v1";
                return result;
            }
        }
        
        std::string address = wallet.getNewAddress("multiaccount");
        if (!address.empty()) {
            result["address"] = address;
            result["rpc_schema"] = "din.rpc.v1";
        } else {
            result["error"] = "Failed to generate new address";
            result["rpc_schema"] = "din.rpc.v1";
        }
        
        return result;
    });

    g_rpcRegistry.registerHandler("multiaccount.list", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        std::lock_guard<std::mutex> lk(g_acc_mu);
        din::Json result = din::arr();
        for (const auto& [id, acc] : g_accounts) {
            din::Json account = din::obj();
            account["id"] = acc.id;
            account["label"] = acc.label;
            account["active"] = (id == g_currentId);
            result.append(account);
        }
        
        din::Json response = din::obj();
        response["result"] = result;
        response["rpc_schema"] = "din.rpc.v1";
        return response;
    });
}
