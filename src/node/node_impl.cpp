#include "node/interfaces.h"
#include "node/rpc_executor.h"
#include "daemon/miner_core.h"
#include "daemon/rpc_server.h"
#include "daemon/config.h"
#include "common/logger.h"
#include "consensus/chainparams.h"
#include "wallet/address.h"
#include "dinero/core/wallet/sqlite_wallet.h"
#include "crypto/dinero_crypto_minimal.h"
#include <memory>
#include <thread>
#include <atomic>

// Forward declarations for consensus functions
namespace dinero {
    void SelectParams(const std::string& network);
}

namespace dinero {
namespace interfaces {

/**
 * Implementation of Node interface that wraps the existing daemon components
 */
class NodeImpl : public Node {
private:
    // Core components (same as daemon globals)
    std::unique_ptr<dinero::Blockchain> m_blockchain;
    std::unique_ptr<dinero::Mining> m_mining;
    std::unique_ptr<dinero::MinerCore> m_miner_core;
    std::unique_ptr<dinero::RPCServer> m_rpc_server;
    std::unique_ptr<dinero::interfaces::RpcExecutor> m_rpc_executor;
    std::unique_ptr<Dinero::Wallet::SQLiteWallet> m_wallet;
    
    // State tracking
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_rpc_running{false};
    NodeInitOptions m_options;
    std::thread m_rpc_thread;
    std::string m_current_address;
    
    // Helper to get network HRP
    std::string getNetworkHRP() {
        if (isRegTest()) return "rdin";
        if (isTestNet()) return "tdin";
        return "din";
    }
    
public:
    NodeImpl() = default;
    
    ~NodeImpl() override {
        if (m_running.load()) {
            stop();
        }
    }
    
    bool start(const NodeInitOptions& options, 
               std::string& error, 
               std::function<void(const std::string&)> progress) override {
        
        if (m_running.load()) {
            error = "Node is already running";
            return false;
        }
        
        m_options = options;
        m_options.adjustForNetwork();
        
        try {
            // Set network parameters based on options
            if (options.testnet) {
                dinero::SelectParams("test");
            } else if (options.regtest) {
                dinero::SelectParams("regtest");
            } else {
                dinero::SelectParams("main");
            }
            
            if (progress) progress("Initializing blockchain...");
            
            // Initialize blockchain (same as daemon main.cpp)
            std::string chain_dir = m_options.datadir;
            if (chain_dir.empty()) {
                chain_dir = "./data";
            } else {
                // Append network suffix to datadir (like Bitcoin Core)
                if (m_options.testnet) {
                    chain_dir += "/testnet";
                } else if (m_options.regtest) {
                    chain_dir += "/regtest";
                }
                // Use datadir directly for SQLite databases (not blockchain_data subdirectory)
            }
            
            m_blockchain = std::make_unique<dinero::Blockchain>(chain_dir);
            
            if (!m_blockchain->initializeGenesisBlock()) {
                error = "Failed to initialize genesis block";
                return false;
            }
            
            if (progress) progress("Configuring network peers...");
            
            // Add bootstrap peers if specified
            if (!m_options.addnodes.empty()) {
                for (const auto& peer : m_options.addnodes) {
                    if (progress) progress("Adding bootstrap peer: " + peer);
                    // TODO: Add peer to networking component when available
                    // For now, log the peers that would be added
                }
            }
            
            if (progress) progress("Initializing mining component...");
            
            // Initialize mining component
            m_mining = std::make_unique<dinero::Mining>();
            if (!m_mining->initialize(m_blockchain.get())) {
                error = "Failed to initialize mining component";
                return false;
            }
            
            // Set mining address if provided
            if (!m_options.mining_address.empty()) {
                m_current_address = m_options.mining_address;
                m_mining->setMiningAddress(m_options.mining_address);
            }
            
            if (progress) progress("Initializing miner core...");
            
            // Initialize MinerCore
            m_miner_core = std::make_unique<dinero::MinerCore>();
            m_miner_core->setMining(m_mining.get());
            m_miner_core->setBlockchain(m_blockchain.get());
            
            if (progress) progress("Starting RPC server...");
            
            // Initialize RPC server if requested
            if (m_options.server) {
                m_rpc_server = std::make_unique<dinero::RPCServer>();
                m_rpc_server->setBlockchain(m_blockchain.get());
                m_rpc_server->setMining(m_mining.get());
                m_rpc_server->setMinerCore(m_miner_core.get());
                
                std::string cookie_path = m_options.datadir + "/.cookie";
                m_rpc_server->setCookiePath(cookie_path);
                
                if (!m_rpc_server->initialize(m_options.rpc_port)) {
                    error = "Failed to initialize RPC server";
                    return false;
                }
                
                // Start RPC server in background thread
                m_rpc_thread = std::thread([this]() {
                    m_rpc_server->start();
                });
                m_rpc_running.store(true);
                
                // Create RPC executor for in-process calls
                m_rpc_executor = std::make_unique<dinero::interfaces::RpcExecutor>(m_rpc_server.get());
            }
            
            // Initialize wallet
            std::string wallet_path = options.datadir + "/wallet.db";
            m_wallet = std::make_unique<Dinero::Wallet::SQLiteWallet>();
            if (!m_wallet->initialize(wallet_path)) {
                error = "Failed to initialize wallet database";
                return false;
            }
            
            // Initialize HD wallet if not already done
            if (!m_wallet->initializeHDWallet("")) {
                g_logger.warning("HD wallet initialization failed - wallet may need setup");
            }
            
            m_running.store(true);
            
            if (progress) progress("Node started successfully");
            return true;
            
        } catch (const std::exception& e) {
            error = "Exception during node startup: " + std::string(e.what());
            return false;
        }
    }
    
