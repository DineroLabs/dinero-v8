#include "daemon/rpc/multi_account_rpc_registration.h"
#include "daemon/rpc/multi_account_rpc_handlers.h"
#include "common/logger.h"
#include "common/Log.hpp"

namespace dinero {
namespace rpc {

std::unique_ptr<MultiAccountRpcHandlers> MultiAccountRpcRegistration::handlers_;

void MultiAccountRpcRegistration::registerMultiAccountMethods(RPCServer& server) {
    try {
        LOG_I("Registering Multi-Account RPC methods...");
        
        // Create handlers instance
        handlers_ = std::make_unique<MultiAccountRpcHandlers>(server);
        
        // Account Management Methods
        server.registerMethod("multiaccount.create", [](const Json::Value& params) -> Json::Value {
            return handlers_->createAccount(params);
        });
        
        server.registerMethod("multiaccount.delete", [](const Json::Value& params) -> Json::Value {
            return handlers_->deleteAccount(params);
        });
        
        server.registerMethod("multiaccount.restore", [](const Json::Value& params) -> Json::Value {
            return handlers_->restoreAccount(params);
        });
        
        server.registerMethod("multiaccount.switch", [](const Json::Value& params) -> Json::Value {
            return handlers_->switchToAccount(params);
        });
        
        server.registerMethod("multiaccount.rename", [](const Json::Value& params) -> Json::Value {
            return handlers_->renameAccount(params);
        });
        
        server.registerMethod("multiaccount.list", [](const Json::Value& params) -> Json::Value {
            return handlers_->listAccounts(params);
        });
        
        server.registerMethod("multiaccount.info", [](const Json::Value& params) -> Json::Value {
            return handlers_->getAccountInfo(params);
        });
        
        // Address Management Methods
        server.registerMethod("multiaccount.getnewaddress", [](const Json::Value& params) -> Json::Value {
            return handlers_->generateNewAddress(params);
        });
        
        server.registerMethod("multiaccount.getaddressat", [](const Json::Value& params) -> Json::Value {
            return handlers_->generateAddressAt(params);
        });
        
        server.registerMethod("multiaccount.listaddresses", [](const Json::Value& params) -> Json::Value {
            return handlers_->listAddresses(params);
        });
        
        server.registerMethod("multiaccount.getcurrentaddress", [](const Json::Value& params) -> Json::Value {
            return handlers_->getCurrentAddress(params);
        });
        
        server.registerMethod("multiaccount.validateaddress", [](const Json::Value& params) -> Json::Value {
            return handlers_->validateAddress(params);
        });
        
        // Transaction Management Methods
        server.registerMethod("multiaccount.send", [](const Json::Value& params) -> Json::Value {
            return handlers_->sendTransaction(params);
        });
        
        server.registerMethod("multiaccount.history", [](const Json::Value& params) -> Json::Value {
            return handlers_->getTransactionHistory(params);
        });
        
        server.registerMethod("multiaccount.balance", [](const Json::Value& params) -> Json::Value {
            return handlers_->getAccountBalance(params);
        });
        
        server.registerMethod("multiaccount.totalbalance", [](const Json::Value& params) -> Json::Value {
            return handlers_->getTotalBalance(params);
        });
        
        // Account Settings Methods
        server.registerMethod("multiaccount.getsettings", [](const Json::Value& params) -> Json::Value {
            return handlers_->getAccountSettings(params);
        });
        
        server.registerMethod("multiaccount.setsettings", [](const Json::Value& params) -> Json::Value {
            return handlers_->setAccountSettings(params);
        });
        
        server.registerMethod("multiaccount.gettypes", [](const Json::Value& params) -> Json::Value {
            return handlers_->getAccountTypes(params);
        });
        
        server.registerMethod("multiaccount.getcolors", [](const Json::Value& params) -> Json::Value {
            return handlers_->getAccountColors(params);
        });
        
        server.registerMethod("multiaccount.geticons", [](const Json::Value& params) -> Json::Value {
            return handlers_->getAccountIcons(params);
        });
        
        // Backup and Recovery Methods
        server.registerMethod("multiaccount.export", [](const Json::Value& params) -> Json::Value {
            return handlers_->exportAccount(params);
        });
        
        server.registerMethod("multiaccount.exportall", [](const Json::Value& params) -> Json::Value {
            return handlers_->exportAllAccounts(params);
        });
        
        server.registerMethod("multiaccount.import", [](const Json::Value& params) -> Json::Value {
            return handlers_->importAccount(params);
        });
        
        server.registerMethod("multiaccount.importall", [](const Json::Value& params) -> Json::Value {
            return handlers_->importAllAccounts(params);
        });
        
        server.registerMethod("multiaccount.getmnemonic", [](const Json::Value& params) -> Json::Value {
            return handlers_->getAccountMnemonic(params);
        });
        
        // Utility Methods
        server.registerMethod("multiaccount.current", [](const Json::Value& params) -> Json::Value {
            return handlers_->getCurrentAccount(params);
        });
        
        server.registerMethod("multiaccount.count", [](const Json::Value& params) -> Json::Value {
            return handlers_->getAccountCount(params);
        });
        
        server.registerMethod("multiaccount.statistics", [](const Json::Value& params) -> Json::Value {
            return handlers_->getAccountStatistics(params);
        });
        
        server.registerMethod("multiaccount.isactive", [](const Json::Value& params) -> Json::Value {
            return handlers_->isAccountActive(params);
        });
        
        server.registerMethod("multiaccount.ishidden", [](const Json::Value& params) -> Json::Value {
            return handlers_->isAccountHidden(params);
        });
        
        LOG_I("✅ Multi-Account RPC methods registered successfully");
        LOG_I("📋 Available methods:");
        LOG_I("   Account Management: multiaccount.create, multiaccount.delete, multiaccount.restore, multiaccount.switch, multiaccount.rename, multiaccount.list, multiaccount.info");
        LOG_I("   Address Management: multiaccount.getnewaddress, multiaccount.getaddressat, multiaccount.listaddresses, multiaccount.getcurrentaddress, multiaccount.validateaddress");
        LOG_I("   Transaction Management: multiaccount.send, multiaccount.history, multiaccount.balance, multiaccount.totalbalance");
        LOG_I("   Account Settings: multiaccount.getsettings, multiaccount.setsettings, multiaccount.gettypes, multiaccount.getcolors, multiaccount.geticons");
        LOG_I("   Backup & Recovery: multiaccount.export, multiaccount.exportall, multiaccount.import, multiaccount.importall, multiaccount.getmnemonic");
        LOG_I("   Utility Functions: multiaccount.current, multiaccount.count, multiaccount.statistics, multiaccount.isactive, multiaccount.ishidden");
        
    } catch (const std::exception& e) {
        LOG_E("Failed to register Multi-Account RPC methods: " + std::string(e.what()));
        throw;
    }
}

} // namespace rpc
} // namespace dinero
