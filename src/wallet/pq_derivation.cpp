/*
 * V7 wallet-side PQ key derivation — implementation.
 *
 * See include/wallet/pq_derivation.h for the narrative.
 */

#include "wallet/pq_derivation.h"
#include "crypto/sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

#include <openssl/crypto.h>  // OPENSSL_cleanse
#include <openssl/hmac.h>    // HMAC

namespace dinero::wallet::pq {

namespace {

// Locked wallet-identity salt. Changing this breaks every v7 wallet.
// Using const char[] (not const char*) so sizeof gives the correct length.
constexpr char        kHkdfSalt[]       = "dinero-v7-ml-dsa-65";
constexpr std::size_t kHkdfSaltBytes    = sizeof(kHkdfSalt) - 1;  // excludes NUL
static_assert(kHkdfSaltBytes == 19, "salt byte length must match spec exactly");

constexpr std::size_t kHkdfOutputBytes  = 32;
constexpr std::size_t kHkdfInfoBytes    = 4;
constexpr std::size_t kSha256OutputBytes = 32;
constexpr std::size_t kHmacBlockBytes   = 32;  // SHA-256 output size

// Manual zeroize helper — same primitive v5 uses elsewhere.
void SecureZero(void* ptr, std::size_t n) {
    OPENSSL_cleanse(ptr, n);
}

class CleanseGuard {
public:
    CleanseGuard(void* ptr, std::size_t size) : ptr_(ptr), size_(size) {}
    ~CleanseGuard() { SecureZero(ptr_, size_); }

    CleanseGuard(const CleanseGuard&) = delete;
    CleanseGuard& operator=(const CleanseGuard&) = delete;

private:
    void* ptr_;
    std::size_t size_;
};

// HKDF-SHA256 per RFC 5869. Output length capped at kHmacBlockBytes here
// because we only ever need 32 bytes for the pq_seed — keeps the inner
// buffer small and the logic obvious.
void HkdfSha256Expand32(const uint8_t* prk, std::size_t prk_len,
                        const uint8_t* info, std::size_t info_len,
                        uint8_t* out32) {
    if (prk_len > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        info_len != kHkdfInfoBytes) {
        throw std::runtime_error("invalid HKDF-SHA256 expand input");
    }

    // A single HKDF-Expand iteration is sufficient for L == 32 bytes:
    // T(1) = HMAC(PRK, info || 0x01).
    std::array<uint8_t, kHkdfInfoBytes + 1> input{};
    std::memcpy(input.data(), info, info_len);
    input[info_len] = 0x01;

    uint8_t t[kHmacBlockBytes]{};
    CleanseGuard t_guard(t, sizeof(t));
    unsigned int tlen = 0;
    if (HMAC(EVP_sha256(),
             prk, static_cast<int>(prk_len),
             input.data(), input.size(),
             t, &tlen) == nullptr ||
        tlen != kHmacBlockBytes) {
        throw std::runtime_error("HKDF-SHA256 expand failed");
    }

    std::memcpy(out32, t, kHkdfOutputBytes);
}

void HkdfSha256Extract(const uint8_t* salt, std::size_t salt_len,
                       const uint8_t* ikm,  std::size_t ikm_len,
                       uint8_t* prk_out32) {
    if (salt_len > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("invalid HKDF-SHA256 salt length");
    }

    unsigned int prk_len = 0;
    if (HMAC(EVP_sha256(),
             salt, static_cast<int>(salt_len),
             ikm, ikm_len,
             prk_out32, &prk_len) == nullptr ||
        prk_len != kSha256OutputBytes) {
        throw std::runtime_error("HKDF-SHA256 extract failed");
    }
}

} // namespace

std::array<uint8_t, 32> DerivePQSeed(Bip32PrivKey priv_key,
                                     Bip32ChainCode chain_code,
                                     uint32_t leaf_index) {
    CleanseGuard priv_key_guard(priv_key.data(), priv_key.size());
    CleanseGuard chain_code_guard(chain_code.data(), chain_code.size());

    // ikm = priv_key || chain_code  (64 bytes, fixed order per spec).
    alignas(8) uint8_t ikm[BIP32_PRIVKEY_BYTES + BIP32_CHAINCODE_BYTES];
    CleanseGuard ikm_guard(ikm, sizeof(ikm));
    std::memcpy(ikm,                        priv_key.data(),   BIP32_PRIVKEY_BYTES);
    std::memcpy(ikm + BIP32_PRIVKEY_BYTES,  chain_code.data(), BIP32_CHAINCODE_BYTES);

    // info = LE32(leaf_index).
    uint8_t info[kHkdfInfoBytes];
    info[0] = static_cast<uint8_t>((leaf_index >> 0)  & 0xff);
    info[1] = static_cast<uint8_t>((leaf_index >> 8)  & 0xff);
    info[2] = static_cast<uint8_t>((leaf_index >> 16) & 0xff);
    info[3] = static_cast<uint8_t>((leaf_index >> 24) & 0xff);

    // Extract.
    uint8_t prk[kSha256OutputBytes];
    CleanseGuard prk_guard(prk, sizeof(prk));
    HkdfSha256Extract(reinterpret_cast<const uint8_t*>(kHkdfSalt), kHkdfSaltBytes,
                      ikm, sizeof(ikm),
                      prk);

    // Expand to 32 bytes.
    std::array<uint8_t, 32> pq_seed{};
    HkdfSha256Expand32(prk, sizeof(prk), info, sizeof(info), pq_seed.data());

    // The guards wipe all input and intermediate key material on both
    // success and OpenSSL failure paths.
    return pq_seed;
}

dinero::wallet::SecureKeypair
DerivePQKeypair(Bip32PrivKey priv_key,
                Bip32ChainCode chain_code,
                uint32_t leaf_index) {
    auto pq_seed = DerivePQSeed(priv_key, chain_code, leaf_index);

    dinero::consensus::pq::ml_dsa_65::Seed ml_seed{};
    std::memcpy(ml_seed.data(), pq_seed.data(), ml_seed.size());

    auto kp = dinero::consensus::pq::ml_dsa_65::KeygenFromSeed(ml_seed);

    // Wipe seed material before handing the SecureKeypair back. The
    // SecureKeypair constructor copies the 4032-byte secret into its owner
    // and scrubs this local kp source before returning.
    SecureZero(pq_seed.data(), pq_seed.size());
    SecureZero(ml_seed.data(), ml_seed.size());

    return dinero::wallet::SecureKeypair(std::move(kp));
}

std::array<uint8_t, 32> ComputeSingleLeafMerkleRoot(
    uint8_t scheme_id,
    const dinero::consensus::pq::ml_dsa_65::PublicKey& pubkey) {
    // SHA256(scheme_id || pubkey)
    std::array<uint8_t, 32> out{};
    dinero::crypto::CSHA256()
        .Write(&scheme_id, 1)
        .Write(pubkey.data(), pubkey.size())
        .Finalize(out.data());
    return out;
}

} // namespace dinero::wallet::pq
