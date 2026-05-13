#include "wallet/bip32_deriver.h"
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <openssl/crypto.h>  // OPENSSL_cleanse
#include <stdexcept>

// Use the single canonical HMAC-SHA512 from dinero_crypto_minimal.cpp
extern void hmac_sha512(const uint8_t* key, size_t keylen, const uint8_t* data, size_t datalen, uint8_t out64[64]);

namespace dinero {

// Route to canonical hmac_sha512 — no local OpenSSL HMAC copy
static void HMAC512(const uint8_t* key, size_t key_len,
                    const uint8_t* data, size_t data_len,
                    uint8_t* out) {
    ::hmac_sha512(key, key_len, data, data_len, out);
}

// Big-endian uint32
static uint32_t U32BE(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}

BIP32Deriver::BIP32Deriver(const uint8_t* seed, size_t seed_len) {
    if (seed_len != 64) {
        throw std::runtime_error("BIP32Deriver: seed must be 64 bytes");
    }

    // Create secp256k1 context
    ctx_ = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    if (!ctx_) {
        throw std::runtime_error("BIP32Deriver: failed to create secp256k1 context");
    }

    // BIP32: HMAC-SHA512(Key = "Bitcoin seed", Data = seed)
    static const uint8_t BITCOIN_SEED[] = "Bitcoin seed";
    HMAC512(BITCOIN_SEED, 12, seed, seed_len, I_);

    // Split: IL = private key, IR = chain code
    memcpy(k_, I_, 32);
    memcpy(c_, I_ + 32, 32);

    // Verify key is valid
    if (!secp256k1_ec_seckey_verify(ctx_, k_)) {
        zeroize();
        throw std::runtime_error("BIP32Deriver: derived invalid master key");
    }
}

BIP32Deriver::BIP32Deriver(const uint8_t* private_key, const uint8_t* chain_code) {
    ctx_ = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    if (!ctx_) {
        throw std::runtime_error("BIP32Deriver: failed to create secp256k1 context");
    }

    memcpy(k_, private_key, 32);
    memcpy(c_, chain_code, 32);
    memset(I_, 0, 64);

    if (!secp256k1_ec_seckey_verify(ctx_, k_)) {
        zeroize();
        throw std::runtime_error("BIP32Deriver: invalid private key");
    }
}

BIP32Deriver::~BIP32Deriver() {
    zeroize();
}

BIP32Deriver::BIP32Deriver(BIP32Deriver&& other) noexcept
    : ctx_(other.ctx_) {
    memcpy(k_, other.k_, 32);
    memcpy(c_, other.c_, 32);
    memcpy(I_, other.I_, 64);

    // Clear source
    other.ctx_ = nullptr;
    OPENSSL_cleanse(other.k_, 32);
    OPENSSL_cleanse(other.c_, 32);
    OPENSSL_cleanse(other.I_, 64);
}

BIP32Deriver& BIP32Deriver::operator=(BIP32Deriver&& other) noexcept {
    if (this != &other) {
        zeroize();

        ctx_ = other.ctx_;
        memcpy(k_, other.k_, 32);
        memcpy(c_, other.c_, 32);
        memcpy(I_, other.I_, 64);

        other.ctx_ = nullptr;
        OPENSSL_cleanse(other.k_, 32);
        OPENSSL_cleanse(other.c_, 32);
        OPENSSL_cleanse(other.I_, 64);
    }
    return *this;
}

void BIP32Deriver::zeroize() {
    // Zeroize all key material
    OPENSSL_cleanse(k_, sizeof(k_));
    OPENSSL_cleanse(c_, sizeof(c_));
    OPENSSL_cleanse(I_, sizeof(I_));

    // Destroy secp256k1 context
    if (ctx_) {
        secp256k1_context_destroy(ctx_);
        ctx_ = nullptr;
    }
}

void BIP32Deriver::deriveHardened(uint32_t index) {
    deriveChild(index | HARDENED, true);
}

void BIP32Deriver::deriveNormal(uint32_t index) {
    deriveChild(index, false);
}

void BIP32Deriver::deriveChild(uint32_t index, bool hardened) {
    uint8_t data[1 + 32 + 4];  // 0x00 + key/pubkey + index

    if (hardened) {
        // Hardened: data = 0x00 || ser256(kpar) || ser32(i)
        data[0] = 0x00;
        memcpy(data + 1, k_, 32);
    } else {
        // Normal: data = serP(Kpar) || ser32(i)
        secp256k1_pubkey P;
        if (!secp256k1_ec_pubkey_create(ctx_, &P, k_)) {
            throw std::runtime_error("BIP32: pubkey create failed");
        }
        size_t publen = 33;
        secp256k1_ec_pubkey_serialize(ctx_, data, &publen, &P, SECP256K1_EC_COMPRESSED);
    }

    // Append index in big-endian
    uint32_t be = U32BE(index);
    memcpy(data + (hardened ? 33 : 33), &be, 4);

    // HMAC-SHA512
    HMAC512(c_, 32, data, hardened ? 37 : 37, I_);

    // Tweak private key: k_child = k_parent + IL
    uint8_t tweak[32];
    memcpy(tweak, I_, 32);

    if (!secp256k1_ec_seckey_tweak_add(ctx_, k_, tweak)) {
        OPENSSL_cleanse(tweak, 32);
        throw std::runtime_error("BIP32: tweak add failed");
    }

    // Update chain code
    memcpy(c_, I_ + 32, 32);

    // Zeroize intermediate
    OPENSSL_cleanse(tweak, 32);
    OPENSSL_cleanse(data, sizeof(data));
}

std::array<uint8_t, 32> BIP32Deriver::getPrivateKey() const {
    std::array<uint8_t, 32> result;
    memcpy(result.data(), k_, 32);
    return result;
}

std::array<uint8_t, 32> BIP32Deriver::getChainCode() const {
    std::array<uint8_t, 32> result;
    memcpy(result.data(), c_, 32);
    return result;
}

std::array<uint8_t, 33> BIP32Deriver::getCompressedPubkey() const {
    secp256k1_pubkey P;
    if (!secp256k1_ec_pubkey_create(ctx_, &P, k_)) {
        throw std::runtime_error("BIP32: pubkey create failed");
    }

    std::array<uint8_t, 33> result;
    size_t len = 33;
    secp256k1_ec_pubkey_serialize(ctx_, result.data(), &len, &P, SECP256K1_EC_COMPRESSED);
    return result;
}

std::array<uint8_t, 32> BIP32Deriver::getXOnlyPubkey() const {
    secp256k1_pubkey P;
    if (!secp256k1_ec_pubkey_create(ctx_, &P, k_)) {
        throw std::runtime_error("BIP32: pubkey create failed");
    }

    secp256k1_xonly_pubkey xonly;
    if (!secp256k1_xonly_pubkey_from_pubkey(ctx_, &xonly, nullptr, &P)) {
        throw std::runtime_error("BIP32: xonly pubkey conversion failed");
    }

    std::array<uint8_t, 32> result;
    secp256k1_xonly_pubkey_serialize(ctx_, result.data(), &xonly);
    return result;
}

bool BIP32Deriver::isValid() const {
    return ctx_ && secp256k1_ec_seckey_verify(ctx_, k_);
}

} // namespace dinero
