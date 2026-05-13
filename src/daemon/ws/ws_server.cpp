#include "ws/ws_server.h"
#include "ws/ws_http_session.h"
#include "ws/ws_rate_limiter.h"
#include "daemon/auth_cookie.h"
#include "daemon/ws_globals.h"  // For g_subscriptions
#include "events/event_sink.h"
#include "version_config.h"
#include "rpc/ws_adapter_registry.h"  // For event bus client registration
#include "rpc/rpc_registry.h"   // For RPC registry fallback
#include "din_json.h"            // For din::Json
#include "common/logger.h"       // For structured logging
#include <json/json.h>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/http.hpp>
#include <memory>
#include <string>
#include <utility>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <mutex>
#include <atomic>
#include <sstream>

using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;

namespace dinero {

// Forward declaration for global session lookup
class WsSession;

// Global registry of active WebSocket sessions for drain mechanism
static std::mutex g_sessions_mutex;
static std::unordered_map<int, std::shared_ptr<WsSession>> g_active_sessions;
static std::atomic<bool> g_shutdown_in_progress{false};

// Global rate limiter for all WebSocket connections (P1 security hardening)
static ws::RateLimiter g_ws_rate_limiter;

// Dev mode flag (set from main.cpp via command line)
extern bool g_dev_mode;

} // namespace dinero

// Global RPC registry (in global namespace, not dinero namespace)
extern RpcRegistry g_rpcRegistry;

namespace dinero {

// Phase 2.3: WebSocket session with full JSON-RPC subscription protocol
class WsSession : public std::enable_shared_from_this<WsSession> {
public:
  WsSession(tcp::socket&& socket)
    : ws_(std::move(socket)),
      drain_timer_(ws_.get_executor()),
      fd_(-1) {}

  ~WsSession() {
    // Unregister from g_subscriptions on destruction
    if (fd_ != -1) {
      // Only perform cleanup if we're NOT in shutdown
      // During shutdown, mutex and global objects may be destroyed
      if (!g_shutdown_in_progress.load(std::memory_order_acquire)) {
        // Safe to unregister during normal operation
        if (g_subscriptions) {
          g_subscriptions->remove_connection(fd_);
        }

        // P1 Security: Clean up rate limiter state
        g_ws_rate_limiter.RemoveConnection(fd_);

        // Unregister from event bus client registry
        dinero::rpc::ws_adapter_unregister_client(fd_);

        // Safe to lock and erase during normal operation
        try {
          std::lock_guard<std::mutex> lock(g_sessions_mutex);
          g_active_sessions.erase(fd_);
        } catch (const std::system_error& e) {
          // Mutex destroyed during shutdown - this is OK, skip cleanup
        }

        std::cout << "[WsSession] Connection " << fd_ << " closed and unregistered" << std::endl;
      }
      // else: Shutdown in progress, skip cleanup to avoid mutex/global access
    }
  }

  // Run with HTTP request (called after authentication)
  template<class Body, class Allocator>
  void run(http::request<Body, http::basic_fields<Allocator>> req) {
    // Get file descriptor for registration
    fd_ = ws_.next_layer().native_handle();

    // Register this session globally for drain mechanism
    {
      std::lock_guard<std::mutex> lock(g_sessions_mutex);
      g_active_sessions[fd_] = shared_from_this();
    }

    // Register with g_subscriptions
    g_subscriptions->add_connection(fd_);

    // Register with event bus client registry
    // Generate a client ID from the file descriptor
    std::string client_id = "ws_" + std::to_string(fd_);
    dinero::rpc::ws_adapter_register_client(client_id, fd_);

    std::cout << "[WsSession] New authenticated connection registered with fd=" << fd_ << " (client_id: " << client_id << ")" << std::endl;

    // Set WebSocket limits and backpressure
    ws_.read_message_max(1 << 20); // 1MB max message size
    ws_.set_option(websocket::stream_base::timeout::suggested(
        boost::beast::role_type::server));
    ws_.set_option(websocket::stream_base::decorator(
      [](websocket::response_type& res) {
        res.set(http::field::server, std::string("dinero-ws/") + DINERO_CLI_GIT_SHA);
      }));

    // Accept WebSocket connection WITH the HTTP request
    // This is the key change - we pass the request to async_accept
    ws_.async_accept(req, [self=shared_from_this()](boost::system::error_code ec){
      if(ec) {
        std::cerr << "[WsSession] Accept error: " << ec.message() << std::endl;
        return;
      }
      self->do_read();
      self->start_drain_timer();  // Start periodic event draining
    });
  }

