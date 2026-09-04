/**
 * @file daemon_integration_example.cpp
 * @brief Complete example showing how to integrate Explorer API v1 with your Dinero daemon
 * 
 * This example shows the exact integration points needed in your existing daemon.
 * Copy the relevant parts into your actual daemon code.
 */

#include <iostream>
#include <string>
#include <thread>
#include <chrono>

// Your existing includes
#include "daemon/blockchain.h"
#include "daemon/mining.h"
#include "daemon/rpc_server.h"
#include "common/logger.h"

// Explorer API includes
#include "explorer/explorer_integration.h"

// Example of integrating Explorer API into your existing daemon
class DineroDaemonWithExplorer {
private:
    std::string datadir_;
    std::unique_ptr<dinero::Blockchain> blockchain_;
    std::unique_ptr<dinero::Mining> mining_;
    std::unique_ptr<dinero::RPCServer> rpc_server_;
    
public:
    explicit DineroDaemonWithExplorer(const std::string& datadir) : datadir_(datadir) {}
    
    bool Initialize() {
        dinero::g_logger.info("=== Dinero Daemon with Explorer API Starting ===");
        
        // 1. Initialize your existing components first
        blockchain_ = std::make_unique<dinero::Blockchain>();
        if (!blockchain_->initialize(datadir_)) {
            dinero::g_logger.error("Failed to initialize blockchain");
            return false;
        }
        
        mining_ = std::make_unique<dinero::Mining>();
        if (!mining_->initialize()) {
            dinero::g_logger.error("Failed to initialize mining");
            return false;
        }
        
        rpc_server_ = std::make_unique<dinero::RPCServer>();
        if (!rpc_server_->initialize(20998)) {
            dinero::g_logger.error("Failed to initialize RPC server");
            return false;
        }
        
        // 2. Initialize Explorer API
        if (!explorer_initialize(datadir_.c_str())) {
            dinero::g_logger.error("Failed to initialize Explorer API v1");
            return false;
        }
        dinero::g_logger.info("✅ Explorer API v1 initialized");
        
        // 3. Set component references for Explorer
        explorer_set_blockchain(blockchain_.get());
        explorer_set_mining(mining_.get());
        // explorer_set_node(node_.get()); // If you have a node interface
        
        // 4. Integrate Explorer with your HTTP server
        rpc_server_->setCustomHandler([this](const std::string& method, 
                                            const std::string& path,
                                            const std::string& query,
                                            const std::string& body) -> std::string {
            return this->handleHTTPRequest(method, path, query, body);
        });
        
        dinero::g_logger.info("✅ Daemon initialization complete");
        return true;
    }
    
    void Shutdown() {
        dinero::g_logger.info("Shutting down daemon...");
        
        // Shutdown in reverse order
        explorer_shutdown();
        
        if (rpc_server_) {
            rpc_server_->shutdown();
        }
        
        if (mining_) {
            mining_->shutdown();
        }
        
        if (blockchain_) {
            blockchain_->shutdown();
        }
        
        dinero::g_logger.info("✅ Daemon shutdown complete");
    }
    
