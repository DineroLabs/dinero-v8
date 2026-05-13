#pragma once

#ifdef QT_CORE_LIB
#include <QString>
#include <QMap>
#else
#include <string>
#include <map>
#endif
#ifdef QT_CORE_LIB
#include <QHash>
#endif

/**
 * @brief Unified RPC alias map combining wallet and node methods
 */
#ifdef QT_CORE_LIB
inline const QHash<QString, QString>& rpcAliasMap() {
    static const QHash<QString, QString> m {
#else
inline const std::map<std::string, std::string>& rpcAliasMap() {
    static const std::map<std::string, std::string> m {
#endif
        // Wallet info methods
        {"getwalletinfo",     "wallet.info"},
        {"walletinfo",        "wallet.info"},
        
        // Address methods
        {"getnewaddress",     "wallet.getnewaddress"},
        {"validateaddress",   "wallet.validateaddress"},
        {"listaddresses",     "wallet.listaddresses"},
        {"getaddressinfo",    "wallet.getaddressinfo"},
        
        // Transaction methods
        {"listtransactions",  "wallet.listtransactions"},
        {"sendtoaddress",     "wallet.sendtoaddress"},
        {"sendmany",          "wallet.sendmany"},
        {"gettransaction",    "wallet.gettransaction"},
        
        // Balance methods
        {"getbalance",        "wallet.getbalance"},
        {"getunconfirmedbalance", "wallet.getunconfirmedbalance"},
        
        // Wallet management
        {"encryptwallet",     "wallet.encryptwallet"},
        {"walletpassphrase",  "wallet.walletpassphrase"},
        {"walletpassphrasechange", "wallet.walletpassphrasechange"},
        {"walletlock",        "wallet.walletlock"},
        
        // Backup and restore
        {"backupwallet",      "wallet.backupwallet"},
        {"importwallet",      "wallet.importwallet"},
        {"dumpwallet",        "wallet.dumpwallet"},
        
        // UTXO methods
        {"listunspent",       "wallet.listunspent"},
        {"lockunspent",       "wallet.lockunspent"},
        {"listlockunspent",   "wallet.listlockunspent"},
        
        // Blockchain info
        {"getblockchaininfo",  "blockchain.info"},
        {"getbestblockhash",   "blockchain.getbestblockhash"},
        {"getblockcount",      "blockchain.getblockcount"},
        {"getblock",           "blockchain.getblock"},
        {"getblockhash",       "blockchain.getblockhash"},
        
        // Network info
        {"getnetworkinfo",     "network.info"},
        {"getpeerinfo",        "network.getpeerinfo"},
        {"getconnectioncount", "network.getconnectioncount"},
        
        // Mining info
        {"getmininginfo",      "mining.info"},
        {"getdifficulty",      "mining.getdifficulty"},
        
        // Raw transactions (node-level)
        {"getrawtransaction",  "blockchain.getrawtransaction"},
        {"sendrawtransaction", "blockchain.sendrawtransaction"},
        {"decoderawtransaction", "blockchain.decoderawtransaction"}
    };
    return m;
}

// Legacy compatibility shims
#ifdef QT_CORE_LIB
inline const QHash<QString, QString>& walletAliasMap() { return rpcAliasMap(); }
inline const QHash<QString, QString>& nodeAliasMap()   { return rpcAliasMap(); }
#else
inline const std::map<std::string, std::string>& walletAliasMap() { return rpcAliasMap(); }
inline const std::map<std::string, std::string>& nodeAliasMap()   { return rpcAliasMap(); }
#endif
