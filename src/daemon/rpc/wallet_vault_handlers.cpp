#include "daemon/rpc/wallet_vault_handlers.h"
#include "wallet/wallet_manager.h"
#include "wallet/key_vault.h"
#include "common/logger.h"
#include "common/log_redactor.h"
#include <stdexcept>

namespace dinero::rpc {

din::Json RpcWalletLock(dinero::WalletManager* wallet_manager) {
    din::Json result;
    
    try {
        if (!wallet_manager) {
            result["success"] = false;
            result["error"] = "WALLET_MANAGER_UNAVAILABLE";
            result["message"] = "Wallet manager not available";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        if (!wallet_manager->hasActiveWallet()) {
            result["success"] = false;
            result["error"] = "NO_ACTIVE_WALLET";
            result["message"] = "No active wallet to lock";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        // Lock the wallet using WalletManager
        wallet_manager->lockWallet();
        
        result["success"] = true;
        result["message"] = "Wallet locked successfully";
        result["locked"] = true;
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
        
        dinero::g_logger.info("Wallet locked via RPC");
        
    } catch (const std::exception& e) {
        result["success"] = false;
        result["error"] = "INTERNAL_ERROR";
        result["message"] = "Internal error during wallet lock: " + std::string(e.what());
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
    }
    
    return result;
}

din::Json RpcWalletUnlock(const din::Json& params, dinero::WalletManager* wallet_manager) {
    din::Json result;
    
    // Log request with sensitive data redacted
    std::string safe_request = LogRedactor::RedactSensitive(params);
    dinero::g_logger.info("Processing wallet.unlock request: " + safe_request);
    
    try {
        if (!params.isObject()) {
            result["success"] = false;
            result["error"] = "INVALID_PARAMS";
            result["message"] = "Parameters must be an object";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        std::string password;
        if (params.isMember("passphrase") && params["passphrase"].isString()) {
            password = params["passphrase"].asString();
        } else if (params.isMember("password") && params["password"].isString()) {
            password = params["password"].asString();
        } else {
            result["success"] = false;
            result["error"] = "INVALID_PARAMS";
            result["message"] = "Missing required parameter: password";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        if (!wallet_manager) {
            result["success"] = false;
            result["error"] = "WALLET_MANAGER_UNAVAILABLE";
            result["message"] = "Wallet manager not available";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        if (!wallet_manager->hasActiveWallet()) {
            result["success"] = false;
            result["error"] = "NO_ACTIVE_WALLET";
            result["message"] = "No active wallet to unlock";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        int timeout_seconds = 900; // Default 15 minutes
        
        if (params.isMember("timeout") && params["timeout"].isInt()) {
            timeout_seconds = params["timeout"].asInt();
        }
        
        // Try to unlock the wallet using WalletManager
        try {
            wallet_manager->unlockWallet(password, timeout_seconds);
            
            result["success"] = true;
            result["message"] = "Wallet unlocked successfully";
            result["locked"] = false;
            result["timeout_seconds"] = timeout_seconds;
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            
            dinero::g_logger.info("Wallet unlocked via RPC (timeout: " + std::to_string(timeout_seconds) + "s)");
            
        } catch (const std::runtime_error& e) {
            // Wallet unlock failed - wrong password or encryption issue
            std::string error_msg = e.what();
            
            if (error_msg.find("not encrypted") != std::string::npos) {
                result["success"] = false;
                result["error"] = "WALLET_NOT_ENCRYPTED";
                result["message"] = "Wallet is not encrypted";
            } else if (error_msg.find("passphrase") != std::string::npos || 
                      error_msg.find("password") != std::string::npos) {
                result["success"] = false;
                result["error"] = "WRONG_PASSPHRASE";
                result["message"] = "Invalid password";
            } else {
                result["success"] = false;
                result["error"] = "UNLOCK_FAILED";
                result["message"] = error_msg;
            }
            
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            
            dinero::g_logger.warn("Wallet unlock failed: " + error_msg);
        }
        
    } catch (const std::exception& e) {
        result["success"] = false;
        result["error"] = "INTERNAL_ERROR";
        result["message"] = "Internal error during wallet unlock: " + std::string(e.what());
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
    }
    
    return result;
}

din::Json RpcWalletChangePassphrase(const din::Json& params, dinero::WalletManager* wallet_manager) {
    din::Json result;
    
    // Log request with sensitive data redacted
    std::string safe_request = LogRedactor::RedactSensitive(params);
    dinero::g_logger.info("Processing wallet.changepassphrase request: " + safe_request);
    
    try {
        if (!params.isObject()) {
            result["success"] = false;
            result["error"] = "INVALID_PARAMS";
            result["message"] = "Parameters must be an object";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        if (!params.isMember("old_passphrase") || !params["old_passphrase"].isString() ||
            !params.isMember("new_passphrase") || !params["new_passphrase"].isString()) {
            result["success"] = false;
            result["error"] = "INVALID_PARAMS";
            result["message"] = "Missing required parameters: old_passphrase, new_passphrase";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        if (!wallet_manager) {
            result["success"] = false;
            result["error"] = "WALLET_MANAGER_UNAVAILABLE";
            result["message"] = "Wallet manager not available";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        if (!wallet_manager->hasActiveWallet()) {
            result["success"] = false;
            result["error"] = "NO_ACTIVE_WALLET";
            result["message"] = "No active wallet";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        std::string old_passphrase = params["old_passphrase"].asString();
        std::string new_passphrase = params["new_passphrase"].asString();
        
        // Basic validation
        if (new_passphrase.length() < 8) {
            result["success"] = false;
            result["error"] = "WEAK_PASSPHRASE";
            result["message"] = "New passphrase must be at least 8 characters";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        // Change passphrase using WalletManager
        try {
            wallet_manager->changePassphrase(old_passphrase, new_passphrase);
            
            result["success"] = true;
            result["message"] = "Passphrase changed successfully. All keys have been rewrapped.";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            
            dinero::g_logger.info("Wallet passphrase changed via RPC");
            
        } catch (const std::runtime_error& e) {
            std::string error_msg = e.what();
            
            if (error_msg.find("passphrase") != std::string::npos || 
                error_msg.find("password") != std::string::npos) {
                result["success"] = false;
                result["error"] = "WRONG_PASSPHRASE";
                result["message"] = "Invalid old passphrase";
            } else {
                result["success"] = false;
                result["error"] = "CHANGE_FAILED";
                result["message"] = error_msg;
            }
            
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            
            dinero::g_logger.warn("Passphrase change failed: " + error_msg);
        }
        
    } catch (const std::exception& e) {
        result["success"] = false;
        result["error"] = "INTERNAL_ERROR";
        result["message"] = "Internal error during passphrase change: " + std::string(e.what());
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
    }
    
    return result;
}

din::Json RpcWalletListAddresses(dinero::WalletManager* wallet_manager) {
    din::Json result;
    
    try {
        if (!wallet_manager) {
            result["success"] = false;
            result["error"] = "WALLET_MANAGER_UNAVAILABLE";
            result["message"] = "Wallet manager not available";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        if (!wallet_manager->hasActiveWallet()) {
            result["success"] = false;
            result["error"] = "NO_ACTIVE_WALLET";
            result["message"] = "No active wallet";
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
            return result;
        }
        
        // Get addresses from wallet manager
        din::Json addresses(Json::arrayValue);
        
        // Get all addresses from the wallet manager
        auto address_list = wallet_manager->listAddresses(true); // include labels
        
        for (const auto& addr_row : address_list) {
            din::Json addr;
            addr["address"] = addr_row.address;
            addr["label"] = addr_row.label.value_or("");
            addr["account"] = addr_row.account;
            addr["change"] = addr_row.change;
            addr["index"] = addr_row.index;
            addr["external"] = addr_row.external;
            addr["has_key"] = !addr_row.external; // HD addresses have keys, external ones don't
            addresses.append(addr);
        }
        
        result["success"] = true;
        result["addresses"] = addresses;
        result["count"] = addresses.size();
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
        
    } catch (const std::exception& e) {
        result["success"] = false;
        result["error"] = "INTERNAL_ERROR";
        result["message"] = "Internal error listing addresses: " + std::string(e.what());
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
    }
    
    return result;
}

} // namespace dinero::rpc