  // Legacy run without request (for backwards compatibility during migration)
  void run() {
    // Get file descriptor for registration
    fd_ = ws_.next_layer().native_handle();

    // Register this session globally for drain mechanism
    {
      std::lock_guard<std::mutex> lock(g_sessions_mutex);
      g_active_sessions[fd_] = shared_from_this();
    }

    // Register with g_subscriptions
    g_subscriptions->add_connection(fd_);
    std::cout << "[WsSession] New connection registered with fd=" << fd_ << std::endl;

    // Set WebSocket limits and backpressure
    ws_.read_message_max(1 << 20); // 1MB max message size
    ws_.set_option(websocket::stream_base::timeout::suggested(
        boost::beast::role_type::server));
    ws_.set_option(websocket::stream_base::decorator(
      [](websocket::response_type& res) {
        res.set(http::field::server, std::string("dinero-ws/") + DINERO_CLI_GIT_SHA);
      }));

    // Accept WebSocket connection
    ws_.async_accept([self=shared_from_this()](boost::system::error_code ec){
      if(ec) {
        std::cerr << "[WsSession] Accept error: " << ec.message() << std::endl;
        return;
      }
      self->do_read();
      self->start_drain_timer();  // Start periodic event draining
    });
  }

  // Send a message to this WebSocket client
  bool send(const std::string& msg) {
    auto self = shared_from_this();
    try {
      ws_.text(true);
      ws_.async_write(boost::asio::buffer(msg),
        [self, msg](boost::system::error_code ec, std::size_t){
          if (ec) {
            std::cerr << "[WsSession fd=" << self->fd_ << "] Write error: " << ec.message() << std::endl;
          }
        });
      return true;
    } catch (const std::exception& e) {
      std::cerr << "[WsSession fd=" << fd_ << "] Send exception: " << e.what() << std::endl;
      return false;
    }
  }

private:
  void do_read() {
    auto self = shared_from_this();
    ws_.async_read(buf_, [self](boost::system::error_code ec, std::size_t){
      if(ec) {
        if (ec != websocket::error::closed) {
          std::cerr << "[WsSession fd=" << self->fd_ << "] Read error: " << ec.message() << std::endl;
        }
        return;
      }
      self->handle_message();
      self->buf_.consume(self->buf_.size());
      self->do_read();
    });
  }

  // Phase 2.3: Full JSON-RPC subscription protocol handler
  void handle_message() {
    // P1 Security: Rate limiting check (before processing message)
    if (!g_ws_rate_limiter.AllowMessage(fd_)) {
      std::cerr << "[WsSession fd=" << fd_ << "] Rate limit exceeded, dropping message" << std::endl;
      // Optionally send error response
      send_error(Json::Value(Json::nullValue), -32000, "Rate limit exceeded");
      return;
    }

    std::string msg = boost::beast::buffers_to_string(buf_.data());

    try {
      // Parse JSON-RPC request
      Json::Value req;
      Json::Reader reader;
      if (!reader.parse(msg, req)) {
        send_error(Json::Value(Json::nullValue), -32700, "Parse error");
        return;
      }

      // Extract fields
      std::string method = req.isMember("method") && req["method"].isString() ? req["method"].asString() : "";
      Json::Value id = req.get("id", Json::Value(Json::nullValue));

      if (method == "subscribe") {
        handle_subscribe(req, id);
      } else if (method == "unsubscribe") {
        handle_unsubscribe(req, id);
      } else if (method == "ping") {
        handle_ping(req, id);
      } else {
        // Fallback to RPC registry for other methods (e.g., ws_subscribe, ws_unsubscribe, etc.)
        handle_rpc_method(method, req, id);
      }

    } catch (const std::exception& e) {
      std::cerr << "[WsSession fd=" << fd_ << "] Parse error: " << e.what() << std::endl;
      send_error(Json::Value(Json::nullValue), -32700, "Parse error");
    }
  }

