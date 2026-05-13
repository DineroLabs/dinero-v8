#include "network/tls_manager.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include "compat/net_compat.h"

namespace dinero {
namespace network {

std::unique_ptr<TLSManager> g_tls_manager;

// CertificateInfo implementation
bool CertificateInfo::isValid() const {
    auto now = std::chrono::system_clock::now();
    return now >= not_before && now <= not_after;
}

bool CertificateInfo::isExpired() const {
    auto now = std::chrono::system_clock::now();
    return now > not_after;
}

std::string CertificateInfo::toString() const {
    std::ostringstream oss;
    oss << "Subject: " << subject << "\n";
    oss << "Issuer: " << issuer << "\n";
    oss << "Serial: " << serial_number << "\n";
    oss << "Valid: " << (isValid() ? "Yes" : "No") << "\n";
    oss << "Self-signed: " << (is_self_signed ? "Yes" : "No") << "\n";
    oss << "Fingerprint: " << fingerprint_sha256;
    return oss.str();
}

// TLSConnection implementation
TLSConnection::TLSConnection(SSL* ssl, bool is_server) 
    : ssl_(ssl), is_server_(is_server) {}

TLSConnection::~TLSConnection() {
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
    }
}

bool TLSConnection::isConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ssl_ && SSL_get_shutdown(ssl_) == 0;
}

bool TLSConnection::isHandshakeComplete() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ssl_ && SSL_is_init_finished(ssl_);
}

std::string TLSConnection::getPeerAddress() const {
    // TODO: Extract peer address from SSL connection
    return "unknown";
}

std::optional<CertificateInfo> TLSConnection::getPeerCertificate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!ssl_) return std::nullopt;
    
    X509* cert = SSL_get_peer_certificate(ssl_);
    if (!cert) return std::nullopt;
    
    CertificateInfo info;
    
    // Extract subject
    char* subject_str = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
    if (subject_str) {
        info.subject = subject_str;
        OPENSSL_free(subject_str);
    }
    
    // Extract issuer
    char* issuer_str = X509_NAME_oneline(X509_get_issuer_name(cert), nullptr, 0);
    if (issuer_str) {
        info.issuer = issuer_str;
        OPENSSL_free(issuer_str);
    }
    
    // Check if self-signed
    info.is_self_signed = (X509_check_issued(cert, cert) == X509_V_OK);
    
    X509_free(cert);
    return info;
}

int TLSConnection::read(void* buffer, int size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ssl_) return -1;
    
    int result = SSL_read(ssl_, buffer, size);
    if (result <= 0) {
        int error = SSL_get_error(ssl_, result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            return -1;
        }
    }
    return result;
}

int TLSConnection::write(const void* buffer, int size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ssl_) return -1;
    
    int result = SSL_write(ssl_, buffer, size);
    if (result <= 0) {
        int error = SSL_get_error(ssl_, result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            return -1;
        }
    }
    return result;
}

std::string TLSConnection::getCipherSuite() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ssl_) return "";
    
    const char* cipher = SSL_get_cipher_name(ssl_);
    return cipher ? cipher : "";
}

std::string TLSConnection::getProtocolVersion() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ssl_) return "";
    
    const char* version = SSL_get_version(ssl_);
    return version ? version : "";
}

std::string TLSConnection::getLastError() const {
    unsigned long error = ERR_get_error();
    if (error == 0) return "";
    
    char buffer[256];
    ERR_error_string_n(error, buffer, sizeof(buffer));
    return std::string(buffer);
}

// TLSConfig validation
bool TLSConfig::validate() const {
    if (mode == TLSMode::OFF) return true;
    
    // For TLS modes, we need either cert files or auto-generation
    if (!auto_generate_cert && (cert_file.empty() || key_file.empty())) {
        return false;
    }
    
    // Validate protocol versions
    if (min_protocol != "TLSv1.2" && min_protocol != "TLSv1.3") {
        return false;
    }
    
    return true;
}

std::string TLSConfig::toString() const {
    std::ostringstream oss;
    oss << "TLS Mode: ";
    switch (mode) {
        case TLSMode::OFF: oss << "OFF"; break;
        case TLSMode::LOOPBACK_ONLY: oss << "LOOPBACK_ONLY"; break;
        case TLSMode::PUBLIC: oss << "PUBLIC"; break;
    }
    oss << "\n";
    oss << "Certificate: " << cert_file << "\n";
    oss << "Private Key: " << key_file << "\n";
    oss << "Require Client Cert: " << (require_client_cert ? "Yes" : "No") << "\n";
    oss << "Auto Generate Cert: " << (auto_generate_cert ? "Yes" : "No");
    return oss.str();
}

// TLSManager implementation
TLSManager::TLSManager(const TLSConfig& config) 
    : config_(config), server_ctx_(nullptr), client_ctx_(nullptr), 
      certificate_(nullptr), private_key_(nullptr) {}

TLSManager::~TLSManager() {
    shutdown();
}

