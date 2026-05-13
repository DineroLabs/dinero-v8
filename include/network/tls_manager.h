#pragma once

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <optional>
#include <functional>
#include <mutex>
#include <atomic>
#include <chrono>

// Forward declarations for OpenSSL types
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct x509_st X509;
typedef struct x509_store_ctx_st X509_STORE_CTX;
typedef struct evp_pkey_st EVP_PKEY;

namespace dinero {
namespace network {

/**
 * TLS security modes for daemon vNext
 */
enum class TLSMode {
    OFF,            // No TLS encryption (plaintext)
    LOOPBACK_ONLY,  // TLS only for loopback connections (127.0.0.1, ::1)
    PUBLIC          // TLS for all connections with optional mTLS
};

/**
 * TLS certificate information
 */
struct CertificateInfo {
    std::string subject;
    std::string issuer;
    std::string serial_number;
    std::chrono::system_clock::time_point not_before;
    std::chrono::system_clock::time_point not_after;
    std::string fingerprint_sha256;
    std::vector<std::string> san_dns_names;
    std::vector<std::string> san_ip_addresses;
    bool is_self_signed;
    bool is_ca;
    
    bool isValid() const;
    bool isExpired() const;
    std::string toString() const;
};

/**
 * TLS connection context
 */
class TLSConnection {
public:
    TLSConnection(SSL* ssl, bool is_server);
    ~TLSConnection();
    
    // Connection state
    bool isConnected() const;
    bool isHandshakeComplete() const;
    std::string getPeerAddress() const;
    
    // Certificate information
    std::optional<CertificateInfo> getPeerCertificate() const;
    std::optional<CertificateInfo> getOwnCertificate() const;
    
    // Data transfer
    int read(void* buffer, int size);
    int write(const void* buffer, int size);
    
    // TLS specific
    std::string getCipherSuite() const;
    std::string getProtocolVersion() const;
    bool isClientAuth() const;
    
    // Error handling
    std::string getLastError() const;

private:
    SSL* ssl_;
    bool is_server_;
    mutable std::mutex mutex_;
    
    friend class TLSManager;
};

/**
 * TLS configuration with safe defaults and ALPN support
 */
struct TLSConfig {
    TLSMode mode = TLSMode::LOOPBACK_ONLY;  // Safe default for development
    std::string cert_file;
    std::string key_file;
    std::string ca_file;
    std::string ca_path;
    bool require_client_cert = false;
    bool verify_peer = true;
    int verify_depth = 9;
    std::string cipher_list = "ECDHE+AESGCM:ECDHE+CHACHA20:DHE+AESGCM:DHE+CHACHA20:!aNULL:!MD5:!DSS";
    std::string cipher_suites = "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256";
    std::vector<std::string> protocols = {"TLSv1.2", "TLSv1.3"};
    
    // ALPN (Application-Layer Protocol Negotiation) support
    std::vector<std::string> alpn_protocols = {"din-jsonrpc/1", "din-ws/1", "h2", "http/1.1"};
    bool enable_alpn = true;
    
    // Safe defaults for different environments
    static TLSConfig forDevelopment() {
        TLSConfig config;
        config.mode = TLSMode::LOOPBACK_ONLY;
        config.require_client_cert = false;
        return config;
    }
    bool enable_ocsp = false;       // OCSP stapling
    bool enable_session_resumption = true;
    std::chrono::seconds session_timeout{300};
    
    // Certificate generation (for development)
    bool auto_generate_cert = false;
    std::string cert_subject = "/CN=dinero-daemon";
    std::vector<std::string> cert_san_dns;
    std::vector<std::string> cert_san_ips;
    
    // Validation
    bool validate() const;
    std::string toString() const;
};

/**
 * TLS Manager for daemon vNext
 * 
 * Provides TLS encryption with multiple security modes:
 * - OFF: No encryption (development only)
 * - LOOPBACK_ONLY: TLS for localhost connections only
 * - PUBLIC: Full TLS with optional mutual authentication
 */
class TLSManager {
public:
    using ClientCertValidator = std::function<bool(const CertificateInfo&)>;
    
    explicit TLSManager(const TLSConfig& config);
    ~TLSManager();
    
    /**
     * Initialize TLS subsystem
     */
    bool initialize();
    
    /**
     * Shutdown TLS subsystem
     */
    void shutdown();
    
    /**
     * Check if TLS is enabled for given address
     */
    bool isTLSRequired(const std::string& peer_address) const;
    
    /**
     * Create server TLS context
     */
    std::unique_ptr<TLSConnection> createServerConnection(int socket_fd);
    
    /**
     * Create client TLS context
     */
    std::unique_ptr<TLSConnection> createClientConnection(int socket_fd, 
                                                         const std::string& server_name = "");
    
    /**
     * Certificate management
     */
    bool loadCertificate(const std::string& cert_file, const std::string& key_file);
    bool generateSelfSignedCertificate(const std::string& subject,
                                     const std::vector<std::string>& san_dns = {},
                                     const std::vector<std::string>& san_ips = {});
    std::optional<CertificateInfo> getCertificateInfo() const;
    
