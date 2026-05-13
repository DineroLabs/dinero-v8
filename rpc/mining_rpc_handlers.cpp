// SPDX-License-Identifier: MIT
// Dinero - Mining RPC Handler Implementations

#include "daemon/rpc/wallet_rpc_extras.h"
#include "mining/mining_manager.h"
#include "wallet/wallet_manager.h"
#include "common/logger.h"
#include <stdexcept>

// Use dinero namespace for consistency
using namespace dinero;

// Mining runtime control
json::json rpc_mining_info(RpcServer& server, const json::json& params) {
    try {
        auto& mining_manager = MiningManager::getInstance();
        auto mining_info = mining_manager.getMiningInfo();
        
        json::json result;
        result["mining"] = mining_info.is_mining;
        result["threads"] = mining_info.thread_count;
        result["hashrate"] = mining_info.hashrate;
        result["address"] = mining_info.mining_address;
        result["blocks_mined"] = mining_info.blocks_mined;
        result["last_block_time"] = mining_info.last_block_time;
        result["difficulty"] = mining_info.difficulty;
        result["network_hashrate"] = mining_info.network_hashrate;
        
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to get mining info: ") + e.what();
        return error;
    }
}

json::json rpc_mining_start(RpcServer& server, const json::json& params) {
    try {
        int threads = RpcValidation::get_int(params, "threads", 0); // 0 = auto-detect
        
        auto& mining_manager = MiningManager::getInstance();
        
        // Validate mining address is set and wallet-owned
        auto& wallet_manager = WalletManager::getInstance();
        std::string mining_address = mining_manager.getMiningAddress();
        
        if (mining_address.empty()) {
            json::json error;
            error["error"]["code"] = -25;
            error["error"]["message"] = "No mining address set. Use mining.setaddress first.";
            return error;
        }
        
        if (!wallet_manager.isAddressOwned(mining_address)) {
            json::json error;
            error["error"]["code"] = -26;
            error["error"]["message"] = "Mining address not owned by active wallet. Mining rewards will not be spendable.";
            return error;
        }
        
        // Start mining
        bool started = mining_manager.startMining(threads);
        
        json::json result;
        result["started"] = started;
        result["threads"] = threads > 0 ? threads : mining_manager.getOptimalThreadCount();
        result["address"] = mining_address;
        result["message"] = started ? "Mining started successfully" : "Failed to start mining";
        
        dinero::g_logger.info("Mining start: " + (started ? "success" : "failed") + 
                    " threads: " + std::to_string(threads));
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to start mining: ") + e.what();
        return error;
    }
}

json::json rpc_mining_stop(RpcServer& server, const json::json& params) {
    try {
        auto& mining_manager = MiningManager::getInstance();
        
        // Stop mining
        bool stopped = mining_manager.stopMining();
        
        json::json result;
        result["stopped"] = stopped;
        result["message"] = stopped ? "Mining stopped successfully" : "Mining was not running";
        
        dinero::g_logger.info("Mining stop: " + (stopped ? "success" : "not running"));
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to stop mining: ") + e.what();
        return error;
    }
}

json::json rpc_mining_setaddress(RpcServer& server, const json::json& params) {
    try {
        RpcValidation::require_string(params, "address");
        std::string address = params["address"].get<std::string>();
        
        auto& wallet_manager = WalletManager::getInstance();
        auto& mining_manager = MiningManager::getInstance();
        
        // Validate address format
        if (!wallet_manager.isValidAddress(address)) {
            json::json error;
            error["error"]["code"] = -5;
            error["error"]["message"] = "Invalid address format";
            return error;
        }
        
        // Validate address is owned by active wallet
        if (!wallet_manager.isAddressOwned(address)) {
            json::json error;
            error["error"]["code"] = -26;
            error["error"]["message"] = "Address not owned by active wallet. Mining rewards will not be spendable.";
            return error;
        }
        
        // Set mining address
        bool set = mining_manager.setMiningAddress(address);
        
        json::json result;
        result["address"] = address;
        result["set"] = set;
        result["owned"] = true;
        result["message"] = set ? "Mining address set successfully" : "Failed to set mining address";
        
        dinero::g_logger.info("Mining address set: " + address);
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to set mining address: ") + e.what();
        return error;
    }
}

json::json rpc_mining_getaddress(RpcServer& server, const json::json& params) {
    try {
        auto& mining_manager = MiningManager::getInstance();
        auto& wallet_manager = WalletManager::getInstance();
        
        std::string address = mining_manager.getMiningAddress();
        bool is_owned = !address.empty() && wallet_manager.isAddressOwned(address);
        
        json::json result;
        result["address"] = address;
        result["ismine"] = is_owned;
        result["source"] = address.empty() ? "unset" : "configured";
        
        if (!address.empty() && !is_owned) {
            result["warning"] = "Mining address not owned by active wallet";
        }
        
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to get mining address: ") + e.what();
        return error;
    }
}