    void stop() override {
        if (!m_running.load()) {
            return;
        }
        
        dinero::g_logger.info("Stopping embedded node...");
        
        // Stop mining
        if (m_mining) {
            m_mining->stopMining();
            m_mining->shutdown();
        }
        
        // Stop RPC server
        if (m_rpc_server && m_rpc_running.load()) {
            m_rpc_server->shutdown();
            if (m_rpc_thread.joinable()) {
                m_rpc_thread.join();
            }
            m_rpc_running.store(false);
        }
        
        // Stop blockchain
        if (m_blockchain) {
            m_blockchain->shutdown();
        }
        
        m_running.store(false);
        dinero::g_logger.info("Embedded node stopped");
    }
    
    bool isRunning() const override {
        return m_running.load();
    }
    
    // Blockchain queries
    int getBestBlockHeight() const override {
        if (!m_blockchain) return 0;
        return m_blockchain->getLatestHeight();
    }
    
    std::string getBestBlockHash() const override {
        if (!m_blockchain) return "";
        return m_blockchain->getLatestHash();
    }
    
    int getConnectionCount() const override {
        if (!m_blockchain) return 0;
        // TODO: Implement connection count from P2P layer
        return 0;
    }
    
    // Network info
    void getNetworkStats(int& peers, double& hashrate) const override {
        peers = getConnectionCount();
        
        // Get hashrate from mining component
        hashrate = 0.0;
        if (m_mining) {
            hashrate = m_mining->getCurrentHashrate();
        }
    }
    
    bool isTestNet() const override {
        return false; // Placeholder - would need to check active network
    }
    
    bool isRegTest() const override {
        return false; // Placeholder - would need to check active network
    }
    
    // Wallet operations
    bool hasWallet() const override {
        // For now, assume wallet is always available
        // TODO: Implement proper wallet detection
        return true;
    }
    
    bool loadWallet(const std::string& name, std::string& error) override {
        // TODO: Implement wallet loading
        error = "Wallet loading not yet implemented";
        return false;
    }
    
    bool createWallet(const std::string& name, std::string& error) override {
        // TODO: Implement wallet creation
        error = "Wallet creation not yet implemented";
        return false;
    }
    
    double getBalance() const override {
        // TODO: Implement balance retrieval
        return 0.0;
    }
    
