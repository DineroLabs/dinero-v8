// SPDX-License-Identifier: MIT
// Dinero CLI - Tiny Boost.Beast HTTP JSON-RPC Client Wrapper

#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <json/json.h>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class RpcHttpError : public std::runtime_error {
public:
    int code;
    RpcHttpError(int c, const std::string& msg) : std::runtime_error(msg), code(c) {}
};

class RpcHttpClient {
private:
    std::string host_;
    std::string port_;
    std::string auth_header_;
    int timeout_ms_;
    int max_retries_;
    
    net::io_context ioc_;
    tcp::resolver resolver_;
    beast::tcp_stream stream_;

public:
    RpcHttpClient(const std::string& host, const std::string& port, 
                  const std::string& cookie_content = "", 
                  int timeout_ms = 30000, int max_retries = 3)
        : host_(host), port_(port), timeout_ms_(timeout_ms), max_retries_(max_retries)
        , resolver_(ioc_), stream_(ioc_)
    {
        if (!cookie_content.empty()) {
            // Cookie format is "username:password"
            std::string encoded = base64_encode(cookie_content);
            auth_header_ = "Basic " + encoded;
        }
    }

    // Main RPC call method with retries and exponential backoff
    Json::Json::Value call(const std::string& method, const Json::Json::Value& params = Json::Json::Value::null);
    
    // Print equivalent curl command for debugging
    std::string toCurl(const std::string& method, const Json::Json::Value& params = Json::Json::Value::null) const;

private:
    std::string base64_encode(const std::string& input) const;
    Json::Json::Value single_call(const std::string& method, const Json::Json::Value& params);
    void connect_with_timeout();
    void disconnect();
};

// Inline implementation for header-only convenience
inline std::string RpcHttpClient::base64_encode(const std::string& input) const {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    int val = 0, valb = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            result.push_back(chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) result.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (result.size() % 4) result.push_back('=');
    return result;
}

inline void RpcHttpClient::connect_with_timeout() {
    auto const results = resolver_.resolve(host_, port_);
    stream_.expires_after(std::chrono::milliseconds(timeout_ms_));
    stream_.connect(results);
}

inline void RpcHttpClient::disconnect() {
    beast::error_code ec;
    stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
    // Ignore errors on shutdown
}

inline Json::Json::Value RpcHttpClient::single_call(const std::string& method, const Json::Json::Value& params) {
    connect_with_timeout();
    
    // Build JSON-RPC 2.0 request
    Json::Json::Value request;
    request["jsonrpc"] = "2.0";
    request["method"] = method;
    request["params"] = params;
    request["id"] = 1;
    
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    std::string body = Json::writeString(builder, request);
    
    // Build HTTP request
    http::request<http::string_body> req{http::verb::post, "/", 11};
    req.set(http::field::host, host_);
    req.set(http::field::user_agent, "dinero-cli/1.0");
    req.set(http::field::content_type, "application/json");
    if (!auth_header_.empty()) {
        req.set(http::field::authorization, auth_header_);
    }
    req.body() = body;
    req.prepare_payload();
    
    // Send request
    stream_.expires_after(std::chrono::milliseconds(timeout_ms_));
    http::write(stream_, req);
    
    // Read response
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream_, buffer, res);
    
    disconnect();
    
    // Check HTTP status
    if (res.result_int() != 200) {
        std::string error_body = res.body().substr(0, 256);
        if (res.result_int() == 401) {
            throw RpcHttpError(401, "Authentication failed - check cookie file");
        }
        throw RpcHttpError(res.result_int(), "HTTP " + std::to_string(res.result_int()) + ": " + error_body);
    }
    
    // Parse JSON response
    Json::CharReaderBuilder reader_builder;
    Json::Json::Value response;
    std::string parse_errors;
    std::istringstream response_stream(res.body());
    
    if (!Json::parseFromStream(reader_builder, response_stream, &response, &parse_errors)) {
        throw RpcHttpError(500, "Invalid JSON response: " + parse_errors);
    }
    
    // Check for JSON-RPC error
    if (response.contains("error") && !response["error"].isNull()) {
        Json::Json::Value error = response["error"];
        int code = error.value("code", -1);
        std::string message = error.value("message", "Unknown RPC error");
        throw RpcHttpError(code, "RPC error " + std::to_string(code) + ": " + message);
    }
    
    return response.get("result", Json::Json::Value::null);
}

inline Json::Json::Value RpcHttpClient::call(const std::string& method, const Json::Json::Value& params) {
    int attempt = 0;
    while (attempt <= max_retries_) {
        try {
            return single_call(method, params);
        } catch (const RpcHttpError& e) {
            // Don't retry auth failures or client errors (4xx)
            if (e.code == 401 || (e.code >= 400 && e.code < 500)) {
                throw;
            }
            
            attempt++;
            if (attempt > max_retries_) {
                throw;
            }
            
            // Exponential backoff: 100ms, 200ms, 400ms, 800ms...
            int delay_ms = 100 * (1 << (attempt - 1));
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
    
    // Should never reach here
    throw RpcHttpError(500, "Unexpected retry loop exit");
}

inline std::string RpcHttpClient::toCurl(const std::string& method, const Json::Json::Value& params) const {
    Json::Json::Value request;
    request["jsonrpc"] = "2.0";
    request["method"] = method;
    request["params"] = params;
    request["id"] = 1;
    
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    std::string body = Json::writeString(builder, request);
    
    std::string curl = "curl -X POST";
    curl += " -H 'Content-Type: application/json'";
    if (!auth_header_.empty()) {
        curl += " -H 'Authorization: " + auth_header_ + "'";
    }
    curl += " -d '" + body + "'";
    curl += " http://" + host_ + ":" + port_ + "/";
    
    return curl;
}
