// SPDX-License-Identifier: MIT
// Dinero - Mempool RPC Handler Implementation

#include "rpc/rpc_registry.h"
#include "mempool/policy_engine.h"
#include "daemon/mempool.h"
#include "daemon/tx_mempool.h"
#include "daemon/main.h"
#include "common/logger.h"
#include <stdexcept>
#include <sstream>
#include <algorithm>

namespace dinero {
namespace rpc {

namespace {

double SatPerKbToDin(uint64_t sat_per_kb) {
    return static_cast<double>(sat_per_kb) / 100000000.0;
}

double BaselineRelayFee() {
    if (!dinero::legacy::g_mempool()) {
        return 0.0;
    }
    return SatPerKbToDin(dinero::legacy::g_mempool()->GetPolicy().min_relay_feerate);
}

} // namespace

// Fee estimation RPC handler
Json::Value rpc_estimatefee(const Json::Value& params) {
    Json::Value result;
    
    try {
        uint32_t target_blocks = 6;  // Default target
        if (params.isArray() && params.size() > 0 && params[0].isNumeric()) {
            target_blocks = params[0].asUInt();
        }
        
        const double fee_rate = BaselineRelayFee();
        
        result["feerate"] = fee_rate;
        result["blocks"] = target_blocks;
        
        dinero::g_logger.info("Fee estimate requested for " + std::to_string(target_blocks) + " blocks");
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Fee estimation error: ") + e.what();
        return error;
    }
}

// Multiple fee estimates RPC handler
Json::Value rpc_getfeeestimates(const Json::Value& params) {
    Json::Value result;
    
    try {
        const double baseline = BaselineRelayFee();
        std::vector<std::pair<uint32_t, std::string>> targets = {
            {1, "IMMEDIATE"},
            {3, "FAST"},
            {6, "NORMAL"},
            {12, "SLOW"}
        };
        
        for (const auto& [blocks, name] : targets) {
            const double multiplier =
                (blocks <= 1) ? 1.50 :
                (blocks <= 3) ? 1.25 :
                (blocks <= 6) ? 1.00 : 0.75;
            const double fee_rate = std::max(0.0, baseline * multiplier);
            result[name] = fee_rate;
        }
        
        dinero::g_logger.info("Multiple fee estimates requested");
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Fee estimates error: ") + e.what();
        return error;
    }
}

// Smart fee estimation RPC handler
Json::Value rpc_estimatesmartfee(const Json::Value& params) {
    Json::Value result;
    
    try {
        uint32_t target_blocks = 6;  // Default target
        std::string estimate_mode = "CONSERVATIVE";  // Default mode
        
        if (params.isArray() && params.size() > 0 && params[0].isNumeric()) {
            target_blocks = params[0].asUInt();
        }
        if (params.isArray() && params.size() > 1 && params[1].isString()) {
            estimate_mode = params[1].asString();
        }
        
        const double baseline = BaselineRelayFee();
        const double mode_multiplier = (estimate_mode == "ECONOMICAL") ? 0.85 : 1.0;
        const double urgency_multiplier =
            (target_blocks <= 1) ? 1.5 :
            (target_blocks <= 3) ? 1.25 :
            (target_blocks <= 6) ? 1.0 : 0.8;
        const double fee_rate = std::max(0.0, baseline * mode_multiplier * urgency_multiplier);
        
        result["feerate"] = fee_rate;
        result["blocks"] = target_blocks;
        result["errors"] = Json::Value(Json::arrayValue);
        
        dinero::g_logger.info("Smart fee estimate requested for " + std::to_string(target_blocks) + " blocks");
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Smart fee estimation error: ") + e.what();
        return error;
    }
}

// Get mempool entry RPC handler
Json::Value rpc_getmempoolentry(const Json::Value& params) {
    Json::Value result;
    
    try {
        extern std::unique_ptr<dinero::TxMempool> g_mempool;
        
        if (!dinero::legacy::g_mempool()) {
            Json::Value error;
            error["error"]["code"] = -32603;
            error["error"]["message"] = "Mempool not initialized";
            return error;
        }
        
        if (params.size() < 1) {
            Json::Value error;
            error["error"]["code"] = -1;
            error["error"]["message"] = "Transaction ID parameter required";
            return error;
        }
        
        std::string txid = params[0].asString();
        
        // Get real mempool entry
        const auto* entry = dinero::legacy::g_mempool()->Get(txid);
        
        if (!entry) {
            Json::Value error;
            error["error"]["code"] = -5;
            error["error"]["message"] = "Transaction not in mempool";
            return error;
        }
        
        // Return real data from mempool entry
        result["size"] = static_cast<Json::UInt>(entry->tx_size);
        result["fee"] = static_cast<Json::UInt64>(entry->fee);
        result["modifiedfee"] = static_cast<Json::UInt64>(entry->fee);
        result["time"] = static_cast<Json::UInt64>(entry->time);
        result["height"] = static_cast<Json::Int>(entry->height);
        result["descendantcount"] = static_cast<Json::UInt>(entry->descendant_count);
        result["descendantsize"] = static_cast<Json::UInt>(entry->descendant_size);
        result["descendantfees"] = static_cast<Json::UInt64>(entry->descendant_fees);
        result["ancestorcount"] = static_cast<Json::UInt>(entry->ancestor_count);
        result["ancestorsize"] = static_cast<Json::UInt>(entry->ancestor_size);
        result["ancestorfees"] = static_cast<Json::UInt64>(entry->ancestor_fees);
        result["depends"] = Json::Value(Json::arrayValue);
        for (const auto& dep : entry->depends) {
            result["depends"].append(dep.GetHex());
        }
        result["spentby"] = Json::Value(Json::arrayValue);
        for (const auto& child : entry->spentby) {
            result["spentby"].append(child.GetHex());
        }
        result["bip125-replaceable"] = entry->rbf_enabled;
        
        dinero::g_logger.info("Mempool entry requested for txid: " + txid);
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Get mempool entry error: ") + e.what();
        return error;
    }
}

// Get mempool info RPC handler
Json::Value rpc_getmempoolinfo(const Json::Value& params) {
    Json::Value result;
    
    try {
        // Get real mempool information from global instance
        extern std::unique_ptr<dinero::TxMempool> g_mempool;
        
        if (!dinero::legacy::g_mempool()) {
            Json::Value error;
            error["error"]["code"] = -32603;
            error["error"]["message"] = "Mempool not initialized";
            return error;
        }
        
        // Get real stats from mempool
        result["size"] = static_cast<Json::UInt64>(dinero::legacy::g_mempool()->Size());
        result["bytes"] = static_cast<Json::UInt64>(dinero::legacy::g_mempool()->Bytes());
        result["usage"] = static_cast<Json::UInt64>(dinero::legacy::g_mempool()->Bytes()); // Usage = bytes for now
        const auto& policy = dinero::legacy::g_mempool()->GetPolicy();
        result["maxmempool"] = static_cast<Json::UInt64>(policy.max_size_bytes);
        result["mempoolminfee"] = BaselineRelayFee();
        result["minrelaytxfee"] = BaselineRelayFee();
        
        dinero::g_logger.info("Mempool info: " + std::to_string(dinero::legacy::g_mempool()->Size()) + " txs, " + std::to_string(dinero::legacy::g_mempool()->Bytes()) + " bytes");
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Get mempool info error: ") + e.what();
        return error;
    }
}

// Get raw mempool RPC handler
Json::Value rpc_getrawmempool(const Json::Value& params) {
    Json::Value result;
    
    try {
        extern std::unique_ptr<dinero::TxMempool> g_mempool;
        
        if (!dinero::legacy::g_mempool()) {
            Json::Value error;
            error["error"]["code"] = -32603;
            error["error"]["message"] = "Mempool not initialized";
            return error;
        }
        
        bool verbose = false;
        if (params.isArray() && params.size() > 0 && params[0].isBool()) {
            verbose = params[0].asBool();
        }
        
        // Get real transaction IDs from mempool
        std::vector<std::string> txids = dinero::legacy::g_mempool()->GetTxIds();
        
        if (verbose) {
            result = Json::Value(Json::objectValue);
            // Return detailed transaction information
            for (const auto& txid : txids) {
                const auto* entry = dinero::legacy::g_mempool()->Get(txid);
                if (entry) {
                    Json::Value tx_info;
                    tx_info["size"] = static_cast<Json::UInt>(entry->tx_size);
                    tx_info["fee"] = static_cast<Json::UInt64>(entry->fee);
                    tx_info["time"] = static_cast<Json::UInt64>(entry->time);
                    tx_info["height"] = static_cast<Json::UInt>(entry->height);
                    tx_info["descendantcount"] = static_cast<Json::UInt>(entry->ancestor_count);
                    tx_info["descendantsize"] = static_cast<Json::UInt>(entry->ancestor_size);
                    tx_info["ancestorcount"] = static_cast<Json::UInt>(entry->ancestor_count);
                    tx_info["ancestorsize"] = static_cast<Json::UInt>(entry->ancestor_size);
                    result[txid] = tx_info;
                }
            }
        } else {
            result = Json::Value(Json::arrayValue);
            // Return array of transaction IDs
            for (const auto& txid : txids) {
                result.append(txid);
            }
        }
        
        dinero::g_logger.info("Raw mempool requested: " + std::to_string(txids.size()) + " transactions (verbose: " + std::to_string(verbose) + ")");
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Get raw mempool error: ") + e.what();
        return error;
    }
}

// Get mempool ancestors RPC handler
Json::Value rpc_getmempoolancestors(const Json::Value& params) {
    Json::Value result;
    
    try {
        extern std::unique_ptr<dinero::TxMempool> g_mempool;
        
        if (!dinero::legacy::g_mempool()) {
            Json::Value error;
            error["error"]["code"] = -32603;
            error["error"]["message"] = "Mempool not initialized";
            return error;
        }
        
        if (params.size() < 1) {
            Json::Value error;
            error["error"]["code"] = -1;
            error["error"]["message"] = "Transaction ID parameter required";
            return error;
        }
        
        std::string txid = params[0].asString();
        bool verbose = false;
        if (params.size() > 1 && params[1].isBool()) {
            verbose = params[1].asBool();
        }
        
        // Check if transaction exists in mempool
        if (!dinero::legacy::g_mempool()->Exists(txid)) {
            Json::Value error;
            error["error"]["code"] = -5;
            error["error"]["message"] = "Transaction not in mempool";
            return error;
        }
        
        // Get real ancestors from mempool
        std::set<std::string> ancestors = dinero::legacy::g_mempool()->GetAncestors(txid);
        
        if (verbose) {
            result = Json::Value(Json::objectValue);
            for (const auto& ancestor_txid : ancestors) {
                const auto* entry = dinero::legacy::g_mempool()->Get(ancestor_txid);
                if (entry) {
                    Json::Value tx_info;
                    tx_info["size"] = static_cast<Json::UInt>(entry->tx_size);
                    tx_info["fee"] = static_cast<Json::UInt64>(entry->fee);
                    tx_info["modifiedfee"] = static_cast<Json::UInt64>(entry->fee);
                    result[ancestor_txid] = tx_info;
                }
            }
        } else {
            result = Json::Value(Json::arrayValue);
            for (const auto& ancestor_txid : ancestors) {
                result.append(ancestor_txid);
            }
        }
        
        dinero::g_logger.info("Mempool ancestors requested for txid: " + txid + " (found " + std::to_string(ancestors.size()) + ")");
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Get mempool ancestors error: ") + e.what();
        return error;
    }
}

// Get mempool descendants RPC handler
Json::Value rpc_getmempooldescendants(const Json::Value& params) {
    Json::Value result;
    
    try {
        extern std::unique_ptr<dinero::TxMempool> g_mempool;
        
        if (!dinero::legacy::g_mempool()) {
            Json::Value error;
            error["error"]["code"] = -32603;
            error["error"]["message"] = "Mempool not initialized";
            return error;
        }
        
        if (params.size() < 1) {
            Json::Value error;
            error["error"]["code"] = -1;
            error["error"]["message"] = "Transaction ID parameter required";
            return error;
        }
        
        std::string txid = params[0].asString();
        bool verbose = false;
        if (params.size() > 1 && params[1].isBool()) {
            verbose = params[1].asBool();
        }
        
        // Check if transaction exists in mempool
        if (!dinero::legacy::g_mempool()->Exists(txid)) {
            Json::Value error;
            error["error"]["code"] = -5;
            error["error"]["message"] = "Transaction not in mempool";
            return error;
        }
        
        // Get real descendants from mempool
        std::set<std::string> descendants = dinero::legacy::g_mempool()->GetDescendants(txid);
        
        if (verbose) {
            result = Json::Value(Json::objectValue);
            for (const auto& descendant_txid : descendants) {
                const auto* entry = dinero::legacy::g_mempool()->Get(descendant_txid);
                if (entry) {
                    Json::Value tx_info;
                    tx_info["size"] = static_cast<Json::UInt>(entry->tx_size);
                    tx_info["fee"] = static_cast<Json::UInt64>(entry->fee);
                    tx_info["modifiedfee"] = static_cast<Json::UInt64>(entry->fee);
                    result[descendant_txid] = tx_info;
                }
            }
        } else {
            result = Json::Value(Json::arrayValue);
            for (const auto& descendant_txid : descendants) {
                result.append(descendant_txid);
            }
        }
        
        dinero::g_logger.info("Mempool descendants requested for txid: " + txid + " (found " + std::to_string(descendants.size()) + ")");
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Get mempool descendants error: ") + e.what();
        return error;
    }
}

} // namespace rpc
} // namespace dinero