  void handle_subscribe(const Json::Value& req, const Json::Value& id) {
    try {
      Json::Value params = req.get("params", Json::Value());
      if (!params.isObject()) {
        send_error(id, -32602, "Invalid params: expected object");
        return;
      }

      std::string topic = params.get("topic", "").asString();
      if (topic.empty()) {
        send_error(id, -32602, "Missing required parameter: topic");
        return;
      }

      // Subscribe to the topic
      g_subscriptions->subscribe(fd_, topic);

      std::cout << "[WsSession fd=" << fd_ << "] Subscribed to topic: " << topic << std::endl;

      // Send initial sync state for syncProgress subscription
      if (topic == "syncProgress") {
        #if DIN_WS_BROADCAST
        // Broadcast initial state (assume synced; will be corrected on next block if syncing)
        // This ensures GUI shows sync status immediately instead of staying on "Checking..."
        extern void BroadcastSyncProgress(bool ibd, double progress, int eta_s);
        BroadcastSyncProgress(false, 1.0, 0);
        #endif
      }

      // Send success response
      Json::Value response;
      response["jsonrpc"] = "2.0";
      response["id"] = id;
      response["result"]["subscribed"] = true;
      response["result"]["topic"] = topic;

      send_response(response);

    } catch (const std::exception& e) {
      send_error(id, -32603, std::string("Internal error: ") + e.what());
    }
  }

  void handle_unsubscribe(const Json::Value& req, const Json::Value& id) {
    try {
      Json::Value params = req.get("params", Json::Value());
      if (!params.isObject()) {
        send_error(id, -32602, "Invalid params: expected object");
        return;
      }

      std::string topic = params.get("topic", "").asString();
      if (topic.empty()) {
        send_error(id, -32602, "Missing required parameter: topic");
        return;
      }

      // Unsubscribe from the topic
      g_subscriptions->unsubscribe(fd_, topic);

      std::cout << "[WsSession fd=" << fd_ << "] Unsubscribed from topic: " << topic << std::endl;

      // Send success response
      Json::Value response;
      response["jsonrpc"] = "2.0";
      response["id"] = id;
      response["result"]["unsubscribed"] = true;
      response["result"]["topic"] = topic;

      send_response(response);

    } catch (const std::exception& e) {
      send_error(id, -32603, std::string("Internal error: ") + e.what());
    }
  }

  void handle_ping(const Json::Value& req, const Json::Value& id) {
    Json::Value response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = "pong";
    send_response(response);
  }

  // Handle RPC methods via global RPC registry
  void handle_rpc_method(const std::string& method, const Json::Value& req, const Json::Value& id) {
    try {
      // Look up handler in RPC registry
      RpcHandler* handler_ptr = g_rpcRegistry.lookup(method);
      if (!handler_ptr) {
        dinero::g_logger.error("[WS-RPC] Method not found: " + method + " (fd=" + std::to_string(fd_) + ")");
        send_error(id, -32601, "Method not found: " + method);
        return;
      }

      // Create execution context with client_id
      ExecutionContext ctx;
      ctx.client_id = dinero::rpc::ws_adapter_get_client_id(fd_);

      // Get params (default to empty object if not provided)
      Json::Value params = req.get("params", Json::Value(Json::objectValue));

      // Structured logging
      Json::FastWriter compact;
      std::string params_str = compact.write(params);
      if (params_str.length() > 100) {
        params_str = params_str.substr(0, 97) + "...";
      }
      dinero::g_logger.info("[WS-RPC] client=" + ctx.client_id +
                           " method=" + method +
                           " params=" + params_str);

      // Convert to din::Json format
      din::Json vnext_params = params;  // din::Json is typedef for Json::Value

      // Call handler
      auto start = std::chrono::steady_clock::now();
      din::Json result = (*handler_ptr)(ctx, vnext_params);
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start
      ).count();

      // Log completion time for slow operations
      if (duration > 100) {
        dinero::g_logger.info("[WS-RPC] client=" + ctx.client_id +
                             " method=" + method +
                             " duration=" + std::to_string(duration) + "ms");
      }

      // Send response
      Json::Value response;
      response["jsonrpc"] = "2.0";
      response["id"] = id;
      response["result"] = result;  // din::Json converts back to Json::Value
      send_response(response);

    } catch (const std::exception& e) {
      dinero::g_logger.error("[WS-RPC] Error in method " + method + ": " + e.what() +
                            " (fd=" + std::to_string(fd_) + ")");
      send_error(id, -32603, std::string("Internal error: ") + e.what());
    }
  }