bool TLSManager::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (initialized_.load()) return true;
    
    if (config_.mode == TLSMode::OFF) {
        initialized_.store(true);
        return true;
    }
    
    if (!initializeOpenSSL()) {
        return false;
    }
    
    if (!setupServerContext() || !setupClientContext()) {
        return false;
    }
    
    // Load or generate certificate
    if (config_.auto_generate_cert) {
        if (!generateSelfSignedCertificate(config_.cert_subject, 
                                         config_.cert_san_dns, 
                                         config_.cert_san_ips)) {
            return false;
        }
    } else {
        if (!loadCertificate(config_.cert_file, config_.key_file)) {
            return false;
        }
    }
    
    initialized_.store(true);
    return true;
}

void TLSManager::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!initialized_.load()) return;
    
    if (server_ctx_) {
        SSL_CTX_free(server_ctx_);
        server_ctx_ = nullptr;
    }
    
    if (client_ctx_) {
        SSL_CTX_free(client_ctx_);
        client_ctx_ = nullptr;
    }
    
    if (certificate_) {
        X509_free(certificate_);
        certificate_ = nullptr;
    }
    
    if (private_key_) {
        EVP_PKEY_free(private_key_);
        private_key_ = nullptr;
    }
    
    initialized_.store(false);
}

bool TLSManager::isTLSRequired(const std::string& peer_address) const {
    switch (config_.mode) {
        case TLSMode::OFF:
            return false;
        case TLSMode::LOOPBACK_ONLY:
            return isLoopbackAddress(peer_address);
        case TLSMode::PUBLIC:
            return true;
    }
    return false;
}

std::unique_ptr<TLSConnection> TLSManager::createServerConnection(int socket_fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!initialized_.load() || !server_ctx_) return nullptr;
    
    SSL* ssl = SSL_new(server_ctx_);
    if (!ssl) return nullptr;
    
    if (SSL_set_fd(ssl, socket_fd) != 1) {
        SSL_free(ssl);
        return nullptr;
    }
    
    connections_created_++;
    return std::make_unique<TLSConnection>(ssl, true);
}

std::unique_ptr<TLSConnection> TLSManager::createClientConnection(int socket_fd, const std::string& server_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!initialized_.load() || !client_ctx_) return nullptr;
    
    SSL* ssl = SSL_new(client_ctx_);
    if (!ssl) return nullptr;
    
    if (!server_name.empty()) {
        SSL_set_tlsext_host_name(ssl, server_name.c_str());
    }
    
    if (SSL_set_fd(ssl, socket_fd) != 1) {
        SSL_free(ssl);
        return nullptr;
    }
    
    connections_created_++;
    return std::make_unique<TLSConnection>(ssl, false);
}

bool TLSManager::loadCertificate(const std::string& cert_file, const std::string& key_file) {
    return loadCertificateFromFile(cert_file) && loadPrivateKeyFromFile(key_file);
}

bool TLSManager::generateSelfSignedCertificate(const std::string& subject,
                                             const std::vector<std::string>& san_dns,
                                             const std::vector<std::string>& san_ips) {
    private_key_ = generatePrivateKey();
    if (!private_key_) return false;
    
    certificate_ = generateCertificate(subject, san_dns, san_ips);
    if (!certificate_) return false;
    
    // Set certificate and key in SSL contexts
    if (server_ctx_) {
        SSL_CTX_use_certificate(server_ctx_, certificate_);
        SSL_CTX_use_PrivateKey(server_ctx_, private_key_);
    }
    
    if (client_ctx_) {
        SSL_CTX_use_certificate(client_ctx_, certificate_);
        SSL_CTX_use_PrivateKey(client_ctx_, private_key_);
    }
    
    return true;
}

TLSManager::TLSStats TLSManager::getStats() const {
    TLSStats stats;
    stats.connections_created = connections_created_.load();
    stats.handshakes_completed = handshakes_completed_.load();
    stats.handshake_failures = handshake_failures_.load();
    stats.cert_verification_failures = cert_verification_failures_.load();
    stats.bytes_encrypted = bytes_encrypted_.load();
    stats.bytes_decrypted = bytes_decrypted_.load();
    return stats;
}

std::string TLSManager::getOpenSSLVersion() {
    return OpenSSL_version(OPENSSL_VERSION);
}

bool TLSManager::isLoopbackAddress(const std::string& address) {
    // Check IPv4 loopback
    if (address == "127.0.0.1" || address.substr(0, 4) == "127.") {
        return true;
    }
    
    // Check IPv6 loopback
    if (address == "::1" || address == "localhost") {
        return true;
    }
    
    return false;
}

// Private methods
bool TLSManager::initializeOpenSSL() {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    
    // Seed random number generator
    if (RAND_load_file("/dev/urandom", 32) != 32) {
        // Fallback for systems without /dev/urandom
        RAND_poll();
    }
    
    return true;
}

