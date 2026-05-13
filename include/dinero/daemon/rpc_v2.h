#pragma once

namespace dinero {

// Forward declarations
class RPCServer;

namespace rpcv2 {

struct Context {
    RPCServer* rpc_server;
    void* blockchain;
    void* mining;
    void* mempool;
};

void RegisterV2RPC(RPCServer& server, const Context& ctx);

} // namespace rpcv2
} // namespace dinero
