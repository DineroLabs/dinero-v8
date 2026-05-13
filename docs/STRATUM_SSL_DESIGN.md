# Stratum V1 + SSL/TLS Implementation Design

## Overview

Add TLS/SSL encryption to Stratum V1 mining server to provide secure communication between miners and pool servers. This protects credentials, mining work, and share submissions from eavesdropping and man-in-the-middle attacks.

## Goals

1. **Security**: Encrypt all Stratum traffic using TLS 1.2+
2. **Backward Compatibility**: Support both plain TCP (port 3333) and SSL (port 3334)
3. **Performance**: Minimal overhead (<5% latency increase)
4. **Certificate Management**: Auto-generate self-signed certs for testing, support custom certs for production
5. **Miner Compatibility**: Work with standard SSL-capable miners (cgminer, bfgminer, etc.)

## Architecture

### Dual-Port Design

```
Port 3333 (Plain TCP)    Port 3334 (SSL/TLS)
       |                         |
       v                         v
  StratumServer            StratumServer
  (no encryption)         (SSL wrapper)
       |                         |
       +-------------------------+
                  |
            handleClient()
            (same logic)
```

### SSL Context Lifecycle

```cpp
1. Initialization (daemon startup):
   - SSL_library_init()
   - Create SSL_CTX
   - Load certificate + private key
   - Set cipher suites

2. Per-Connection (client connect):
   - SSL_new(ctx)
   - SSL_set_fd(ssl, client_socket)
   - SSL_accept() - TLS handshake
   - Wrap socket I/O with SSL_read/SSL_write

3. Cleanup (shutdown):
   - SSL_free(ssl) per connection
   - SSL_CTX_free(ctx) on shutdown
```

## Implementation Plan

### Phase 1: SSL Context Initialization

**File:** `src/stratum_bridge/stratum_server_complete.cpp`

```cpp
class StratumServer {
private:
    SSL_CTX* ssl_ctx_ = nullptr;  // OpenSSL context
    bool ssl_enabled_ = false;
    std::string cert_path_;
    std::string key_path_;

public:
    // Initialize SSL context
    bool initSSL(const std::string& cert_path, const std::string& key_path);

    // Generate self-signed certificate if none exists
    bool generateSelfSignedCert(const std::string& cert_path, const std::string& key_path);
};
```

**Implementation Steps:**
1. Add OpenSSL headers: `<openssl/ssl.h>`, `<openssl/err.h>`
2. Initialize OpenSSL library in constructor
3. Create SSL_CTX with `TLS_server_method()`
4. Set minimum TLS version: `SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION)`
5. Load certificate and private key
6. Set cipher suites (strong ciphers only)

### Phase 2: SSL Socket Wrapper

**File:** `include/stratum_bridge/ssl_socket.h` (new file)

```cpp
namespace dinero {

/**
 * SSL Socket Wrapper
 *
 * Wraps raw TCP socket with OpenSSL SSL layer.
 * Provides transparent encryption/decryption.
 */
class SSLSocket {
public:
    SSLSocket(int socket_fd, SSL_CTX* ctx);
    ~SSLSocket();

    // Perform SSL handshake
    bool accept();

    // Read/write with SSL
    ssize_t read(void* buf, size_t count);
    ssize_t write(const void* buf, size_t count);

    // Check connection state
    bool isConnected() const;
    std::string getError() const;

private:
    int socket_fd_;
    SSL* ssl_;
    bool connected_;
};

} // namespace dinero
```

### Phase 3: Stratum Server SSL Integration

**Modify:** `src/stratum_bridge/stratum_server_complete.cpp`

```cpp
void StratumServer::handleClient(int client_socket, struct sockaddr_in client_addr) {
    // If SSL enabled, wrap socket
    std::unique_ptr<SSLSocket> ssl_socket;
    if (ssl_enabled_) {
        ssl_socket = std::make_unique<SSLSocket>(client_socket, ssl_ctx_);
        if (!ssl_socket->accept()) {
            g_logger.error("[StratumServer] SSL handshake failed: " + ssl_socket->getError());
            close(client_socket);
            return;
        }
        g_logger.info("[StratumServer] SSL handshake successful");
    }

    // Read/write using SSL wrapper if enabled
    auto read_fn = [&](void* buf, size_t count) -> ssize_t {
        return ssl_socket ? ssl_socket->read(buf, count) : recv(client_socket, buf, count, 0);
    };

    auto write_fn = [&](const void* buf, size_t count) -> ssize_t {
        return ssl_socket ? ssl_socket->write(buf, count) : send(client_socket, buf, count, 0);
    };

    // Use read_fn/write_fn for all I/O
    // ... existing Stratum protocol logic ...
}
```

### Phase 4: CLI Configuration

