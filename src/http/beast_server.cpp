#include "http/beast_server.h"
#include "rpc/rpc_registry.h"
#include "../../include/log/json_logger.h"
#include "../../include/http/trace.h"
#include "din_json.h"
#include "daemon/auth_cookie.h"
#include "daemon/rpc/auth_middleware.h"
#include "daemon/access_log_policy.h"
#include "metrics/metrics_registry.h"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/config.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <unordered_map>

namespace dinero {

// HTTP session class for handling individual connections
class BeastHttpServer::HttpSession : public std::enable_shared_from_this<BeastHttpServer::HttpSession> {
public:
    HttpSession(tcp::socket&& socket, BeastHttpServer* server)
        : stream_(std::move(socket))
        , server_(server) {
    }
    
    void run() {
        net::dispatch(stream_.get_executor(),
                     beast::bind_front_handler(&HttpSession::doRead, shared_from_this()));
    }
    
private:
    void doRead() {
        req_ = {};
        stream_.expires_after(server_->request_timeout_);
        
        http::async_read(stream_, buffer_, req_,
            beast::bind_front_handler(&HttpSession::onRead, shared_from_this()));
    }
    
    void onRead(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);
        
        if (ec == http::error::end_of_stream) {
            return doClose();
        }
        
        if (ec) {
            return;
        }
        
        // Check request size limit
        if (req_.body().size() > server_->max_request_size_) {
            sendResponse(createErrorResponse(http::status::payload_too_large, 
                                           "Request body too large"));
            return;
        }
        
        // Process the request
        auto response = server_->routeRequest(req_);
        sendResponse(std::move(response));
    }
    
    void sendResponse(http::response<http::string_body>&& response) {
        auto sp = std::make_shared<http::response<http::string_body>>(std::move(response));
        res_ = sp;
        
        http::async_write(stream_, *sp,
            beast::bind_front_handler(&HttpSession::onWrite, shared_from_this(), sp->need_eof()));
    }
    
    void onWrite(bool close, beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);
        
        if (ec) {
            return;
        }
        
        if (close) {
            return doClose();
        }
        
        res_ = nullptr;
        doRead();
    }
    
    void doClose() {
        beast::error_code ec;
        stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
    }
    
    http::response<http::string_body> createErrorResponse(
        http::status status, 
        const std::string& message) {
        
        http::response<http::string_body> res{status, req_.version()};
        res.set(http::field::server, "dinerod-vnext/1.0");
        res.set(http::field::content_type, "application/json");
        res.keep_alive(req_.keep_alive());
        
        din::Json error;
        error["error"] = message;
        error["status"] = static_cast<int>(status);
        
        res.body() = din::dump(error);
        res.prepare_payload();
        
        return res;
    }
    
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    std::shared_ptr<http::response<http::string_body>> res_;
    BeastHttpServer* server_;
};

BeastHttpServer::BeastHttpServer(const std::string& address, unsigned short port, int threads)
    : address_(address)
    , port_(port)
    , thread_count_(threads)
    , ioc_(std::make_unique<net::io_context>(threads)) {
    
    // Register built-in handlers
    registerHandler("/healthz", [this](const auto& req, const auto& target, const auto& body) {
        return handleHealth(req, target, body);
    });
    
    registerHandler("/metrics", [this](const auto& req, const auto& target, const auto& body) {
        return handleMetrics(req, target, body);
    });
    
    registerHandler("/", [this](const auto& req, const auto& target, const auto& body) {
        return handleRpc(req, target, body);
    });
}

BeastHttpServer::~BeastHttpServer() {
    stop();
}

bool BeastHttpServer::start() {
    if (running_.load()) {
        return false;
    }
    
    try {
        auto const address = net::ip::make_address(address_);
        acceptor_ = std::make_unique<tcp::acceptor>(*ioc_, tcp::endpoint{address, port_});
        
        // Start accepting connections
        doAccept();
        
        // Run the I/O service on the requested number of threads
        threads_.reserve(thread_count_);
        for (int i = 0; i < thread_count_; ++i) {
            threads_.emplace_back([this] {
                ioc_->run();
            });
        }
        
        running_.store(true);
        
        std::cout << "[HTTP] Beast server listening on " << address_ << ":" << port_ 
                  << " with " << thread_count_ << " threads" << std::endl;
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "[HTTP] Failed to start Beast server: " << e.what() << std::endl;
        return false;
    }
}

