#include "rpc_ws.hpp"
#include <iostream>

namespace dinero::rpc {

struct WsServer {
    // Placeholder for WebSocket server implementation
    // This will be populated with the actual WebSocket code
};

WsServer* start_ws_rpc(WsConfig cfg) {
    std::cout << "WebSocket RPC server starting on path: " << cfg.path << std::endl;
    // TODO: Implement WebSocket server startup
    return new WsServer();
}

void stop_ws_rpc(WsServer* server) {
    if (server) {
        std::cout << "WebSocket RPC server stopping" << std::endl;
        // TODO: Implement WebSocket server shutdown
        delete server;
    }
}

} // namespace dinero::rpc