    void Run() {
        dinero::g_logger.info("Daemon running... Press Ctrl+C to stop");
        
        // Start RPC server in background
        std::thread rpc_thread([this]() {
            rpc_server_->start();
        });
        
        // Main daemon loop
        while (true) {
            // Your existing daemon logic here
            
            // Example: Process new blocks
            processNewBlocks();
            
            // Example: Process mempool
            processMempoolTransactions();
            
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        if (rpc_thread.joinable()) {
            rpc_thread.join();
        }
    }
    
private:
    // HTTP request handler that integrates Explorer API
    std::string handleHTTPRequest(const std::string& method, 
                                 const std::string& path,
                                 const std::string& query,
                                 const std::string& body) {
        // Check if it's an Explorer API request
        if (path.substr(0, 8) == "/api/v1/") {
            const char* response = explorer_handle_http_request(
                method.c_str(), 
                path.c_str(), 
                query.c_str(), 
                body.c_str()
            );
            
            if (response) {
                return std::string(response);
            }
        }
        
        // Fall back to existing RPC handling
        return handleRPCRequest(method, path, query, body);
    }
    
    // Your existing RPC handler
    std::string handleRPCRequest(const std::string& method, 
                                const std::string& path,
                                const std::string& query,
                                const std::string& body) {
        // Your existing RPC logic here
        if (path == "/") {
            // Handle JSON-RPC requests
            return processJSONRPC(body);
        }
        
        // Return 404 for unknown paths
        return "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    }
    
    std::string processJSONRPC(const std::string& body) {
        // Your existing JSON-RPC processing
        return R"({"jsonrpc":"2.0","id":1,"result":"ok"})";
    }
    
    // Example of block processing with Explorer integration
    void processNewBlocks() {
        // Check for new blocks in your blockchain
        uint32_t current_height = blockchain_->getBestHeight();
        static uint32_t last_processed_height = 0;
        
        // Process any new blocks
        for (uint32_t height = last_processed_height + 1; height <= current_height; ++height) {
            processBlock(height);
            last_processed_height = height;
        }
    }
    
    void processBlock(uint32_t height) {
        // Get block data from your blockchain
        std::string block_hash = blockchain_->getBlockHash(height);
        std::string block_data = blockchain_->getBlockData(height);
        uint64_t timestamp = blockchain_->getBlockTime(height);
        
        // Your existing block processing logic here
        // ...
        
        // Index the block for Explorer API
        explorer_on_new_block(height, block_hash.c_str(), block_data.c_str(), timestamp);
        
        // Process transactions in the block
        auto transactions = blockchain_->getBlockTransactions(height);
        for (size_t tx_index = 0; tx_index < transactions.size(); ++tx_index) {
            const auto& tx = transactions[tx_index];
            
            std::string txid = tx.GetHash().GetHex();
            std::string raw_hex = tx.GetHex();
            
            // Index the transaction
            explorer_on_new_transaction(txid.c_str(), raw_hex.c_str(), height, tx_index);
            
            // Index address/UTXO changes
            indexTransactionAddresses(tx, height, tx_index);
        }
        
        // Broadcast WebSocket update
        explorer_broadcast_new_block(height, block_hash.c_str(), timestamp);
        
        dinero::g_logger.info("Processed block " + std::to_string(height) + " with Explorer indexing");
    }
    
    void indexTransactionAddresses(const CTransaction& tx, uint32_t height, uint32_t tx_index) {
        std::string txid = tx.GetHash().GetHex();
        
        // Index outputs (new UTXOs)
        for (size_t vout = 0; vout < tx.vout.size(); ++vout) {
            const auto& output = tx.vout[vout];
            
            // Convert scriptPubKey to Electrum-style scripthash
            std::string script_hex = HexStr(output.scriptPubKey);
            std::string scripthash = calculateElectrumScripthash(script_hex);
            
            // Index the new UTXO
            explorer_on_address_activity(scripthash.c_str(), txid.c_str(), vout, 
                                       output.nJson::Value, height, 0); // 0 = not spent
        }
        
        // Index inputs (spent UTXOs)
        for (const auto& input : tx.vin) {
            if (input.prevout.IsNull()) continue; // Skip coinbase
            
            // Look up the previous output to get its scripthash
            std::string prev_txid = input.prevout.hash.GetHex();
            uint32_t prev_vout = input.prevout.n;
            
            // Get the previous transaction to find the scripthash
            auto prev_tx = blockchain_->getTransaction(prev_txid);
            if (prev_tx && prev_vout < prev_tx->vout.size()) {
                std::string prev_script_hex = HexStr(prev_tx->vout[prev_vout].scriptPubKey);
                std::string prev_scripthash = calculateElectrumScripthash(prev_script_hex);
                
                // Mark the UTXO as spent
                explorer_on_address_activity(prev_scripthash.c_str(), prev_txid.c_str(), 
                                           prev_vout, 0, height, 1); // 1 = spent
            }
        }
    }
    
    // Example mempool processing
    void processMempoolTransactions() {
        auto mempool_txs = blockchain_->getMempoolTransactions();
        
        for (const auto& tx : mempool_txs) {
            std::string txid = tx.GetHash().GetHex();
            std::string raw_hex = tx.GetHex();
            double fee_rate = calculateFeeRate(tx);
            
            // Index as unconfirmed transaction (height = 0)
            explorer_on_new_transaction(txid.c_str(), raw_hex.c_str(), 0, 0);
            
            // Broadcast WebSocket update
            explorer_broadcast_new_tx(txid.c_str(), fee_rate);
        }
    }
    
    // Helper functions
    std::string calculateElectrumScripthash(const std::string& script_hex) {
        // Convert hex to bytes
        std::vector<uint8_t> script_bytes = ParseHex(script_hex);
        
        // SHA256 hash
        uint8_t hash[32];
        SHA256(script_bytes.data(), script_bytes.size(), hash);
        
        // Reverse for little-endian (Electrum format)
        std::reverse(hash, hash + 32);
        
        // Convert to hex string
        return HexStr(hash, hash + 32);
    }
    
    double calculateFeeRate(const CTransaction& tx) {
        // Calculate fee rate in sat/vB
        // This is a simplified example
        uint64_t fee = 1000; // Calculate actual fee
        uint64_t vsize = tx.GetVirtualSize();
        return static_cast<double>(fee) / static_cast<double>(vsize);
    }
};

// Example main function
int main(int argc, char* argv[]) {
    std::string datadir = "./data";
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.substr(0, 10) == "--datadir=") {
            datadir = arg.substr(10);
        } else if (arg.substr(0, 9) == "-datadir=") {
            datadir = arg.substr(9);
        }
    }
    
    // Initialize and run daemon
    DineroDaemonWithExplorer daemon(datadir);
    
    if (!daemon.Initialize()) {
        std::cerr << "Failed to initialize daemon" << std::endl;
        return 1;
    }
    
    // Set up signal handlers for graceful shutdown
    std::signal(SIGINT, [](int) {
        std::cout << "\nShutting down..." << std::endl;
        std::exit(0);
    });
    
    try {
        daemon.Run();
    } catch (const std::exception& e) {
        std::cerr << "Daemon error: " << e.what() << std::endl;
        daemon.Shutdown();
        return 1;
    }
    
    daemon.Shutdown();
    return 0;
}

/*
 * Compilation example:
 * 
 * g++ -std=c++20 -I../include \
 *     daemon_integration_example.cpp \
 *     -ldinero_explorer -ldinero_common \
 *     -lsqlite3 -ljsoncpp -lssl -lcrypto \
 *     -o daemon_with_explorer
 * 
 * Usage:
 * ./daemon_with_explorer --datadir=/path/to/data
 * 
 * Test the Explorer API:
 * curl -s localhost:20998/api/v1/health | jq
 * curl -s localhost:20998/api/v1/chain/tip | jq
 */