void BeastHttpServer::stop() {
    if (!running_.load()) {
        return;
    }
    
    stopping_.store(true);
    
    if (acceptor_) {
        acceptor_->close();
    }
    
    ioc_->stop();
    
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    
    threads_.clear();
    running_.store(false);
    stopping_.store(false);
    
    std::cout << "[HTTP] Beast server stopped" << std::endl;
}

void BeastHttpServer::registerHandler(const std::string& path, HttpHandler handler) {
    handlers_[path] = std::move(handler);
}

void BeastHttpServer::doAccept() {
    acceptor_->async_accept(
        net::make_strand(*ioc_),
        beast::bind_front_handler(&BeastHttpServer::onAccept, this));
}

void BeastHttpServer::onAccept(beast::error_code ec, tcp::socket socket) {
    if (ec) {
        if (!stopping_.load()) {
            std::cerr << "[HTTP] Accept error: " << ec.message() << std::endl;
        }
    } else {
        std::make_shared<HttpSession>(std::move(socket), this)->run();
    }
    
    if (!stopping_.load()) {
        doAccept();
    }
}

http::response<http::string_body> BeastHttpServer::routeRequest(
    const http::request<http::string_body>& req) {
    
    std::string target = std::string(req.target());
    
    // Extract or generate trace ID
    std::string trace_id = "";
    if(auto it = req.find("X-Trace-Id"); it != req.end()) {
        trace_id = std::string(it->value());
    }
    if(trace_id.empty()) {
        trace_id = din_generate_trace_id();
    }
    din_set_current_trace_id(trace_id);
    
    // Log request (demote to DEBUG to reduce noise)
    DIN_LOG_INFO("http", "request", 
        "\"method\":\"" + std::string(req.method_string()) + "\",\"target\":\"" + target + "\"");
    
    // Find matching handler
    HttpHandler* handler = nullptr;
    
    // Exact match first
    auto it = handlers_.find(target);
    if (it != handlers_.end()) {
        handler = &it->second;
    } else {
        // Check for root handler for RPC
        if (target == "/" || target.empty()) {
            auto root_it = handlers_.find("/");
            if (root_it != handlers_.end()) {
                handler = &root_it->second;
            }
        }
    }
    
    http::response<http::string_body> response;
    
    if (handler) {
        try {
            response = (*handler)(req, target, req.body());
        } catch (const std::exception& e) {
            response = createErrorResponse(http::status::internal_server_error, 
                                         std::string("Handler error: ") + e.what(), req);
        }
    } else {
        response = handleNotFound(req, target, req.body());
    }
    
    // Add trace ID header
    response.set("X-Trace-Id", trace_id);
    
    // Log response (demote to DEBUG to reduce noise)
    DIN_LOG_INFO("http", "response",
        "\"status\":" + std::to_string(static_cast<int>(response.result())) + ",\"content_type\":\"" + std::string(response[http::field::content_type]) + "\"");
    
    return response;
}

http::response<http::string_body> BeastHttpServer::handleHealth(
    const http::request<http::string_body>& req,
    const std::string& target,
    const std::string& body) {
    
    if (!health_enabled_) {
        return createErrorResponse(http::status::not_found, "Health endpoint disabled", req);
    }
    
    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::server, "dinerod-vnext/1.0");
    res.set(http::field::content_type, "application/json");
    res.keep_alive(req.keep_alive());
    
    din::Json health;
    health["status"] = "ok";
    health["timestamp"] = static_cast<int64_t>(std::time(nullptr));
    health["version"] = DIN_VERSION_STRING;
    health["semver"] = DIN_VERSION_STRING;
    health["git"] = DIN_GIT_HASH;
    health["channel"] = DIN_CHANNEL;
    health["uptime_seconds"] = 0;
    
    res.body() = din::dump(health);
    res.prepare_payload();
    
    return res;
}

http::response<http::string_body> BeastHttpServer::handleMetrics(
    const http::request<http::string_body>& req,
    const std::string& target,
    const std::string& body) {
    
    if (!metrics_enabled_) {
        return createErrorResponse(http::status::not_found, "Metrics endpoint disabled", req);
    }
    
    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::server, "dinerod-vnext/1.0");
    res.set(http::field::content_type, "text/plain; version=0.0.4; charset=utf-8");
    res.keep_alive(req.keep_alive());
    
    // Use the unified metrics registry
    res.body() = dinero::metrics::MetricsRegistry::ExportMetrics();
    res.prepare_payload();
    
    return res;
}

