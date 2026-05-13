#include "net/tls_defaults.h"
#include <cstdlib>
#include <iostream>
#include <fstream>

namespace dinero {
namespace net {

TLSConfig TLSDefaults::getDevelopmentConfig() {
    return TLSConfig{
        .mode = TLSMode::LOOPBACK_ONLY,
        .cert_file = "/tmp/dinero-dev.crt",
        .key_file = "/tmp/dinero-dev.key", 
        .ca_file = "",
        .allowed_ciphers = {
            "ECDHE-RSA-AES256-GCM-SHA384",
            "ECDHE-RSA-AES128-GCM-SHA256",
            "AES256-GCM-SHA384",
            "AES128-GCM-SHA256"
        },
        .alpn_protocols = getDineroALPN(),
        .require_client_cert = false,
        .verify_peer = false,
        .min_tls_version = 12  // TLS 1.2
    };
}

TLSConfig TLSDefaults::getProductionConfig() {
    return TLSConfig{
        .mode = TLSMode::PUBLIC,
        .cert_file = "/etc/dinero/tls/server.crt",
        .key_file = "/etc/dinero/tls/server.key",
        .ca_file = "/etc/dinero/tls/ca.crt",
        .allowed_ciphers = getProductionCiphers(),
        .alpn_protocols = getDineroALPN(),
        .require_client_cert = false,
        .verify_peer = true,
        .min_tls_version = 13  // TLS 1.3 required for production
    };
}

TLSConfig TLSDefaults::getAdminConfig() {
    return TLSConfig{
        .mode = TLSMode::MTLS_REQUIRED,
        .cert_file = "/etc/dinero/tls/admin-server.crt",
        .key_file = "/etc/dinero/tls/admin-server.key",
        .ca_file = "/etc/dinero/tls/admin-ca.crt",
        .allowed_ciphers = getProductionCiphers(),
        .alpn_protocols = {"din-admin/1"},
        .require_client_cert = true,
        .verify_peer = true,
        .min_tls_version = 13  // TLS 1.3 required for admin
    };
}

TLSConfig TLSDefaults::getConfigForEnvironment() {
    std::string env_type = getEnvironmentType();
    
    if (env_type == "development" || env_type == "dev") {
        return getDevelopmentConfig();
    } else if (env_type == "admin") {
        return getAdminConfig();
    } else {
        // Default to production for safety
        return getProductionConfig();
    }
}

bool TLSDefaults::validateConfig(const TLSConfig& config) {
    // Check certificate files exist
    std::ifstream cert_file(config.cert_file);
    if (!cert_file.good()) {
        std::cerr << "TLS certificate file not found: " << config.cert_file << std::endl;
        return false;
    }
    
    std::ifstream key_file(config.key_file);
    if (!key_file.good()) {
        std::cerr << "TLS key file not found: " << config.key_file << std::endl;
        return false;
    }
    
    // Validate cert/key pair
    if (!TLSCertUtils::verifyCertKeyPair(config.cert_file, config.key_file)) {
        std::cerr << "TLS certificate and key do not match" << std::endl;
        return false;
    }
    
    // Check CA file if specified
    if (!config.ca_file.empty()) {
        std::ifstream ca_file(config.ca_file);
        if (!ca_file.good()) {
            std::cerr << "TLS CA file not found: " << config.ca_file << std::endl;
            return false;
        }
        
        if (!TLSCertUtils::validateCertChain(config.cert_file, config.ca_file)) {
            std::cerr << "TLS certificate chain validation failed" << std::endl;
            return false;
        }
    }
    
    // Check certificate expiration
    int days_until_expiry = TLSCertUtils::getDaysUntilExpiry(config.cert_file);
    if (days_until_expiry < 30) {
        std::cerr << "WARNING: TLS certificate expires in " << days_until_expiry << " days" << std::endl;
        if (days_until_expiry <= 0) {
            return false;
        }
    }
    
    // Validate TLS version
    if (config.min_tls_version < 12) {
        std::cerr << "TLS version too low: minimum TLS 1.2 required" << std::endl;
        return false;
    }
    
    // Production safety checks
    if (config.mode == TLSMode::PUBLIC || config.mode == TLSMode::MTLS_REQUIRED) {
        if (config.min_tls_version < 13) {
            std::cerr << "WARNING: Production should use TLS 1.3" << std::endl;
        }
        
        if (!config.verify_peer && config.mode == TLSMode::PUBLIC) {
            std::cerr << "WARNING: Production should verify peer certificates" << std::endl;
        }
        
        if (!config.require_client_cert && config.mode == TLSMode::MTLS_REQUIRED) {
            std::cerr << "ERROR: mTLS mode requires client certificates" << std::endl;
            return false;
        }
    }
    
    return true;
}

std::vector<std::string> TLSDefaults::getProductionCiphers() {
    return {
        // TLS 1.3 cipher suites (preferred)
        "TLS_AES_256_GCM_SHA384",
        "TLS_CHACHA20_POLY1305_SHA256",
        "TLS_AES_128_GCM_SHA256",
        
        // TLS 1.2 cipher suites (fallback)
        "ECDHE-RSA-AES256-GCM-SHA384",
        "ECDHE-RSA-CHACHA20-POLY1305",
        "ECDHE-RSA-AES128-GCM-SHA256",
        "ECDHE-ECDSA-AES256-GCM-SHA384",
        "ECDHE-ECDSA-CHACHA20-POLY1305",
        "ECDHE-ECDSA-AES128-GCM-SHA256"
    };
}

std::vector<std::string> TLSDefaults::getDineroALPN() {
    return {
        "din-jsonrpc/1",  // JSON-RPC over TLS
        "din-ws/1",       // WebSocket over TLS
        "h2",             // HTTP/2
        "http/1.1"        // HTTP/1.1 fallback
    };
}

std::string TLSDefaults::getEnvironmentType() {
    const char* env = std::getenv("DINERO_TLS_MODE");
    if (env) {
        return std::string(env);
    }
    
    // Check other environment indicators
    env = std::getenv("NODE_ENV");
    if (env) {
        return std::string(env);
    }
    
    env = std::getenv("ENVIRONMENT");
    if (env) {
        return std::string(env);
    }
    
    // Default to production for safety
    return "production";
}

// TLSCertUtils implementation
bool TLSCertUtils::generateSelfSignedCert(const std::string& cert_file, 
                                        const std::string& key_file) {
    // Generate self-signed certificate for development
    std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + key_file + 
                     " -out " + cert_file + 
                     " -days 365 -nodes -subj '/CN=localhost/O=Dinero Dev'";
    
    int result = std::system(cmd.c_str());
    return result == 0;
}

bool TLSCertUtils::validateCertChain(const std::string& cert_file, 
                                   const std::string& ca_file) {
    std::string cmd = "openssl verify";
    if (!ca_file.empty()) {
        cmd += " -CAfile " + ca_file;
    }
    cmd += " " + cert_file + " >/dev/null 2>&1";
    
    int result = std::system(cmd.c_str());
    return result == 0;
}

int TLSCertUtils::getDaysUntilExpiry(const std::string& cert_file) {
    std::string cmd = "openssl x509 -in " + cert_file + 
                     " -noout -dates | grep 'notAfter' | cut -d= -f2";
    
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return -1;
    
    char buffer[256];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }
    pclose(pipe);
    
