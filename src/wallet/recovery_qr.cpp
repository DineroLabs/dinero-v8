#include "wallet/recovery_qr.h"
#include "crypto/wallet_crypto.h"
#include <openssl/rand.h>
#include <array>
#include <cstring>
#include <stdexcept>

namespace dinero {

// ============================================================================
// Base64 helpers (RFC 4648)
// ============================================================================

static const char B64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string Base64Encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) n |= static_cast<uint32_t>(data[i + 2]);

        out.push_back(B64_TABLE[(n >> 18) & 0x3f]);
        out.push_back(B64_TABLE[(n >> 12) & 0x3f]);
        out.push_back((i + 1 < data.size()) ? B64_TABLE[(n >> 6) & 0x3f] : '=');
        out.push_back((i + 2 < data.size()) ? B64_TABLE[n & 0x3f] : '=');
    }
    return out;
}

static int B64_DECODE(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::vector<uint8_t> Base64Decode(const std::string& b64) {
    std::vector<uint8_t> out;
    out.reserve((b64.size() / 4) * 3);

    for (size_t i = 0; i + 3 < b64.size(); i += 4) {
        int a = B64_DECODE(b64[i]);
        int b = B64_DECODE(b64[i + 1]);
        int c = (b64[i + 2] != '=') ? B64_DECODE(b64[i + 2]) : 0;
        int d = (b64[i + 3] != '=') ? B64_DECODE(b64[i + 3]) : 0;
        if (a < 0 || b < 0) break;

        uint32_t n = (a << 18) | (b << 12) | (c << 6) | d;
        out.push_back((n >> 16) & 0xff);
        if (b64[i + 2] != '=') out.push_back((n >> 8) & 0xff);
        if (b64[i + 3] != '=') out.push_back(n & 0xff);
    }
    return out;
}

// ============================================================================
// RecoveryQR
// ============================================================================

std::string RecoveryQR::Encode(
    const std::string& mnemonic,
    const std::vector<uint8_t>& profile_data,
    const std::string& passphrase) {

    // Build plaintext: mnemonic_len(2 LE) || mnemonic || profile_len(2 LE) || profile_data
    std::vector<uint8_t> plaintext;
    uint16_t mnemonic_len = static_cast<uint16_t>(mnemonic.size());
    uint16_t profile_len = static_cast<uint16_t>(profile_data.size());
    plaintext.reserve(4 + mnemonic_len + profile_len);

    plaintext.push_back(mnemonic_len & 0xff);
    plaintext.push_back((mnemonic_len >> 8) & 0xff);
    plaintext.insert(plaintext.end(), mnemonic.begin(), mnemonic.end());

    plaintext.push_back(profile_len & 0xff);
    plaintext.push_back((profile_len >> 8) & 0xff);
    plaintext.insert(plaintext.end(), profile_data.begin(), profile_data.end());

    // Generate random salt (16 bytes) and nonce (12 bytes)
    std::vector<uint8_t> salt(16);
    std::vector<uint8_t> nonce(12);
    RAND_bytes(salt.data(), 16);
    RAND_bytes(nonce.data(), 12);

    // Derive encryption key: Argon2id(passphrase, salt, t=3, m=64MB, p=1)
    std::string pw = passphrase.empty() ? "" : passphrase;
    std::array<uint8_t, 32> key;
    if (!crypto::deriveKeyArgon2id(pw, salt, 3, 65536, 1, key)) {
        throw std::runtime_error("RecoveryQR: key derivation failed");
    }

    // Encrypt: AES-256-GCM
    auto ciphertext = crypto::encryptAesGcm(plaintext, key, nonce);

    // Zeroize sensitive data
    std::fill(plaintext.begin(), plaintext.end(), 0);
    std::fill(key.begin(), key.end(), 0);

    // Build payload: salt(16) || nonce(12) || ciphertext(N)
    std::vector<uint8_t> payload;
    payload.reserve(16 + 12 + ciphertext.size());
    payload.insert(payload.end(), salt.begin(), salt.end());
    payload.insert(payload.end(), nonce.begin(), nonce.end());
    payload.insert(payload.end(), ciphertext.begin(), ciphertext.end());

    // Return: header + newline + base64(payload)
    return std::string(HEADER) + "\n" + Base64Encode(payload);
}

bool RecoveryQR::Decode(
    const std::string& qr_data,
    const std::string& passphrase,
    std::string& mnemonic_out,
    std::vector<uint8_t>& profile_data_out) {

    // Check header
    std::string header_line = std::string(HEADER) + "\n";
    if (qr_data.size() < header_line.size() ||
        qr_data.substr(0, header_line.size()) != header_line) {
        return false;
    }

    // Decode base64 payload
    std::string b64 = qr_data.substr(header_line.size());
    auto payload = Base64Decode(b64);

    // Minimum: salt(16) + nonce(12) + at least some ciphertext
    if (payload.size() < 28 + 16) {
        return false;
    }

    // Extract salt, nonce, ciphertext
    std::vector<uint8_t> salt(payload.begin(), payload.begin() + 16);
    std::vector<uint8_t> nonce(payload.begin() + 16, payload.begin() + 28);
    std::vector<uint8_t> ciphertext(payload.begin() + 28, payload.end());

    // Derive key
    std::string pw = passphrase.empty() ? "" : passphrase;
    std::array<uint8_t, 32> key;
    if (!crypto::deriveKeyArgon2id(pw, salt, 3, 65536, 1, key)) {
        return false;
    }

    // Decrypt
    std::vector<uint8_t> plaintext;
    try {
        plaintext = crypto::decryptAesGcm(ciphertext, key, nonce);
    } catch (...) {
        std::fill(key.begin(), key.end(), 0);
        return false;  // Authentication failed (wrong passphrase)
    }
    std::fill(key.begin(), key.end(), 0);

    // Parse plaintext: mnemonic_len(2) || mnemonic || profile_len(2) || profile
    if (plaintext.size() < 4) {
        return false;
    }

    size_t off = 0;
    uint16_t mnemonic_len = static_cast<uint16_t>(plaintext[off])
        | (static_cast<uint16_t>(plaintext[off + 1]) << 8);
    off += 2;

    if (plaintext.size() < off + mnemonic_len + 2) {
        return false;
    }

    mnemonic_out.assign(plaintext.begin() + off,
        plaintext.begin() + off + mnemonic_len);
    off += mnemonic_len;

    uint16_t profile_len = static_cast<uint16_t>(plaintext[off])
        | (static_cast<uint16_t>(plaintext[off + 1]) << 8);
    off += 2;

    if (plaintext.size() < off + profile_len) {
        return false;
    }

    profile_data_out.assign(plaintext.begin() + off,
        plaintext.begin() + off + profile_len);

    // Zeroize plaintext
    std::fill(plaintext.begin(), plaintext.end(), 0);
    return true;
}

} // namespace dinero