    std::string getNewAddress(const std::string& addressType) override {
        if (!m_wallet) {
            g_logger.error("Wallet not initialized - cannot generate address");
            return "";
        }
        
        if (!m_wallet->isHDWalletUnlocked()) {
            g_logger.error("HD wallet is locked - cannot generate address");
            return "";
        }
        
        std::string address = m_wallet->getNewHDAddress();
        if (address.empty()) {
            g_logger.error("Failed to generate HD address from wallet");
            return "";
        }
        
        g_logger.info("Generated new HD address: " + address);
        return address;
    }
    
    bool sendToAddress(const std::string& address, double amount, 
                      std::string& txid, std::string& error) override {
        // TODO: Implement transaction sending
        error = "Transaction sending not yet implemented";
        return false;
    }
    
    // Mining operations
    bool setGenerate(bool generate, int threads, std::string& error) override {
        if (!m_mining || !m_miner_core) {
            error = "Mining components not initialized";
            return false;
        }
        
        try {
            if (generate) {
                // Get the mining address from the Mining component (set via setminingaddress RPC)
                std::string mining_address = m_mining->getMiningAddress();
                if (!mining_address.empty()) {
                    return m_miner_core->start(mining_address, threads);
                } else {
                    error = "No mining address set. Use setminingaddress first.";
                    return false;
                }
            } else {
                m_miner_core->stop();
                return true;
            }
        } catch (const std::exception& e) {
            error = "Mining error: " + std::string(e.what());
            return false;
        }
    }
    
    bool getMiningInfo(bool& generating, int& threads, double& hashrate) const override {
        if (!m_miner_core) {
            generating = false;
            threads = 0;
            hashrate = 0.0;
            return false;
        }
        
        auto stats = m_miner_core->getStats();
        generating = stats.running;
        threads = stats.threads;
        hashrate = stats.hashrate;
        return true;
    }
    
    bool generateToAddress(int blocks, const std::string& address, std::string& error) override {
        if (!m_mining || !m_blockchain) {
            error = "Mining or blockchain component not initialized";
            return false;
        }
        
        try {
            // Set temporary mining address
            std::string old_address = m_mining->getMiningAddress();
            m_mining->setMiningAddress(address);
            
            // Generate blocks one by one
            bool success = true;
            for (int i = 0; i < blocks && success; ++i) {
                success = m_mining->startMining();
            }
            
            // Restore old address
            if (!old_address.empty()) {
                m_mining->setMiningAddress(old_address);
            }
            
            if (!success) {
                error = "Failed to generate blocks";
                return false;
            }
            
            return true;
        } catch (const std::exception& e) {
            error = "Block generation error: " + std::string(e.what());
            return false;
        }
    }
    
    // RPC server control
    bool isRpcServerRunning() const override {
        return m_rpc_running.load();
    }
    
    int getRpcPort() const override {
        return m_options.rpc_port;
    }
    
    // In-process RPC execution (Bitcoin-Qt style)
    std::string executeRpc(const std::string& command) override {
        if (!m_rpc_executor) {
            throw std::runtime_error("RPC executor not available");
        }
        return m_rpc_executor->executeCommand(command);
    }
    
    Json::Value executeRpcJson(const std::string& method, const Json::Value& params) override {
        if (!m_rpc_executor) {
            Json::Value error;
            error["code"] = -1;
            error["message"] = "RPC executor not available";
            return error;
        }
        return m_rpc_executor->executeJson(method, params);
    }
    
    std::vector<Json::Value> executeRpcBatch(
        const std::vector<std::pair<std::string, Json::Value>>& requests) override {
        if (!m_rpc_executor) {
            std::vector<Json::Value> errors;
            Json::Value error;
            error["code"] = -1;
            error["message"] = "RPC executor not available";
            errors.resize(requests.size(), error);
            return errors;
        }
        return m_rpc_executor->executeBatch(requests);
    }
};

// Factory function implementation
std::unique_ptr<Node> MakeNode() {
    return std::make_unique<NodeImpl>();
}

} // namespace interfaces
} // namespace dinero
