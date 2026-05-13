#include "daemon/ws_connection.hpp"
#include "daemon/daemon_context.h"
#include "rpc/rpc_registry.h"
#include "daemon/auth_cookie.h"
#include "daemon/services/config_service.h"
#include "daemon/rpc_authcookie.h"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include "compat/jsoncpp_compat.h"
#include <string>
#include <unordered_map>

using tcp = boost::asio::ip::tcp;
namespace websocket = boost::beast::websocket;

namespace dinero {

class WsSession : public std::enable_shared_from_this<WsSession> {
public:
  WsSession(tcp::socket&& socket, dinero::RpcRegistry& registry)
    : ws_(std::move(socket)), registry_(registry) {}

  void run() {
    ws_.set_option(websocket::stream_base::timeout::suggested(
        boost::beast::role_type::server));
    ws_.set_option(websocket::stream_base::decorator(
      [](websocket::response_type& res) {
        res.set(boost::beast::http::field::server, "dinero-ws");
      }));
    
    // Accept WebSocket upgrade with path validation
    ws_.async_accept([self=shared_from_this()](boost::system::error_code ec){
      if(ec) return;
      
      // Note: Path validation would be done during HTTP upgrade
      // For now, we assume the HTTP server has already validated the path
      
      self->do_read();
    });
  }

  void handle_message();

private:
  void do_read() {
    auto self = shared_from_this();
    ws_.async_read(buf_, [self](boost::system::error_code ec, std::size_t){
      if(ec) return;
      self->handle_message();
      self->buf_.consume(self->buf_.size());
      self->do_read();
    });
  }

  void write(std::string s) {
    auto self = shared_from_this();
    ws_.text(true);
    ws_.async_write(boost::asio::buffer(std::move(s)),
                    [self](boost::system::error_code, std::size_t){});
  }

  websocket::stream<tcp::socket> ws_;
  boost::beast::flat_buffer buf_;
  dinero::RpcRegistry& registry_;
};

// Simple RPC adapter - connects Beast WS to vNext RpcRegistry
std::string rpc_handle_text(dinero::RpcRegistry& registry, const std::string& payload, 
                           const std::string& wallet_name, const std::string& auth_header) {
    try {
        // Parse JSON-RPC request
        Json::Value request;
        Json::Reader reader;
        if (!reader.parse(payload, request)) {
            Json::Value error_response(Json::objectValue);
            error_response["jsonrpc"] = "2.0";
            error_response["id"] = Json::Value(Json::nullValue);
            Json::Value error_obj(Json::objectValue);
            error_obj["code"] = -32700;
            error_obj["message"] = "Parse error";
            error_response["error"] = error_obj;
            
            Json::FastWriter writer;
            return writer.write(error_response);
        }
        
        // Week 7: Proper auth validation using cookie-based authentication
        // Extract Authorization header from WebSocket upgrade request
        std::unordered_map<std::string, std::string> headers_lowercased;
        // Note: Headers are already extracted in HTTP upgrade phase
        // For WebSocket, we validate the auth_header passed from HTTP upgrade
        
        // Use the same cookie-based auth as HTTP RPC
        std::string cookie_path;
        if (auto* daemon_ctx = dinero::DaemonContext::instance(); daemon_ctx && daemon_ctx->config) {
            auto config = std::dynamic_pointer_cast<dinero::ConfigService>(daemon_ctx->config);
            if (config) {
                cookie_path = GetAuthCookiePath(
                    config->DataDir(),
                    config->GetString("rpccookiefile", "")
                );
            }
        }
        if (cookie_path.empty()) {
            Json::Value error_response(Json::objectValue);
            error_response["jsonrpc"] = "2.0";
            error_response["id"] = Json::Value(Json::nullValue);
            Json::Value error_obj(Json::objectValue);
            error_obj["code"] = -32001;
            error_obj["message"] = "Authentication failed - RPC cookie path unavailable";
            error_response["error"] = error_obj;

            Json::FastWriter writer;
            return writer.write(error_response);
        }
        bool auth_valid = dinero::check_basic_authorization(headers_lowercased, cookie_path);
        
        // If no Authorization header in map, try parsing from auth_header string
        if (!auth_valid && !auth_header.empty()) {
            // Parse "Basic base64(...)" format
            if (auth_header.length() >= 6 && auth_header.substr(0, 6) == "Basic ") {
                headers_lowercased["authorization"] = auth_header;
                auth_valid = dinero::check_basic_authorization(headers_lowercased, cookie_path);
            }
        }
        
        if (!auth_valid) {
            // Return JSON-RPC error for authentication failure
            Json::Value error_response(Json::objectValue);
            error_response["jsonrpc"] = "2.0";
            Json::Value error_obj(Json::objectValue);
            error_obj["code"] = -32001;
            error_obj["message"] = "Authentication failed - invalid or missing Authorization header";
            error_response["error"] = error_obj;
            error_response["id"] = Json::Value(Json::nullValue);
            
            Json::FastWriter writer;
            return writer.write(error_response);
        }
        
        ExecutionContext ctx;
        ctx.client_address = "127.0.0.1";
        ctx.auth_header = auth_header;
        
        // Process request via vNext registry
        auto result = registry.handleRequest(ctx, request);
        
        Json::FastWriter writer;
        return writer.write(result);
        
    } catch (const std::exception& e) {
        // Return JSON-RPC error response
        Json::Value error_response(Json::objectValue);
        error_response["jsonrpc"] = "2.0";
        Json::Value error_obj(Json::objectValue);
        error_obj["code"] = -32603;
        error_obj["message"] = "Internal error: " + std::string(e.what());
        error_response["error"] = error_obj;
        error_response["id"] = Json::Value(Json::nullValue);
        
        Json::FastWriter writer;
        return writer.write(error_response);
    }
}

// Implementation of WsSession::handle_message() from ws_server.cpp
void WsSession::handle_message() {
  std::string text = boost::beast::buffers_to_string(buf_.data());

  // Strict JSON-RPC validation - reject non-JSON-RPC requests
  Json::Value rpc_request;
  Json::Reader reader;
  
  if (!reader.parse(text, rpc_request)) {
    rpc_request = Json::Value(Json::objectValue);
  }
  
  if (!rpc_request.isMember("jsonrpc")) {
    // Return JSON-RPC error for invalid request
    Json::Value error_response(Json::objectValue);
    error_response["jsonrpc"] = "2.0";
    error_response["id"] = Json::Value(Json::nullValue);
    Json::Value error_obj(Json::objectValue);
    error_obj["code"] = -32600;
    error_obj["message"] = "Invalid Request";
    error_response["error"] = error_obj;
    
    write(Json::FastWriter().write(error_response));
    return;
  }

  // Optional: extract wallet path/auth from a per-message envelope
  // Expect either raw JSON-RPC object, or:
  // { "wallet":"<name>", "auth":"__cookie__:...", "rpc": {jsonrpc:"2.0", ... } }
  std::string wallet, auth;
  std::string payload = text;
  try {
    Json::Value envelope;
    Json::Reader reader;
    if (reader.parse(text, envelope) && envelope.isMember("rpc")) {
      if (envelope.isMember("wallet")) {
        wallet = envelope.get("wallet", "").asString();
      }
      if (envelope.isMember("auth")) {
        auth = envelope.get("auth", "").asString();
      }
      Json::FastWriter writer;
      payload = writer.write(envelope["rpc"]);
    }
  } catch (...) {
    // Leave as-is; RPC layer will validate JSON-RPC
  }

  auto response = rpc_handle_text(registry_, payload, wallet, auth);
  write(response);
}

} // namespace dinero
