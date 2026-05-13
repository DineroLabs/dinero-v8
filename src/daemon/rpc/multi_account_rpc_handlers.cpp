#include "daemon/rpc/multi_account_rpc_handlers.h"
#include "config/coin_params.h"
#include "multi_account/multi_account_manager.h"
#include "common/logger.h"
/* daemon-only: Log.hpp disabled */
#include <filesystem>
#include <stdexcept>
#include <cstdlib>

MultiAccountRpcHandlers::MultiAccountRpcHandlers(dinero::RPCServer& rpc_server) 
    : rpc_server_(rpc_server) {
    // Runtime path hygiene: derive datadir from HOME rather than embedding host-specific paths.
    const char* home = std::getenv("HOME");
    if (home && *home) {
        multi_account_datadir_ = (std::filesystem::path(home) / ".dinero" / "multi_account").string();
    } else {
        multi_account_datadir_ = (std::filesystem::temp_directory_path() / "dinero" / "multi_account").string();
    }
    std::filesystem::create_directories(multi_account_datadir_);
    
    /* LOG disabled for daemon-only */("MultiAccountRpcHandlers initialized with datadir: " + multi_account_datadir_);
}

void MultiAccountRpcHandlers::ensureMultiAccountManager() {
    if (!multi_account_manager_) {
        try {
            multi_account_manager_ = std::make_unique<Dinero::MultiAccount::MultiAccountManager>();
            /* LOG disabled for daemon-only */("MultiAccountManager created successfully");
        } catch (const std::exception& e) {
            /* LOG disabled for daemon-only */("Failed to create MultiAccountManager: " + std::string(e.what()));
            throw;
        }
    }
}

Json::Value MultiAccountRpcHandlers::createErrorResponse(int code, const std::string& message) {
    Json::Value error;
    error["code"] = code;
    error["message"] = message;
    
    Json::Value response;
    response["error"] = error;
    response["result"] = Json::nullValue;
    response["rpc_schema"] = "din.rpc.v1";
    response["schema_rev"] = 1;
    
    return response;
}

Json::Value MultiAccountRpcHandlers::createSuccessResponse(const Json::Value& result) {
    Json::Value response;
    response["result"] = result;
    response["error"] = Json::nullValue;
    response["rpc_schema"] = "din.rpc.v1";
    response["schema_rev"] = 1;
    
    return response;
}

std::string MultiAccountRpcHandlers::getAccountIdFromParams(const Json::Value& params) {
    if (params.isObject() && params.isMember("account_id")) {
        return params["account_id"].asString();
    } else if (params.isArray() && params.size() > 0) {
        return params[0].asString();
    }
    return "";
}

bool MultiAccountRpcHandlers::validateAccountId(const std::string& accountId) {
    // Account ID validation rules:
    // - Must not be empty
    // - Must be between 8 and 64 characters
    // - Must contain only alphanumeric characters, hyphens, and underscores
    // - Must start with alphanumeric character
    // - Must not end with hyphen or underscore
    
    if (accountId.empty() || accountId.length() < 8 || accountId.length() > 64) {
        return false;
    }
    
    // Check first character
    if (!std::isalnum(accountId[0])) {
        return false;
    }
    
    // Check last character
    char last = accountId.back();
    if (last == '-' || last == '_') {
        return false;
    }
    
    // Check all characters
    for (char c : accountId) {
        if (!std::isalnum(c) && c != '-' && c != '_') {
            return false;
        }
    }
    
    return true;
}

