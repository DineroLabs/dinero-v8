#include "daemon/security_hardening.h"
#include "common/logger.h"
#include <filesystem>
#include <fstream>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace dinero {

bool EnsureCookiePermissions(const std::string& cookie_path) {
#ifdef _WIN32
    // Windows uses ACLs, not POSIX permissions — skip permission check
    (void)cookie_path;
    return true;
#else
    try {
        if (!std::filesystem::exists(cookie_path)) {
            g_logger.info("Cookie file does not exist: " + cookie_path);
            return false;
        }

        // Check current permissions
        struct stat st;
        if (stat(cookie_path.c_str(), &st) != 0) {
            g_logger.error("Failed to stat cookie file: " + cookie_path);
            return false;
        }

        // Check if permissions are too broad (readable by group/others)
        if (st.st_mode & (S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) {
            g_logger.info("Cookie file has overly broad permissions: " + cookie_path);

            // Fix permissions to 0600 (owner read/write only)
            if (chmod(cookie_path.c_str(), S_IRUSR | S_IWUSR) != 0) {
                g_logger.error("Failed to fix cookie file permissions: " + cookie_path);
                return false;
            }

            g_logger.info("Fixed cookie file permissions to 0600: " + cookie_path);
        }

        return true;
    } catch (const std::exception& e) {
        g_logger.error("Exception checking cookie permissions: " + std::string(e.what()));
        return false;
    }
#endif
}

bool ValidateLoopbackBinding(const std::string& bind_address, int port) {
    if (bind_address != "127.0.0.1" && bind_address != "localhost" && bind_address != "::1") {
        g_logger.info("🚨 SECURITY WARNING: Server binding to non-loopback address: " + bind_address + ":" + std::to_string(port));
        g_logger.info("🚨 This exposes RPC services to external networks!");
        g_logger.info("🚨 For production, use a reverse proxy with proper TLS and authentication");
        return false;
    }
    
    g_logger.info("✅ Server safely bound to loopback address: " + bind_address + ":" + std::to_string(port));
    return true;
}

bool ValidateMiningAddressOnStartup(const std::string& datadir) {
    try {
        std::filesystem::path mining_file = std::filesystem::path(datadir) / "mining_address.txt";
        
        if (!std::filesystem::exists(mining_file)) {
            // No mining address file is fine
            return true;
        }
        
        std::ifstream file(mining_file);
        if (!file.is_open()) {
            g_logger.error("Failed to open mining address file: " + mining_file.string());
            return false;
        }
        
        std::string addr;
        if (!std::getline(file, addr)) {
            g_logger.info("Empty mining address file, removing: " + mining_file.string());
            std::filesystem::remove(mining_file);
            return true;
        }
        
        // Remove whitespace
        addr.erase(addr.find_last_not_of(" \n\r\t") + 1);
        
        if (addr.empty()) {
            g_logger.info("Empty mining address, removing file: " + mining_file.string());
            std::filesystem::remove(mining_file);
            return true;
        }
        
        // Basic HRP validation
        // TODO: Add proper chain params access
        std::string expected_hrp = "rdin1"; // regtest default
        if (addr.substr(0, expected_hrp.length()) != expected_hrp) {
            g_logger.error("Mining address has wrong network HRP: " + addr);
            g_logger.error("Expected: " + expected_hrp + ", removing invalid file");
            std::filesystem::remove(mining_file);
            return false;
        }
        
        // Basic validation - full wallet ownership check happens in mining RPC handlers
        
        g_logger.info("Mining address validation passed: " + addr);
        return true;
        
    } catch (const std::exception& e) {
        g_logger.error("Exception validating mining address: " + std::string(e.what()));
        return false;
    }
}

std::string ScrubAuthFromHeaders(const std::string& headers) {
    std::string scrubbed = headers;
    
    // Find Authorization header and replace with [REDACTED]
    size_t auth_pos = scrubbed.find("Authorization:");
    if (auth_pos != std::string::npos) {
        size_t line_end = scrubbed.find('\n', auth_pos);
        if (line_end == std::string::npos) {
            line_end = scrubbed.length();
        }
        
        scrubbed.replace(auth_pos, line_end - auth_pos, "Authorization: [REDACTED]");
    }
    
    // Also scrub cookie values
    auth_pos = scrubbed.find("Cookie:");
    if (auth_pos != std::string::npos) {
        size_t line_end = scrubbed.find('\n', auth_pos);
        if (line_end == std::string::npos) {
            line_end = scrubbed.length();
        }
        
        scrubbed.replace(auth_pos, line_end - auth_pos, "Cookie: [REDACTED]");
    }
    
    return scrubbed;
}

} // namespace dinero
