#pragma once

#include <string>
#include <vector>
#include <memory>
#include "compat/jsoncpp_compat.h"

namespace dinero {
    class RPCServer;
}

namespace dinero {
namespace interfaces {

/**
 * RpcExecutor - Provides in-process RPC execution for Qt GUI
 * This eliminates HTTP overhead and provides Bitcoin-Qt style direct calls
 */
class RpcExecutor {
public:
    explicit RpcExecutor(dinero::RPCServer* rpc_server);
    ~RpcExecutor() = default;
    
    // Direct RPC execution (no HTTP overhead)
    std::string executeCommand(const std::string& command);
    Json::Value executeJson(const std::string& method, const Json::Value& params);
    
    // Batch execution for efficiency
    std::vector<Json::Value> executeBatch(
        const std::vector<std::pair<std::string, Json::Value>>& requests);
    
    // Command parsing utilities
    bool parseCommand(const std::string& command, std::string& method, Json::Value& params);
    
    // High-level convenience methods (avoid RPC parsing overhead)
    struct WalletInfo {
        double balance = 0.0;
        bool encrypted = false;
        bool unlocked = false;
        std::string name;
    };
    
    struct MiningInfo {
        bool generating = false;
        int threads = 0;
        double hashrate = 0.0;
        std::string mining_address;
    };
    
    struct BlockchainInfo {
        int blocks = 0;
        std::string bestblockhash;
        double difficulty = 0.0;
        std::string chain;
        bool initialblockdownload = false;
    };
    
    // High-level methods that bypass RPC parsing
    WalletInfo getWalletInfo();
    MiningInfo getMiningInfo();
    BlockchainInfo getBlockchainInfo();
    std::string getNewAddress(const std::string& addressType = "bech32");
    bool validateAddress(const std::string& address);
    
private:
    dinero::RPCServer* m_rpc_server;
    
    // Helper methods
    Json::Value createErrorResponse(int code, const std::string& message);
    void validateRpcServer();
};

} // namespace interfaces
} // namespace dinero
