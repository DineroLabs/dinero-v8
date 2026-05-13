#pragma once

#include "rpc/rpc_types.h"
#include "json/json.h"

// Use Json::Value directly for now
namespace din {
    using Json = Json::Value;
}

namespace dinero {
    class WalletManager;
}

namespace dinero::rpc {

// Wallet vault management RPCs
din::Json RpcWalletLock(dinero::WalletManager* wallet_manager);
din::Json RpcWalletUnlock(const din::Json& params, dinero::WalletManager* wallet_manager);
din::Json RpcWalletChangePassphrase(const din::Json& params, dinero::WalletManager* wallet_manager);
din::Json RpcWalletListAddresses(dinero::WalletManager* wallet_manager);

} // namespace dinero::rpc
