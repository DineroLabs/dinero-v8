#include "dinero/cli/url_parser.hpp"
#include <regex>
#include <vector>
#include <cctype>

// Simplified IPv6 validation without Boost dependency
static bool is_valid_ipv6_literal(std::string s) {
    // RFC 6874: zone id must be percent-encoded in URLs ("%25zone")
    // Separate the IPv6 address from the zone ID
    std::string ipv6_part = s;
    std::string zone_part;
    
    size_t zone_pos = s.find("%25");
    if (zone_pos != std::string::npos) {
        ipv6_part = s.substr(0, zone_pos);
        zone_part = s.substr(zone_pos + 3); // Skip "%25"
        
        // Zone ID should be non-empty alphanumeric
        if (zone_part.empty()) return false;
        for (char c : zone_part) {
            if (!std::isalnum(c)) return false;
        }
    }
    
    // Basic IPv6 validation without external dependencies
    if (ipv6_part.empty()) return false;
    
    // Must contain at least one colon
    if (ipv6_part.find(':') == std::string::npos) return false;
    
    // Check for valid double colon usage (no more than one "::")
    size_t double_colon_pos = ipv6_part.find("::");
    if (double_colon_pos != std::string::npos) {
        // Check for multiple "::" which is invalid
        if (ipv6_part.find("::", double_colon_pos + 2) != std::string::npos) {
            return false;
        }
    }
    
    // Simplified validation: just check for valid hex digits and colons
    // Don't try to parse segments perfectly - trust the resolver for final validation
    for (char c : ipv6_part) {
        if (!std::isxdigit(c) && c != ':') {
            return false;
        }
    }
    
    // Must have at least one hex digit
    bool has_hex = false;
    for (char c : ipv6_part) {
        if (std::isxdigit(c)) {
            has_hex = true;
            break;
        }
    }
    if (!has_hex) return false;
    
    return true;
}

namespace dinero {
namespace cli {

ParsedUrl ParseUrl(const std::string& url) {
    ParsedUrl result;
    
    // HTTPS fail-fast - not supported (security policy)
    if (url.substr(0, 8) == "https://") {
        result.valid = false;
        result.error_message = "HTTPS URLs are not supported. Use HTTP with proper authentication instead.";
        return result;
    }
    
    // Parse URL components manually for better IPv6 handling
    if (url.substr(0, 7) != "http://") {
        result.error_message = "Invalid URL format. Expected: http://host[:port][/path]";
        return result;
    }
    
    // Extract authority and path
    std::string remainder = url.substr(7); // Skip "http://"
    std::string authority, path;
    
    auto slash_pos = remainder.find('/');
    if (slash_pos != std::string::npos) {
        authority = remainder.substr(0, slash_pos);
        path = remainder.substr(slash_pos);
    } else {
        authority = remainder;
        path = "/";
    }
    
    if (authority.empty()) {
        result.error_message = "Missing host in URL";
        return result;
    }
    
    // Parse host and port from authority
    std::string host, port_str;
    
    // Handle bracketed IPv6: [host]%25zone?:port
    if (!authority.empty() && authority.front() == '[') {
        auto rb = authority.find(']');
        if (rb == std::string::npos) {
            result.error_message = "IPv6 bracket not closed";
            return result;
        }
        
        const std::string inner = authority.substr(1, rb - 1); // content inside [...]
        if (!is_valid_ipv6_literal(inner)) {
            result.error_message = "Invalid IPv6 literal inside brackets";
            return result;
        }
        
        host = inner; // keep as-is (with %25 if present)
        if (rb + 1 < authority.size()) {
            if (authority[rb + 1] == ':') {
                port_str = authority.substr(rb + 2);
            } else {
                // next char should be '/' for path; anything else is malformed
                if (authority[rb + 1] != '/') {
                    result.error_message = "Unexpected character after ]";
                    return result;
                }
            }
        }
    } else {
        // Handle IPv4/hostname: host[:port]
        auto colon = authority.rfind(':');
        // Only treat as port separator if it's the only colon (not IPv6)
        if (colon != std::string::npos && authority.find(':') == colon) {
            host = authority.substr(0, colon);
            port_str = authority.substr(colon + 1);
        } else {
            host = authority;
        }
    }
    
    // Port validation
    if (!port_str.empty()) {
        try {
            int port_num = std::stoi(port_str);
            if (port_num < 1 || port_num > 65535) {
                result.error_message = "Port number out of range (1-65535)";
                return result;
            }
            result.port = static_cast<uint16_t>(port_num);
        } catch (...) {
            result.error_message = "Invalid port number";
            return result;
        }
    } else {
        result.port = 80; // Default HTTP port
    }
    
    // Set parsed components
    result.scheme = "http";
    result.host = host;
    result.path = path;
    result.valid = true;
    
    return result;
}

} // namespace cli
} // namespace dinero
