#include "rpc_auth.h"
#include "secure_random.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdlib>

// Base64 encoding table
static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

// Helper function to expand ~ in paths
static std::string expand_tilde(const std::string& path) {
    if (path.empty() || path[0] != '~') {
        return path;
    }

    const char* home = std::getenv("HOME");
    if (!home) {
        return path;  // Can't expand, return as-is
    }

    if (path.length() == 1) {
        return std::string(home);
    }

    if (path[1] == '/') {
        return std::string(home) + path.substr(1);
    }

    return path;
}

RpcAuth::RpcAuth(const std::string& data_dir)
    : data_dir_(expand_tilde(data_dir)), rpc_allow_ip_("127.0.0.1"), dev_mode_(false) {
    cookie_path_ = data_dir_ + "/.cookie";
}

bool RpcAuth::generate_cookie() {
    try {
        // Create data directory if it doesn't exist
        std::filesystem::create_directories(data_dir_);
        
        // Generate cryptographically secure password
        std::string password = generate_random_password(32);
        
        // Cookie format: __cookie__:password
        cookie_content_ = "__cookie__:" + password;
        
        // Write cookie file with restricted permissions
        // Use temporary file first for atomic write
        std::string tmp_path = cookie_path_ + ".tmp";
        std::ofstream cookie_file(tmp_path, std::ios::trunc | std::ios::binary);
        if (!cookie_file.is_open()) {
            std::cerr << "Failed to create cookie file: " << cookie_path_ << std::endl;
            std::cerr << "  Error: Cannot open temporary file: " << tmp_path << std::endl;
            return false;
        }
        
        cookie_file << cookie_content_ << std::endl;
        cookie_file.flush();
        cookie_file.close();
        
        // Set file permissions to 0600 (owner read/write only) before rename
        try {
            std::filesystem::permissions(
                tmp_path, 
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                std::filesystem::perm_options::replace
            );
        } catch (const std::exception& e) {
            std::cerr << "Warning: Failed to set cookie file permissions: " << e.what() << std::endl;
            // Continue anyway - file will still work
        }
        
        // Atomic rename (replaces existing file if any)
        try {
            std::filesystem::rename(tmp_path, cookie_path_);
        } catch (const std::exception& e) {
            std::cerr << "Failed to rename cookie file: " << e.what() << std::endl;
            std::filesystem::remove(tmp_path);  // Clean up temp file
            return false;
        }
        
        // Verify file was created
        if (!std::filesystem::exists(cookie_path_)) {
            std::cerr << "ERROR: Cookie file was not created at: " << cookie_path_ << std::endl;
            return false;
        }
        
        std::cout << "✅ RPC cookie generated: " << cookie_path_ << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Failed to generate cookie: " << e.what() << std::endl;
        std::cerr << "  Cookie path: " << cookie_path_ << std::endl;
        std::cerr << "  Data dir: " << data_dir_ << std::endl;
        return false;
    }
}

bool RpcAuth::load_cookie() {
    try {
        if (!std::filesystem::exists(cookie_path_)) {
            return false;  // File doesn't exist - this is normal on first run
        }

        std::ifstream cookie_file(cookie_path_);
        if (!cookie_file.is_open()) {
            std::cerr << "Failed to open cookie file: " << cookie_path_ << std::endl;
            return false;
        }

        std::getline(cookie_file, cookie_content_);
        cookie_file.close();

        // Trim ALL whitespace (spaces, tabs, CR, LF)
        // Trim from end
        while (!cookie_content_.empty() &&
               (cookie_content_.back() == ' ' || cookie_content_.back() == '\t' ||
                cookie_content_.back() == '\r' || cookie_content_.back() == '\n')) {
            cookie_content_.pop_back();
        }
        // Trim from start
        while (!cookie_content_.empty() &&
               (cookie_content_[0] == ' ' || cookie_content_[0] == '\t' ||
                cookie_content_[0] == '\r' || cookie_content_[0] == '\n')) {
            cookie_content_.erase(0, 1);
        }

        if (cookie_content_.empty()) {
            std::cerr << "Empty cookie file: " << cookie_path_ << std::endl;
            return false;
        }

        // Validate cookie format: username:password
        size_t colon_pos = cookie_content_.find(':');
        if (colon_pos == std::string::npos) {
            std::cerr << "Invalid cookie format (missing ':' separator): " << cookie_path_ << std::endl;
            return false;
        }

        std::string username = cookie_content_.substr(0, colon_pos);
        std::string password = cookie_content_.substr(colon_pos + 1);

        // Validate username and password are non-empty
        if (username.empty()) {
            std::cerr << "Invalid cookie format (empty username): " << cookie_path_ << std::endl;
            return false;
        }

        if (password.empty()) {
            std::cerr << "Invalid cookie format (empty password): " << cookie_path_ << std::endl;
            return false;
        }

        // Validate no embedded whitespace in credentials
        if (username.find(' ') != std::string::npos || username.find('\t') != std::string::npos ||
            password.find(' ') != std::string::npos || password.find('\t') != std::string::npos) {
            std::cerr << "Invalid cookie format (embedded whitespace in credentials): " << cookie_path_ << std::endl;
            return false;
        }

        std::cout << "✅ RPC cookie loaded from: " << cookie_path_ << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Failed to load cookie: " << e.what() << std::endl;
        return false;
    }
}

