#pragma once
#include "daemon/rpc/multi_account_rpc_handlers.h"
#include "daemon/rpc_server.h"

namespace dinero {
namespace rpc {

/**
 * Multi-Account RPC Registration
 * 
 * Registers all multi-account RPC methods with the RPC server
 */
class MultiAccountRpcRegistration {
public:
    static void registerMultiAccountMethods(RPCServer& server);
    
private:
    static std::unique_ptr<MultiAccountRpcHandlers> handlers_;
};

} // namespace rpc
} // namespace dinero
