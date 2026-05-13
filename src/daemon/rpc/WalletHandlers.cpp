#include "daemon/DaemonRpc.h"
#include "WalletHandlers.h"
#include "wallet/wallet_manager.h"
#include "wallet/bip39.h"
#include "common/logger.h"
#include "address/addr_codec.h"
#include "consensus/coin_type.h"
#include <stdexcept>
#include <json/json.h>

#include <filesystem>
#include <glob.h>
#include <vector>

#if DIN_ENABLE_LEGACY_RPC
  #include "daemon/rpc_server.h"
  static dinero::RPCServer* rpc_server_ = nullptr;   // existing wiring
#else
  // vNext build: no legacy server; provide a no-op sink
  struct RpcLegacySink {
    void publish(const char* /*topic*/, const Json::Value& /*payload*/) noexcept {}
  };
  static RpcLegacySink rpc_legacy_sink_;
#endif

// Replace every direct publish with this macro
#define DIN_RPC_PUBLISH(topic, payload)                 \
  do {                                                  \
    if (false) { /* placeholder for macro */ }          \
  } while (0)

#if DIN_ENABLE_LEGACY_RPC
WalletHandlers::WalletHandlers(dinero::RPCServer& rpc_server) : rpc_server_(rpc_server), wallet_manager_(nullptr) {
#else
WalletHandlers::WalletHandlers() : wallet_manager_(nullptr) {
#endif
    // Set up nodeinfo path pattern in the active system temp dir.
    nodeinfo_path_ = (std::filesystem::temp_directory_path() / "dinero-nodeinfo-*.json").string();
}

Json::Value WalletHandlers::create(const Json::Value& params) {
#if DIN_ENABLE_LEGACY_RPC
    if (!wallet_manager_) wallet_manager_ = rpc_server_.getWalletManager();
#endif
    // TODO: Add proper logging
    // LOG_I("WalletHandlers::create called, wm=" + std::string(wallet_manager_ ? "YES" : "NO"));
    if (!wallet_manager_) {
        // TODO: Add proper logging
        // LOG_W("WalletManager not injected into RPC context");
        Json::Value result;
        result["error"] = "Wallet manager not available";
        return result;
    }
    
    Json::Value result;
    
    try {
        std::string name = "default";
        std::string bip39_passphrase;

        if (params.isObject() && params.isMember("name")) {
            name = params["name"].asString();
        } else if (params.isArray() && params.size() > 0) {
            name = params[0].asString();
        }

        if (params.isObject() && params.isMember("passphrase")) {
            bip39_passphrase = params["passphrase"].asString();
        } else if (params.isArray() && params.size() > 1) {
            bip39_passphrase = params[1].asString();
        }
        
        // Create wallet in wallet manager
        try {
            wallet_manager_->create(name);
        } catch (const std::exception& e) {
            result["error"] = "WalletManager create failed: " + std::string(e.what());
            result["success"] = false;
            return result;
        }

        const std::string mnemonic = dinero::bip39::Generate(dinero::bip39::WordCount::Words12);
        if (mnemonic.empty()) {
            result["success"] = false;
            result["error"] = "Failed to generate BIP39 mnemonic";
            return result;
        }

        std::vector<uint8_t> seed;
        if (!dinero::bip39::MnemonicToSeed(mnemonic, bip39_passphrase, seed)) {
            result["success"] = false;
            result["error"] = "Failed to derive seed from mnemonic";
            return result;
        }

        if (!wallet_manager_->storeMasterSeed(seed, "", true)) {
            result["success"] = false;
            result["error"] = "Failed to store wallet seed in database";
            return result;
        }
        
        result["success"] = true;
        result["name"] = name;
        result["seed_phrase"] = mnemonic;
        
    } catch (const std::exception& e) {
        result["error"] = e.what();
        result["success"] = false;
    }
    
    return result;
}

Json::Value WalletHandlers::restore(const Json::Value& params) {
    Json::Value result;
    
    try {
        // Extract mnemonic (required)
        std::string mnemonic;
        if (params.isObject() && params.isMember("mnemonic")) {
            mnemonic = params["mnemonic"].asString();
        } else if (params.isArray() && params.size() > 0) {
            mnemonic = params[0].asString();
        } else {
            result["error"] = "Missing required parameter: mnemonic";
            result["success"] = false;
            return result;
        }
        
        // Extract optional passphrase
        std::string passphrase = "";
        if (params.isObject() && params.isMember("passphrase")) {
            passphrase = params["passphrase"].asString();
        } else if (params.isArray() && params.size() > 1) {
            passphrase = params[1].asString();
        }
        
        // Extract optional wallet name
        std::string name = "default";
        if (params.isObject() && params.isMember("name")) {
            name = params["name"].asString();
        }

        if (!dinero::bip39::ValidateMnemonic(mnemonic)) {
            result["success"] = false;
            result["error"] = "Invalid mnemonic";
            return result;
        }
        
        // Create wallet in wallet manager if available
#if DIN_ENABLE_LEGACY_RPC
        if (!wallet_manager_) {
            wallet_manager_ = rpc_server_.getWalletManager();
        }
#endif
        
        if (wallet_manager_) {
            try {
                if (!wallet_manager_->exists(name)) {
                    wallet_manager_->create(name);
                } else {
                    wallet_manager_->open(name);
                }
            } catch (const std::exception& e) {
                result["success"] = false;
                result["error"] = "Failed to open or create wallet: " + std::string(e.what());
                return result;
            }
        } else {
            result["success"] = false;
            result["error"] = "Wallet manager not available";
            return result;
        }

        std::vector<uint8_t> seed;
        if (!dinero::bip39::MnemonicToSeed(mnemonic, passphrase, seed)) {
            result["success"] = false;
            result["error"] = "Failed to derive seed from mnemonic";
            return result;
        }

        if (!wallet_manager_->storeMasterSeed(seed, "", true)) {
            result["success"] = false;
            result["error"] = "Failed to store restored seed in database";
            return result;
        }
        
        result["success"] = true;
        result["name"] = name;
        result["message"] = "Wallet restored successfully from mnemonic";
        
    } catch (const std::exception& e) {
        result["error"] = e.what();
        result["success"] = false;
    }
    
    return result;
}

Json::Value WalletHandlers::load(const Json::Value& params) {
    Json::Value result;
    
    try {
        std::string name = "default";
        if (params.isObject() && params.isMember("name")) {
            name = params["name"].asString();
        } else if (params.isArray() && params.size() > 0) {
            name = params[0].asString();
        }
        
        // Get wallet manager from RPC server (lazy initialization)
#if DIN_ENABLE_LEGACY_RPC
        if (!wallet_manager_) {
            wallet_manager_ = rpc_server_.getWalletManager();
        }
#endif
        
        if (!wallet_manager_) {
            result["error"] = "Wallet manager not available";
            result["ok"] = false;
            return result;
        }
        
        // Open wallet in wallet manager
        wallet_manager_->open(name);
        
        // Set wallet as loaded in RPC server
#if DIN_ENABLE_LEGACY_RPC
        rpc_server_.setWalletLoaded(true);
#endif
        
        result["ok"] = true;
        result["name"] = name;
        
    } catch (const std::exception& e) {
        result["error"] = e.what();
        result["ok"] = false;
    }
    
    return result;
}

Json::Value WalletHandlers::info(const Json::Value& params) {
    Json::Value result;
    
    try {
        (void)params;
#if DIN_ENABLE_LEGACY_RPC
        if (!wallet_manager_) {
            wallet_manager_ = rpc_server_.getWalletManager();
        }
#endif

        if (!wallet_manager_ || !wallet_manager_->hasActiveWallet()) {
            result["name"] = Json::Value(Json::nullValue);
            result["status"] = "No wallet loaded";
            result["balance"] = 0.0;
            result["txs"] = 0;
            return result;
        }

        const auto balance = wallet_manager_->getBalance();
        const auto txs = wallet_manager_->getTransactionHistory(1, 0);
        result["name"] = wallet_manager_->current();
        result["status"] = wallet_manager_->isWalletLocked() ? "Locked" : "Unlocked";
        result["balance"] = balance.total;
        result["txs"] = static_cast<Json::UInt64>(txs.size());
        result["encrypted"] = wallet_manager_->isWalletEncrypted();
        
    } catch (const std::exception& e) {
        result["error"] = e.what();
    }
    
    return result;
}

std::optional<NodeInfo> WalletHandlers::loadNodeInfo() {
    // Find the most recent nodeinfo file
    glob_t glob_result;
    if (glob(nodeinfo_path_.c_str(), GLOB_TILDE, nullptr, &glob_result) != 0) {
        return std::nullopt;
    }
    
    if (glob_result.gl_pathc == 0) {
        globfree(&glob_result);
        return std::nullopt;
    }
    
    // Use the first (most recent) match
    std::string path = glob_result.gl_pathv[0];
    globfree(&glob_result);
    
    // TODO: Implement loadNodeInfo function
    return std::nullopt;
}

std::unique_ptr<DaemonRpc> WalletHandlers::createDaemonRpc() {
    // TODO: Implement DaemonRpc creation
    return nullptr;
}

Json::Value WalletHandlers::getNewAddress(const Json::Value& params) {
    Json::Value result;
    
    try {
        // Get wallet manager from RPC server (lazy initialization)
#if DIN_ENABLE_LEGACY_RPC
        if (!wallet_manager_) {
            wallet_manager_ = rpc_server_.getWalletManager();
        }
#endif
        
        // Check if wallet manager is available and has active wallet
        if (!wallet_manager_) {
            result["code"] = -18;
            result["message"] = "Wallet manager not available";
            return result;
        }
        
        // Extract parameters (handle both array and object formats)
        std::string purpose = "receive";  // default
        int account = 0;  // default account
        int change = 0;   // external addresses (receive)
        
        if (params.isObject()) {
            if (params.isMember("purpose") && params["purpose"].isString()) {
                purpose = params["purpose"].asString();
            }
            if (params.isMember("account") && params["account"].isInt()) {
                account = params["account"].asInt();
            }
        } else if (params.isArray() && params.size() > 0) {
            // Handle positional parameters if needed
            if (params.size() > 0 && params[0].isString()) {
                purpose = params[0].asString();
            }
            if (params.size() > 1 && params[1].isInt()) {
                account = params[1].asInt();
            }
        }
        
        if (account != 0) {
            result["code"] = -8;
            result["message"] = "Only account 0 is currently supported";
            return result;
        }

        std::string address_type = "legacy";
        if (purpose == "taproot" || purpose == "p2tr") {
            address_type = "taproot";
        }

        std::string address;
        if (purpose == "change") {
            address = wallet_manager_->getNewChangeAddress(purpose, address_type);
            change = 1;
        } else {
            address = wallet_manager_->getNewAddress(purpose, address_type);
            change = 0;
        }

        if (address.empty()) {
            result["code"] = -4;
            result["message"] = "Address derivation failed";
            return result;
        }

        int index = 0;
        std::string path;
        auto rows = wallet_manager_->listAddresses(true);
        for (const auto& row : rows) {
            if (row.address != address) {
                continue;
            }

            index = row.index;
            const uint32_t purpose_num = (row.type == "p2tr") ? 86u : 84u;
            path = "m/" + std::to_string(purpose_num) + "'/" +
                   std::to_string(dinero::consensus::DINERO_COIN_TYPE) + "'/" +
                   std::to_string(row.account) + "'/" +
                   std::to_string(row.change) + "/" +
                   std::to_string(row.index);
            break;
        }

        result["address"] = address;
        result["path"] = path;
        result["index"] = index;
        result["purpose"] = purpose;
        return result;
        
    } catch (const std::exception& e) {
        result["error"] = e.what();
        return result;
    }
}

Json::Value WalletHandlers::listAddresses(const Json::Value& params) {
    Json::Value result;
    Json::Value addresses(Json::arrayValue);
    
    try {
        (void)params;
        // Get wallet manager from RPC server (lazy initialization)
#if DIN_ENABLE_LEGACY_RPC
        if (!wallet_manager_) {
            wallet_manager_ = rpc_server_.getWalletManager();
        }
#endif

        if (!wallet_manager_) {
            result["code"] = -18;
            result["message"] = "Wallet manager not available";
            return result;
        }

        auto rows = wallet_manager_->listAddresses(true);
        for (const auto& row : rows) {
            Json::Value addr;
            addr["address"] = row.address;
            if (row.label) {
                addr["label"] = *row.label;
            } else {
                addr["label"] = Json::Value(Json::nullValue);
            }
            addr["account"] = row.account;
            addr["change"] = row.change;
            addr["index"] = row.index;
            addr["external"] = row.external;
            addr["type"] = row.type;
            addresses.append(addr);
        }
        
        result["addresses"] = addresses;
        
    } catch (const std::exception& e) {
        result["error"] = e.what();
    }
    
    return result;
}
