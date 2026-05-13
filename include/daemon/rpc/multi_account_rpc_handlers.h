#if !DIN_BUILD_GUI
#include <multi_account/multi_account_manager_stub.h>
#endif
#pragma once
#include "compat/jsoncpp_compat.h"
#include <string>
#include <memory>
#include <optional>
#include "multi_account/multi_account_manager.h"
#include "daemon/rpc_server.h"

namespace dinero {
    class MultiAccountManager;
}

/**
 * Multi-Account RPC Handlers
 * 
 * Provides comprehensive RPC API for managing multiple HD wallet accounts
 */
class MultiAccountRpcHandlers {
public:
    explicit MultiAccountRpcHandlers(dinero::RPCServer& rpc_server);
    
    // Account Management
    Json::Value createAccount(const Json::Value& params);
    Json::Value deleteAccount(const Json::Value& params);
    Json::Value restoreAccount(const Json::Value& params);
    Json::Value switchToAccount(const Json::Value& params);
    Json::Value renameAccount(const Json::Value& params);
    Json::Value listAccounts(const Json::Value& params);
    Json::Value getAccountInfo(const Json::Value& params);
    
    // Address Management
    Json::Value generateNewAddress(const Json::Value& params);
    Json::Value generateAddressAt(const Json::Value& params);
    Json::Value listAddresses(const Json::Value& params);
    Json::Value getCurrentAddress(const Json::Value& params);
    Json::Value validateAddress(const Json::Value& params);
    
    // Transaction Management
    Json::Value sendTransaction(const Json::Value& params);
    Json::Value getTransactionHistory(const Json::Value& params);
    Json::Value getAccountBalance(const Json::Value& params);
    Json::Value getTotalBalance(const Json::Value& params);
    Json::Value signTransaction(const Json::Value& params);
    Json::Value broadcastTransaction(const Json::Value& params);
    Json::Value getUTXOs(const Json::Value& params);
    Json::Value estimateFee(const Json::Value& params);
    Json::Value getTransactionStatus(const Json::Value& params);
    Json::Value createTransaction(const Json::Value& params);
    Json::Value getTransactionDetails(const Json::Value& params);
    
    // Account Settings
    Json::Value getAccountSettings(const Json::Value& params);
    Json::Value setAccountSettings(const Json::Value& params);
    Json::Value getAccountTypes(const Json::Value& params);
    Json::Value getAccountColors(const Json::Value& params);
    Json::Value getAccountIcons(const Json::Value& params);
    
    // Backup and Recovery
    Json::Value exportAccount(const Json::Value& params);
    Json::Value exportAllAccounts(const Json::Value& params);
    Json::Value importAccount(const Json::Value& params);
    Json::Value importAllAccounts(const Json::Value& params);
    Json::Value getAccountMnemonic(const Json::Value& params);
    
    // Utility Functions
    Json::Value getCurrentAccount(const Json::Value& params);
    Json::Value getAccountCount(const Json::Value& params);
    Json::Value getAccountStatistics(const Json::Value& params);
    Json::Value isAccountActive(const Json::Value& params);
    Json::Value isAccountHidden(const Json::Value& params);

private:
    dinero::RPCServer& rpc_server_;
    std::unique_ptr<Dinero::MultiAccount::MultiAccountManager> multi_account_manager_;
    std::string multi_account_datadir_;
    
    void ensureMultiAccountManager();
    Json::Value createErrorResponse(int code, const std::string& message);
    Json::Value createSuccessResponse(const Json::Value& result);
    std::string getAccountIdFromParams(const Json::Value& params);
    bool validateAccountId(const std::string& accountId);
};
