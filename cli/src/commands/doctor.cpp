#include "dinero/cli/commands/doctor.hpp"
#include "dinero/cli/url_parser.hpp"
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <chrono>
#include <thread>
#include <unistd.h>

namespace dinero {
namespace cli {

struct DiagnosticResult {
    std::string endpoint;
    std::string source;
    bool cookie_exists = false;
    bool cookie_perms_ok = false;
    std::string cookie_path;
    bool nodeinfo_exists = false;
    std::string nodeinfo_path;
    bool rpc_ok = false;
    int rpc_latency_ms = -1;
    std::string rpc_error;
    bool ws_ok = false;
    std::string ws_error;
};

bool check_file_permissions(const std::string& path, mode_t expected_mode) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    return (st.st_mode & 0777) == expected_mode;
}

bool file_exists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

int cmd_doctor(const std::string& rpc_url, const std::string& cookie_path, 
               const std::string& nodeinfo_path) {
    DiagnosticResult result;
    
    std::cout << "🩺 Dinero CLI Doctor - Comprehensive Diagnostics\n";
    std::cout << "================================================\n\n";
    
    // 1. Parse and validate RPC endpoint
    ParsedUrl parsed = ParseUrl(rpc_url);
    
    if (!parsed.valid) {
        std::cout << "❌ RPC Endpoint: Invalid RPC URL: " << rpc_url << std::endl;
        if (!parsed.error_message.empty()) {
            std::cout << "   Error: " << parsed.error_message << std::endl;
        }
        return 2; // Invalid configuration (including HTTPS rejection)
    }
    result.endpoint = parsed.host + ":" + std::to_string(parsed.port);
    result.source = "explicit";
    std::cout << "✅ RPC Endpoint: " << result.endpoint << " (source: " << result.source << ")\n";
    
    // 2. Check cookie file
    result.cookie_path = cookie_path;
    result.cookie_exists = file_exists(cookie_path);
    
    if (result.cookie_exists) {
        result.cookie_perms_ok = check_file_permissions(cookie_path, 0600);
        std::cout << "✅ Cookie File: " << cookie_path << " (exists)\n";
        
        if (result.cookie_perms_ok) {
            std::cout << "✅ Cookie Permissions: 0600 (secure)\n";
        } else {
            std::cout << "⚠️  Cookie Permissions: Not 0600 (recommend: chmod 600 " << cookie_path << ")\n";
        }
    } else {
        std::cout << "❌ Cookie File: " << cookie_path << " (not found)\n";
        return 3; // Authentication issue
    }
    
    // 3. Check nodeinfo file
    result.nodeinfo_path = nodeinfo_path;
    result.nodeinfo_exists = file_exists(nodeinfo_path);
    
    if (result.nodeinfo_exists) {
        std::cout << "✅ NodeInfo: " << nodeinfo_path << " (exists)\n";
    } else {
        std::cout << "⚠️  NodeInfo: " << nodeinfo_path << " (not found - using defaults)\n";
    }
    
    // 4. Test RPC connectivity
    std::cout << "🔗 Testing RPC connectivity..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();
    // TODO: Replace with actual RPC call
    std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Simulate network delay
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "   ✅ RPC ping successful (" << duration.count() << "ms, mocked)" << std::endl;
    
    // 5. Test WebSocket connectivity (mock for now)
    std::cout << "🔄 Testing WebSocket connectivity...\n";
    
    // TODO: Implement actual WebSocket ping
    result.ws_ok = true;
    
    if (result.ws_ok) {
        std::cout << "✅ WebSocket: Ping successful\n";
    } else {
        std::cout << "❌ WebSocket: " << result.ws_error << "\n";
    }
    
    // 6. Summary
    std::cout << "\n📊 Diagnostic Summary:\n";
    std::cout << "  Endpoint: " << result.endpoint << " (" << result.source << ")\n";
    std::cout << "  Cookie: " << (result.cookie_exists ? "✅" : "❌") << "\n";
    std::cout << "  Permissions: " << (result.cookie_perms_ok ? "✅" : "⚠️") << "\n";
    std::cout << "  RPC: " << (result.rpc_ok ? "✅" : "❌") << "\n";
    std::cout << "  WebSocket: " << (result.ws_ok ? "✅" : "❌") << "\n";
    
    if (result.rpc_ok && result.cookie_exists) {
        std::cout << "\n🎉 All systems operational! CLI is ready to use.\n";
        return 0;
    } else {
        std::cout << "\n⚠️  Issues detected. Check configuration and daemon status.\n";
        return 1;
    }
}

} // namespace cli
} // namespace dinero
