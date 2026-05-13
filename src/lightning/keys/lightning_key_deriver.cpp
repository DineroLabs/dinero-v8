#include "lightning/keys/lightning_key_deriver.h"
#include "wallet/bip32_deriver.h"
#include <openssl/hmac.h>
#include <openssl/crypto.h>  // OPENSSL_cleanse
#include <secp256k1.h>
#include <stdexcept>
#include <cstring>

namespace dinero {
namespace lightning {

// ═══════════════════════════════════════════════════════════════════════════════
// Construction / Destruction
// ═══════════════════════════════════════════════════════════════════════════════

LightningKeyDeriver::LightningKeyDeriver(const uint8_t* seed, size_t seed_len, uint32_t coin_type)
    : seed_(seed, seed + seed_len), coin_type_(coin_type) {
    if (seed_len != 64) {
        throw std::runtime_error("LightningKeyDeriver: seed must be 64 bytes");
    }
}

LightningKeyDeriver::LightningKeyDeriver(const std::vector<uint8_t>& seed, uint32_t coin_type)
    : seed_(seed), coin_type_(coin_type) {
    if (seed.size() != 64) {
        throw std::runtime_error("LightningKeyDeriver: seed must be 64 bytes");
    }
}

LightningKeyDeriver::~LightningKeyDeriver() {
    // SECURITY: Zeroize seed on destruction
    OPENSSL_cleanse(seed_.data(), seed_.size());
}

LightningKeyDeriver::LightningKeyDeriver(LightningKeyDeriver&& other) noexcept
    : seed_(std::move(other.seed_)), coin_type_(other.coin_type_) {
    other.coin_type_ = 0;
}

LightningKeyDeriver& LightningKeyDeriver::operator=(LightningKeyDeriver&& other) noexcept {
    if (this != &other) {
        OPENSSL_cleanse(seed_.data(), seed_.size());
        seed_ = std::move(other.seed_);
        coin_type_ = other.coin_type_;
        other.coin_type_ = 0;
    }
    return *this;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Channel Keys - BIP84 extended paths (chains 3-7)
// ═══════════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> LightningKeyDeriver::DeriveChannelKey(uint32_t chain, uint32_t channel_index) {
    // BIP84: m/84'/coin_type'/0'/chain/channel_index
    dinero::BIP32Deriver deriver(seed_.data(), 64);
    deriver.deriveHardened(84);
    deriver.deriveHardened(coin_type_);
    deriver.deriveHardened(0);
    deriver.deriveNormal(chain);
    deriver.deriveNormal(channel_index);

    auto k = deriver.getPrivateKey();
    return std::vector<uint8_t>(k.begin(), k.end());
}

std::vector<uint8_t> LightningKeyDeriver::GetFundingKey(uint32_t channel_index) {
    return DeriveChannelKey(3, channel_index);  // Chain 3: Funding
}

std::vector<uint8_t> LightningKeyDeriver::GetRevocationBaseKey(uint32_t channel_index) {
    return DeriveChannelKey(4, channel_index);  // Chain 4: Revocation
}

std::vector<uint8_t> LightningKeyDeriver::GetPaymentBaseKey(uint32_t channel_index) {
    return DeriveChannelKey(5, channel_index);  // Chain 5: Payment
}

std::vector<uint8_t> LightningKeyDeriver::GetDelayedPaymentBaseKey(uint32_t channel_index) {
    return DeriveChannelKey(6, channel_index);  // Chain 6: Delayed payment
}

std::vector<uint8_t> LightningKeyDeriver::GetHTLCBaseKey(uint32_t channel_index) {
    return DeriveChannelKey(7, channel_index);  // Chain 7: HTLC
}

// ═══════════════════════════════════════════════════════════════════════════════
// Revocation Basepoint Secret (HMAC-SHA256 based)
// ═══════════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> LightningKeyDeriver::GetRevocationBasepointSecret(const std::string& channel_id) {
    if (channel_id.empty()) {
        throw std::runtime_error("GetRevocationBasepointSecret: channel_id cannot be empty");
    }

    // Construct message: prefix || channel_id
    const char* prefix = "dinero-lightning-revocation";
    std::string message = std::string(prefix) + channel_id;

    // Compute HMAC-SHA256(seed, message)
    std::vector<uint8_t> result(32);
    unsigned int hmac_len = 0;

    HMAC(EVP_sha256(),
         seed_.data(), static_cast<int>(seed_.size()),
         reinterpret_cast<const unsigned char*>(message.data()), message.size(),
         result.data(),
         &hmac_len);

    if (hmac_len != 32) {
        throw std::runtime_error("GetRevocationBasepointSecret: HMAC-SHA256 produced invalid length");
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Node Identity (m/84'/coin'/9735'/account'/key_index')
// ═══════════════════════════════════════════════════════════════════════════════

ILightningKeyProvider::NodeIdentity LightningKeyDeriver::GetNodeIdentity(uint32_t account, uint32_t key_index) {
    // All hardened derivation for maximum security
    dinero::BIP32Deriver deriver(seed_.data(), 64);
    deriver.deriveHardened(84);          // BIP84 (Native SegWit)
    deriver.deriveHardened(coin_type_);  // Coin type
    deriver.deriveHardened(9735);        // Lightning purpose (9735 = LN default port)
    deriver.deriveHardened(account);     // Account index
    deriver.deriveHardened(key_index);   // Key index

    NodeIdentity identity;
    auto k = deriver.getPrivateKey();
    identity.privkey.assign(k.begin(), k.end());

    auto pub = deriver.getCompressedPubkey();
    identity.pubkey.assign(pub.begin(), pub.end());

    return identity;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Utility: Public Key Derivation
// ═══════════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> LightningKeyDeriver::GetPublicKey(const std::vector<uint8_t>& private_key) {
    if (private_key.size() != 32) {
        throw std::invalid_argument("Private key must be 32 bytes");
    }

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    if (!ctx) {
        throw std::runtime_error("Failed to create secp256k1 context");
    }

    secp256k1_pubkey pubkey_obj;
    if (!secp256k1_ec_pubkey_create(ctx, &pubkey_obj, private_key.data())) {
        secp256k1_context_destroy(ctx);
        throw std::runtime_error("Failed to create public key");
    }

    std::vector<uint8_t> pubkey(33);
    size_t pubkey_len = 33;
    secp256k1_ec_pubkey_serialize(ctx, pubkey.data(), &pubkey_len, &pubkey_obj, SECP256K1_EC_COMPRESSED);

    secp256k1_context_destroy(ctx);
    return pubkey;
}

} // namespace lightning
} // namespace dinero
