// SPDX-License-Identifier: MIT
// Dinero - Diagnostics RPC Handler Implementations

#include "daemon/rpc/wallet_rpc_extras.h"
#include "common/logger.h"
#include <stdexcept>
#include <ctime>

// Use dinero namespace for consistency
using namespace dinero;

// Node diagnostics and meta
json::json rpc_node_info(RpcServer& server, const json::json& params) {
    try {
        auto& blockchain = Blockchain::getInstance();
        
        json::json result;
        
        dinero::g_logger.info("Node info requested");
        result["version"] = "1.0.0";
        result["protocol_version"] = 1;
        result["uptime"] = std::time(nullptr) - blockchain.getStartTime();
        
        // Blockchain state
        result["blocks"] = blockchain.getBlockCount();
        result["best_block_hash"] = blockchain.getBestBlockHash();
        result["difficulty"] = blockchain.getDifficulty();
        result["network"] = blockchain.getNetworkName();
        
        // Connection info
        result["connections"] = blockchain.getConnectionCount();
        result["peers"] = json::json(json::arrayjson);
        auto peers = blockchain.getPeerList();
        for (const auto& peer : peers) {
            json::json peer_obj;
            peer_obj["address"] = peer.address;
            peer_obj["version"] = peer.version;
            peer_obj["height"] = peer.height;
            result["peers"].push_back(peer_obj);
        }
        
        // Memory pool
        result["mempool_size"] = blockchain.getMempoolSize();
        result["mempool_bytes"] = blockchain.getMempoolBytes();
        
        // Verification progress
        result["verification_progress"] = blockchain.getVerificationProgress();
        result["initial_block_download"] = blockchain.isInitialBlockDownload();
        
        // Warnings
        json::json warnings = json::json(json::arrayjson);
        auto warning_list = blockchain.getWarnings();
        for (const auto& warning : warning_list) {
            warnings.push_back(warning);
        }
        result["warnings"] = warnings;
        
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to get node info: ") + e.what();
        return error;
    }
}

json::json rpc_rpc_methods(RpcServer& server, const json::json& params) {
    try {
        // Get list of all registered RPC methods
        auto methods = server.getRegisteredMethods();
        
        json::json result;
        result["methods"] = json::json(json::arrayjson);
        
        dinero::g_logger.info("RPC methods list requested");
        json::json wallet_methods = json::json(json::arrayjson);
        json::json mining_methods = json::json(json::arrayjson);
        json::json blockchain_methods = json::json(json::arrayjson);
        json::json network_methods = json::json(json::arrayjson);
        json::json util_methods = json::json(json::arrayjson);
        
        for (const auto& method : methods) {
            json::json method_obj;
            method_obj["name"] = method.name;
            method_obj["description"] = method.description;
            method_obj["category"] = method.category;
            
            // Add to appropriate category
            if (method.category == "wallet") {
                wallet_methods.push_back(method_obj);
            } else if (method.category == "mining") {
                mining_methods.push_back(method_obj);
            } else if (method.category == "blockchain") {
                blockchain_methods.push_back(method_obj);
            } else if (method.category == "network") {
                network_methods.push_back(method_obj);
            } else {
                util_methods.push_back(method_obj);
            }
            
            result["methods"].push_back(method_obj);
        }
        
        // Organized by category
        result["categories"]["wallet"] = wallet_methods;
        result["categories"]["mining"] = mining_methods;
        result["categories"]["blockchain"] = blockchain_methods;
        result["categories"]["network"] = network_methods;
        result["categories"]["util"] = util_methods;
        
        result["count"] = static_cast<int>(methods.size());
        
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to list RPC methods: ") + e.what();
        return error;
    }
}
