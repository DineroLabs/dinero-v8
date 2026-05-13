#include "config/ws_config.h"
#include "util/args.h"
#include <algorithm>

namespace dinero {
namespace config {

std::string normalizePath(const std::string& p) {
    if (p.empty()) return "/ws";
    
    std::string result = p;
    
    // Ensure leading slash
    if (result[0] != '/') {
        result.insert(result.begin(), '/');
    }
    
    // Optional: collapse repeated slashes (simple implementation)
    size_t pos = 0;
    while ((pos = result.find("//", pos)) != std::string::npos) {
        result.erase(pos, 1);
    }
    
    return result;
}

WsConfig LoadWsConfig() {
    WsConfig c;
    
    // Parse from command line arguments (this is a simplified version)
    // In a real implementation, you'd use a proper argument parser
    c.bind = "127.0.0.1";  // Default
    c.port = 18332;        // Default
    c.path = "/ws";        // Default
    
    // This is a placeholder - the actual parsing happens in main.cpp
    // We'll pass the values directly to avoid circular dependencies
    
    return c;
}

} // namespace config
} // namespace dinero
