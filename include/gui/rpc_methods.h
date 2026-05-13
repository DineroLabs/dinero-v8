#pragma once
#include <QString>
#include <QSet>
#include <QJsonObject>
#include <QJsonArray>

// vNext-only RPC method allow-list
// This enum prevents calling legacy or unauthorized methods
enum class RpcMethod {
    // Core system
    Help,
    GetBuildInfo,
    GetNetworkInfo,
    GetMempoolInfo,
    
    // Blockchain
    GetBlockchainInfo,
    GetBlockTemplate,
    SubmitBlock,
    
    // Mining
    MiningStatus,
    MiningStop,
    GenerateToAddress,
    
    // Wallet
    WalletCreate,
    WalletLoad,
    GetNewAddress,
    WalletValidateAddress,
    
    // Transactions
    CreateRawTransaction,
    FundRawTransaction,
    SignRawTransactionWithWallet,
    FinalizePsbt,
    SendRawTransaction,
    
    // Explorer (read-only)
    GetBlock,
    GetTransaction,
    GetRawTransaction,
};

// Convert enum to string - only these methods are allowed
inline const QString& toString(RpcMethod method) {
    static const QMap<RpcMethod, QString> methodMap = {
        // Core system
        {RpcMethod::Help, "help"},
        {RpcMethod::GetBuildInfo, "getbuildinfo"},
        {RpcMethod::GetNetworkInfo, "getnetworkinfo"},
        {RpcMethod::GetMempoolInfo, "getmempoolinfo"},
        
        // Blockchain
        {RpcMethod::GetBlockchainInfo, "getblockchaininfo"},
        {RpcMethod::GetBlockTemplate, "getblocktemplate"},
        {RpcMethod::SubmitBlock, "submitblock"},
        
        // Mining
        {RpcMethod::MiningStatus, "mining.status"},
        {RpcMethod::MiningStop, "mining.stop"},
        {RpcMethod::GenerateToAddress, "generatetoaddress"},
        
        // Wallet
        {RpcMethod::WalletCreate, "wallet.create"},
        {RpcMethod::WalletLoad, "wallet.load"},
        {RpcMethod::GetNewAddress, "getnewaddress"},
        {RpcMethod::WalletValidateAddress, "wallet.validateaddress"},
        
        // Transactions
        {RpcMethod::CreateRawTransaction, "createrawtransaction"},
        {RpcMethod::FundRawTransaction, "fundrawtransaction"},
        {RpcMethod::SignRawTransactionWithWallet, "signrawtransactionwithwallet"},
        {RpcMethod::FinalizePsbt, "finalizepsbt"},
        {RpcMethod::SendRawTransaction, "sendrawtransaction"},
        
        // Explorer
        {RpcMethod::GetBlock, "getblock"},
        {RpcMethod::GetTransaction, "gettransaction"},
        {RpcMethod::GetRawTransaction, "getrawtransaction"},
    };
    
    static QString empty;
    auto it = methodMap.find(method);
    return (it != methodMap.end()) ? it.value() : empty;
}

// Get all allowed method names as a set for validation
inline QSet<QString> getAllowedMethods() {
    static QSet<QString> allowed;
    if (allowed.isEmpty()) {
        for (int i = 0; i < static_cast<int>(RpcMethod::GetRawTransaction) + 1; ++i) {
            RpcMethod method = static_cast<RpcMethod>(i);
            QString methodName = toString(method);
            if (!methodName.isEmpty()) {
                allowed.insert(methodName);
            }
        }
    }
    return allowed;
}

// Validate if a method name is in the allow-list
inline bool isMethodAllowed(const QString& methodName) {
    return getAllowedMethods().contains(methodName);
}

// RPC schema validation
struct RpcSchemaInfo {
    QString rpcSchema;
    int schemaRev;
    QString version;
    QString semver;
    bool isValid;
    
    RpcSchemaInfo() : schemaRev(0), isValid(false) {}
    
    bool isVNextCompatible() const {
        return isValid && 
               rpcSchema == "din.rpc.v1" && 
               schemaRev >= 1;
    }
};

// Parse schema info from getbuildinfo response
inline RpcSchemaInfo parseSchemaInfo(const QJsonObject& buildInfo) {
    RpcSchemaInfo info;
    
    if (buildInfo.contains("rpc_schema")) {
        info.rpcSchema = buildInfo["rpc_schema"].toString();
    }
    if (buildInfo.contains("schema_rev")) {
        info.schemaRev = buildInfo["schema_rev"].toInt();
    }
    if (buildInfo.contains("version")) {
        info.version = buildInfo["version"].toString();
    }
    if (buildInfo.contains("semver")) {
        info.semver = buildInfo["semver"].toString();
    }
    
    info.isValid = !info.rpcSchema.isEmpty() && info.schemaRev > 0;
    return info;
}
