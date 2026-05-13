#pragma once
#include <functional>
#include <string>
#include <json/json.h>

namespace dinero::rpc {

struct RpcConfig {
  std::string bind_ip = "127.0.0.1";
  int port = 20998;
  bool require_auth = true;
  int rate = 2;
  int burst = 2;
  bool local_bypass = false;
  std::string cookie_path; // empty = default
};

// JsonCpp helper functions
inline Json::Value json_null() { return Json::Value(); }
inline std::string get_string(const Json::Value& v, const char* k, std::string d="") {
  return v.isMember(k) && v[k].isString() ? v[k].asString() : std::move(d);
}

// Called per HTTP POST / JSON-RPC request
using RpcHandler = std::function<Json::Value(const Json::Value&)>;

struct HttpRpcServer; // opaque

// Start/stop HTTP JSON-RPC server. No WebSocket side-effects.
HttpRpcServer* start_http_rpc(const RpcConfig& cfg, RpcHandler handler);
void stop_http_rpc(HttpRpcServer*);

} // namespace dinero::rpc
