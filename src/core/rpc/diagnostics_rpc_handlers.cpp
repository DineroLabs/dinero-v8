// SPDX-License-Identifier: MIT
// Dinero - Node Diagnostics RPC Handler Implementation

#include "daemon/rpc/wallet_rpc_extras.h"
#include "wallet/wallet_manager.h"
#if DIN_ENABLE_LEGACY_RPC
#include "daemon/rpc_server.h"
#endif
#include "common/logger.h"
#include "cli/version.h"
#include "p2p/p2p_wire_protocol.h"
#include <stdexcept>
#include <sstream>
#include <chrono>
#include <vector>

using namespace dinero;

// Parameter validation helpers (reused from other RPC handlers)
namespace RpcValidation {
    void require_string(const Json::Value& params, const std::string& field) {
        if (!params.isMember(field) || !params[field].isString()) {
            throw std::invalid_argument("Missing or invalid string field: " + field);
        }
    }
    
    void require_number(const Json::Value& params, const std::string& field) {
        if (!params.isMember(field) || !params[field].isNumeric()) {
            throw std::invalid_argument("Missing or invalid number field: " + field);
        }
    }
    
    std::string get_string(const Json::Value& params, const std::string& field, const std::string& default_val) {
        if (params.isMember(field) && params[field].isString()) {
            return params[field].asString();
        }
        return default_val;
    }
    
    double get_number(const Json::Value& params, const std::string& field, double default_val) {
        if (params.isMember(field) && params[field].isNumeric()) {
            return params[field].asDouble();
        }
        return default_val;
    }
    
    int get_int(const Json::Value& params, const std::string& field, int default_val) {
        if (params.isMember(field) && params[field].isNumeric()) {
            return params[field].asInt();
        }
        return default_val;
    }
    
    bool get_bool(const Json::Value& params, const std::string& field, bool default_val) {
        if (params.isMember(field) && params[field].isBool()) {
            return params[field].asBool();
        }
        return default_val;
    }
}

// Node information RPC implementation
Json::Value rpc_node_info(dinero::RPCServer& server, const Json::Value& params) {
    try {
        Json::Value result;
        
        // Basic node information from real version system
        auto version_info = dinero::cli::getVersionInfo();
        result["version"] = version_info.version;
        result["protocol_version"] = static_cast<int>(din::p2p::g_network_config.protocol_version);
        result["user_agent"] = din::p2p::g_network_config.user_agent_prefix;
        
        // Network information
        result["network"] = "regtest"; // TODO: Detect actual network from chainparams
        result["testnet"] = true; // TODO: Detect if testnet from chainparams
        result["regtest"] = true; // TODO: Detect if regtest from chainparams
        
        // Blockchain information
        result["blocks"] = 0; // TODO: Get from blockchain database
        result["headers"] = 0; // TODO: Get header count from blockchain database
        result["best_block_hash"] = ""; // TODO: Get best block hash from blockchain database
        result["difficulty"] = 1.0; // TODO: Get current difficulty from blockchain database
        result["median_time"] = static_cast<int64_t>(std::time(nullptr)); // TODO: Get median block time from blockchain database
        
        // Connection information
        result["connections"] = 0; // TODO: Get from PeerManager component
        result["connections_in"] = 0; // TODO: Get inbound connections from PeerManager
        result["connections_out"] = 0; // TODO: Get outbound connections from PeerManager
        
        // Memory pool information
        result["mempool_size"] = 0; // TODO: Get from Mempool
        result["mempool_bytes"] = 0; // TODO: Get mempool memory usage
        result["mempool_usage"] = 0; // TODO: Get mempool usage
        
        // Mining information
        result["mining"] = false; // TODO: Get mining status
        result["hashrate"] = 0.0; // TODO: Get current hashrate
        result["networkhashps"] = 0.0; // TODO: Get network hashrate
        
        // Wallet information (if available)
        // TODO: Get wallet information from server
        result["wallet_loaded"] = false;
        
        // System information
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        result["time"] = static_cast<int64_t>(time_t);
        result["uptime"] = 0; // TODO: Calculate actual uptime
        
        // RPC information
        result["rpc_active"] = true;
        result["rpc_port"] = 20998; // TODO: Get actual port from server
        
        // Warnings and errors
        std::vector<std::string> warnings;
        // TODO: Add proper warnings based on system state
        
        if (!warnings.empty()) {
            result["warnings"] = Json::Value(Json::arrayValue);
            for (const auto& warning : warnings) {
                result["warnings"].append(warning);
            }
        }
        
        dinero::g_logger.info("Retrieved node information");
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to get node info: ") + e.what();
        dinero::g_logger.error("node.info error: " + std::string(e.what()));
        return error;
    }
}

