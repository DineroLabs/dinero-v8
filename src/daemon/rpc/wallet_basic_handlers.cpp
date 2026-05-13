#include "daemon/rpc/wallet_basic_handlers.h"
#include "wallet/wallet_manager.h"
#include "common/logger.h"
#include <stdexcept>

namespace dinero::rpc {

din::Json RpcWalletCreate(const din::Json& params, 
                          dinero::WalletManager* wallet_manager) {
    din::Json result;
    
    try {
        // Check wallet manager
        if (!wallet_manager) {
            throw std::runtime_error("Wallet manager not available");
        }
        
        // Parse parameters
        std::string name = "default";
        bool encrypted = false;
        std::string passphrase;
        
        if (params.isObject()) {
            if (params.isMember("name") && params["name"].isString()) {
                name = params["name"].asString();
            }
            if (params.isMember("encrypted") && params["encrypted"].isBool()) {
                encrypted = params["encrypted"].asBool();
            }
            if (params.isMember("passphrase") && params["passphrase"].isString()) {
                passphrase = params["passphrase"].asString();
            }
        }
        
        // Validate parameters
        if (encrypted && passphrase.empty()) {
            throw std::runtime_error("Passphrase required for encrypted wallet");
        }
        
        // Check if wallet already exists
        if (wallet_manager->exists(name)) {
            throw std::runtime_error("Wallet '" + name + "' already exists");
        }
        
        // Create the wallet
        wallet_manager->create(name);
        
        // If encryption was requested, encrypt it
        if (encrypted) {
            // Load the wallet first to encrypt it
            wallet_manager->open(name);
            wallet_manager->encryptWallet(passphrase);
        }
        
        result["name"] = name;
        result["created"] = true;
        result["encrypted"] = encrypted;
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
        
        dinero::g_logger.info("Created wallet: " + name + (encrypted ? " (encrypted)" : ""));
        
    } catch (const std::exception& e) {
        result["error"] = e.what();
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
    }
    
    return result;
}

din::Json RpcWalletLoad(const din::Json& params, 
                        dinero::WalletManager* wallet_manager) {
    din::Json result;
    
    try {
        // Check wallet manager
        if (!wallet_manager) {
            throw std::runtime_error("Wallet manager not available");
        }
        
        // Parse parameters
        std::string name = "default";
        
        if (params.isObject() && params.isMember("name") && params["name"].isString()) {
            name = params["name"].asString();
        }
        
        // Check if wallet exists
        if (!wallet_manager->exists(name)) {
            throw std::runtime_error("Wallet '" + name + "' does not exist. Use wallet.create first.");
        }
        
        // Load/activate the wallet
        wallet_manager->open(name);
        
        result["name"] = name;
        result["active"] = true;
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
        
        dinero::g_logger.info("Loaded wallet: " + name);
        
    } catch (const std::exception& e) {
        result["error"] = e.what();
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
    }
    
    return result;
}

din::Json RpcWalletList(const din::Json& params, 
                        dinero::WalletManager* wallet_manager) {
    din::Json result;
    
    try {
        // Check wallet manager
        if (!wallet_manager) {
            throw std::runtime_error("Wallet manager not available");
        }
        
        // Get list of wallets
        auto wallets = wallet_manager->listWallets();
        
        // Convert to JSON array
        din::Json wallet_array = din::Json(Json::arrayValue);
        for (const auto& wallet : wallets) {
            wallet_array.append(wallet);
        }
        
        result["wallets"] = wallet_array;
        
        // Add current wallet info
        if (wallet_manager->hasActiveWallet()) {
            result["current"] = wallet_manager->getCurrentWalletName();
        } else {
            result["current"] = din::Json();
        }
        
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
        
    } catch (const std::exception& e) {
        result["error"] = e.what();
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
    }
    
    return result;
}

din::Json RpcWalletStatus(const din::Json& params, 
                          dinero::WalletManager* wallet_manager) {
    din::Json result;
    
    try {
        // Check wallet manager
        if (!wallet_manager) {
            throw std::runtime_error("Wallet manager not available");
        }
        
        // Get wallet status
        if (wallet_manager->hasActiveWallet()) {
            std::string current_name = wallet_manager->getCurrentWalletName();
            result["name"] = current_name;
            result["active"] = true;
            result["encrypted"] = wallet_manager->isWalletEncrypted();
            result["locked"] = wallet_manager->isWalletLocked();
        } else {
            result["name"] = din::Json();
            result["active"] = false;
            result["encrypted"] = false;
            result["locked"] = false;
        }
        
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
        
    } catch (const std::exception& e) {
        result["error"] = e.what();
        result["rpc_schema"] = "din.rpc.v1";
        result["schema_rev"] = 1;
    }
    
    return result;
}

} // namespace dinero::rpc
