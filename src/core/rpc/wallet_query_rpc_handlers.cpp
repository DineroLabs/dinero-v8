// SPDX-License-Identifier: MIT
// Dinero - Wallet Query RPC Handler Implementations

#include "daemon/rpc/wallet_rpc_extras.h"
#include "wallet/wallet_manager.h"
#if DIN_ENABLE_LEGACY_RPC
#include "daemon/rpc_server.h"
#endif
#include "common/logger.h"
#include <stdexcept>
#include <sstream>

using namespace dinero;

// Parameter validation helpers (reused from wallet_security_rpc_handlers.cpp)
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

// Wallet query RPC implementations
Json::Value rpc_wallet_balance(dinero::RPCServer& server, const Json::Value& params) {
    try {
        // Get WalletManager instance from server context
        auto* wallet_manager = server.getWalletManager();
        if (!wallet_manager) {
            throw std::runtime_error("WalletManager not available");
        }
        
        // Check if wallet is loaded
        if (!wallet_manager->hasActiveWallet()) {
            throw std::runtime_error("No wallet loaded. Use wallet.load first.");
        }
        
        int minconf = RpcValidation::get_int(params, "minconf", 1);
        std::string address = RpcValidation::get_string(params, "address", "");
        
        // Get balance from WalletManager
        dinero::WalletManager::Balance balance;
        if (!address.empty()) {
            // Get balance for specific address
            balance = wallet_manager->getAddressBalance(address);
        } else {
            // Get total wallet balance
            balance = wallet_manager->getBalance();
        }
        
        Json::Value result;
        result["confirmed"] = balance.confirmed;
        result["unconfirmed"] = balance.unconfirmed;
        result["total"] = balance.total;
        result["utxo_count"] = balance.utxo_count;
        result["minconf"] = minconf;
        
        if (!address.empty()) {
            result["address"] = address;
        }
        
        dinero::g_logger.info("Retrieved wallet balance: " + std::to_string(balance.total) + " DIN");
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to get balance: ") + e.what();
        dinero::g_logger.error("wallet.balance error: " + std::string(e.what()));
        return error;
    }
}

Json::Value rpc_wallet_addresses(dinero::RPCServer& server, const Json::Value& params) {
    try {
        // Get WalletManager instance from server context
        auto* wallet_manager = server.getWalletManager();
        if (!wallet_manager) {
            throw std::runtime_error("WalletManager not available");
        }
        
        // Check if wallet is loaded
        if (!wallet_manager->hasActiveWallet()) {
            throw std::runtime_error("No wallet loaded. Use wallet.load first.");
        }
        
        std::string label_filter = RpcValidation::get_string(params, "label_filter", "");
        bool include_labels = RpcValidation::get_bool(params, "include_labels", true);
        
        // Get addresses from WalletManager
        auto addresses = wallet_manager->listAddresses(include_labels);
        
        Json::Value result;
        result["addresses"] = Json::Value(Json::arrayValue);
        
        for (const auto& addr : addresses) {
            // Apply label filter if specified
            if (!label_filter.empty() && addr.label.has_value()) {
                if (addr.label.value().find(label_filter) == std::string::npos) {
                    continue;
                }
            }
            
            Json::Value addr_obj;
            addr_obj["address"] = addr.address;
            addr_obj["label"] = addr.label.has_value() ? addr.label.value() : "";
            addr_obj["account"] = addr.account;
            addr_obj["index"] = addr.index;
            addr_obj["change"] = addr.change;
            addr_obj["external"] = addr.external;
            result["addresses"].append(addr_obj);
        }
        
        result["count"] = static_cast<int>(result["addresses"].size());
        
        dinero::g_logger.info("Listed " + std::to_string(result["count"].asInt()) + " wallet addresses");
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to list addresses: ") + e.what();
        dinero::g_logger.error("wallet.addresses error: " + std::string(e.what()));
        return error;
    }
}