// RPC methods listing implementation
Json::Value rpc_rpc_methods(dinero::RPCServer& server, const Json::Value& params) {
    try {
        Json::Value result;
        
        // Get available methods from the server
        // TODO: This would integrate with the actual RPC registry to get real method names
        std::vector<std::string> methods = {
            // Wallet lifecycle
            "wallet.create",
            "wallet.load",
            "wallet.encrypt",
            "wallet.lock",
            "wallet.unlock",
            "wallet.change_passphrase",
            
            // Wallet queries
            "wallet.info",
            "wallet.balance",
            "wallet.addresses",
            "wallet.utxos",
            "wallet.history",
            "wallet.label",
            "wallet.getnewaddress",
            "wallet.validateaddress",
            
            // Transaction operations
            "tx.send",
            
            // Mining control
            "mining.info",
            "mining.start",
            "mining.stop",
            "mining.setaddress",
            "mining.getaddress",
            
            // Node diagnostics
            "node.info",
            "rpc.methods",
            
            // Legacy compatibility methods
            "getnewaddress",
            "validateaddress",
            "getbalance",
            "listtransactions",
            "listunspent",
            "sendtoaddress",
            "getmininginfo",
            "setminingaddress",
            "getminingaddress",
            
            // Core blockchain methods
            "getblockcount",
            "getbestblockhash",
            "getblockchaininfo",
            "getnetworkinfo",
            "getmempoolinfo"
        };
        
        result["methods"] = Json::Value(Json::arrayValue);
        for (const auto& method : methods) {
            Json::Value method_info;
            method_info["name"] = method;
            method_info["category"] = "general"; // TODO: Implement method categorization
            method_info["description"] = "RPC method"; // TODO: Implement method descriptions
            result["methods"].append(method_info);
        }
        
        result["count"] = static_cast<int>(methods.size());
        result["rpc_version"] = "2.0";
        result["server_version"] = "1.0.0"; // TODO: Get actual version
        
        dinero::g_logger.info("Listed " + std::to_string(methods.size()) + " RPC methods");
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to list RPC methods: ") + e.what();
        dinero::g_logger.error("rpc.methods error: " + std::string(e.what()));
        return error;
    }
}

// Helper function to categorize RPC methods
std::string getMethodCategory(const std::string& method) {
    if (method.find("wallet.") == 0) return "wallet";
    if (method.find("tx.") == 0) return "transaction";
    if (method.find("mining.") == 0) return "mining";
    if (method.find("node.") == 0) return "node";
    if (method.find("rpc.") == 0) return "rpc";
    if (method == "getnewaddress" || method == "validateaddress" || 
        method == "getbalance" || method == "listtransactions" || 
        method == "listunspent" || method == "sendtoaddress") return "wallet_legacy";
    if (method == "getmininginfo" || method == "setminingaddress" || 
        method == "getminingaddress") return "mining_legacy";
    return "blockchain";
}

// Helper function to provide method descriptions
std::string getMethodDescription(const std::string& method) {
    static const std::map<std::string, std::string> descriptions = {
        {"wallet.create", "Create a new wallet"},
        {"wallet.load", "Load an existing wallet"},
        {"wallet.encrypt", "Encrypt wallet with passphrase"},
        {"wallet.lock", "Lock encrypted wallet"},
        {"wallet.unlock", "Unlock encrypted wallet"},
        {"wallet.change_passphrase", "Change wallet passphrase"},
        {"wallet.info", "Get wallet information and status"},
        {"wallet.balance", "Get wallet balance"},
        {"wallet.addresses", "List wallet addresses"},
        {"wallet.utxos", "List unspent transaction outputs"},
        {"wallet.history", "Get transaction history"},
        {"wallet.label", "Set or get address label"},
        {"wallet.getnewaddress", "Generate new address"},
        {"wallet.validateaddress", "Validate address"},
        {"tx.send", "Send transaction with advanced options"},
        {"mining.info", "Get mining information"},
        {"mining.start", "Start mining"},
        {"mining.stop", "Stop mining"},
        {"mining.setaddress", "Set mining payout address"},
        {"mining.getaddress", "Get mining payout address"},
        {"node.info", "Get node information and status"},
        {"rpc.methods", "List available RPC methods"},
        {"getblockcount", "Get current block height"},
        {"getbestblockhash", "Get best block hash"},
        {"getblockchaininfo", "Get blockchain information"},
        {"getnetworkinfo", "Get network information"},
        {"getmempoolinfo", "Get memory pool information"}
    };
    
    auto it = descriptions.find(method);
    return it != descriptions.end() ? it->second : "No description available";
}
