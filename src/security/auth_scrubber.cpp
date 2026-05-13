#include "security/auth_scrubber.h"
#include <algorithm>
#include <sstream>
#include <regex>

namespace dinero {
namespace security {

const std::vector<std::string> AuthScrubber::SENSITIVE_HEADERS = {
    "authorization",
    "cookie", 
    "x-auth-token",
    "x-api-key",
    "x-access-token"
};

std::string AuthScrubber::scrubHttpRequest(const std::string& request) {
    std::istringstream iss(request);
    std::ostringstream oss;
    std::string line;
    bool first_line = true;
    
    while (std::getline(iss, line)) {
        // Remove \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        if (first_line) {
            // Keep the request line (e.g., "POST /rpc HTTP/1.1")
            oss << line << "\n";
            first_line = false;
        } else if (line.empty()) {
            // End of headers
            oss << line << "\n";
            break;
        } else {
            // Process header line
            oss << scrubHttpHeader(line) << "\n";
        }
    }
    
    // Add any remaining body content (but don't process it)
    std::string remaining;
    while (std::getline(iss, line)) {
        remaining += line + "\n";
    }
    oss << remaining;
    
    return oss.str();
}

std::string AuthScrubber::scrubHttpHeader(const std::string& header_line) {
    size_t colon_pos = header_line.find(':');
    if (colon_pos == std::string::npos) {
        return header_line; // Not a header line
    }
    
    std::string header_name = header_line.substr(0, colon_pos);
    
    // Convert to lowercase for comparison
    std::string header_name_lower = header_name;
    std::transform(header_name_lower.begin(), header_name_lower.end(), 
                   header_name_lower.begin(), ::tolower);
    
    if (isSensitiveHeader(header_name_lower)) {
        return header_name + ": [REDACTED]";
    }
    
    return header_line;
}

bool AuthScrubber::isSensitiveHeader(const std::string& header_name) {
    std::string header_lower = header_name;
    std::transform(header_lower.begin(), header_lower.end(), 
                   header_lower.begin(), ::tolower);
    
    return std::find(SENSITIVE_HEADERS.begin(), SENSITIVE_HEADERS.end(), 
                     header_lower) != SENSITIVE_HEADERS.end();
}

std::string AuthScrubber::extractSafeRequestLine(const std::string& request) {
    size_t first_newline = request.find('\n');
    if (first_newline == std::string::npos) {
        // Single line, check if it looks like HTTP request line
        if (request.find("HTTP/") != std::string::npos) {
            return request;
        }
        return "[INVALID REQUEST]";
    }
    
    std::string first_line = request.substr(0, first_newline);
    
    // Remove \r if present
    if (!first_line.empty() && first_line.back() == '\r') {
        first_line.pop_back();
    }
    
    // Basic validation - should contain method and HTTP version
    if (first_line.find("HTTP/") != std::string::npos && 
        (first_line.find("GET ") == 0 || first_line.find("POST ") == 0 || 
         first_line.find("PUT ") == 0 || first_line.find("DELETE ") == 0 ||
         first_line.find("OPTIONS ") == 0 || first_line.find("HEAD ") == 0)) {
        return first_line;
    }
    
    return "[INVALID REQUEST LINE]";
}

} // namespace security
} // namespace dinero
