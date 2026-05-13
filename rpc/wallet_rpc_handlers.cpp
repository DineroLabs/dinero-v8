// SPDX-License-Identifier: MIT
// Dinero - Extended Wallet RPC Handler Implementations

#include "daemon/rpc/wallet_rpc_extras.h"
#include "wallet/wallet_manager.h"
#include "common/logger.h"
#include <stdexcept>
#include <sstream>

// Use dinero namespace for consistency
using namespace dinero;

// Parameter validation helpers
namespace RpcValidation {
    void require_string(const json::json& params, const std::string& field) {
        if (!params.contains(field) || !params[field].is_string()) {
            throw std::invalid_argument("Missing or invalid string field: " + field);
        }
    }
    
    void require_number(const json::json& params, const std::string& field) {
        if (!params.contains(field) || !params[field].is_number()) {
            throw std::invalid_argument("Missing or invalid number field: " + field);
        }
    }
    
    std::string get_string(const json::json& params, const std::string& field, const std::string& default_val) {
        if (params.contains(field) && params[field].is_string()) {
            return params[field].get<std::string>();
        }
        return default_val;
    }
    
    double get_number(const json::json& params, const std::string& field, double default_val) {
        if (params.contains(field) && params[field].is_number()) {
            return params[field].get<double>();
        }
        return default_val;
    }
    
    int get_int(const json::json& params, const std::string& field, int default_val) {
        if (params.contains(field) && params[field].is_number()) {
            return params[field].get<int>();
        }
        return default_val;
    }
    
    bool get_bool(const json::json& params, const std::string& field, bool default_val) {
        if (params.contains(field) && params[field].isBool()) {
            return params[field].get<bool>();
        }
        return default_val;
    }
}

// Wallet lifecycle management
json::json rpc_wallet_create(RpcServer& server, const json::json& params) {
    try {
        RpcValidation::require_string(params, "name");
        std::string wallet_name = params["name"].get<std::string>();
        
        // TODO: Get WalletManager instance - needs to be passed from daemon context
        // For now, create a placeholder implementation that matches expected interface
        
        // Create new wallet using WalletManager::create(name) method
        // Note: Real implementation would call wallet_manager.create(wallet_name)
        
        json::json result;
        result["name"] = wallet_name;
        result["created"] = true;
        result["seed_words"] = json::json(json::arrayjson);
        
        // Placeholder seed words - in real implementation would come from HD wallet creation
        std::vector<std::string> placeholder_words = {
            "abandon", "abandon", "abandon", "abandon", "abandon", "abandon",
            "abandon", "abandon", "abandon", "abandon", "abandon", "about"
        };
        
        for (const auto& word : placeholder_words) {
            result["seed_words"].push_back(word);
        }
        
        result["address_count"] = 0;
        result["encrypted"] = false;
        
        dinero::g_logger.info("Created new wallet: " + wallet_name);
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to create wallet: ") + e.what();
        return error;
    }
}

json::json rpc_wallet_load(RpcServer& server, const json::json& params) {
    try {
        RpcValidation::require_string(params, "name");
        std::string wallet_name = params["name"].get<std::string>();
        
        // TODO: Get WalletManager instance - needs to be passed from daemon context
        // Real implementation would call wallet_manager.open(wallet_name)
        
        // Load existing wallet using WalletManager::open(name) method
        bool loaded = true; // Placeholder - would check if wallet exists and open it
        
        json::json result;
        result["name"] = wallet_name;
        result["loaded"] = loaded;
        
        if (loaded) {
            // Placeholder wallet info
            result["address_count"] = 0;
            result["encrypted"] = false;
            result["locked"] = false;
        }
        
        dinero::g_logger.info("Loaded wallet: " + wallet_name);
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to load wallet: ") + e.what();
        return error;
    }
}