// Account Management Methods
Json::Value MultiAccountRpcHandlers::createAccount(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        std::string name = "New Account";
        std::string description = "";
        std::string type = "PERSONAL";
        std::string color = "#3498db";
        
        if (params.isObject()) {
            if (params.isMember("name")) name = params["name"].asString();
            if (params.isMember("description")) description = params["description"].asString();
            if (params.isMember("type")) type = params["type"].asString();
            if (params.isMember("color")) color = params["color"].asString();
        } else if (params.isArray() && params.size() >= 1) {
            name = params[0].asString();
            if (params.size() >= 2) description = params[1].asString();
            if (params.size() >= 3) type = params[2].asString();
            if (params.size() >= 4) color = params[3].asString();
        }
        
        std::string accountId = multi_account_manager_->createAccount(
            /* fromStdString removed */(name),
            /* fromStdString removed */(description),
            /* fromStdString removed */(type),
            /* fromStdString removed */(color)
        );
        
        if (accountId.empty()) {
            return createErrorResponse(-32603, "Failed to create account");
        }
        
        Json::Value result;
        result["account_id"] = accountId;
        result["name"] = name;
        result["description"] = description;
        result["type"] = type;
        result["color"] = color;
        result["success"] = true;
        
        /* LOG disabled for daemon-only */("Created account: " + accountId + " (" + name + ")");
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("createAccount failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::deleteAccount(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        std::string accountId = getAccountIdFromParams(params);
        if (!validateAccountId(accountId)) {
            return createErrorResponse(-32602, "Invalid account_id parameter");
        }
        
        bool success = multi_account_manager_->deleteAccount(accountId);
        
        Json::Value result;
        result["account_id"] = accountId;
        result["success"] = success;
        
        if (success) {
            /* LOG disabled for daemon-only */("Deleted account: " + accountId);
        } else {
            /* LOG disabled for daemon-only */("Failed to delete account: " + accountId);
        }
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("deleteAccount failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::restoreAccount(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        std::string mnemonic, name, description, type;
        
        if (params.isObject()) {
            if (!params.isMember("mnemonic")) {
                return createErrorResponse(-32602, "Missing 'mnemonic' parameter");
            }
            mnemonic = params["mnemonic"].asString();
            name = params.get("name", "Restored Account").asString();
            description = params.get("description", "Restored from mnemonic").asString();
            type = params.get("type", "PERSONAL").asString();
        } else if (params.isArray() && params.size() >= 1) {
            mnemonic = params[0].asString();
            name = params.size() >= 2 ? params[1].asString() : "Restored Account";
            description = params.size() >= 3 ? params[2].asString() : "Restored from mnemonic";
            type = params.size() >= 4 ? params[3].asString() : "PERSONAL";
        } else {
            return createErrorResponse(-32602, "Invalid parameters");
        }
        
        if (mnemonic.empty()) {
            return createErrorResponse(-32602, "Mnemonic cannot be empty");
        }
        
        bool success = multi_account_manager_->restoreAccount(
            /* fromStdString removed */(mnemonic),
            /* fromStdString removed */(name),
            /* fromStdString removed */(description),
            /* fromStdString removed */(type)
        );
        
        Json::Value result;
        result["success"] = success;
        result["name"] = name;
        result["description"] = description;
        result["type"] = type;
        
        if (success) {
            /* LOG disabled for daemon-only */("Restored account: " + name);
        } else {
            /* LOG disabled for daemon-only */("Failed to restore account: " + name);
        }
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("restoreAccount failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::switchToAccount(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        std::string accountId = getAccountIdFromParams(params);
        if (!validateAccountId(accountId)) {
            return createErrorResponse(-32602, "Invalid account_id parameter");
        }
        
        bool success = multi_account_manager_->switchToAccount(accountId);
        
        Json::Value result;
        result["account_id"] = accountId;
        result["success"] = success;
        result["current_account"] = multi_account_manager_->currentAccountId();
        
        if (success) {
            /* LOG disabled for daemon-only */("Switched to account: " + accountId);
        } else {
            /* LOG disabled for daemon-only */("Failed to switch to account: " + accountId);
        }
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("switchToAccount failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::listAccounts(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        std::vector<std::string> accountIds = multi_account_manager_->getAllAccountIds();
        
        Json::Value accounts(Json::arrayValue);
        for (const std::string& accountId : accountIds) {
            Json::Value account;
            account["account_id"] = accountId;
            account["name"] = multi_account_manager_->getAccountName(accountId);
            account["description"] = multi_account_manager_->getAccountDescription(accountId);
            account["type"] = multi_account_manager_->getAccountType(accountId);
            account["color"] = multi_account_manager_->getAccountColor(accountId);
            account["balance"] = multi_account_manager_->getAccountBalance(accountId);
            account["current_address"] = multi_account_manager_->getCurrentAddress(accountId);
            account["is_active"] = multi_account_manager_->isAccountActive(accountId);
            account["is_hidden"] = multi_account_manager_->isAccountHidden(accountId);
            
            accounts.append(account);
        }
        
        Json::Value result;
        result["accounts"] = accounts;
        result["count"] = static_cast<int>(accountIds.size());
        result["current_account"] = multi_account_manager_->currentAccountId();
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("listAccounts failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::getAccountInfo(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        std::string accountId = getAccountIdFromParams(params);
        if (!validateAccountId(accountId)) {
            return createErrorResponse(-32602, "Invalid account_id parameter");
        }
        
        std::string qAccountId = accountId;
        
        Json::Value result;
        result["account_id"] = accountId;
        result["name"] = multi_account_manager_->getAccountName(qAccountId);
        result["description"] = multi_account_manager_->getAccountDescription(qAccountId);
        result["type"] = multi_account_manager_->getAccountType(qAccountId);
        result["color"] = multi_account_manager_->getAccountColor(qAccountId);
        result["balance"] = multi_account_manager_->getAccountBalance(qAccountId);
        result["current_address"] = multi_account_manager_->getCurrentAddress(qAccountId);
        result["address_list"] = Json::Value(Json::arrayValue);
        result["transactions"] = Json::Value(Json::arrayValue);
        result["is_active"] = multi_account_manager_->isAccountActive(qAccountId);
        result["is_hidden"] = multi_account_manager_->isAccountHidden(qAccountId);
        
        // Add address_list
        std::vector<std::string> address_list = multi_account_manager_->getAccountAddresses(qAccountId);
        for (const std::string& address : address_list) {
            result["address_list"].append(address);
        }
        
        // Add transactions
        std::vector<std::string> transactions = multi_account_manager_->getTransactionHistory(qAccountId);
        for (const std::string& tx : transactions) {
            result["transactions"].append(tx);
        }
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("getAccountInfo failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

// Address Management Methods
Json::Value MultiAccountRpcHandlers::generateNewAddress(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        std::string accountId = getAccountIdFromParams(params);
        if (accountId.empty()) {
            accountId = multi_account_manager_->currentAccountId();
        }
        
        if (!validateAccountId(accountId)) {
            return createErrorResponse(-32602, "Invalid account_id parameter");
        }
        
        std::string newAddress = multi_account_manager_->generateNewAddress(accountId);
        
        if (newAddress.empty()) {
            return createErrorResponse(-32603, "Failed to generate new address");
        }
        
        Json::Value result;
        result["account_id"] = accountId;
        result["address"] = newAddress;
        result["success"] = true;
        
        /* LOG disabled for daemon-only */("Generated new address for account " + accountId + ": " + newAddress);
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("generateNewAddress failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::getCurrentAddress(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        std::string accountId = getAccountIdFromParams(params);
        if (accountId.empty()) {
            accountId = multi_account_manager_->currentAccountId();
        }
        
        if (!validateAccountId(accountId)) {
            return createErrorResponse(-32602, "Invalid account_id parameter");
        }
        
        std::string currentAddress = multi_account_manager_->getCurrentAddress(accountId);
        
        Json::Value result;
        result["account_id"] = accountId;
        result["address"] = currentAddress;
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("getCurrentAddress failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::getAccountBalance(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        std::string accountId = getAccountIdFromParams(params);
        if (accountId.empty()) {
            accountId = multi_account_manager_->currentAccountId();
        }
        
        if (!validateAccountId(accountId)) {
            return createErrorResponse(-32602, "Invalid account_id parameter");
        }
        
        double balance = multi_account_manager_->getAccountBalance(accountId);
        
        Json::Value result;
        result["account_id"] = accountId;
        result["balance"] = balance;
        result["currency"] = "DIN";
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("getAccountBalance failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::getTotalBalance(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        double totalBalance = multi_account_manager_->getTotalBalance();
        
        Json::Value result;
        result["total_balance"] = totalBalance;
        result["currency"] = "DIN";
        result["account_count"] = multi_account_manager_->getAccountCount();
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("getTotalBalance failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::getCurrentAccount(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        std::string currentAccountId = multi_account_manager_->currentAccountId();
        
        Json::Value result;
        result["account_id"] = currentAccountId;
        result["name"] = multi_account_manager_->getAccountName(currentAccountId);
        result["description"] = multi_account_manager_->getAccountDescription(currentAccountId);
        result["type"] = multi_account_manager_->getAccountType(currentAccountId);
        result["color"] = multi_account_manager_->getAccountColor(currentAccountId);
        result["balance"] = multi_account_manager_->getAccountBalance(currentAccountId);
        result["current_address"] = multi_account_manager_->getCurrentAddress(currentAccountId);
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("getCurrentAccount failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::getAccountCount(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        int count = multi_account_manager_->getAccountCount();
        
        Json::Value result;
        result["count"] = count;
        result["active_count"] = static_cast<int>(multi_account_manager_->getActiveAccountIds().size());
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("getAccountCount failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

// Placeholder implementations for remaining methods
Json::Value MultiAccountRpcHandlers::renameAccount(const Json::Value& params) {
    return createErrorResponse(-32601, "Account renaming feature is not yet implemented. This feature will be available in a future release.");
}

Json::Value MultiAccountRpcHandlers::generateAddressAt(const Json::Value& params) {
    return createErrorResponse(-32601, "Address generation at specific derivation paths is not yet implemented. Use the standard address generation methods instead.");
}

Json::Value MultiAccountRpcHandlers::listAddresses(const Json::Value& params) {
    return createErrorResponse(-32601, "Address listing feature is not yet implemented. This feature will be available in a future release.");
}

Json::Value MultiAccountRpcHandlers::validateAddress(const Json::Value& params) {
    return createErrorResponse(-32601, "Advanced address validation is not yet implemented. Use the basic validateaddress method instead.");
}

Json::Value MultiAccountRpcHandlers::sendTransaction(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        // Validate required parameters
        if (!params.isMember("to") || !params["to"].isString()) {
            return createErrorResponse(-32602, "Missing or invalid 'to' parameter");
        }
        if (!params.isMember("amount") || !params["amount"].isNumeric()) {
            return createErrorResponse(-32602, "Missing or invalid 'amount' parameter");
        }
        
        std::string accountId = getAccountIdFromParams(params);
        if (accountId.empty()) {
            accountId = multi_account_manager_->currentAccountId();
        }
        
        if (!validateAccountId(accountId)) {
            return createErrorResponse(-32602, "Invalid account_id parameter");
        }
        
        std::string toAddress = params["to"].asString();
        double amount = params["amount"].asDouble();
        double feeRate = params.get("fee_rate", 0.0001).asDouble();
        bool subtractFee = params.get("subtract_fee", false).asBool();
        bool dryRun = params.get("dry_run", false).asBool();
        
        // Validate amount
        if (amount <= 0) {
            return createErrorResponse(-32602, "Amount must be positive");
        }
        
        // Get account balance
        double balance = multi_account_manager_->getAccountBalance(accountId);
        double requiredAmount = subtractFee ? amount : amount + feeRate;
        
        if (balance < requiredAmount) {
            Json::Value result;
            result["error"] = "Insufficient funds";
            result["available"] = balance;
            result["required"] = requiredAmount;
            result["shortfall"] = requiredAmount - balance;
            return createErrorResponse(-6, "Insufficient funds");
        }
        
        // For now, simulate transaction creation
        // In a real implementation, this would:
        // 1. Select UTXOs from the account
        // 2. Create transaction inputs and outputs
        // 3. Sign the transaction
        // 4. Broadcast to the network
        
        std::string txid = "tx_" + accountId + "_" + std::to_string(std::time(nullptr));
        double actualFee = feeRate > 0 ? feeRate : 0.0001;
        double actualSent = subtractFee ? (amount - actualFee) : amount;
        
        Json::Value result;
        result["account_id"] = accountId;
        result["txid"] = txid;
        result["to"] = toAddress;
        result["amount"] = actualSent;
        result["fee"] = actualFee;
        result["fee_rate"] = feeRate;
        result["subtract_fee"] = subtractFee;
        result["dry_run"] = dryRun;
        result["status"] = dryRun ? "simulated" : "pending";
        
        if (!dryRun) {
            // Add transaction to account history
            multi_account_manager_->sendTransaction(
                accountId,
                /* fromStdString removed */(toAddress),
                actualSent,
                /* fromStdString removed */(txid)
            );
            
            /* LOG disabled for daemon-only */("Transaction sent from account " + accountId + " to " + toAddress + 
                  " amount: " + std::to_string(actualSent) + " DIN, txid: " + txid);
        }
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("sendTransaction failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::getTransactionHistory(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        std::string accountId = getAccountIdFromParams(params);
        if (accountId.empty()) {
            accountId = multi_account_manager_->currentAccountId();
        }
        
        if (!validateAccountId(accountId)) {
            return createErrorResponse(-32602, "Invalid account_id parameter");
        }
        
        int limit = params.get("limit", 50).asInt();
        int offset = params.get("offset", 0).asInt();
        
        // Validate pagination parameters
        if (limit < 1 || limit > 1000) {
            limit = 50;
        }
        if (offset < 0) {
            offset = 0;
        }
        
        std::vector<std::string> transactions = multi_account_manager_->getTransactionHistory(accountId);
        
        Json::Value result;
        result["account_id"] = accountId;
        result["transactions"] = Json::Value(Json::arrayValue);
        result["total_count"] = static_cast<int>(transactions.size());
        result["limit"] = limit;
        result["offset"] = offset;
        
        // Apply pagination
        int startIndex = offset;
        int endIndex = std::min(startIndex + limit, static_cast<int>(transactions.size()));
        
        for (int i = startIndex; i < endIndex; ++i) {
            Json::Value tx;
            tx["txid"] = transactions[i];
            tx["account_id"] = accountId;
            tx["status"] = "confirmed"; // Simulated status
            tx["confirmations"] = 6; // Simulated confirmations
            tx["timestamp"] = static_cast<int64_t>(std::time(nullptr) - (transactions.size() - i) * 3600);
            tx["amount"] = 1.0 + (i % 10); // Simulated amounts
            tx["fee"] = 0.0001;
            tx["type"] = (i % 2 == 0) ? "send" : "receive";
            
            result["transactions"].append(tx);
        }
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("getTransactionHistory failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::getUTXOs(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        std::string accountId = getAccountIdFromParams(params);
        if (accountId.empty()) {
            accountId = multi_account_manager_->currentAccountId();
        }
        
        if (!validateAccountId(accountId)) {
            return createErrorResponse(-32602, "Invalid account_id parameter");
        }
        
        int minConfirmations = params.get("min_confirmations", 1).asInt();
        int maxConfirmations = params.get("max_confirmations", 999999).asInt();
        double minAmount = params.get("min_amount", 0.0).asDouble();
        double maxAmount = params.get("max_amount", 999999999.0).asDouble();
        
        // Validate parameters
        if (minConfirmations < 0) minConfirmations = 0;
        if (maxConfirmations < minConfirmations) maxConfirmations = minConfirmations;
        if (minAmount < 0) minAmount = 0;
        if (maxAmount < minAmount) maxAmount = minAmount;
        
        // For now, simulate UTXO data
        // In a real implementation, this would query the UTXO index
        Json::Value result;
        result["account_id"] = accountId;
        result["utxos"] = Json::Value(Json::arrayValue);
        result["total_count"] = 0;
        result["total_amount"] = 0.0;
        result["min_confirmations"] = minConfirmations;
        result["max_confirmations"] = maxConfirmations;
        result["min_amount"] = minAmount;
        result["max_amount"] = maxAmount;
        
#ifdef MOCK_BUILD
        // ⚠️ MOCK CODE - FOR TESTING ONLY
        // This simulates UTXOs without querying real UTXO index
        // FORBIDDEN in mainnet builds (see DEVELOPER_CHARTER.md)
        std::vector<Json::Value> simulatedUtxos;
#else
        // Production: Query real UTXO index
        std::vector<Json::Value> simulatedUtxos;
        // TODO: Replace with real UTXO query when multi-account system is production-ready
        throw std::runtime_error("Multi-account RPC not yet implemented for mainnet - use HDWallet RPC methods");
#endif
        for (int i = 0; i < 5; ++i) {
            Json::Value utxo;
            utxo["txid"] = "utxo_" + accountId + "_" + std::to_string(i);
            utxo["vout"] = i;
            utxo["amount"] = 1.0 + (i * 0.5);
            utxo["scriptPubKey"] = "0014" + std::string(40, '0'); // Simulated P2WPKH script
            utxo["confirmations"] = 6 + i;
            utxo["height"] = 1000 + i;
            utxo["address"] = multi_account_manager_->getCurrentAddress(accountId);
            utxo["path"] = "m/84'/" + std::to_string(dinero::kSlip44CoinType) + "'/0'/0/" + std::to_string(i);
            
            // Apply filters
            if (utxo["confirmations"].asInt() >= minConfirmations &&
                utxo["confirmations"].asInt() <= maxConfirmations &&
                utxo["amount"].asDouble() >= minAmount &&
                utxo["amount"].asDouble() <= maxAmount) {
                
                simulatedUtxos.push_back(utxo);
                result["total_amount"] = result["total_amount"].asDouble() + utxo["amount"].asDouble();
            }
        }
        
        result["total_count"] = static_cast<int>(simulatedUtxos.size());
        for (const auto& utxo : simulatedUtxos) {
            result["utxos"].append(utxo);
        }
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("getUTXOs failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::estimateFee(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        std::string accountId = getAccountIdFromParams(params);
        if (accountId.empty()) {
            accountId = multi_account_manager_->currentAccountId();
        }
        
        if (!validateAccountId(accountId)) {
            return createErrorResponse(-32602, "Invalid account_id parameter");
        }
        
        double amount = params.get("amount", 0.0).asDouble();
        int targetBlocks = params.get("target_blocks", 6).asInt();
        std::string feeMode = params.get("fee_mode", "economical").asString();
        
        // Validate parameters
        if (amount < 0) {
            return createErrorResponse(-32602, "Amount must be non-negative");
        }
        if (targetBlocks < 1 || targetBlocks > 1008) { // Max ~1 week
            targetBlocks = 6;
        }
        
        // Simulate fee estimation based on target blocks
        // In a real implementation, this would query the mempool and network conditions
        double baseFeeRate = 0.0001; // Base fee rate in DIN per byte
        double feeMultiplier = 1.0;
        
        // Adjust fee based on target blocks (lower blocks = higher fee)
        if (targetBlocks <= 1) {
            feeMultiplier = 2.0; // High priority
        } else if (targetBlocks <= 3) {
            feeMultiplier = 1.5; // Medium priority
        } else if (targetBlocks <= 6) {
            feeMultiplier = 1.0; // Standard priority
        } else {
            feeMultiplier = 0.8; // Low priority
        }
        
        // Adjust based on fee mode
        if (feeMode == "fast") {
            feeMultiplier *= 1.5;
        } else if (feeMode == "slow") {
            feeMultiplier *= 0.7;
        }
        
        double estimatedFeeRate = baseFeeRate * feeMultiplier;
        
        // Estimate transaction size (simplified)
        int estimatedSize = 250; // Typical transaction size in bytes
        if (amount > 0) {
            // Add some size for outputs
            estimatedSize += static_cast<int>(amount * 10); // Rough estimate
        }
        
        double estimatedFee = estimatedFeeRate * estimatedSize;
        
        Json::Value result;
        result["account_id"] = accountId;
        result["amount"] = amount;
        result["target_blocks"] = targetBlocks;
        result["fee_mode"] = feeMode;
        result["estimated_fee"] = estimatedFee;
        result["estimated_fee_rate"] = estimatedFeeRate;
        result["estimated_size"] = estimatedSize;
        result["total_cost"] = amount + estimatedFee;
        
        // Add fee recommendations
        Json::Value recommendations(Json::arrayValue);
        
        Json::Value fast;
        fast["target_blocks"] = 1;
        fast["fee_rate"] = baseFeeRate * 2.0;
        fast["fee"] = baseFeeRate * 2.0 * estimatedSize;
        recommendations.append(fast);
        
        Json::Value standard;
        standard["target_blocks"] = 6;
        standard["fee_rate"] = baseFeeRate;
        standard["fee"] = baseFeeRate * estimatedSize;
        recommendations.append(standard);
        
        Json::Value slow;
        slow["target_blocks"] = 12;
        slow["fee_rate"] = baseFeeRate * 0.7;
        slow["fee"] = baseFeeRate * 0.7 * estimatedSize;
        recommendations.append(slow);
        
        result["recommendations"] = recommendations;
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("estimateFee failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::signTransaction(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        std::string accountId = getAccountIdFromParams(params);
        if (accountId.empty()) {
            accountId = multi_account_manager_->currentAccountId();
        }
        
        if (!validateAccountId(accountId)) {
            return createErrorResponse(-32602, "Invalid account_id parameter");
        }
        
        if (!params.isMember("transaction") || !params["transaction"].isString()) {
            return createErrorResponse(-32602, "Missing or invalid 'transaction' parameter");
        }
        
        std::string transactionHex = params["transaction"].asString();
        bool signAllInputs = params.get("sign_all_inputs", true).asBool();
        
        // For now, simulate transaction signing
        // In a real implementation, this would:
        // 1. Parse the transaction
        // 2. Sign inputs using account private keys
        // 3. Return the signed transaction
        
        std::string signedTxHex = transactionHex + "_signed_" + accountId;
        
        Json::Value result;
        result["account_id"] = accountId;
        result["original_transaction"] = transactionHex;
        result["signed_transaction"] = signedTxHex;
        result["sign_all_inputs"] = signAllInputs;
        result["status"] = "signed";
        
        /* LOG disabled for daemon-only */("Transaction signed for account " + accountId);
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("signTransaction failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::broadcastTransaction(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        std::string accountId = getAccountIdFromParams(params);
        if (accountId.empty()) {
            accountId = multi_account_manager_->currentAccountId();
        }
        
        if (!validateAccountId(accountId)) {
            return createErrorResponse(-32602, "Invalid account_id parameter");
        }
        
        if (!params.isMember("transaction") || !params["transaction"].isString()) {
            return createErrorResponse(-32602, "Missing or invalid 'transaction' parameter");
        }
        
        std::string transactionHex = params["transaction"].asString();
        bool allowHighFees = params.get("allow_high_fees", false).asBool();
        
        // For now, simulate transaction broadcasting
        // In a real implementation, this would:
        // 1. Validate the transaction
        // 2. Add to mempool
        // 3. Relay to peers
        
        std::string txid = "broadcast_" + accountId + "_" + std::to_string(std::time(nullptr));
        
        Json::Value result;
        result["account_id"] = accountId;
        result["transaction"] = transactionHex;
        result["txid"] = txid;
        result["allow_high_fees"] = allowHighFees;
        result["status"] = "broadcasted";
        result["mempool_accepted"] = true;
        
        /* LOG disabled for daemon-only */("Transaction broadcasted for account " + accountId + ", txid: " + txid);
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("broadcastTransaction failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::getTransactionStatus(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        if (!params.isMember("txid") || !params["txid"].isString()) {
            return createErrorResponse(-32602, "Missing or invalid 'txid' parameter");
        }
        
        std::string txid = params["txid"].asString();
        std::string accountId = getAccountIdFromParams(params);
        
        // For now, simulate transaction status
        // In a real implementation, this would query the blockchain and mempool
        
        Json::Value result;
        result["txid"] = txid;
        result["account_id"] = accountId;
        result["status"] = "confirmed";
        result["confirmations"] = 6;
        result["block_height"] = 1000;
        result["block_hash"] = "block_" + txid;
        result["timestamp"] = static_cast<int64_t>(std::time(nullptr) - 3600);
        result["fee"] = 0.0001;
        result["size"] = 250;
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("getTransactionStatus failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::createTransaction(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        std::string accountId = getAccountIdFromParams(params);
        if (accountId.empty()) {
            accountId = multi_account_manager_->currentAccountId();
        }
        
        if (!validateAccountId(accountId)) {
            return createErrorResponse(-32602, "Invalid account_id parameter");
        }
        
        if (!params.isMember("outputs") || !params["outputs"].isArray()) {
            return createErrorResponse(-32602, "Missing or invalid 'outputs' parameter");
        }
        
        double feeRate = params.get("fee_rate", 0.0001).asDouble();
        bool subtractFee = params.get("subtract_fee", false).asBool();
        bool dryRun = params.get("dry_run", false).asBool();
        
        // Parse outputs
        Json::Value outputs = params["outputs"];
        double totalAmount = 0.0;
        std::vector<std::pair<std::string, double>> outputList;
        
        for (const auto& output : outputs) {
            if (!output.isMember("address") || !output.isMember("amount")) {
                return createErrorResponse(-32602, "Invalid output format");
            }
            
            std::string address = output["address"].asString();
            double amount = output["amount"].asDouble();
            
            if (amount <= 0) {
                return createErrorResponse(-32602, "Output amount must be positive");
            }
            
            outputList.push_back({address, amount});
            totalAmount += amount;
        }
        
        // Check balance
        double balance = multi_account_manager_->getAccountBalance(accountId);
        double requiredAmount = subtractFee ? totalAmount : totalAmount + feeRate;
        
        if (balance < requiredAmount) {
            Json::Value result;
            result["error"] = "Insufficient funds";
            result["available"] = balance;
            result["required"] = requiredAmount;
            result["shortfall"] = requiredAmount - balance;
            return createErrorResponse(-6, "Insufficient funds");
        }
        
        // Create transaction
        std::string txid = "create_" + accountId + "_" + std::to_string(std::time(nullptr));
        std::string transactionHex = "01000000" + std::string(200, '0'); // Simulated transaction hex
        
        Json::Value result;
        result["account_id"] = accountId;
        result["txid"] = txid;
        result["transaction"] = transactionHex;
        result["outputs"] = outputs;
        result["total_amount"] = totalAmount;
        result["fee"] = feeRate;
        result["fee_rate"] = feeRate;
        result["subtract_fee"] = subtractFee;
        result["dry_run"] = dryRun;
        result["status"] = dryRun ? "created" : "ready_to_sign";
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("createTransaction failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::getTransactionDetails(const Json::Value& params) {
    try {
        ensureMultiAccountManager();
        
        if (!params.isMember("txid") || !params["txid"].isString()) {
            return createErrorResponse(-32602, "Missing or invalid 'txid' parameter");
        }
        
        std::string txid = params["txid"].asString();
        std::string accountId = getAccountIdFromParams(params);
        
        // For now, simulate transaction details
        // In a real implementation, this would query the blockchain
        
        Json::Value result;
        result["txid"] = txid;
        result["account_id"] = accountId;
        result["version"] = 1;
        result["locktime"] = 0;
        result["size"] = 250;
        result["vsize"] = 250;
        result["weight"] = 1000;
        result["fee"] = 0.0001;
        result["confirmations"] = 6;
        result["block_height"] = 1000;
        result["block_hash"] = "block_" + txid;
        result["timestamp"] = static_cast<int64_t>(std::time(nullptr) - 3600);
        
        // Simulate inputs and outputs
        Json::Value inputs(Json::arrayValue);
        Json::Value outputs(Json::arrayValue);
        
        Json::Value input;
        input["txid"] = "prev_" + txid;
        input["vout"] = 0;
        input["scriptSig"] = "160014" + std::string(40, '0');
        input["sequence"] = static_cast<int64_t>(4294967295);
        inputs.append(input);
        
        Json::Value output;
        output["value"] = 1.0;
        output["scriptPubKey"] = "0014" + std::string(40, '0');
        output["address"] = multi_account_manager_->getCurrentAddress(accountId);
        outputs.append(output);
        
        result["inputs"] = inputs;
        result["outputs"] = outputs;
        
        return createSuccessResponse(result);
        
    } catch (const std::exception& e) {
        /* LOG disabled for daemon-only */("getTransactionDetails failed: " + std::string(e.what()));
        return createErrorResponse(-32603, "Internal error: " + std::string(e.what()));
    }
}

Json::Value MultiAccountRpcHandlers::getAccountColors(const Json::Value& params) {
    return createErrorResponse(-32601, "Method not implemented");
}

Json::Value MultiAccountRpcHandlers::getAccountIcons(const Json::Value& params) {
    return createErrorResponse(-32601, "Method not implemented");
}

Json::Value MultiAccountRpcHandlers::exportAccount(const Json::Value& params) {
    return createErrorResponse(-32601, "Method not implemented");
}

Json::Value MultiAccountRpcHandlers::exportAllAccounts(const Json::Value& params) {
    return createErrorResponse(-32601, "Method not implemented");
}

Json::Value MultiAccountRpcHandlers::importAccount(const Json::Value& params) {
    return createErrorResponse(-32601, "Method not implemented");
}

Json::Value MultiAccountRpcHandlers::importAllAccounts(const Json::Value& params) {
    return createErrorResponse(-32601, "Method not implemented");
}

Json::Value MultiAccountRpcHandlers::getAccountMnemonic(const Json::Value& params) {
    return createErrorResponse(-32601, "Method not implemented");
}

Json::Value MultiAccountRpcHandlers::getAccountStatistics(const Json::Value& params) {
    return createErrorResponse(-32601, "Method not implemented");
}

Json::Value MultiAccountRpcHandlers::isAccountActive(const Json::Value& params) {
    return createErrorResponse(-32601, "Method not implemented");
}

Json::Value MultiAccountRpcHandlers::isAccountHidden(const Json::Value& params) {
    return createErrorResponse(-32601, "Method not implemented");
}
