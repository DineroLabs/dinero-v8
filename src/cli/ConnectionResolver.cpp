#include "cli/ConnectionResolver.h"
#include <stdexcept>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <pwd.h>
#include <cstdlib>

namespace dinero::cli {

// Helper to expand ~ and $ENV in paths
static std::string expand_path(const std::string& path) {
    if (path.empty()) return path;
    
    std::string result = path;
    
    // Expand ~ to home directory
    if (result[0] == '~') {
        const char* home = getenv("HOME");
        if (!home) {
            // Fallback to getpwuid
            struct passwd* pw = getpwuid(getuid());
            if (pw && pw->pw_dir) {
                home = pw->pw_dir;
            }
        }
        if (home) {
            result = std::string(home) + result.substr(1);
        }
    }
    
    // Expand environment variables
    size_t start = 0;
    while ((start = result.find("$", start)) != std::string::npos) {
        size_t end = result.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_", start + 1);
        if (end == std::string::npos) end = result.length();
        
        std::string var_name = result.substr(start + 1, end - start - 1);
        const char* var_value = getenv(var_name.c_str());
        if (var_value) {
            result.replace(start, end - start, var_value);
            start += strlen(var_value);
        } else {
            start = end;
        }
    }
    
    return result;
}

// Helper to read cookie file with path expansion
static std::string read_cookie_file(const std::string& path) {
    std::string expanded_path = expand_path(path);
    std::ifstream f(expanded_path);
    if (!f) {
        throw std::runtime_error("cookie file not found: " + expanded_path);
    }
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // Trim trailing whitespace
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
}

ConnResolved resolve(const ConnInput& input) {
    ConnResolved result;
    
    // Handle --no-nodeinfo flag
    if (input.no_nodeinfo) {
        if (!input.rpc_url_flag || !input.cookie_file_flag) {
            throw std::runtime_error("--no-nodeinfo requires both --rpc-url and --cookie-file");
        }
        
        result.rpc_url = *input.rpc_url_flag;
        result.cookie_path = *input.cookie_file_flag;
        result.source = ConnResolved::Source::Flags;
        result.rpc_url_source = "--rpc-url";
        result.cookie_source = "--cookie-file";
        result.discovery_method = "explicit overrides (--no-nodeinfo)";
        return result;
    }
    
    // RPC URL resolution
    if (input.rpc_url_flag) {
        result.rpc_url = *input.rpc_url_flag;
        result.rpc_url_source = "--rpc-url";
    } else if (input.nodeinfo_rpc_url) {
        result.rpc_url = *input.nodeinfo_rpc_url;
        result.rpc_url_source = "nodeinfo.rpc.url";
    } else {
        throw std::runtime_error("RPC URL not provided. Use --rpc-url or set rpc.url in nodeinfo.json.");
    }
    
    // Cookie resolution
    if (input.cookie_file_flag) {
        result.cookie_path = expand_path(*input.cookie_file_flag);
        result.cookie_source = "--cookie-file";
    } else if (input.nodeinfo_cookie_path) {
        result.cookie_path = expand_path(*input.nodeinfo_cookie_path);
        result.cookie_source = "nodeinfo.rpc.cookie_path";
    } else if (input.nodeinfo_cookie_literal && input.accept_insecure_cookie) {
        result.cookie_literal = *input.nodeinfo_cookie_literal;
        result.cookie_source = "nodeinfo.rpc.cookie (literal)";
    } else {
        throw std::runtime_error("Cookie not provided. Use --cookie-file or put cookie path in nodeinfo.json.");
    }
    
    // Determine source type and connection summary
    if (input.rpc_url_flag && input.cookie_file_flag) {
        result.source = ConnResolved::Source::Flags;
        result.discovery_method = "explicit overrides";
        result.connection_summary = "explicit overrides";
    } else if (input.rpc_url_flag || input.cookie_file_flag) {
        result.source = ConnResolved::Source::Mixed;
        result.discovery_method = "mixed (flags + nodeinfo)";
        result.connection_summary = "mixed (flags + nodeinfo)";
    } else {
        result.source = ConnResolved::Source::Nodeinfo;
        result.discovery_method = "auto-discovered";
        result.connection_summary = "auto-discovered";
    }
    
    // Special case for --no-nodeinfo
    if (input.no_nodeinfo) {
        result.connection_summary = "explicit overrides (--no-nodeinfo)";
    }
    
    return result;
}

} // namespace dinero::cli
