#include "ws/ws_http_session.h"
#include "daemon/auth_cookie.h"
#include "rpc/token_manager.h"
#include "version_config.h"
#include <iostream>
#include <unordered_map>
#include <algorithm>

using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;

namespace dinero {

// Forward declaration - WsSession is defined in ws_server.cpp
// We only need to know it exists and has these methods
class WsSession;

// Helper function to create and run WsSession - defined in ws_server.cpp
// This avoids template instantiation issues
extern std::shared_ptr<WsSession> create_ws_session_with_request(
    tcp::socket&& socket,
    http::request<http::empty_body>&& req);

WsHttpSession::WsHttpSession(
    tcp::socket&& socket,
    const std::string& cookie_path)
    : stream_(std::move(socket))
    , cookie_path_(cookie_path)
{
}

void WsHttpSession::run() {
    do_read();
}

void WsHttpSession::do_read() {
    // Construct a new parser for each message
    parser_.emplace();

    // Apply a reasonable limit to the allowed size of the body in bytes
    parser_->body_limit(10000);

    // Set the timeout
    stream_.expires_after(std::chrono::seconds(30));

    // Read the HTTP request
    http::async_read(
        stream_,
        buffer_,
        parser_->get(),
        boost::beast::bind_front_handler(
            &WsHttpSession::on_read,
            shared_from_this()));
}

void WsHttpSession::on_read(
    boost::beast::error_code ec,
    std::size_t bytes_transferred)
{
    boost::ignore_unused(bytes_transferred);

    // This means they closed the connection
    if (ec == http::error::end_of_stream) {
        stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
        return;
    }

    if (ec) {
        std::cerr << "[WsHttpSession] Read error: " << ec.message() << std::endl;
        return;
    }

    // Check if this is a WebSocket upgrade request
    if (websocket::is_upgrade(parser_->get())) {
        std::cout << "[WsHttpSession] WebSocket upgrade detected" << std::endl;

        // Check authentication before upgrading
        if (!is_authenticated()) {
            std::cout << "[WsHttpSession] Authentication failed" << std::endl;
            send_unauthorized();
            return;
        }

        std::cout << "[WsHttpSession] Authentication successful, upgrading to WebSocket" << std::endl;

        // Authentication successful - create WebSocket session and transfer ownership
        // Extract the request before releasing the socket
        auto req = parser_->release();

        // Create and start WebSocket session using helper function from ws_server.cpp
        // This avoids template instantiation issues across translation units
        auto ws_session = create_ws_session_with_request(stream_.release_socket(), std::move(req));
        return;
    }

    // Not a WebSocket upgrade - reject
    std::cout << "[WsHttpSession] Not a WebSocket upgrade request" << std::endl;
    send_forbidden();
}

bool WsHttpSession::is_authenticated() const {
    // Get the HTTP request
    const auto& req = parser_->get();

    // Get client IP address
    std::string client_ip = "127.0.0.1";
    try {
        auto remote_endpoint = stream_.socket().remote_endpoint();
        client_ip = remote_endpoint.address().to_string();
    } catch (...) {
        // Fallback to localhost if we can't get IP
        client_ip = "127.0.0.1";
    }

    // Build lowercase header map
    std::unordered_map<std::string, std::string> headers_lowercased;

    for (const auto& field : req) {
        std::string name(field.name_string().data(), field.name_string().size());
        std::string value(field.value().data(), field.value().size());

        // Convert name to lowercase
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return std::tolower(c); });

        headers_lowercased[name] = value;
    }

    // Check for Authorization header
    auto auth_it = headers_lowercased.find("authorization");
    if (auth_it != headers_lowercased.end()) {
        std::string auth_header = auth_it->second;

        // Check if it's a Bearer token
        if (auth_header.size() > 7 && auth_header.substr(0, 7) == "Bearer ") {
            std::string token = auth_header.substr(7);

            // Trim whitespace
            size_t start = token.find_first_not_of(" \t\r\n");
            size_t end = token.find_last_not_of(" \t\r\n");
            if (start != std::string::npos) {
                token = token.substr(start, end - start + 1);
            }

            // Validate token using TokenManager
            auto& tm = dinero::rpc::TokenManager::instance();
            bool valid = tm.ValidateToken(token, client_ip, "wallet");

            if (valid) {
                std::cout << "[WsHttpSession] Token authentication successful for " << client_ip << std::endl;
                return true;
            } else {
                std::cout << "[WsHttpSession] Token authentication failed for " << client_ip << std::endl;
                // Fall through to cookie auth
            }
        }
    }

    // Fall back to existing cookie-based authentication
    bool cookie_valid = check_basic_authorization(headers_lowercased, cookie_path_);

    if (cookie_valid) {
        std::cout << "[WsHttpSession] Cookie authentication successful for " << client_ip << std::endl;
    } else {
        // Check localhost bypass
        if (client_ip == "127.0.0.1" || client_ip == "::1") {
            std::cout << "[WsHttpSession] Localhost bypass for " << client_ip << std::endl;
            return true;
        }
        std::cout << "[WsHttpSession] Authentication failed for " << client_ip << std::endl;
    }

    return cookie_valid;
}

void WsHttpSession::send_unauthorized() {
    // Create 401 Unauthorized response
    http::response<http::string_body> res{http::status::unauthorized, parser_->get().version()};
    res.set(http::field::server, std::string("dinero-ws/") + DINERO_CLI_GIT_SHA);
    res.set(http::field::content_type, "text/plain");
    // Support both Bearer token and Basic auth
    res.set(http::field::www_authenticate, "Bearer realm=\"dinero-rpc\", Basic realm=\"dinero-rpc\"");
    res.body() = "Unauthorized: Invalid or missing authentication credentials\n";
    res.prepare_payload();
    res.keep_alive(false);

    auto self = shared_from_this();
    http::async_write(
        stream_,
        res,
        [self](boost::beast::error_code ec, std::size_t bytes) {
            self->on_write(ec, bytes, true);
        });
}

void WsHttpSession::send_forbidden() {
    // Create 403 Forbidden response
    http::response<http::string_body> res{http::status::forbidden, parser_->get().version()};
    res.set(http::field::server, std::string("dinero-ws/") + DINERO_CLI_GIT_SHA);
    res.set(http::field::content_type, "text/plain");
    res.body() = "Forbidden: This endpoint only accepts WebSocket connections\n";
    res.prepare_payload();
    res.keep_alive(false);

    auto self = shared_from_this();
    http::async_write(
        stream_,
        res,
        [self](boost::beast::error_code ec, std::size_t bytes) {
            self->on_write(ec, bytes, true);
        });
}

void WsHttpSession::on_write(
    boost::beast::error_code ec,
    std::size_t bytes_transferred,
    bool close)
{
    boost::ignore_unused(bytes_transferred);

    if (ec) {
        std::cerr << "[WsHttpSession] Write error: " << ec.message() << std::endl;
        return;
    }

    if (close) {
        // Close the connection
        boost::beast::error_code ec;
        stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
        return;
    }

    // Read another request (keep-alive)
    do_read();
}

} // namespace dinero
