#include "crypto/hash.h"
#include <stdexcept>
#include <string_view>
#include <iostream>

// Check if OpenSSL is available
#ifdef HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/crypto.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  #include <openssl/core.h>
  #include <openssl/core_names.h>
  #include <openssl/provider.h>
#endif
#else
// Fallback to internal crypto if OpenSSL is not available
#include "common/sha256d.h"
#include "crypto/ripemd160.h"
#endif

namespace din::crypto {

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
static OSSL_PROVIDER* g_default_provider = nullptr;
static OSSL_PROVIDER* g_legacy_provider  = nullptr;
#endif

bool OpenSSL_EnsureProviders() {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  // Load default provider (should succeed)
  if (!g_default_provider) g_default_provider = OSSL_PROVIDER_load(nullptr, "default");
  // Load legacy provider for RIPEMD160
  if (!g_legacy_provider)  g_legacy_provider  = OSSL_PROVIDER_load(nullptr, "legacy");

  const bool ok_default = (g_default_provider != nullptr);
  const bool ok_legacy  = (g_legacy_provider  != nullptr);
  if (!ok_default) return false;   // hard fail: SHA256 lives here
  return ok_legacy;                // true if RIPEMD160 available
#else
  return true; // OpenSSL 1.1.1 has built-ins
#endif
}

#ifdef HAVE_OPENSSL
static void evp_digest(const uint8_t* data, size_t size,
                       const EVP_MD* md, uint8_t* out, unsigned int& out_len)
{
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");

    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1)
        { EVP_MD_CTX_free(ctx); throw std::runtime_error("EVP_DigestInit_ex failed"); }

    if (size > 0 && EVP_DigestUpdate(ctx, data, size) != 1)
        { EVP_MD_CTX_free(ctx); throw std::runtime_error("EVP_DigestUpdate failed"); }

    if (EVP_DigestFinal_ex(ctx, out, &out_len) != 1)
        { EVP_MD_CTX_free(ctx); throw std::runtime_error("EVP_DigestFinal_ex failed"); }

    EVP_MD_CTX_free(ctx);
}
#endif

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
// Fetch by name when you want to be explicit about providers.
// For SHA256 we can use EVP_sha256(); for RIPEMD160 prefer fetch to ensure it's present.
static const EVP_MD* fetch_md(std::string_view name) {
    EVP_MD* md = EVP_MD_fetch(nullptr, name.data(), nullptr); // uses loaded providers
    if (!md) throw std::runtime_error(std::string("EVP_MD_fetch failed for ") + std::string(name));
    return md; // note: must be freed with EVP_MD_free after use
}
#endif

Sha256Hash SHA256(const uint8_t* data, size_t size) {
    Sha256Hash out{};
    
#ifdef HAVE_OPENSSL
    unsigned int out_len = 0;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    // EVP_sha256() is fine in 3.x, but this keeps behavior consistent.
    const EVP_MD* md = EVP_sha256();
    evp_digest(data, size, md, out.data(), out_len);
#else
    const EVP_MD* md = EVP_sha256();
    evp_digest(data, size, md, out.data(), out_len);
#endif
    if (out_len != out.size()) throw std::runtime_error("SHA256 digest length mismatch");
#else
    // Fallback to internal crypto
    Dinero::Common::sha256 hasher;
    hasher.update(data, size);
    auto result = hasher.finalize();
    std::copy(result.begin(), result.end(), out.begin());
#endif
    return out;
}

Sha256Hash SHA256D(const uint8_t* data, size_t size) {
    auto first = SHA256(data, size);
    return SHA256(first.data(), first.size());
}

Ripemd160 RIPEMD160(const uint8_t* data, size_t size) {
    Ripemd160 out{};
    
#ifdef HAVE_OPENSSL
    unsigned int out_len = 0;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    // RIPEMD160 is in the legacy provider; ensure it was loaded.
    const EVP_MD* md = EVP_ripemd160(); // works if 'legacy' is loaded
    if (!md) {
        // Fallback: try fetch by name (also requires legacy).
        EVP_MD* fetched = EVP_MD_fetch(nullptr, "RIPEMD160", nullptr);
        if (!fetched) throw std::runtime_error("RIPEMD160 not available (load legacy provider)");
        evp_digest(data, size, fetched, out.data(), out_len);
        EVP_MD_free(fetched);
    } else {
        evp_digest(data, size, md, out.data(), out_len);
    }
#else
    const EVP_MD* md = EVP_ripemd160();
    evp_digest(data, size, md, out.data(), out_len);
#endif
    if (out_len != out.size()) throw std::runtime_error("RIPEMD160 digest length mismatch");
#else
    // Fallback to internal crypto
    auto result = dinero::RIPEMD160(data, size);
    std::copy(result.begin(), result.end(), out.begin());
#endif
    return out;
}

Ripemd160 HASH160(const uint8_t* data, size_t size) {
    auto sha = SHA256(data, size);
    return RIPEMD160(sha.data(), sha.size());
}

bool OpenSSL_SelfTest() {
    try {
        // Test vector: SHA256("hello") = 2cf24dba4fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
        const std::string test_input = "hello";
        auto sha_result = SHA256(reinterpret_cast<const uint8_t*>(test_input.data()), test_input.size());
        
        // Expected SHA256("hello")
        const std::string expected_sha_hex = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
        std::string actual_sha_hex;
        for (int i = 0; i < 32; ++i) {
            char hex[3];
            snprintf(hex, sizeof(hex), "%02x", sha_result[i]);
            actual_sha_hex += hex;
        }
        
        if (actual_sha_hex != expected_sha_hex) {
            std::cerr << "SHA256 test failed: expected " << expected_sha_hex << ", got " << actual_sha_hex << std::endl;
            return false; // SHA256 test failed
        }
        
        // Test vector: HASH160("hello") = b6a9c8c230722b7c748331a8b450f05566dc7d0f
        auto hash160_result = HASH160(reinterpret_cast<const uint8_t*>(test_input.data()), test_input.size());
        
        // Expected HASH160("hello")
        const std::string expected_hash160_hex = "b6a9c8c230722b7c748331a8b450f05566dc7d0f";
        std::string actual_hash160_hex;
        for (int i = 0; i < 20; ++i) {
            char hex[3];
            snprintf(hex, sizeof(hex), "%02x", hash160_result[i]);
            actual_hash160_hex += hex;
        }
        
        if (actual_hash160_hex != expected_hash160_hex) {
            std::cerr << "HASH160 test failed: expected " << expected_hash160_hex << ", got " << actual_hash160_hex << std::endl;
            return false; // HASH160 test failed
        }
        
        return true; // All tests passed
    } catch (const std::exception& e) {
        std::cerr << "Self-test exception: " << e.what() << std::endl;
        return false; // Test failed with exception
    }
}

} // namespace din::crypto
