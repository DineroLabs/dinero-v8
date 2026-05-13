#pragma once
#include <string>

namespace dinero::rpc {

struct WsConfig {
  std::string path = "/rpc.ws";
  bool require_auth = true;
};

struct WsServer; // opaque

// Start/stop WebSocket RPC server
WsServer* start_ws_rpc(WsConfig cfg);
void stop_ws_rpc(WsServer*);

} // namespace dinero::rpc