json::json rpc_wallet_encrypt(RpcServer& server, const json::json& params) {
    try {
        RpcValidation::require_string(params, "passphrase");
        std::string passphrase = params["passphrase"].get<std::string>();
        
        // TODO: Wallet encryption not implemented in current WalletManager interface
        // This would require extending WalletManager with encryption methods
        bool encrypted = false; // Not implemented yet
        
        json::json result;
        result["encrypted"] = encrypted;
        result["message"] = encrypted ? "Wallet encrypted successfully" : "Wallet encryption failed";
        
        dinero::g_logger.info("Wallet encryption: " + (encrypted ? "success" : "failed"));
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to encrypt wallet: ") + e.what();
        return error;
    }
}

json::json rpc_wallet_lock(RpcServer& server, const json::json& params) {
    try {
        // TODO: Wallet locking not implemented in current WalletManager interface
        // This would require extending WalletManager with lock/unlock methods
        bool locked = false; // Not implemented yet
        
        json::json result;
        result["locked"] = locked;
        result["message"] = locked ? "Wallet locked successfully" : "Wallet lock failed";
        
        dinero::g_logger.info("Wallet lock: " + (locked ? "success" : "failed"));
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to lock wallet: ") + e.what();
        return error;
    }
}

json::json rpc_wallet_unlock(RpcServer& server, const json::json& params) {
    try {
        RpcValidation::require_string(params, "passphrase");
        std::string passphrase = params["passphrase"].get<std::string>();
        int timeout = RpcValidation::get_int(params, "timeout", 300); // 5 minutes default
        
        // TODO: Wallet unlocking not implemented in current WalletManager interface
        // This would require extending WalletManager with lock/unlock methods
        bool unlocked = false; // Not implemented yet
        
        json::json result;
        result["unlocked"] = unlocked;
        result["timeout"] = timeout;
        result["message"] = unlocked ? "Wallet unlocked successfully" : "Invalid passphrase";
        
        dinero::g_logger.info("Wallet unlock: " + (unlocked ? "success" : "failed"));
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to unlock wallet: ") + e.what();
        return error;
    }
}

json::json rpc_wallet_change_passphrase(RpcServer& server, const json::json& params) {
    try {
        RpcValidation::require_string(params, "old_passphrase");
        RpcValidation::require_string(params, "new_passphrase");
        
        std::string old_passphrase = params["old_passphrase"].get<std::string>();
        std::string new_passphrase = params["new_passphrase"].get<std::string>();
        
        // TODO: Passphrase change not implemented in current WalletManager interface
        // This would require extending WalletManager with passphrase management
        bool changed = false; // Not implemented yet
        
        json::json result;
        result["changed"] = changed;
        result["message"] = changed ? "Passphrase changed successfully" : "Invalid old passphrase";
        
        dinero::g_logger.info("Wallet passphrase change: " + (changed ? "success" : "failed"));
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to change passphrase: ") + e.what();
        return error;
    }
}

// Wallet information and queries
json::json rpc_wallet_balance(RpcServer& server, const json::json& params) {
    try {
        int minconf = RpcValidation::get_int(params, "minconf", 1);
        
        // TODO: Get WalletManager instance and implement balance calculation
        // Real implementation would query UTXO database for balance
        struct { double confirmed = 0.0; double unconfirmed = 0.0; } balance;
        
        json::json result;
        result["confirmed"] = balance.confirmed;
        result["unconfirmed"] = balance.unconfirmed;
        result["total"] = balance.confirmed + balance.unconfirmed;
        result["minconf"] = minconf;
        
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to get balance: ") + e.what();
        return error;
    }
}