bool TLSManager::setupServerContext() {
    server_ctx_ = SSL_CTX_new(TLS_server_method());
    if (!server_ctx_) return false;
    
    // Configure security options
    SSL_CTX_set_options(server_ctx_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
    
    // Set minimum protocol version
    if (config_.min_protocol == "TLSv1.3") {
        SSL_CTX_set_min_proto_version(server_ctx_, TLS1_3_VERSION);
    } else {
        SSL_CTX_set_min_proto_version(server_ctx_, TLS1_2_VERSION);
    }
    
    // Configure cipher suites
    configureCipherSuites(server_ctx_);
    
    // Set client certificate verification if required
    if (config_.require_client_cert) {
        SSL_CTX_set_verify(server_ctx_, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    }
    
    return true;
}

bool TLSManager::setupClientContext() {
    client_ctx_ = SSL_CTX_new(TLS_client_method());
    if (!client_ctx_) return false;
    
    // Configure security options
    SSL_CTX_set_options(client_ctx_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
    
    // Set minimum protocol version
    if (config_.min_protocol == "TLSv1.3") {
        SSL_CTX_set_min_proto_version(client_ctx_, TLS1_3_VERSION);
    } else {
        SSL_CTX_set_min_proto_version(client_ctx_, TLS1_2_VERSION);
    }
    
    // Configure cipher suites
    configureCipherSuites(client_ctx_);
    
    // Set server certificate verification
    SSL_CTX_set_verify(client_ctx_, SSL_VERIFY_PEER, nullptr);
    
    return true;
}

bool TLSManager::configureCipherSuites(SSL_CTX* ctx) {
    if (config_.cipher_suites.empty()) {
        // Use secure default cipher suites
        const char* cipher_list = "ECDHE+AESGCM:ECDHE+CHACHA20:DHE+AESGCM:DHE+CHACHA20:!aNULL:!MD5:!DSS";
        return SSL_CTX_set_cipher_list(ctx, cipher_list) == 1;
    } else {
        // Use configured cipher suites
        std::string cipher_list;
        for (size_t i = 0; i < config_.cipher_suites.size(); ++i) {
            if (i > 0) cipher_list += ":";
            cipher_list += config_.cipher_suites[i];
        }
        return SSL_CTX_set_cipher_list(ctx, cipher_list.c_str()) == 1;
    }
}

bool TLSManager::loadCertificateFromFile(const std::string& cert_file) {
    FILE* fp = fopen(cert_file.c_str(), "r");
    if (!fp) return false;
    
    certificate_ = PEM_read_X509(fp, nullptr, nullptr, nullptr);
    fclose(fp);
    
    return certificate_ != nullptr;
}

bool TLSManager::loadPrivateKeyFromFile(const std::string& key_file) {
    FILE* fp = fopen(key_file.c_str(), "r");
    if (!fp) return false;
    
    private_key_ = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
    fclose(fp);
    
    return private_key_ != nullptr;
}

EVP_PKEY* TLSManager::generatePrivateKey() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!ctx) return nullptr;
    
    if (EVP_PKEY_keygen_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return nullptr;
    }
    
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return nullptr;
    }
    
    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return nullptr;
    }
    
    EVP_PKEY_CTX_free(ctx);
    return pkey;
}

X509* TLSManager::generateCertificate(const std::string& subject,
                                    const std::vector<std::string>& san_dns,
                                    const std::vector<std::string>& san_ips) {
    X509* cert = X509_new();
    if (!cert) return nullptr;
    
    // Set version (X.509 v3)
    X509_set_version(cert, 2);
    
    // Set serial number
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    
    // Set validity period (1 year)
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 365 * 24 * 3600);
    
    // Set public key
    X509_set_pubkey(cert, private_key_);
    
    // Set subject name
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, 
                              (unsigned char*)subject.c_str(), -1, -1, 0);
    
    // Set issuer name (self-signed)
    X509_set_issuer_name(cert, name);
    
    // Add SAN extension if needed
    if (!san_dns.empty() || !san_ips.empty()) {
        // TODO: Add SAN extension implementation
    }
    
    // Sign the certificate
    if (X509_sign(cert, private_key_, EVP_sha256()) == 0) {
        X509_free(cert);
        return nullptr;
    }
    
    return cert;
}

// Global functions
bool InitializeTLS(const TLSConfig& config) {
    g_tls_manager = std::make_unique<TLSManager>(config);
    return g_tls_manager->initialize();
}

void ShutdownTLS() {
    if (g_tls_manager) {
        g_tls_manager->shutdown();
        g_tls_manager.reset();
    }
}

bool ShouldUseTLS(const std::string& peer_address) {
    if (!g_tls_manager) return false;
    return g_tls_manager->isTLSRequired(peer_address);
}

TLSMode GetTLSModeForAddress(const std::string& peer_address) {
    if (!g_tls_manager) return TLSMode::OFF;
    
    TLSMode mode = g_tls_manager->getMode();
    if (mode == TLSMode::LOOPBACK_ONLY && !TLSManager::isLoopbackAddress(peer_address)) {
        return TLSMode::OFF;
    }
    
    return mode;
}

} // namespace network
} // namespace dinero