    // Parse date and calculate days until expiry
    // This is a simplified implementation
    // In production, use proper date parsing library
    return 365; // Placeholder
}

bool TLSCertUtils::verifyCertKeyPair(const std::string& cert_file, 
                                   const std::string& key_file) {
    // Check if certificate and key match
    std::string cert_cmd = "openssl x509 -in " + cert_file + " -noout -modulus | md5sum";
    std::string key_cmd = "openssl rsa -in " + key_file + " -noout -modulus | md5sum";
    
    FILE* cert_pipe = popen(cert_cmd.c_str(), "r");
    FILE* key_pipe = popen(key_cmd.c_str(), "r");
    
    if (!cert_pipe || !key_pipe) {
        if (cert_pipe) pclose(cert_pipe);
        if (key_pipe) pclose(key_pipe);
        return false;
    }
    
    char cert_hash[64], key_hash[64];
    bool cert_ok = fgets(cert_hash, sizeof(cert_hash), cert_pipe) != nullptr;
    bool key_ok = fgets(key_hash, sizeof(key_hash), key_pipe) != nullptr;
    
    pclose(cert_pipe);
    pclose(key_pipe);
    
    if (!cert_ok || !key_ok) return false;
    
    return std::string(cert_hash) == std::string(key_hash);
}

} // namespace net
} // namespace dinero