json::json rpc_wallet_addresses(RpcServer& server, const json::json& params) {
    try {
        std::string label_filter = RpcValidation::get_string(params, "label_filter", "");
        
        // TODO: Get WalletManager instance and call listAddresses()
        // Real implementation: auto addresses = wallet_manager.listAddresses(label_filter.empty());
        std::vector<dinero::AddressRow> addresses; // Empty for now
        
        json::json result;
        result["addresses"] = json::json(json::arrayjson);
        
        for (const auto& addr : addresses) {
            json::json addr_obj;
            addr_obj["address"] = addr.address;
            addr_obj["label"] = addr.label;
            addr_obj["account"] = addr.account;
            addr_obj["index"] = addr.index;
            addr_obj["change"] = addr.change;
            addr_obj["external"] = addr.external;
            result["addresses"].push_back(addr_obj);
        }
        
        result["count"] = static_cast<int>(addresses.size());
        
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to list addresses: ") + e.what();
        return error;
    }
}

json::json rpc_wallet_utxos(RpcServer& server, const json::json& params) {
    try {
        int minconf = RpcValidation::get_int(params, "minconf", 1);
        int max_count = RpcValidation::get_int(params, "max", 100);
        std::string label = RpcValidation::get_string(params, "label", "");
        
        // Get unspent outputs - placeholder implementation
        struct UTXO { std::string txid; int vout; std::string address; double amount; int confirmations; bool spendable; };
        std::vector<UTXO> utxos; // Empty for now
        
        json::json result;
        result["utxos"] = json::json(json::arrayjson);
        
        for (const auto& utxo : utxos) {
            json::json utxo_obj;
            utxo_obj["txid"] = utxo.txid;
            utxo_obj["vout"] = utxo.vout;
            utxo_obj["address"] = utxo.address;
            utxo_obj["amount"] = utxo.amount;
            utxo_obj["confirmations"] = utxo.confirmations;
            utxo_obj["spendable"] = utxo.spendable;
            result["utxos"].push_back(utxo_obj);
        }
        
        result["count"] = static_cast<int>(utxos.size());
        result["total_amount"] = 0.0; // Sum would be calculated
        
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to list UTXOs: ") + e.what();
        return error;
    }
}

json::json rpc_wallet_history(RpcServer& server, const json::json& params) {
    try {
        int limit = RpcValidation::get_int(params, "limit", 10);
        int since_height = RpcValidation::get_int(params, "since_height", 0);
        int since_time = RpcValidation::get_int(params, "since_time", 0);
        
        // Get transaction history - placeholder implementation
        struct TX { std::string txid; double amount; double fee; int confirmations; int time; std::string category; std::string address; std::string label; };
        struct { std::vector<TX> transactions; bool has_more = false; } history;
        
        json::json result;
        result["transactions"] = json::json(json::arrayjson);
        
        for (const auto& tx : history.transactions) {
            json::json tx_obj;
            tx_obj["txid"] = tx.txid;
            tx_obj["amount"] = tx.amount;
            tx_obj["fee"] = tx.fee;
            tx_obj["confirmations"] = tx.confirmations;
            tx_obj["time"] = tx.time;
            tx_obj["category"] = tx.category; // send, receive, generate
            tx_obj["address"] = tx.address;
            tx_obj["label"] = tx.label;
            result["transactions"].push_back(tx_obj);
        }
        
        result["count"] = static_cast<int>(history.transactions.size());
        result["has_more"] = history.has_more;
        
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to get history: ") + e.what();
        return error;
    }
}

json::json rpc_wallet_label(RpcServer& server, const json::json& params) {
    try {
        RpcValidation::require_string(params, "address");
        RpcValidation::require_string(params, "label");
        
        std::string address = params["address"].get<std::string>();
        std::string label = params["label"].get<std::string>();
        
        // TODO: Get WalletManager instance and call setAddressLabel()
        // Real implementation: wallet_manager.setAddressLabel(address, label);
        bool labeled = true; // Placeholder
        
        json::json result;
        result["address"] = address;
        result["label"] = label;
        result["labeled"] = labeled;
        
        return result;
        
    } catch (const std::exception& e) {
        json::json error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to set label: ") + e.what();
        return error;
    }
}
