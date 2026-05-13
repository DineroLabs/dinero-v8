#pragma once
#include <string>
#include <vector>

struct RpcContext {
    std::string originalMethod;
    std::string walletName;   // std, not QString
    std::string path;         // e.g. "/", "/wallet/main"
};

// Return canonical method like "wallet.getnewaddress"
std::string canonicalizeMethod(const std::string& path,
                               const std::string& method,
                               RpcContext& ctx);

// Return empty string if OK, else error message
std::string validateWalletContext(const std::string& canonicalMethod,
                                  const RpcContext& ctx,
                                  bool hasActiveWallet);

std::string createMethodNotFoundError(const std::string& originalMethod,
                                      const std::vector<std::string>& availableMethods);