**Modify:** `src/daemon/main.cpp`

```cpp
// Add CLI flags
--stratum-ssl                    Enable SSL on Stratum server
--stratum-ssl-port=<port>        SSL port (default: 3334)
--stratum-ssl-cert=<path>        Path to SSL certificate
--stratum-ssl-key=<path>         Path to private key
```

**Auto-Generation:**
- If `--stratum-ssl` enabled but no cert/key provided:
  - Generate self-signed cert at `~/.dinero/stratum_cert.pem`
  - Generate private key at `~/.dinero/stratum_key.pem`
  - Log warning: "Using self-signed certificate (NOT suitable for production)"

### Phase 5: Certificate Generation

**File:** `src/stratum_bridge/ssl_cert_generator.cpp` (new file)

```cpp
bool generateSelfSignedCert(const std::string& cert_path, const std::string& key_path) {
    // Generate RSA 2048-bit key
    EVP_PKEY* pkey = EVP_RSA_gen(2048);

    // Create X509 certificate
    X509* cert = X509_new();
    X509_set_version(cert, 2);  // X509v3

    // Set serial number
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);

    // Set validity (1 year)
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 31536000L);  // 365 days

    // Set subject/issuer
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char*)"DineroCoin Stratum Server", -1, -1, 0);
    X509_set_issuer_name(cert, name);

    // Set public key
    X509_set_pubkey(cert, pkey);

    // Sign certificate
    X509_sign(cert, pkey, EVP_sha256());

    // Write to disk
    FILE* cert_file = fopen(cert_path.c_str(), "wb");
    PEM_write_X509(cert_file, cert);
    fclose(cert_file);

    FILE* key_file = fopen(key_path.c_str(), "wb");
    PEM_write_PrivateKey(key_file, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    fclose(key_file);

    EVP_PKEY_free(pkey);
    X509_free(cert);

    return true;
}
```

## Security Considerations

### Cipher Suites

**Recommended Configuration:**
```cpp
SSL_CTX_set_cipher_list(ctx, "HIGH:!aNULL:!MD5:!RC4");
```

**Strong ciphers only:**
- TLS_AES_256_GCM_SHA384
- TLS_CHACHA20_POLY1305_SHA256
- TLS_AES_128_GCM_SHA256

**Disabled:**
- SSLv2, SSLv3 (vulnerable)
- TLS 1.0, TLS 1.1 (deprecated)
- MD5, RC4, DES (weak algorithms)
- Anonymous ciphers (no authentication)

### Certificate Validation

**Server-Side (Stratum server):**
- Load certificate and private key
- Verify key matches certificate
- Check certificate expiry

**Client-Side (Miners):**
- Miners should verify server certificate (optional for pools, recommended for solo mining)
- For self-signed certs: miner must explicitly trust cert (cgminer `--cert` flag)

### Perfect Forward Secrecy (PFS)

Enable ephemeral key exchange:
```cpp
SSL_CTX_set_options(ctx, SSL_OP_SINGLE_DH_USE | SSL_OP_SINGLE_ECDH_USE);
```

Ensures past sessions can't be decrypted if private key is compromised.

## Performance Analysis

### Latency Impact

**Handshake Overhead:**
- TLS handshake: ~50-100ms (one-time per connection)
- Negligible for long-lived mining connections (hours/days)

**Per-Message Overhead:**
- Encryption/decryption: ~0.1-0.5ms per JSON-RPC message
- Stratum messages: ~200 bytes → minimal impact
- Share submissions: <1% latency increase

**CPU Impact:**
- AES-NI hardware acceleration: <1% CPU overhead
- RSA handshake: ~0.5% CPU per new connection
- Negligible for established connections (symmetric encryption only)

### Memory Impact

**Per-Connection:**
- SSL context: ~8 KB
- SSL buffers: ~16 KB
- Total: ~24 KB per SSL connection

**1000 concurrent miners:**
- Total SSL overhead: ~24 MB (negligible)

## Testing Strategy

### Phase 1: Local Testing

**Test Script:** `test_stratum_ssl.py`

```python
import ssl
import socket
import json

# Create SSL context
ssl_context = ssl.create_default_context()
ssl_context.check_hostname = False
ssl_context.verify_mode = ssl.CERT_NONE  # Self-signed cert

# Connect to SSL Stratum
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
ssl_sock = ssl_context.wrap_socket(sock)
ssl_sock.connect(("127.0.0.1", 3334))

# Test Stratum handshake over SSL
request = {"id": 1, "method": "mining.subscribe", "params": ["test"]}
ssl_sock.sendall(json.dumps(request).encode() + b"\n")
response = ssl_sock.recv(4096)
print(response)
```

### Phase 2: Miner Compatibility