    /**
     * CA and client certificate management
     */
    bool loadCACertificates(const std::string& ca_file, const std::string& ca_path = "");
    void setClientCertValidator(ClientCertValidator validator);
    
    /**
     * Configuration
     */
    TLSMode getMode() const { return config_.mode; }
    const TLSConfig& getConfig() const { return config_; }
    bool updateConfig(const TLSConfig& new_config);
    
    /**
     * Statistics and monitoring
     */
    struct TLSStats {
        uint64_t connections_created;
        uint64_t handshakes_completed;
        uint64_t handshake_failures;
        uint64_t cert_verification_failures;
        uint64_t bytes_encrypted;
        uint64_t bytes_decrypted;
        std::string active_cipher_suites;
    };
    
    TLSStats getStats() const;
    
    /**
     * Certificate validation
     */
    bool validateCertificateChain(const std::vector<std::string>& cert_chain) const;
    bool isCertificateTrusted(const CertificateInfo& cert) const;
    
    /**
     * Utility functions
     */
    static std::string getOpenSSLVersion();
    static std::vector<std::string> getSupportedCipherSuites();
    static bool isLoopbackAddress(const std::string& address);

private:
    TLSConfig config_;
    SSL_CTX* server_ctx_;
    SSL_CTX* client_ctx_;
    X509* certificate_;
    EVP_PKEY* private_key_;
    
    mutable std::mutex mutex_;
    std::atomic<bool> initialized_{false};
    
    // Statistics
    mutable std::atomic<uint64_t> connections_created_{0};
    mutable std::atomic<uint64_t> handshakes_completed_{0};
    mutable std::atomic<uint64_t> handshake_failures_{0};
    mutable std::atomic<uint64_t> cert_verification_failures_{0};
    mutable std::atomic<uint64_t> bytes_encrypted_{0};
    mutable std::atomic<uint64_t> bytes_decrypted_{0};
    
    // Client certificate validation
    ClientCertValidator client_cert_validator_;
    
    // Internal methods
    bool initializeOpenSSL();
    bool setupServerContext();
    bool setupClientContext();
    bool configureCipherSuites(SSL_CTX* ctx);
    bool configureProtocolVersions(SSL_CTX* ctx);
    
    // Certificate helpers
    bool loadCertificateFromFile(const std::string& cert_file);
    bool loadPrivateKeyFromFile(const std::string& key_file);
    X509* generateCertificate(const std::string& subject,
                             const std::vector<std::string>& san_dns,
                             const std::vector<std::string>& san_ips);
    EVP_PKEY* generatePrivateKey();
    
    // Validation callbacks
    static int verifyCertificateCallback(int preverify_ok, X509_STORE_CTX* ctx);
    static int clientCertCallback(SSL* ssl, X509** x509, EVP_PKEY** pkey);
    
    // Utility
    std::string getSSLError() const;
    void updateStats(bool handshake_success, bool cert_valid, size_t bytes_transferred);
};

/**
 * TLS-aware socket wrapper
 */
class SecureSocket {
public:
    SecureSocket(int socket_fd, std::unique_ptr<TLSConnection> tls_conn);
    ~SecureSocket();
    
    // Socket operations
    int read(void* buffer, size_t size);
    int write(const void* buffer, size_t size);
    void close();
    
    // TLS information
    bool isTLSEnabled() const { return tls_connection_ != nullptr; }
    std::optional<CertificateInfo> getPeerCertificate() const;
    std::string getSecurityInfo() const;
    
    // Raw socket access (for non-TLS operations)
    int getSocketFD() const { return socket_fd_; }

private:
    int socket_fd_;
    std::unique_ptr<TLSConnection> tls_connection_;
    bool closed_;
};

/**
 * TLS certificate store for trusted certificates
 */
class CertificateStore {
public:
    CertificateStore();
    ~CertificateStore();
    
    // Certificate management
    bool addTrustedCertificate(const std::string& cert_pem);
    bool addTrustedCertificateFile(const std::string& cert_file);
    bool removeTrustedCertificate(const std::string& fingerprint);
    
    // Validation
    bool isTrusted(const CertificateInfo& cert) const;
    std::vector<CertificateInfo> getTrustedCertificates() const;
    
    // Persistence
    bool saveToFile(const std::string& store_file) const;
    bool loadFromFile(const std::string& store_file);

private:
    std::vector<X509*> trusted_certs_;
    mutable std::mutex mutex_;
    
    void cleanup();
};

/**
 * Global TLS manager instance
 */
extern std::unique_ptr<TLSManager> g_tls_manager;

/**
 * Initialize TLS system
 */
bool InitializeTLS(const TLSConfig& config);

/**
 * Shutdown TLS system
 */
void ShutdownTLS();

/**
 * Utility functions for TLS mode detection
 */
bool ShouldUseTLS(const std::string& peer_address);
TLSMode GetTLSModeForAddress(const std::string& peer_address);

} // namespace network
} // namespace dinero
