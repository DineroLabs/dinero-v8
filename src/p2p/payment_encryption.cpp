/**
 * Payment Encryption Implementation
 */

#include "p2p/payment_encryption.h"
#include "crypto/sha256.h"
#include <secp256k1.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <random>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <stdexcept>

namespace din {
namespace p2p {

// ═══════════════════════════════════════════════════════════════
// HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════════

static std::string bytes_to_hex(const std::vector<uint8_t>& bytes) {
    std::stringstream ss;
    for (uint8_t byte : bytes) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return ss.str();
}

static std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::strtol(byteString.c_str(), nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

// Base64 encoding/decoding (simple implementation)
static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

static std::string base64_encode(const std::vector<uint8_t>& data) {
    std::string ret;
    int i = 0;
    uint8_t char_array_3[3];
    uint8_t char_array_4[4];

    for (uint8_t c : data) {
        char_array_3[i++] = c;
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for (i = 0; i < 4; i++)
                ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }

    if (i) {
        for (int j = i; j < 3; j++)
            char_array_3[j] = '\0';

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

        for (int j = 0; j < i + 1; j++)
            ret += base64_chars[char_array_4[j]];

        while (i++ < 3)
            ret += '=';
    }

    return ret;
}

// ═══════════════════════════════════════════════════════════════
// PAYMENT ENCRYPTION
// ═══════════════════════════════════════════════════════════════

std::string PaymentEncryption::computeHash(const std::vector<uint8_t>& data) {
    return dinero::crypto::double_sha256(data.data(), data.size());
}

std::vector<uint8_t> PaymentEncryption::generateNonce() {
    std::vector<uint8_t> nonce(12);
    if (RAND_bytes(nonce.data(), 12) != 1) {
        throw std::runtime_error("Failed to generate random nonce");
    }
    return nonce;
}

std::pair<std::string, std::string> PaymentEncryption::generateEphemeralKeyPair() {
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);

    // Generate random private key
    std::vector<uint8_t> privkey(32);
    if (RAND_bytes(privkey.data(), 32) != 1) {
        secp256k1_context_destroy(ctx);
        throw std::runtime_error("Failed to generate random private key");
    }

    // Derive public key
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_create(ctx, &pubkey, privkey.data())) {
        secp256k1_context_destroy(ctx);
        throw std::runtime_error("Failed to create public key");
    }

    // Serialize public key (compressed)
    std::vector<uint8_t> pubkey_serialized(33);
    size_t pubkey_len = 33;
    secp256k1_ec_pubkey_serialize(
        ctx,
        pubkey_serialized.data(),
        &pubkey_len,
        &pubkey,
        SECP256K1_EC_COMPRESSED
    );

    secp256k1_context_destroy(ctx);

    return {bytes_to_hex(privkey), bytes_to_hex(pubkey_serialized)};
}

std::vector<uint8_t> PaymentEncryption::deriveSharedSecret(
    const std::string& privkey_hex,
    const std::string& pubkey_hex
) {
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);

    // Parse private key
    std::vector<uint8_t> privkey = hex_to_bytes(privkey_hex);
    if (privkey.size() != 32) {
        secp256k1_context_destroy(ctx);
        throw std::runtime_error("Invalid private key size");
    }

    // Parse public key
    std::vector<uint8_t> pubkey_bytes = hex_to_bytes(pubkey_hex);
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_parse(ctx, &pubkey, pubkey_bytes.data(), pubkey_bytes.size())) {
        secp256k1_context_destroy(ctx);
        throw std::runtime_error("Failed to parse public key");
    }

    // Perform ECDH (simplified - production would use secp256k1_ecdh)
    // For MVP, using pubkey multiplication
    std::vector<uint8_t> shared_secret(32);
    secp256k1_pubkey result;
    if (!secp256k1_ec_pubkey_tweak_mul(ctx, &result, privkey.data())) {
        secp256k1_context_destroy(ctx);
        throw std::runtime_error("ECDH failed");
    }
    // Serialize result as shared secret
    size_t len = 33;
    std::vector<uint8_t> temp(33);
    secp256k1_ec_pubkey_serialize(ctx, temp.data(), &len, &result, SECP256K1_EC_COMPRESSED);
    std::copy(temp.begin(), temp.begin() + 32, shared_secret.begin());

    secp256k1_context_destroy(ctx);
    return shared_secret;
}

std::tuple<std::vector<uint8_t>, std::vector<uint8_t>, std::vector<uint8_t>>
PaymentEncryption::aesGcmEncrypt(
    const std::string& plaintext,
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& nonce_input
) {
    if (key.size() != 32) {
        throw std::runtime_error("AES-256-GCM requires 32-byte key");
    }

    // Generate or use provided nonce
    std::vector<uint8_t> nonce = nonce_input.empty() ? generateNonce() : nonce_input;
    if (nonce.size() != 12) {
        throw std::runtime_error("AES-GCM nonce must be 12 bytes");
    }

    // Create cipher context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create cipher context");
    }

    // Initialize encryption
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to initialize AES-256-GCM");
    }

    // Set nonce length
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to set nonce length");
    }

    // Set key and nonce
    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to set key and nonce");
    }

    // Encrypt plaintext
    std::vector<uint8_t> ciphertext(plaintext.size() + EVP_CIPHER_CTX_block_size(ctx));
    int len = 0;
    if (EVP_EncryptUpdate(
            ctx,
            ciphertext.data(),
            &len,
            reinterpret_cast<const uint8_t*>(plaintext.data()),
            plaintext.size()
        ) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Encryption failed");
    }
    int ciphertext_len = len;

    // Finalize encryption
    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Encryption finalization failed");
    }
    ciphertext_len += len;
    ciphertext.resize(ciphertext_len);

    // Get authentication tag
    std::vector<uint8_t> tag(16);
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to get authentication tag");
    }

    EVP_CIPHER_CTX_free(ctx);

    return {ciphertext, nonce, tag};
}