**cgminer:**
```bash
cgminer -o stratum+ssl://127.0.0.1:3334 -u testworker -p x --cert ~/.dinero/stratum_cert.pem
```

**bfgminer:**
```bash
bfgminer -o stratum+ssl://127.0.0.1:3334 -u testworker -p x --cert ~/.dinero/stratum_cert.pem
```

### Phase 3: Security Audit

**Tools:**
- `openssl s_client -connect 127.0.0.1:3334 -tls1_2` - Test TLS handshake
- `nmap --script ssl-enum-ciphers -p 3334 127.0.0.1` - Enumerate supported ciphers
- `ssllabs-scan` - SSL configuration analysis

## Configuration Examples

### Development (Self-Signed)

```bash
# Auto-generate self-signed cert
./build/bin/dinerod --stratum --stratum-ssl

# Logs:
[StratumServer] SSL enabled (port 3334)
[StratumServer] Generated self-signed certificate: ~/.dinero/stratum_cert.pem
[StratumServer] ⚠️  WARNING: Self-signed certificate (NOT for production)
```

### Production (Custom Cert)

```bash
# Use Let's Encrypt or commercial cert
./build/bin/dinerod --stratum --stratum-ssl \
  --stratum-ssl-cert=/etc/letsencrypt/live/pool.dinero-coin.com/fullchain.pem \
  --stratum-ssl-key=/etc/letsencrypt/live/pool.dinero-coin.com/privkey.pem
```

## File Structure

```
include/stratum_bridge/
  ssl_socket.h              # SSL socket wrapper
  ssl_cert_generator.h      # Certificate generation

src/stratum_bridge/
  ssl_socket.cpp            # SSL socket implementation
  ssl_cert_generator.cpp    # Certificate generation implementation
  stratum_server_complete.cpp  # Modified for SSL support

docs/
  STRATUM_SSL_DESIGN.md     # This file
  STRATUM_SSL_TESTING.md    # Testing guide (to be created)
```

## RPC Monitoring

### Extended Stats Endpoint

`mining.getstratuminfo` extended with SSL stats:

```json
{
  "status": "running",
  "connections": 150,
  "ssl_connections": 120,
  "plain_connections": 30,
  "ssl_enabled": true,
  "ssl_port": 3334,
  "plain_port": 3333,
  "shares_accepted": 45230,
  "shares_rejected": 102,
  "blocks_found": 3,
  "total_hashrate": 1500000000000.0
}
```

## Future Enhancements (Out of Scope for P4)

### P4.5: Certificate Rotation
- Auto-renew Let's Encrypt certificates
- Hot-reload certificates without restarting daemon
- Configurable expiry warnings

### P4.6: Client Certificate Authentication
- Require client certificates for worker authentication
- Whitelist worker certificates
- Mutual TLS (mTLS) for enterprise pools

### P4.7: SNI (Server Name Indication)
- Support multiple domains on single IP
- Virtual hosting for multi-pool setups

## Dependencies

### OpenSSL 3.3.2 (Already Available)

**Libraries:**
- `libssl.a` (1.5 MB) - SSL/TLS protocol
- `libcrypto.a` (7.6 MB) - Cryptographic primitives

**Headers:**
- `<openssl/ssl.h>` - SSL API
- `<openssl/err.h>` - Error handling
- `<openssl/x509.h>` - Certificate handling
- `<openssl/pem.h>` - PEM encoding

**CMake Integration:**
Already configured in `CMakeLists.txt`:
```cmake
set(OPENSSL_ROOT_DIR ${CMAKE_SOURCE_DIR}/third_party/openssl-3.3.2)
set(OPENSSL_SSL_LIBRARY ${OPENSSL_ROOT_DIR}/libssl.a)
set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_ROOT_DIR}/libcrypto.a)
```

## Implementation Checklist

- [ ] Design SSL architecture (THIS DOCUMENT)
- [ ] Implement `SSLSocket` wrapper class
- [ ] Implement `generateSelfSignedCert()` function
- [ ] Add SSL context initialization to `StratumServer`
- [ ] Modify `handleClient()` for SSL handshake
- [ ] Add CLI flags: `--stratum-ssl`, `--stratum-ssl-port`, `--stratum-ssl-cert`, `--stratum-ssl-key`
- [ ] Update `mining.getstratuminfo` RPC with SSL stats
- [ ] Create Python test script for SSL handshake
- [ ] Test with cgminer/bfgminer SSL connections
- [ ] Document SSL configuration in testing guide
- [ ] Security audit with `openssl s_client`

---

**Status:** Design Complete
**Implementation:** Ready to Begin
**Target:** P4 (Stratum + SSL)
**Dependencies:** OpenSSL 3.3.2 ✅ (Already Available)
