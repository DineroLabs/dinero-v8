#pragma once

#include <string>
#include <vector>

namespace dinero {
namespace security {

/**
 * Security utilities for scrubbing sensitive data from logs and debug output
 */
class AuthScrubber {
public:
    /**
     * Scrub Authorization headers from HTTP request text
     * Replaces "Authorization: Bearer <token>" with "Authorization: Bearer [REDACTED]"
     * Replaces "Authorization: Basic <credentials>" with "Authorization: Basic [REDACTED]"
     */
    static std::string scrubHttpRequest(const std::string& request);
    
    /**
     * Scrub sensitive headers from a single HTTP header line
     * Returns the scrubbed header line or original if not sensitive
     */
    static std::string scrubHttpHeader(const std::string& header_line);
    
    /**
     * Check if a header name is sensitive and should be scrubbed
     */
    static bool isSensitiveHeader(const std::string& header_name);
    
    /**
     * Extract just the method and path from HTTP request for safe logging
     * e.g. "POST /rpc HTTP/1.1" -> "POST /rpc"
     */
    static std::string extractSafeRequestLine(const std::string& request);

private:
    static const std::vector<std::string> SENSITIVE_HEADERS;
};

} // namespace security
} // namespace dinero