  void send_response(const Json::Value& response) {
    Json::FastWriter writer;
    std::string json = writer.write(response);
    send(json);
  }

  void send_error(const Json::Value& id, int code, const std::string& message) {
    Json::Value response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"]["code"] = code;
    response["error"]["message"] = message;
    send_response(response);
  }

  // Phase 2.3: Periodic drain timer to deliver queued events
  void start_drain_timer() {
    drain_timer_.expires_after(std::chrono::milliseconds(100));  // 10 Hz drain rate
    auto self = shared_from_this();
    drain_timer_.async_wait([self](boost::system::error_code ec) {
      if (!ec) {
        // Call global drain mechanism
        g_subscriptions->drain_once();
        // Reschedule
        self->start_drain_timer();
      }
    });
  }

  websocket::stream<tcp::socket> ws_;
  boost::beast::flat_buffer buf_;
  boost::asio::steady_timer drain_timer_;
  int fd_;  // File descriptor for registration with g_subscriptions
};

// Helper function for ws_http_session.cpp to create WsSession with request
// This avoids template instantiation issues across translation units
std::shared_ptr<WsSession> create_ws_session_with_request(
    tcp::socket&& socket,
    http::request<http::empty_body>&& req)
{
    auto session = std::make_shared<WsSession>(std::move(socket));
    session->run(std::move(req));
    return session;
}

} // namespace dinero

// Phase 2.3: Implement ws_send_text for g_subscriptions drain mechanism
// This function is called by g_subscriptions.drain_once()
// NOTE: This is in global namespace to match declaration in ws_subscriptions.hpp
bool ws_send_text(int fd, const std::string& s) {
  std::lock_guard<std::mutex> lock(dinero::g_sessions_mutex);
  auto it = dinero::g_active_sessions.find(fd);
  if (it == dinero::g_active_sessions.end()) {
    return false;  // Session not found (may have disconnected)
  }
  return it->second->send(s);
}

namespace dinero {

class WsServer::Listener : public std::enable_shared_from_this<Listener> {
public:
  Listener(boost::asio::io_context& ioc, tcp::endpoint ep, const std::string& cookie_path)
    : ioc_(ioc), acceptor_(ioc), effective_port_(0), cookie_path_(cookie_path) {
    boost::system::error_code ec;
    acceptor_.open(ep.protocol(), ec);
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
    acceptor_.bind(ep, ec);
    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);

    // Capture the effective port after bind (crucial for ephemeral ports)
    if (!ec) {
      effective_port_ = acceptor_.local_endpoint(ec).port();
    }
  }

  void run() { do_accept(); }
  void stop() {
    boost::system::error_code ec; acceptor_.close(ec);
  }

  unsigned short effective_port() const { return effective_port_; }

private:
  void do_accept() {
    acceptor_.async_accept([self=shared_from_this()](boost::system::error_code ec, tcp::socket s){
      if(!ec) {
        // Create HTTP session for authentication before WebSocket upgrade
        auto http_session = std::make_shared<WsHttpSession>(std::move(s), self->cookie_path_);
        http_session->run();
      }
      if(self->acceptor_.is_open()) self->do_accept();
    });
  }

  boost::asio::io_context& ioc_;
  tcp::acceptor acceptor_;
  unsigned short effective_port_;
  std::string cookie_path_;
};

WsServer::WsServer(boost::asio::io_context& ioc,
                   const std::string& ip, unsigned short port,
                   const std::string& cookie_path)
  : listener_(std::make_shared<Listener>(ioc, tcp::endpoint(boost::asio::ip::make_address(ip), port), cookie_path)) {}

void WsServer::run()  { listener_->run(); }
void WsServer::stop() {
  // Signal shutdown to prevent destructors from accessing destroyed globals
  g_shutdown_in_progress.store(true, std::memory_order_release);
  listener_->stop();
}

unsigned short WsServer::effective_port() const {
  return listener_ ? listener_->effective_port() : 0;
}

} // namespace dinero