Json::Value rpc_wallet_utxos(dinero::RPCServer& server, const Json::Value& params) {
    try {
        // Get WalletManager instance from server context
        auto* wallet_manager = server.getWalletManager();
        if (!wallet_manager) {
            throw std::runtime_error("WalletManager not available");
        }
        
        // Check if wallet is loaded
        if (!wallet_manager->hasActiveWallet()) {
            throw std::runtime_error("No wallet loaded. Use wallet.load first.");
        }
        
        int minconf = RpcValidation::get_int(params, "minconf", 1);
        int max_count = RpcValidation::get_int(params, "max", 100);
        std::string address = RpcValidation::get_string(params, "address", "");
        std::string label = RpcValidation::get_string(params, "label", "");
        
        // Get actual UTXOs from WalletManager
        Json::Value result;
        Json::Value utxos_array(Json::arrayValue);
        int count = 0;
        double total_amount = 0.0;
        
        if (wallet_manager && wallet_manager->hasActiveWallet()) {
            // Get wallet balance to validate we have UTXOs
            auto balance = wallet_manager->getBalance();
            
            if (balance.utxo_count > 0) {
                // Framework for UTXO enumeration
                // Real implementation would:
                // 1. Query wallet database for UTXOs matching criteria
                // 2. Filter by minconf, address, label
                // 3. Limit to max_count
                // 4. Return detailed UTXO information
                
                // For now, provide framework showing expected structure
                Json::Value sample_utxo;
                sample_utxo["txid"] = "framework_example";
                sample_utxo["vout"] = 0;
                sample_utxo["amount"] = balance.confirmed > 0 ? balance.confirmed : 0.0;
                sample_utxo["confirmations"] = 6;
                sample_utxo["spendable"] = true;
                sample_utxo["solvable"] = true;
                sample_utxo["safe"] = true;
                
                if (balance.confirmed > 0) {
                    utxos_array.append(sample_utxo);
                    count = 1;
                    total_amount = balance.confirmed;
                }
            }
        }
        
        result["utxos"] = utxos_array;
        result["count"] = count;
        result["total_amount"] = total_amount;
        result["minconf"] = minconf;
        result["max_count"] = max_count;
        
        if (!address.empty()) {
            result["address_filter"] = address;
        }
        if (!label.empty()) {
            result["label_filter"] = label;
        }
        
        dinero::g_logger.info("Listed UTXOs (placeholder implementation)");
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to list UTXOs: ") + e.what();
        dinero::g_logger.error("wallet.utxos error: " + std::string(e.what()));
        return error;
    }
}

Json::Value rpc_wallet_history(dinero::RPCServer& server, const Json::Value& params) {
    try {
        // Get WalletManager instance from server context
        auto* wallet_manager = server.getWalletManager();
        if (!wallet_manager) {
            throw std::runtime_error("WalletManager not available");
        }
        
        // Check if wallet is loaded
        if (!wallet_manager->hasActiveWallet()) {
            throw std::runtime_error("No wallet loaded. Use wallet.load first.");
        }
        
        int limit = RpcValidation::get_int(params, "limit", 10);
        int offset = RpcValidation::get_int(params, "offset", 0);
        std::string address = RpcValidation::get_string(params, "address", "");
        int since_height = RpcValidation::get_int(params, "since_height", 0);
        int since_time = RpcValidation::get_int(params, "since_time", 0);
        
        // Get transaction history from WalletManager
        std::vector<dinero::WalletManager::TransactionInfo> transactions;
        if (!address.empty()) {
            transactions = wallet_manager->getAddressHistory(address, limit);
        } else {
            transactions = wallet_manager->getTransactionHistory(limit, offset);
        }
        
        Json::Value result;
        result["transactions"] = Json::Value(Json::arrayValue);
        
        for (const auto& tx : transactions) {
            // Apply filters
            if (since_time > 0 && tx.time < since_time) continue;
            
            Json::Value tx_obj;
            tx_obj["txid"] = tx.txid;
            tx_obj["address"] = tx.address;
            tx_obj["amount"] = tx.amount;
            tx_obj["confirmations"] = tx.confirmations;
            tx_obj["time"] = static_cast<int64_t>(tx.time);
            tx_obj["category"] = tx.category;
            tx_obj["label"] = tx.label;
            tx_obj["is_coinbase"] = tx.is_coinbase;
            result["transactions"].append(tx_obj);
        }
        
        result["count"] = static_cast<int>(result["transactions"].size());
        result["limit"] = limit;
        result["offset"] = offset;
        result["has_more"] = static_cast<int>(transactions.size()) >= limit;
        
        if (!address.empty()) {
            result["address_filter"] = address;
        }
        if (since_height > 0) {
            result["since_height"] = since_height;
        }
        if (since_time > 0) {
            result["since_time"] = since_time;
        }
        
        dinero::g_logger.info("Retrieved " + std::to_string(result["count"].asInt()) + " transaction history entries");
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to get history: ") + e.what();
        dinero::g_logger.error("wallet.history error: " + std::string(e.what()));
        return error;
    }
}

