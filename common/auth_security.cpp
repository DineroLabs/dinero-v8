// SPDX-License-Identifier: MIT
// Dinero - Authentication Security Improvements

#include "common/auth_security.h"
#include "common/logger.h"
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <iostream>

namespace fs = std::filesystem;

namespace AuthSecurity {

bool check_cookie_permissions(const std::string& cookie_path) {
    try {
        struct stat file_stat;
        if (stat(cookie_path.c_str(), &file_stat) != 0) {
            Logger::error("Cookie file not found: " + cookie_path);
            return false;
        }
        
        // Check if permissions are 0600 (owner read/write only)
        mode_t perms = file_stat.st_mode & 0777;
        if (perms != 0600) {
            Logger::warning("Cookie file has unsafe permissions: " + 
                          std::to_string(perms) + " (expected 0600) - " + cookie_path);
            std::cerr << "⚠ Warning: Cookie file permissions are " << std::oct << perms 
                      << " (should be 0600 for security)\n";
            std::cerr << "  Run: chmod 600 " << cookie_path << "\n";
            return false;
        }
        
        return true;
        
    } catch (const std::exception& e) {
        Logger::error("Failed to check cookie permissions: " + std::string(e.what()));
        return false;
    }
}

std::string get_auth_error_message(int http_code, const std::string& response_body) {
    switch (http_code) {
        case 401:
            return "Authentication failed - check cookie file and permissions";
        case 403:
            return "Access denied - check cookie file permissions (should be 0600)";
        case 404:
            return "RPC endpoint not found - check daemon is running and nodeinfo.json";
        case 500:
            return "Internal server error: " + response_body.substr(0, 100);
        case 503:
            return "Service unavailable - daemon may be starting up or overloaded";
        default:
            return "HTTP " + std::to_string(http_code) + ": " + response_body.substr(0, 100);
    }
}

bool validate_cookie_content(const std::string& cookie_content) {
    // Cookie should be in format "username:password"
    size_t colon_pos = cookie_content.find(':');
    if (colon_pos == std::string::npos || colon_pos == 0 || colon_pos == cookie_content.length() - 1) {
        Logger::error("Invalid cookie format - should be 'username:password'");
        return false;
    }
    
    std::string username = cookie_content.substr(0, colon_pos);
    std::string password = cookie_content.substr(colon_pos + 1);
    
    // Basic validation - both parts should be non-empty and reasonable length
    if (username.length() < 4 || password.length() < 8) {
        Logger::error("Cookie credentials too short - possible corruption");
        return false;
    }
    
    // Check for reasonable base64-like characters
    const std::string valid_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
    for (char c : username + password) {
        if (valid_chars.find(c) == std::string::npos) {
            Logger::warning("Cookie contains unusual characters - may be corrupted");
            break;
        }
    }
    
    return true;
}

std::string read_cookie_file_secure(const std::string& cookie_path) {
    try {
        // Check file exists
        if (!fs::exists(cookie_path)) {
            throw std::runtime_error("Cookie file not found: " + cookie_path);
        }
        
        // Check permissions first
        if (!check_cookie_permissions(cookie_path)) {
            // Continue but warn - don't fail completely
            Logger::warning("Cookie file has unsafe permissions but continuing");
        }
        
        // Read file content
        std::ifstream file(cookie_path);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot open cookie file: " + cookie_path);
        }
        
        std::string content;
        std::getline(file, content);
        
        if (content.empty()) {
            throw std::runtime_error("Cookie file is empty: " + cookie_path);
        }
        
        // Validate content format
        if (!validate_cookie_content(content)) {
            throw std::runtime_error("Invalid cookie file format: " + cookie_path);
        }
        
        Logger::info("Successfully read cookie file: " + cookie_path);
        return content;
        
    } catch (const std::exception& e) {
        Logger::error("Failed to read cookie file: " + std::string(e.what()));
        throw;
    }
}

bool ensure_cookie_security(const std::string& cookie_path) {
    try {
        if (!fs::exists(cookie_path)) {
            Logger::error("Cookie file does not exist: " + cookie_path);
            return false;
        }
        
        // Check and fix permissions if needed
        struct stat file_stat;
        if (stat(cookie_path.c_str(), &file_stat) != 0) {
            Logger::error("Cannot stat cookie file: " + cookie_path);
            return false;
        }
        
        mode_t current_perms = file_stat.st_mode & 0777;
        if (current_perms != 0600) {
            Logger::info("Fixing cookie file permissions from " + 
                        std::to_string(current_perms) + " to 0600");
            
            if (chmod(cookie_path.c_str(), 0600) != 0) {
                Logger::error("Failed to fix cookie file permissions: " + cookie_path);
                return false;
            }
            
            Logger::info("Fixed cookie file permissions: " + cookie_path);
        }
        
        return true;
        
    } catch (const std::exception& e) {
        Logger::error("Failed to ensure cookie security: " + std::string(e.what()));
        return false;
    }
}

} // namespace AuthSecurity
