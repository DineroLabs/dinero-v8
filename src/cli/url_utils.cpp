#include "cli/url_utils.h"
#include <regex>

namespace dinero {
namespace cli {

std::string extractHost(const std::string& rpcUrl) {
    // Parse URL like "http://localhost:20998/" or "https://node.example.com:8332/"
    std::regex urlRegex(R"(https?://([^:]+):(\d+)/?.*?)");
    std::smatch match;
    
    if (std::regex_match(rpcUrl, match, urlRegex)) {
        return match[1].str();
    }
    
    // Fallback: assume localhost if parsing fails
    return "localhost";
}

std::string extractPort(const std::string& rpcUrl) {
    // Parse URL like "http://localhost:20998/" or "https://node.example.com:8332/"
    std::regex urlRegex(R"(https?://([^:]+):(\d+)/?.*?)");
    std::smatch match;
    
    if (std::regex_match(rpcUrl, match, urlRegex)) {
        return match[2].str();
    }
    
    // Fallback: assume default port if parsing fails
    return "20998";
}

} // namespace cli
} // namespace dinero