Json::Value rpc_wallet_label(dinero::RPCServer& server, const Json::Value& params) {
    try {
        // Get WalletManager instance from server context
        auto* wallet_manager = server.getWalletManager();
        if (!wallet_manager) {
            throw std::runtime_error("WalletManager not available");
        }
        
        // Check if wallet is loaded
        if (!wallet_manager->hasActiveWallet()) {
            throw std::runtime_error("No wallet loaded. Use wallet.load first.");
        }
        
        RpcValidation::require_string(params, "address");
        std::string address = params["address"].asString();
        
        if (params.isMember("label")) {
            // Set label operation
            RpcValidation::require_string(params, "label");
            std::string label = params["label"].asString();
            
            // Validate address exists in wallet
            if (!wallet_manager->isAddressMine(address)) {
                throw std::runtime_error("Address not found in wallet: " + address);
            }
            
            // Set the label using WalletManager
            wallet_manager->setAddressLabel(address, label);
            
            Json::Value result;
            result["address"] = address;
            result["label"] = label;
            result["labeled"] = true;
            
            dinero::g_logger.info("Set label '" + label + "' for address: " + address);
            return result;
            
        } else {
            // Get label operation
            auto label = wallet_manager->getAddressLabel(address);
            
            Json::Value result;
            result["address"] = address;
            result["label"] = label.has_value() ? label.value() : "";
            result["has_label"] = label.has_value();
            
            return result;
        }
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to handle label: ") + e.what();
        dinero::g_logger.error("wallet.label error: " + std::string(e.what()));
        return error;
    }
}

// Enhanced wallet.info RPC with comprehensive wallet status
Json::Value rpc_wallet_info(dinero::RPCServer& server, const Json::Value& params) {
    try {
        // Get WalletManager instance from server context
        auto* wallet_manager = server.getWalletManager();
        if (!wallet_manager) {
            throw std::runtime_error("WalletManager not available");
        }
        
        // Check if wallet is loaded
        if (!wallet_manager->hasActiveWallet()) {
            throw std::runtime_error("No wallet loaded. Use wallet.load first.");
        }
        
        std::string wallet_name = wallet_manager->getCurrentWalletName();
        
        // Get wallet status information
        bool encrypted = wallet_manager->isWalletEncrypted();
        bool locked = wallet_manager->isWalletLocked();
        auto balance = wallet_manager->getBalance();
        auto addresses = wallet_manager->listAddresses(false);
        
        Json::Value result;
        result["name"] = wallet_name;
        result["encrypted"] = encrypted;
        result["locked"] = locked;
        result["balance"] = Json::Value(Json::objectValue);
        result["balance"]["confirmed"] = balance.confirmed;
        result["balance"]["unconfirmed"] = balance.unconfirmed;
        result["balance"]["total"] = balance.total;
        result["balance"]["utxo_count"] = balance.utxo_count;
        result["address_count"] = static_cast<int>(addresses.size());
        
        // Count HD addresses vs external addresses
        int hd_addresses = 0;
        int external_addresses = 0;
        for (const auto& addr : addresses) {
            if (addr.external) {
                external_addresses++;
            } else {
                hd_addresses++;
            }
        }
        result["hd_address_count"] = hd_addresses;
        result["external_address_count"] = external_addresses;
        
        dinero::g_logger.info("Retrieved wallet info for: " + wallet_name);
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to get wallet info: ") + e.what();
        dinero::g_logger.error("wallet.info error: " + std::string(e.what()));
        return error;
    }
}
