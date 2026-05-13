// SPDX-License-Identifier: MIT
// Dinero - Wallet Security RPC Handler Implementations

#include "daemon/rpc/wallet_rpc_extras.h"
#include "wallet/wallet_manager.h"
#include "daemon/rpc_server.h"
#include "common/logger.h"
#include <stdexcept>
#include <chrono>

using namespace dinero;

// Wallet security operations - production implementations
Json::Value rpc_wallet_encrypt(RpcServer& server, const Json::Value& params) {
    try {
        RpcValidation::require_string(params, "passphrase");
        std::string passphrase = params["passphrase"].asString();
        
        if (passphrase.empty()) {
            Json::Value error;
            error["error"]["code"] = -3;
            error["error"]["message"] = "Password cannot be empty";
            return error;
        }
        
        // Get WalletManager from server context
        auto* wallet_manager = server.getWalletManager();
        if (!wallet_manager) {
            Json::Value error;
            error["error"]["code"] = -1;
            error["error"]["message"] = "Wallet manager not available";
            return error;
        }
        
        if (!wallet_manager->hasActiveWallet()) {
            Json::Value error;
            error["error"]["code"] = -13;
            error["error"]["message"] = "No active wallet. Use wallet.load first.";
            return error;
        }
        
        if (wallet_manager->isWalletEncrypted()) {
            Json::Value error;
            error["error"]["code"] = -15;
            error["error"]["message"] = "Wallet is already encrypted";
            return error;
        }
        
        // Encrypt the wallet
        wallet_manager->encryptWallet(passphrase);
        
        Json::Value result;
        result["encrypted"] = true;
        result["message"] = "Wallet encrypted successfully. Wallet is now locked.";
        result["wallet"] = wallet_manager->getCurrentWalletName();
        
        dinero::g_logger.info("Wallet encrypted: " + wallet_manager->getCurrentWalletName());
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to encrypt wallet: ") + e.what();
        return error;
    }
}

Json::Value rpc_wallet_lock(RpcServer& server, const Json::Value& params) {
    try {
        // Get WalletManager from server context
        auto* wallet_manager = server.getWalletManager();
        if (!wallet_manager) {
            Json::Value error;
            error["error"]["code"] = -1;
            error["error"]["message"] = "Wallet manager not available";
            return error;
        }
        
        if (!wallet_manager->hasActiveWallet()) {
            Json::Value error;
            error["error"]["code"] = -13;
            error["error"]["message"] = "No active wallet. Use wallet.load first.";
            return error;
        }
        
        if (!wallet_manager->isWalletEncrypted()) {
            Json::Value error;
            error["error"]["code"] = -15;
            error["error"]["message"] = "Wallet is not encrypted";
            return error;
        }
        
        if (wallet_manager->isWalletLocked()) {
            Json::Value error;
            error["error"]["code"] = -13;
            error["error"]["message"] = "Wallet is already locked";
            return error;
        }
        
        // Lock the wallet
        wallet_manager->lockWallet();
        
        Json::Value result;
        result["locked"] = true;
        result["message"] = "Wallet locked successfully";
        result["wallet"] = wallet_manager->getCurrentWalletName();
        
        dinero::g_logger.info("Wallet locked: " + wallet_manager->getCurrentWalletName());
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to lock wallet: ") + e.what();
        return error;
    }
}

Json::Value rpc_wallet_unlock(RpcServer& server, const Json::Value& params) {
    try {
        RpcValidation::require_string(params, "passphrase");
        std::string passphrase = params["passphrase"].asString();
        int timeout = RpcValidation::get_int(params, "timeout", 300); // 5 minutes default
        
        if (passphrase.empty()) {
            Json::Value error;
            error["error"]["code"] = -3;
            error["error"]["message"] = "Password cannot be empty";
            return error;
        }
        
        // Get WalletManager from server context
        auto* wallet_manager = server.getWalletManager();
        if (!wallet_manager) {
            Json::Value error;
            error["error"]["code"] = -1;
            error["error"]["message"] = "Wallet manager not available";
            return error;
        }
        
        if (!wallet_manager->hasActiveWallet()) {
            Json::Value error;
            error["error"]["code"] = -13;
            error["error"]["message"] = "No active wallet. Use wallet.load first.";
            return error;
        }
        
        if (!wallet_manager->isWalletEncrypted()) {
            Json::Value error;
            error["error"]["code"] = -15;
            error["error"]["message"] = "Wallet is not encrypted";
            return error;
        }
        
        if (!wallet_manager->isWalletLocked()) {
            Json::Value result;
            result["unlocked"] = true;
            result["message"] = "Wallet is already unlocked";
            result["timeout"] = timeout;
            result["wallet"] = wallet_manager->getCurrentWalletName();
            return result;
        }
        
        // Attempt to unlock the wallet
        try {
            wallet_manager->unlockWallet(passphrase, timeout);
            
            Json::Value result;
            result["unlocked"] = true;
            result["message"] = "Wallet unlocked successfully";
            result["timeout"] = timeout;
            result["wallet"] = wallet_manager->getCurrentWalletName();
            
            dinero::g_logger.info("Wallet unlocked: " + wallet_manager->getCurrentWalletName() + 
                                " (timeout: " + std::to_string(timeout) + "s)");
            return result;
            
        } catch (const std::exception& unlock_error) {
            Json::Value error;
            error["error"]["code"] = -14;
            error["error"]["message"] = "Invalid password";
            return error;
        }
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to unlock wallet: ") + e.what();
        return error;
    }
}

