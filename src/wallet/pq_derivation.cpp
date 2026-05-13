/*
 * V7 wallet-side PQ key derivation — implementation.
 *
 * See include/wallet/pq_derivation.h for the narrative.
 */

#include "wallet/pq_derivation.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <openssl/crypto.h>  // OPENSSL_cleanse
#include <openssl/hmac.h>    // HMAC, HMAC_CTX_*
#include <openssl/sha.h>     // SHA256

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

// HKDF-SHA256 per RFC 5869. Output length capped at kHmacBlockBytes here
// because we only ever need 32 bytes for the pq_seed — keeps the inner
// buffer small and the logic obvious. Expand-phase handles the general
// case anyway in case we extend it later.
void HkdfSha256Expand32(const uint8_t* prk, std::size_t prk_len,
                        const uint8_t* info, std::size_t info_len,
                        uint8_t* out32) {
    // Single HKDF-Expand iteration sufficient for L <= 32 bytes.
    uint8_t t[kHmacBlockBytes];
    unsigned int tlen = 0;
    const uint8_t counter = 0x01;

    HMAC_CTX* ctx = HMAC_CTX_new();
    HMAC_Init_ex(ctx, prk, static_cast<int>(prk_len), EVP_sha256(), nullptr);
    HMAC_Update(ctx, info, info_len);
    HMAC_Update(ctx, &counter, 1);
    HMAC_Final(ctx, t, &tlen);
    HMAC_CTX_free(ctx);

    // tlen must be 32 for SHA-256.
    std::memcpy(out32, t, kHkdfOutputBytes);
    SecureZero(t, sizeof(t));
}

void HkdfSha256Extract(const uint8_t* salt, std::size_t salt_len,
                       const uint8_t* ikm,  std::size_t ikm_len,
                       uint8_t* prk_out32) {
    unsigned int prk_len = 0;
    HMAC(EVP_sha256(),
         salt, static_cast<int>(salt_len),
         ikm,  ikm_len,
         prk_out32, &prk_len);
    // prk_len must be 32 for SHA-256 — no partial.
}

} // namespace

std::array<uint8_t, 32> DerivePQSeed(Bip32PrivKey priv_key,
                                     Bip32ChainCode chain_code,
                                     uint32_t leaf_index) {
    // ikm = priv_key || chain_code  (64 bytes, fixed order per spec).
    alignas(8) uint8_t ikm[BIP32_PRIVKEY_BYTES + BIP32_CHAINCODE_BYTES];
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
    HkdfSha256Extract(reinterpret_cast<const uint8_t*>(kHkdfSalt), kHkdfSaltBytes,
                      ikm, sizeof(ikm),
                      prk);

    // Expand to 32 bytes.
    std::array<uint8_t, 32> pq_seed{};
    HkdfSha256Expand32(prk, sizeof(prk), info, sizeof(info), pq_seed.data());

    // Zeroize intermediate material. Caller's copies of priv_key /
    // chain_code are local parameters (we took them by value), so the
    // compiler will drop them at end of scope — zeroize them now too so
    // nothing sensitive lingers.
    SecureZero(ikm,               sizeof(ikm));
    SecureZero(prk,               sizeof(prk));
    SecureZero(priv_key.data(),   priv_key.size());
    SecureZero(chain_code.data(), chain_code.size());

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
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, &scheme_id, 1);
    SHA256_Update(&ctx, pubkey.data(), pubkey.size());
    std::array<uint8_t, 32> out{};
    SHA256_Final(out.data(), &ctx);
    return out;
}

} // namespace dinero::wallet::pq
