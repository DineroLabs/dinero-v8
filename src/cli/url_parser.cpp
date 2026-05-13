#include "url_parser.h"
#include <regex>

ParsedUrl parseUrl(const std::string& url) {
    ParsedUrl result;
    
    // Enhanced URL regex supporting IPv6
    std::regex url_regex(R"(^(https?):\/\/(\[([^\]]+)\]|([^:\/\s]+))(?::(\d+))?(\/.*)?$)");
    std::smatch matches;
    
    if (std::regex_match(url, matches, url_regex)) {
        result.scheme = matches[1].str();
        
        // Handle IPv6 vs IPv4/hostname
        if (matches[3].matched) {
            // IPv6 address in brackets
            result.host = matches[3].str();
        } else {
            // IPv4 or hostname
            result.host = matches[4].str();
        }
        
        result.path = matches[6].str();
        if (result.path.empty()) result.path = "/";
        
        // Port handling
        if (matches[5].matched) {
            try {
                result.port = std::stoi(matches[5].str());
                if (result.port < 1 || result.port > 65535) {
                    return result; // Invalid port range
                }
            } catch (...) {
                return result; // Invalid port number
            }
        } else {
            // Default ports
            result.port = (result.scheme == "https") ? 443 : 80;
        }
        
        // Scheme validation
        if (result.scheme == "http" || result.scheme == "https") {
            result.valid = true;
        }
        
        // HTTPS fail-fast - not supported
        if (result.scheme == "https") {
            result.valid = false;
        }
    }
    
    return result;
}
