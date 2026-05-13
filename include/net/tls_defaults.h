#pragma once

#include <string>
#include <vector>

namespace dinero {
namespace net {

/**
 * TLS security modes for different environments
 */
enum class TLSMode {
    LOOPBACK_ONLY,  // Development: localhost only, self-signed certs OK
    PUBLIC,         // Production: public interfaces, proper CA certs required
    MTLS_REQUIRED   // Admin operations: mutual TLS authentication required
};

/**
 * TLS configuration defaults based on environment
 */
struct TLSConfig {
    TLSMode mode;
    std::string cert_file;
    std::string key_file;
    std::string ca_file;
    std::vector<std::string> allowed_ciphers;
    std::vector<std::string> alpn_protocols;
    bool require_client_cert;
    bool verify_peer;
    int min_tls_version;  // 1.2 = TLS 1.2, 1.3 = TLS 1.3
};

/**
 * TLS defaults manager
 */
class TLSDefaults {
public:
    /**
     * Get TLS configuration for development environment
     */
    static TLSConfig getDevelopmentConfig();
    
    /**
     * Get TLS configuration for production environment
     */
    static TLSConfig getProductionConfig();
    
    /**
     * Get TLS configuration for admin operations
     */
    static TLSConfig getAdminConfig();
    
    /**
     * Get TLS configuration based on environment variable
     */
    static TLSConfig getConfigForEnvironment();
    
    /**
     * Validate TLS configuration
     */
    static bool validateConfig(const TLSConfig& config);
    
    /**
     * Get recommended cipher suites for production
     */
    static std::vector<std::string> getProductionCiphers();
    
    /**
     * Get ALPN protocols for Dinero
     */
    static std::vector<std::string> getDineroALPN();
    
private:
    static std::string getEnvironmentType();
};

/**
 * TLS certificate utilities
 */
class TLSCertUtils {
public:
    /**
     * Generate self-signed certificate for development
     */
    static bool generateSelfSignedCert(const std::string& cert_file, 
                                     const std::string& key_file);
    
    /**
     * Validate certificate chain
     */
    static bool validateCertChain(const std::string& cert_file, 
                                const std::string& ca_file = "");
    
    /**
     * Check certificate expiration
     */
    static int getDaysUntilExpiry(const std::string& cert_file);
    
    /**
     * Verify certificate matches private key
     */
    static bool verifyCertKeyPair(const std::string& cert_file, 
                                const std::string& key_file);
};

} // namespace net
} // namespace dinero
