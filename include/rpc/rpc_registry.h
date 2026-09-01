#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>
#include "din_json.h"

// Forward declare DaemonContext (defined in daemon/daemon_context.h)
// Note: DaemonContext is in global namespace, not dinero::
struct DaemonContext;

// Forward declarations for Phase 2A + Phase 3A + STEP 3.1
namespace dinero {
    class TxMempool;
    class UTXOView;
    class WalletManager;
    class Mempool;  // STEP 3.1: New policy-aware mempool
}

// Forward declaration for logger interface
namespace dinero {
    class ILogger;
}

struct ExecutionContext {
    std::string walletName;
    std::string user;
    std::string cookie;
    std::string client_id;  // WebSocket client identifier (for event subscriptions)
    std::unordered_map<std::string, std::string> metadata;

    // Week 2 Migration: Access to service layer via DaemonContext
    // This allows RPC handlers to access services instead of legacy globals
    // Example: ctx.daemon->chainstate->chainDB() instead of dinero::legacy::g_chain_db_direct()
    DaemonContext* daemon = nullptr;

    // Dependency Injection: Logger interface (replaces dinero::g_logger)
    // Typically initialized from daemon->logger_interface or daemon->wallet_logger
    dinero::ILogger* logger = nullptr;

    // Phase 2A: Direct access to mempool and utxo_view (eliminates globals)
    dinero::TxMempool* mempool = nullptr;  // Legacy mempool (deprecated)
    dinero::UTXOView* utxo_view = nullptr;

    // STEP 3.1: New policy-aware mempool (for stats/policy queries)
    dinero::Mempool* mempool_v2 = nullptr;

    // Phase 3A: Direct access to wallet_manager (eliminates g_wallet_manager global)
    dinero::WalletManager* wallet_manager = nullptr;
};

using RpcHandler = std::function<din::Json(const ExecutionContext&, const din::Json&)>;

struct RpcParamMeta {
    std::string name;
    std::string type;
    std::string desc;
    bool required = false;
};

struct RpcResultMeta {
    std::string type;
    std::string desc;
};

struct RpcMethodMeta {
    std::string name;
    std::string ns;
    std::string description;
    std::vector<RpcParamMeta> params;
    RpcResultMeta result;
    std::string help;
};

// Alias metadata for deprecation tracking
struct RpcAliasInfo {
    std::string canonical_name;  // The modern namespaced method name
    std::string message;          // "Use mempool.getinfo" etc.
};

enum class RegisterMode { IfAbsent, Overwrite };

class RpcRegistry {
public:
  bool registerHandler(const std::string& method, RpcHandler fn,
                       const std::string& owner="");
  bool registerHandler(const std::string& method, RpcHandler fn,
                       const RpcMethodMeta& meta, const std::string& owner="");
  bool registerHandler(const std::string& method, RpcHandler fn,
                       RegisterMode mode, const std::string& owner="");
  bool registerHandler(const std::string& method, RpcHandler fn,
                       const RpcMethodMeta& meta, RegisterMode mode, const std::string& owner="");

  // Alias support: allows multiple names for the same handler
  // Example: registerAlias("getmempoolinfo", "mempool.getinfo")
  void registerAlias(const std::string& alias, const std::string& target);

  std::string getMethodOwner(const std::string& method) const;
  RpcHandler* lookup(const std::string& method);
  std::vector<std::string> methodNames() const;
  bool has(const std::string& method) const;

  // Get method metadata (returns nullptr if no metadata registered)
  const RpcMethodMeta* getMethodMeta(const std::string& method) const;

  // Alias introspection (for help/documentation)
  bool isAlias(const std::string& method) const;
  const RpcAliasInfo* getAliasInfo(const std::string& alias) const;
  std::vector<std::string> getAliases(const std::string& canonical_method) const;

  // Restore the process-global registry to its static-registration baseline
  // before wiring handlers that capture a new DaemonContext. The first call
  // records that baseline; later calls discard handlers from the prior daemon
  // lifetime so embedded stop/start cycles cannot retain dangling contexts.
  void beginRuntimeRegistrationCycle();

private:
  mutable std::mutex mtx_;
  std::unordered_map<std::string, RpcHandler>    handlers_;
  std::unordered_map<std::string, RpcMethodMeta> meta_;
  std::unordered_map<std::string, std::string>   owners_;
  std::unordered_map<std::string, std::string>   aliases_;      // alias -> target
  std::unordered_map<std::string, RpcAliasInfo>  alias_info_;   // alias -> metadata
  std::string last_error_;

  bool runtime_baseline_captured_ = false;
  std::unordered_map<std::string, RpcHandler> runtime_baseline_handlers_;
  std::unordered_map<std::string, RpcMethodMeta> runtime_baseline_meta_;
  std::unordered_map<std::string, std::string> runtime_baseline_owners_;
  std::unordered_map<std::string, std::string> runtime_baseline_aliases_;
  std::unordered_map<std::string, RpcAliasInfo> runtime_baseline_alias_info_;
};

// Global RPC registry instance (defined in rpc_registry.cpp)
extern RpcRegistry g_rpcRegistry;
