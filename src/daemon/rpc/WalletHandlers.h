#pragma once
#include "compat/jsoncpp_compat.h"
#include <string>
#include <memory>
#include <optional>
#include "wallet/wallet_manager.h"
#include "daemon/NodeInfo.h"  // Use the common NodeInfo struct
#if DIN_ENABLE_LEGACY_RPC
#include "daemon/rpc_server.h"
#endif

namespace dinero {
    class WalletManager;
}

class DaemonRpc;

class WalletHandlers {
public:
#if DIN_ENABLE_LEGACY_RPC
    explicit WalletHandlers(dinero::RPCServer& rpc_server);
#else
    WalletHandlers();
#endif
    
    Json::Value create(const Json::Value& params);
    Json::Value load(const Json::Value& params);
    Json::Value restore(const Json::Value& params);
    Json::Value info(const Json::Value& params);
    Json::Value getNewAddress(const Json::Value& params);
    Json::Value listAddresses(const Json::Value& params);

private:
#if DIN_ENABLE_LEGACY_RPC
    dinero::RPCServer& rpc_server_;
#endif
    dinero::WalletManager* wallet_manager_;
    std::string nodeinfo_path_;

    std::optional<NodeInfo> loadNodeInfo();
    std::unique_ptr<DaemonRpc> createDaemonRpc();
};
