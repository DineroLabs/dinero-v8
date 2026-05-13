#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>
#include "rpc_http_core.hpp"
#include "rpc_dispatch.hpp"

using json = json::json;

// JSON-RPC dispatch for rpcd harness
json dispatch_rpc(const json& request) {
  std::string method = request.value("method", "");
  json params = request.value("params", json::array());
  json id = request.value("id", json(nullptr));
  
  std::cout << "RPC: " << method;
  if (!params.empty()) std::cout << " params=" << paramsjson::FastWriter().write();
  std::cout << std::endl;
  
  try {
    json result;
    if (method == "getblockcount")      result = 123;
    else if (method == "getnetworkinfo") result = json{{"version","1.0.0-mock"},{"connections",8}};
    else if (method == "setgenerate")    result = true;
    else if (method == "getmininginfo")  result = json{{"threads",12},{"hashrate",0}};
    else if (method == "echo")           result = params;
    else {
      return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {
          {"code", -32601},
          {"message", "Method not found: " + method}
        }}
      };
    }
    
    return {
      {"jsonrpc", "2.0"},
      {"id", id},
      {"result", result}
    };
    
  } catch (const std::exception& e) {
    return {
      {"jsonrpc", "2.0"},
      {"id", id},
      {"error", {
        {"code", -32603},
        {"message", std::string("Internal error: ") + e.what()}
      }}
    };
  }
}

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running = false; }

int main() {
  std::cout << "rpcd - Dinero RPC Development Harness" << std::endl;
  std::cout << "=======================================" << std::endl;
  
  signal(SIGINT,  on_signal);
  signal(SIGTERM, on_signal);
#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
#endif

  dinero::rpc::RpcConfig cfg;
  cfg.bind_ip = "127.0.0.1";
  cfg.port = 18332;                      // harness port
  cfg.require_auth = true;
  cfg.rate = 2; cfg.burst = 2;
  cfg.cookie_path = ""; // use default

  auto handler = [](const dinero::rpc::Json& req) -> dinero::rpc::Json {
    return dispatch_rpc(req);
  };

  auto* srv = dinero::rpc::start_http_rpc(cfg, handler);
  std::cout << "rpcd listening on " << cfg.bind_ip << ":" << cfg.port << std::endl;
  std::cout << "Ready for testing..." << std::endl;

  while (g_running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  
  dinero::rpc::stop_http_rpc(srv);
  std::cout << "rpcd shutdown complete" << std::endl;
  return 0;
}
