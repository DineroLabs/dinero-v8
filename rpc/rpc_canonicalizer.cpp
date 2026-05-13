#include "rpc/rpc_canonicalizer.h"
#include <regex>

std::string canonicalizeMethod(const std::string& path,
                               const std::string& method,
                               RpcContext& ctx)
{
    ctx.path = path;
    ctx.originalMethod = method;
    ctx.walletName.clear();

    static const std::regex walletPathRe("^/wallet/([^/?#]+)");
    std::smatch match;
    if (std::regex_search(path, match, walletPathRe)) {
        ctx.walletName = match[1].str(); // Extract wallet name from regex match
    }

    // If already namespaced, pass through.
    if (method.find('.') != std::string::npos) {
        return method;
    }

    // If request is explicitly wallet-scoped by path and method is bare,
    // assume wallet namespace.
    if (!match.empty()) {
        return "wallet." + method;
    }

    // Common legacy method mappings
    if (method == "getnewaddress" || method == "getbalance" || method == "sendtoaddress" ||
        method == "listtransactions" || method == "listunspent" || method == "createwallet" ||
        method == "loadwallet" || method == "unloadwallet") {
        return "wallet." + method;
    }
    
    if (method == "getblockcount" || method == "getbestblockhash" || method == "getblock" ||
        method == "getblockheader" || method == "getchaintips" || method == "getdifficulty") {
        return "blockchain." + method;
    }
    
    if (method == "getnetworkinfo" || method == "getpeerinfo" || method == "addnode" ||
        method == "disconnectnode" || method == "getconnectioncount") {
        return "network." + method;
    }
    
    if (method == "generate" || method == "generatetoaddress" || method == "getmininginfo" ||
        method == "setgenerate" || method == "gethashmeter") {
        return "mining." + method;
    }

    // Otherwise leave as-is; dispatcher/registry will return -32601 if unknown.
    return method;
}

std::string validateWalletContext(const std::string& canonicalMethod,
                                  const RpcContext& ctx,
                                  bool hasActiveWallet)
{
    if (canonicalMethod.find("wallet.") != 0) {
        // Non-wallet method, no validation needed
        return std::string();
    }
    
    if (!ctx.walletName.empty()) {
        // Explicit wallet name provided via /wallet/<name> path
        return std::string();
    }
    
    if (hasActiveWallet) {
        // Daemon has a default/active wallet to use
        return std::string();
    }
    
    // Wallet method called without context and no active wallet
    return std::string("Wallet context required. Use /wallet/<name> path or ensure a default wallet is loaded.");
}

std::string createMethodNotFoundError(const std::string& originalMethod,
                                      const std::vector<std::string>& availableMethods)
{
    std::string baseMsg = "Method not found: " + originalMethod;
    
    // Add helpful suggestions
    if (originalMethod.find("wallet") != std::string::npos || originalMethod.find("address") != std::string::npos || 
        originalMethod.find("balance") != std::string::npos || originalMethod.find("transaction") != std::string::npos) {
        baseMsg += " (try wallet.* methods or use /wallet/<name> path)";
    } else if (originalMethod.find("block") != std::string::npos || originalMethod.find("chain") != std::string::npos) {
        baseMsg += " (try blockchain.* methods)";
    } else if (originalMethod.find("network") != std::string::npos || originalMethod.find("peer") != std::string::npos) {
        baseMsg += " (try network.* methods)";
    } else if (originalMethod.find("mining") != std::string::npos || originalMethod.find("generate") != std::string::npos) {
        baseMsg += " (try mining.* methods)";
    }
    
    return baseMsg;
}