http::response<http::string_body> BeastHttpServer::handleRpc(
    const http::request<http::string_body>& req,
    const std::string& target,
    const std::string& body) {
    
    if (!rpc_enabled_) {
        return createErrorResponse(http::status::not_found, "RPC endpoint disabled", req);
    }
    
    if (req.method() != http::verb::post) {
        return createErrorResponse(http::status::method_not_allowed, 
                                 "RPC requires POST method", req);
    }
    
    // Check authentication unless --dev-rpc-open is set
    if (!dev_rpc_open_) {
        http::response<http::string_body> auth_error_response{http::status::ok, req.version()};
        if (!checkRpcAuthentication(req, auth_error_response)) {
            return auth_error_response;
        }
    }
    
    // Process JSON-RPC with din::Json adapter
    din::Json response = processJsonRpc(body);
    
    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::server, "dinerod-vnext/1.0");
    res.set(http::field::content_type, "application/json");
    res.keep_alive(req.keep_alive());
    
    res.body() = din::dump(response);
    res.prepare_payload();
    
    return res;
}

http::response<http::string_body> BeastHttpServer::handleNotFound(
    const http::request<http::string_body>& req,
    const std::string& target,
    const std::string& body) {
    
    return createErrorResponse(http::status::not_found, 
                             "Endpoint not found: " + target, req);
}

http::response<http::string_body> BeastHttpServer::createErrorResponse(
    http::status status, 
    const std::string& message,
    const http::request<http::string_body>& req) {
    
    http::response<http::string_body> res{status, req.version()};
    res.set(http::field::server, "dinerod-vnext/1.0");
    res.set(http::field::content_type, "application/json");
    res.keep_alive(req.keep_alive());
    
    din::Json error;
    error["error"] = message;
    error["status"] = static_cast<int>(status);
    error["timestamp"] = static_cast<int64_t>(std::time(nullptr));
    
    res.body() = din::dump(error);
    res.prepare_payload();
    
    return res;
}

din::Json BeastHttpServer::processJsonRpc(const std::string& body) {
    try {
        din::Json in = din::parse(body);
        if (!in.isObject()) {
            din::Json err;
            err["jsonrpc"] = "2.0";
            err["id"] = din::null();
            err["error"]["code"] = -32700;
            err["error"]["message"] = "parse error";
            // ✅ Enforce schema contract on parse errors too
            err["rpc_schema"] = "din.rpc.v1";
            err["schema_rev"] = 1;
            return err;
        }
        
        const std::string method = in.isMember("method") ? in["method"].asString() : "";
        din::Json params = in.isMember("params") ? in["params"] : din::arr();
        
        // Start timing for aggregated access log
        auto start_time = std::chrono::steady_clock::now();
        
        // Demote individual RPC call log to DEBUG
        DIN_LOG_INFO("rpc", "call", "\"method\":\"" + method + "\"");
        
        RpcHandler* handler = rpc_registry_->lookup(method);
        if (!handler) {
            din::Json err;
            err["jsonrpc"] = "2.0";
            err["id"] = in.isMember("id") ? in["id"] : din::null();
            err["error"]["code"] = -32601;
            err["error"]["message"] = "Method not found";
            // ✅ Enforce schema contract on error responses too
            err["rpc_schema"] = "din.rpc.v1";
            err["schema_rev"] = 1;
            DIN_LOG_ERR("rpc", "Method not found: " + method, "");
            return err;
        }
        
        // build your ExecutionContext ctx {...}
        ExecutionContext ctx;
        
        try {
            din::Json result = (*handler)(ctx, params);
            
            din::Json out;
            out["jsonrpc"] = "2.0";
            out["id"] = in.isMember("id") ? in["id"] : din::null();
            out["result"] = result;
            // ✅ Enforce schema contract on successful responses
            out["rpc_schema"] = "din.rpc.v1";
            out["schema_rev"] = 1;
            
            // Demote individual RPC return log to DEBUG
            DIN_LOG_INFO("rpc", "return", "\"method\":\"" + method + "\"");
            
            // Calculate duration and log aggregated access
            auto end_time = std::chrono::steady_clock::now();
            uint32_t dur_ms = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count());
            
            // Determine log level based on access policy
            bool is_error = false; // 200 response
            bool is_slow = dur_ms >= dinero::g_accesslog.slow_ms;
            bool want_ok = dinero::g_accesslog.sample_ok(method);
            
            auto level = (is_error || is_slow || want_ok) ? "INFO" : "DEBUG";
            
            // Single aggregated access log
            std::string trace_id = din_current_trace_id();
            DIN_LOG_INFO("rpc_access", "access", 
                "\"method\":\"" + method + "\",\"status\":200,\"dur_ms\":" + std::to_string(dur_ms) + ",\"target\":\"/\"");
            
            return out;
        } catch (const std::exception& e) {
            // Handle RPC handler exceptions properly
            din::Json err;
            err["jsonrpc"] = "2.0";
            err["id"] = in.isMember("id") ? in["id"] : din::null();
            err["error"]["code"] = -32603;  // Internal error
            err["error"]["message"] = e.what();
            err["rpc_schema"] = "din.rpc.v1";
            err["schema_rev"] = 1;
            
            // Calculate duration for error case
            auto end_time = std::chrono::steady_clock::now();
            uint32_t dur_ms = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count());
            
            // Always log errors at INFO level
            std::string trace_id = din_current_trace_id();
            DIN_LOG_INFO("rpc_access", "access", 
                "\"method\":\"" + method + "\",\"status\":500,\"dur_ms\":" + std::to_string(dur_ms) + ",\"target\":\"/\",\"error\":\"" + e.what() + "\"");
            
            DIN_LOG_ERR("rpc", "Handler exception: " + std::string(e.what()), "");
            return err;
        }
    } catch (const std::exception& e) {
        din::Json err;
        err["jsonrpc"] = "2.0";
        err["id"] = din::null();
        err["error"]["code"] = -32700;
        err["error"]["message"] = "parse error";
        // ✅ Enforce schema contract on exception errors too
        err["rpc_schema"] = "din.rpc.v1";
        err["schema_rev"] = 1;
        return err;
    }
}