void RpcAuth::set_static_credentials(const std::string& username, const std::string& password) {
    static_username_ = username;
    static_password_ = password;
    
    if (!username.empty() && !password.empty()) {
        std::cout << "✅ RPC static credentials configured for user: " << username << std::endl;
        std::cout << "   (iOS and mobile apps can now use username/password auth)" << std::endl;
    }
}

bool RpcAuth::validate_request(const std::string& authorization_header) const {
    if (authorization_header.empty()) {
        return false;
    }
    
    // Check for Basic authentication
    if (authorization_header.length() < 6 || authorization_header.substr(0, 6) != "Basic ") {
        return false;
    }
    
    // Decode Basic auth
    std::string username, password;
    if (!decode_basic_auth(authorization_header, username, password)) {
        return false;
    }
    
    // Trim provided credentials too
    while (!username.empty() && (username.back() == '\r' || username.back() == '\n')) {
        username.pop_back();
    }
    while (!password.empty() && (password.back() == '\r' || password.back() == '\n')) {
        password.pop_back();
    }
    
    // DUAL AUTH: Check against BOTH cookie AND static credentials
    std::string provided_creds = username + ":" + password;

    // METHOD 1: Check against cookie credentials (for desktop apps)
    if (!cookie_content_.empty()) {
        std::string expected_creds = cookie_content_;

        // Constant-time string comparison (prevent timing attacks)
        if (provided_creds.length() == expected_creds.length()) {
            int result = 0;
            for (size_t i = 0; i < provided_creds.length(); i++) {
                result |= (provided_creds[i] ^ expected_creds[i]);
            }

            if (result == 0) {
                return true;  // ✅ Cookie auth successful!
            }
        }
    }
    
    // METHOD 2: Check against static credentials (for iOS apps)
    if (!static_username_.empty() && !static_password_.empty()) {
        std::string static_creds = static_username_ + ":" + static_password_;
        
        // Constant-time string comparison (prevent timing attacks)
        if (provided_creds.length() == static_creds.length()) {
            int result = 0;
            for (size_t i = 0; i < provided_creds.length(); i++) {
                result |= (provided_creds[i] ^ static_creds[i]);
            }
            
            if (result == 0) {
                return true;  // ✅ Static auth successful!
            }
        }
    }
    
    return false;  // ❌ Both methods failed
}

bool RpcAuth::is_localhost_request(const std::string& client_ip) const {
    return client_ip == "127.0.0.1" || client_ip == "::1" || client_ip == "localhost";
}

std::string RpcAuth::generate_random_password(size_t length) {
    std::string password;
    password.reserve(length);
    
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    const size_t charset_size = sizeof(charset) - 1;
    
    for (size_t i = 0; i < length; i++) {
        uint32_t random_index = SecureRandom::GetUInt32() % charset_size;
        password += charset[random_index];
    }
    
    return password;
}

std::string RpcAuth::encode_basic_auth(const std::string& username, const std::string& password) {
    std::string credentials = username + ":" + password;
    return "Basic " + base64_encode(credentials);
}

bool RpcAuth::decode_basic_auth(const std::string& auth_header, std::string& username, std::string& password) const {
    if (auth_header.length() < 7 || auth_header.substr(0, 6) != "Basic ") {
        return false;
    }

    std::string encoded = auth_header.substr(6);
    // Trim any whitespace from encoded Base64 string
    encoded.erase(0, encoded.find_first_not_of(" \t\r\n"));
    encoded.erase(encoded.find_last_not_of(" \t\r\n") + 1);

    std::string decoded = base64_decode(encoded);
    
    size_t colon_pos = decoded.find(':');
    if (colon_pos == std::string::npos) {
        return false;
    }
    
    username = decoded.substr(0, colon_pos);
    password = decoded.substr(colon_pos + 1);
    
    return true;
}

std::string RpcAuth::base64_encode(const std::string& input) const {
    std::string result;
    int val = 0, valb = -6;
    
    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            result.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    
    if (valb > -6) {
        result.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    }
    
    while (result.size() % 4) {
        result.push_back('=');
    }
    
    return result;
}

std::string RpcAuth::base64_decode(const std::string& input) const {
    std::string result;
    int val = 0, valb = -8;
    
    for (unsigned char c : input) {
        if (c == '=') break;
        
        const char* pos = std::find(base64_chars, base64_chars + 64, c);
        if (pos == base64_chars + 64) continue; // Invalid character
        
        val = (val << 6) + (pos - base64_chars);
        valb += 6;
        if (valb >= 0) {
            result.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    
    return result;
}
