#pragma once
#include <QtCore>
#include <string>

// Minimal Beast WebSocket client for CLI one-shot requests
class WsClient {
public:
    // Send single JSON-RPC request and return response
    static std::string call(const std::string& url, const std::string& auth_header, const std::string& json_request);
    
    // Check if WebSocket URL is available (quick connection test)
    static bool isAvailable(const std::string& url);
};

// Auth helper for cookie-based Basic authentication
class Auth {
public:
    static Auth fromCookie(const QString& cookie_path);
    
    std::string header() const { return auth_header_; }
    bool isValid() const { return !auth_header_.empty(); }

private:
    std::string auth_header_;
    Auth(const std::string& header) : auth_header_(header) {}
};