std::optional<std::string> PaymentEncryption::aesGcmDecrypt(
    const std::vector<uint8_t>& ciphertext,
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& nonce,
    const std::vector<uint8_t>& tag
) {
    if (key.size() != 32 || nonce.size() != 12 || tag.size() != 16) {
        return std::nullopt;
    }

    // Create cipher context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return std::nullopt;
    }

    // Initialize decryption
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }

    // Set nonce length
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }

    // Set key and nonce
    if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }

    // Decrypt ciphertext
    std::vector<uint8_t> plaintext(ciphertext.size() + EVP_CIPHER_CTX_block_size(ctx));
    int len = 0;
    if (EVP_DecryptUpdate(
            ctx,
            plaintext.data(),
            &len,
            ciphertext.data(),
            ciphertext.size()
        ) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }
    int plaintext_len = len;

    // Set authentication tag
    if (EVP_CIPHER_CTX_ctrl(
            ctx,
            EVP_CTRL_GCM_SET_TAG,
            16,
            const_cast<uint8_t*>(tag.data())
        ) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }

    // Finalize decryption (verifies tag)
    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;  // Authentication failed
    }
    plaintext_len += len;
    plaintext.resize(plaintext_len);

    EVP_CIPHER_CTX_free(ctx);

    return std::string(plaintext.begin(), plaintext.end());
}

// ═══════════════════════════════════════════════════════════════
// HIGH-LEVEL API
// ═══════════════════════════════════════════════════════════════

EncryptedPaymentHandle PaymentEncryption::encrypt(
    const std::string& plaintext,
    const std::string& method_id
) {
    // Generate ephemeral key pair
    auto [ephemeral_privkey, ephemeral_pubkey] = generateEphemeralKeyPair();

    // For self-encryption, use ephemeral key as both sides
    // In real usage, recipient would use their own private key
    auto shared_secret = deriveSharedSecret(ephemeral_privkey, ephemeral_pubkey);

    // Encrypt with AES-256-GCM
    auto [ciphertext, nonce, tag] = aesGcmEncrypt(plaintext, shared_secret);

    // Compute hash
    std::vector<uint8_t> data_to_hash;
    data_to_hash.insert(data_to_hash.end(), ciphertext.begin(), ciphertext.end());
    data_to_hash.insert(data_to_hash.end(), nonce.begin(), nonce.end());
    data_to_hash.insert(data_to_hash.end(), tag.begin(), tag.end());
    std::string hash = computeHash(data_to_hash);

    return {
        method_id,
        ephemeral_pubkey,
        ciphertext,
        nonce,
        tag,
        hash,
        static_cast<int64_t>(std::time(nullptr))
    };
}

std::optional<std::string> PaymentEncryption::decrypt(
    const EncryptedPaymentHandle& encrypted,
    const std::string& privkey_hex
) {
    // Verify integrity first
    if (!verify(encrypted)) {
        return std::nullopt;
    }

    // Derive shared secret using recipient's private key
    auto shared_secret = deriveSharedSecret(privkey_hex, encrypted.ephemeral_pubkey);

    // Decrypt
    return aesGcmDecrypt(encrypted.ciphertext, shared_secret, encrypted.nonce, encrypted.tag);
}

bool PaymentEncryption::verify(const EncryptedPaymentHandle& encrypted) {
    std::vector<uint8_t> data_to_hash;
    data_to_hash.insert(data_to_hash.end(), encrypted.ciphertext.begin(), encrypted.ciphertext.end());
    data_to_hash.insert(data_to_hash.end(), encrypted.nonce.begin(), encrypted.nonce.end());
    data_to_hash.insert(data_to_hash.end(), encrypted.tag.begin(), encrypted.tag.end());

    std::string computed_hash = computeHash(data_to_hash);
    return computed_hash == encrypted.hash;
}

std::string EncryptedPaymentHandle::toBase64() const {
    // Serialize to binary format
    std::vector<uint8_t> data;

    // method_id (null-terminated string)
    data.insert(data.end(), method_id.begin(), method_id.end());
    data.push_back(0);

    // ephemeral_pubkey (hex string, null-terminated)
    data.insert(data.end(), ephemeral_pubkey.begin(), ephemeral_pubkey.end());
    data.push_back(0);

    // ciphertext length + data
    uint32_t ct_len = ciphertext.size();
    data.push_back((ct_len >> 24) & 0xFF);
    data.push_back((ct_len >> 16) & 0xFF);
    data.push_back((ct_len >> 8) & 0xFF);
    data.push_back(ct_len & 0xFF);
    data.insert(data.end(), ciphertext.begin(), ciphertext.end());

    // nonce (12 bytes)
    data.insert(data.end(), nonce.begin(), nonce.end());

    // tag (16 bytes)
    data.insert(data.end(), tag.begin(), tag.end());

    // hash (null-terminated string)
    data.insert(data.end(), hash.begin(), hash.end());
    data.push_back(0);

    // timestamp (8 bytes)
    uint64_t ts = encrypted_at;
    for (int i = 7; i >= 0; i--) {
        data.push_back((ts >> (i * 8)) & 0xFF);
    }

    return base64_encode(data);
}

EncryptedPaymentHandle EncryptedPaymentHandle::fromBase64(const std::string& b64) {
    // Placeholder - would need full base64 decoder
    // For now, return empty structure
    return {};
}

} // namespace p2p
} // namespace din