Json::Value rpc_wallet_change_passphrase(RpcServer& server, const Json::Value& params) {
    try {
        RpcValidation::require_string(params, "old_passphrase");
        RpcValidation::require_string(params, "new_passphrase");
        
        std::string old_passphrase = params["old_passphrase"].asString();
        std::string new_passphrase = params["new_passphrase"].asString();
        
        if (old_passphrase.empty() || new_passphrase.empty()) {
            Json::Value error;
            error["error"]["code"] = -3;
            error["error"]["message"] = "Passphrases cannot be empty";
            return error;
        }
        
        if (old_passphrase == new_passphrase) {
            Json::Value error;
            error["error"]["code"] = -3;
            error["error"]["message"] = "New passphrase must be different from old passphrase";
            return error;
        }
        
        // Get WalletManager from server context
        auto* wallet_manager = server.getWalletManager();
        if (!wallet_manager) {
            Json::Value error;
            error["error"]["code"] = -1;
            error["error"]["message"] = "Wallet manager not available";
            return error;
        }
        
        if (!wallet_manager->hasActiveWallet()) {
            Json::Value error;
            error["error"]["code"] = -13;
            error["error"]["message"] = "No active wallet. Use wallet.load first.";
            return error;
        }
        
        if (!wallet_manager->isWalletEncrypted()) {
            Json::Value error;
            error["error"]["code"] = -15;
            error["error"]["message"] = "Wallet is not encrypted";
            return error;
        }
        
        // Attempt to change passphrase
        try {
            wallet_manager->changePassphrase(old_passphrase, new_passphrase);
            
            Json::Value result;
            result["changed"] = true;
            result["message"] = "Passphrase changed successfully";
            result["wallet"] = wallet_manager->getCurrentWalletName();
            
            dinero::g_logger.info("Wallet passphrase changed: " + wallet_manager->getCurrentWalletName());
            return result;
            
        } catch (const std::exception& change_error) {
            Json::Value error;
            error["error"]["code"] = -14;
            error["error"]["message"] = "Invalid old passphrase";
            return error;
        }
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to change passphrase: ") + e.what();
        return error;
    }
}

// Enhanced wallet info with encryption status
Json::Value rpc_wallet_info(RpcServer& server, const Json::Value& params) {
    try {
        // Get WalletManager from server context
        auto* wallet_manager = server.getWalletManager();
        if (!wallet_manager) {
            Json::Value error;
            error["error"]["code"] = -1;
            error["error"]["message"] = "Wallet manager not available";
            return error;
        }
        
        if (!wallet_manager->hasActiveWallet()) {
            Json::Value error;
            error["error"]["code"] = -13;
            error["error"]["message"] = "No active wallet. Use wallet.load first.";
            return error;
        }
        
        // Get wallet info and balance
        auto balance = wallet_manager->getBalance();
        auto addresses = wallet_manager->listAddresses(true);
        
        Json::Value result;
        result["name"] = wallet_manager->getCurrentWalletName();
        result["encrypted"] = wallet_manager->isWalletEncrypted();
        result["locked"] = wallet_manager->isWalletLocked();
        result["address_count"] = static_cast<int>(addresses.size());
        
        // Balance information
        result["balance"]["confirmed"] = balance.confirmed;
        result["balance"]["unconfirmed"] = balance.unconfirmed;
        result["balance"]["total"] = balance.total;
        result["balance"]["utxo_count"] = balance.utxo_count;
        
        // Wallet status
        if (wallet_manager->isWalletEncrypted()) {
            result["status"] = wallet_manager->isWalletLocked() ? "locked" : "unlocked";
        } else {
            result["status"] = "unencrypted";
        }
        
        return result;
        
    } catch (const std::exception& e) {
        Json::Value error;
        error["error"]["code"] = -1;
        error["error"]["message"] = std::string("Failed to get wallet info: ") + e.what();
        return error;
    }
}
