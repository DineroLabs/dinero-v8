#pragma once

#include <string>
#include <filesystem>

// RPC cookie authentication system
// Generates and validates .cookie files for secure RPC access

class RpcAuth {
public:
    RpcAuth(const std::string& data_dir);
    
    // Cookie management
    bool generate_cookie();
    bool load_cookie();
    std::string get_cookie_path() const { return cookie_path_; }
    
    // Static credentials (for mobile apps)
    void set_static_credentials(const std::string& username, const std::string& password);
    bool has_static_credentials() const { return !static_username_.empty(); }
    
    // Authentication
    bool validate_request(const std::string& authorization_header) const;
    bool is_localhost_request(const std::string& client_ip) const;
    
    // Security settings
    void set_rpc_allow_ip(const std::string& allowed_ip) { rpc_allow_ip_ = allowed_ip; }
    // Compatibility flag retained for callers, but it no longer disables auth.
    void set_dev_mode(bool dev_mode) { dev_mode_ = dev_mode; }
    
private:
    std::string data_dir_;
    std::string cookie_path_;
    std::string cookie_content_;
    std::string static_username_;    // NEW: Static credentials for iOS
    std::string static_password_;    // NEW: Static credentials for iOS
    std::string rpc_allow_ip_;
    bool dev_mode_;
    
    // Utility functions
    std::string generate_random_password(size_t length = 32);
    std::string encode_basic_auth(const std::string& username, const std::string& password);
    bool decode_basic_auth(const std::string& auth_header, std::string& username, std::string& password) const;
    std::string base64_encode(const std::string& input) const;
    std::string base64_decode(const std::string& input) const;
};
