#pragma once

#include <string>
#include <functional>
#include <memory>
#include <cstdint>
#include <vector>
#include "compat/jsoncpp_compat.h"

namespace dinero {
namespace interfaces {

/**
 * Node initialization options - similar to Bitcoin Core's AppInitParameterInteraction
 */
struct NodeInitOptions {
    std::string datadir;
    bool server = true;
    bool listen = true;
    int rpc_port = 20998;  // Default mainnet port
    int p2p_port = 20999;  // Default mainnet port
    bool testnet = false;
    bool regtest = false;
    std::string mining_address;
    int mining_threads = 1;
    std::vector<std::string> addnodes;  // Bootstrap peer addresses
    
    // Network-specific port adjustments
    void adjustForNetwork() {
        if (testnet) {
            rpc_port = 20998;
            p2p_port = 21000;
        } else if (regtest) {
            rpc_port = 20996;
            p2p_port = 21001;
        } else {
            // mainnet uses correct ports
            rpc_port = 20998;
            p2p_port = 20999;
        }
    }
};

/**
 * Node interface - similar to Bitcoin Core's interfaces::Node
 * This abstracts the core node functionality for GUI use
 */
class Node {
public:
    virtual ~Node() = default;
    
    // Lifecycle management
    virtual bool start(const NodeInitOptions& options, 
                      std::string& error, 
                      std::function<void(const std::string&)> progress = nullptr) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    
    // Blockchain queries
    virtual int getBestBlockHeight() const = 0;
    virtual std::string getBestBlockHash() const = 0;
    virtual int getConnectionCount() const = 0;
    
    // Network info
    virtual void getNetworkStats(int& peers, double& hashrate) const = 0;
    virtual bool isTestNet() const = 0;
    virtual bool isRegTest() const = 0;
    
    // Wallet operations
    virtual bool hasWallet() const = 0;
    virtual bool loadWallet(const std::string& name, std::string& error) = 0;
    virtual bool createWallet(const std::string& name, std::string& error) = 0;
    virtual double getBalance() const = 0;
    virtual std::string getNewAddress(const std::string& addressType = "") = 0;
    virtual bool sendToAddress(const std::string& address, double amount, 
                              std::string& txid, std::string& error) = 0;
    
    // Mining operations
    virtual bool setGenerate(bool generate, int threads, std::string& error) = 0;
    virtual bool getMiningInfo(bool& generating, int& threads, double& hashrate) const = 0;
    virtual bool generateToAddress(int blocks, const std::string& address, std::string& error) = 0;
    
    // RPC server control (for embedded mode)
    virtual bool isRpcServerRunning() const = 0;
    virtual int getRpcPort() const = 0;
    
    // In-process RPC execution (Bitcoin-Qt style)
    virtual std::string executeRpc(const std::string& command) = 0;
    virtual Json::Value executeRpcJson(const std::string& method, const Json::Value& params) = 0;
    virtual std::vector<Json::Value> executeRpcBatch(
        const std::vector<std::pair<std::string, Json::Value>>& requests) = 0;
};

/**
 * @brief Factory function to create a Node instance
 * 
 * This function creates and returns a Node instance that can be used
 * to interact with the Dinero blockchain daemon.
 * 
 * @return std::unique_ptr<Node> A unique pointer to a Node instance
 */
std::unique_ptr<Node> MakeNode();

} // namespace interfaces
} // namespace dinero
