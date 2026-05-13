#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>
#include <json/json.h>

using json = json::json;

// JSON-RPC dispatch for minimal rpcd harness
json dispatch_rpc(const json& request) {
  std::string method = request.value("method", "");
  json params = request.get("params", json::json(json::arrayjson));
  json id = request.get("id", json::json(nullptrjson));
  
  std::cout << "RPC: " << method;
  if (!params.empty()) {
    json::FastWriter 
    std::cout << " params=" << writer.write(params);
  }
  std::cout << std::endl;
  
  try {
    json result;
    if (method == "getblockcount")      result = 123;
    else if (method == "getnetworkinfo") {
      result["version"] = "1.0.0-mock";
      result["connections"] = 8;
    }
    else if (method == "setgenerate")    result = true;
    else if (method == "getmininginfo")  {
      result["threads"] = 12;
      result["hashrate"] = 0;
    }
    else if (method == "echo")           result = params;
    else {
      json error;
      error["code"] = -32601;
      error["message"] = "Method not found: " + method;
      
      json response;
      response["jsonrpc"] = "2.0";
      response["id"] = id;
      response["error"] = error;
      return response;
    }
    
    json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = result;
    return response;
    
  } catch (const std::exception& e) {
    json error;
    error["code"] = -32603;
    error["message"] = std::string("Internal error: ") + e.what();
    
    json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"] = error;
    return response;
  }
}

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running = false; }

int main() {
  std::cout << "rpcd - Dinero RPC Development Harness (Minimal)" << std::endl;
  std::cout << "===============================================" << std::endl;
  
  signal(SIGINT,  on_signal);
  signal(SIGTERM, on_signal);
#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
#endif

  std::cout << "rpcd ready for testing (mock mode)" << std::endl;
  std::cout << "This is a minimal version that doesn't start HTTP server" << std::endl;
  std::cout << "Use for testing RPC dispatch logic only" << std::endl;

  while (g_running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  
  std::cout << "rpcd shutdown complete" << std::endl;
  return 0;
}