bool BeastHttpServer::checkRpcAuthentication(const http::request<http::string_body>& req, 
                                                     http::response<http::string_body>& error_response) {
    if (cookie_path_.empty()) {
        // No cookie path configured, allow access (for backward compatibility)
        return true;
    }
    
    // Extract Authorization header
    auto auth_it = req.find(http::field::authorization);
    if (auth_it == req.end()) {
        // No auth header - create standard 401 response
        error_response.result(http::status::unauthorized);
        error_response.set(http::field::www_authenticate, 
                          R"(Basic realm="Dinero RPC", Bearer realm="Dinero RPC")");
        error_response.set(http::field::content_type, "application/json");
        error_response.body() = R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"Unauthorized"},"result":null,"rpc_schema":"din.rpc.v1"})";
        error_response.content_length(error_response.body().size());
        return false;
    }
    
    std::string auth_header = std::string(auth_it->value());
    
    // Try Bearer token first (Auth v1)
    if (auth_store_) {
        int status;
        std::string www_auth, json_body;
        if (dinero::rpc::authorize_request(auth_header, auth_store_, status, www_auth, json_body)) {
            return true; // Bearer token succeeded
        }
        if (status == 401) {
            // Bearer token failed - use detailed error from middleware
            error_response.result(static_cast<http::status>(status));
            if (!www_auth.empty()) {
                error_response.set(http::field::www_authenticate, www_auth);
            }
            error_response.set(http::field::content_type, "application/json");
            error_response.body() = json_body;
            error_response.content_length(error_response.body().size());
            return false;
        }
        // If status != 401, it wasn't a Bearer token, fall through to Basic auth
    }
    
    // Fall back to Basic auth (cookie)
    std::unordered_map<std::string, std::string> headers_lowercased;
    headers_lowercased["authorization"] = auth_header;
    if (!dinero::check_basic_authorization(headers_lowercased, cookie_path_)) {
        // Basic auth failed - create standard 401 response
        error_response.result(http::status::unauthorized);
        error_response.set(http::field::www_authenticate, 
                          R"(Basic realm="Dinero RPC", Bearer realm="Dinero RPC")");
        error_response.set(http::field::content_type, "application/json");
        error_response.body() = R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"Unauthorized"},"result":null,"rpc_schema":"din.rpc.v1"})";
        error_response.content_length(error_response.body().size());
        return false;
    }
    
    return true;
}

std::string BeastHttpServer::generateTraceId() const {
    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    static thread_local std::uniform_int_distribution<uint64_t> dis;
    
    uint64_t trace_id = dis(gen);
    
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << trace_id;
    return oss.str();
}

void BeastHttpServer::logRequest(const std::string& method, const std::string& target, 
                                const std::string& trace_id, int status_code) const {
    std::cout << "[HTTP] " << method << " " << target 
              << " -> " << status_code 
              << " (trace: " << trace_id << ")" << std::endl;
}

} // namespace dinero
