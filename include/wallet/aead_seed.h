#pragma once
/**
 * AES-256-GCM encryption for the 32-byte v7 PQ wallet seed.
 *
 * Spec: docs/consensus/V7_WALLET_SCHEMA.md §2 "Encryption at rest".
 *
 * Inputs:
 *   - 32-byte plaintext seed (the HKDF output from pq_derivation.cpp)
 *   - 32-byte master key (the wallet's Argon2id-derived encryption key)
 *
 * Outputs:
 *   - 32-byte ciphertext
 *   - 12-byte nonce  (random per-encryption; caller gets it back)
 *   - 16-byte auth tag
 *
 * The caller persists all three fields. The master key is held only while
 * the wallet is unlocked. Ciphertext + nonce + tag are stored in the
 * wallet database; the master key is derived from the user's password.
 *
 * Wallet-layer only. Consensus does not handle secrets.
 */

#include "consensus/pq/ml_dsa_65.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace dinero::wallet {

constexpr std::size_t AEAD_KEY_BYTES       = 32;  // AES-256
constexpr std::size_t AEAD_NONCE_BYTES     = 12;  // GCM IV
constexpr std::size_t AEAD_TAG_BYTES       = 16;  // GCM auth tag
constexpr std::size_t AEAD_SEED_BYTES      = 32;  // v7 pq_seed

using AeadKey        = std::array<uint8_t, AEAD_KEY_BYTES>;
using AeadNonce      = std::array<uint8_t, AEAD_NONCE_BYTES>;
using AeadTag        = std::array<uint8_t, AEAD_TAG_BYTES>;
using AeadSeed       = std::array<uint8_t, AEAD_SEED_BYTES>;
using AeadCiphertext = std::array<uint8_t, AEAD_SEED_BYTES>;

struct AeadSealOutput {
    AeadCiphertext ciphertext;
    AeadNonce      nonce;
    AeadTag        tag;
};

/**
 * Encrypt a 32-byte seed. The nonce is drawn from the OpenSSL CSPRNG on
 * every call — same seed encrypted twice produces different ciphertexts.
 *
 * Throws std::runtime_error on OpenSSL failure (which should never happen
 * in practice for fixed-size inputs).
 */
AeadSealOutput SealSeed(const AeadSeed& plaintext, const AeadKey& key);

enum class AeadOpenResult : uint8_t {
    Ok              = 0,
    AuthFailed      = 1,   ///< tag didn't match — wrong key or tampered data
    InternalError   = 2,   ///< OpenSSL API failure
};

/**
 * Decrypt a 32-byte ciphertext. Returns the plaintext via out-param on
 * success. On AuthFailed, `out` is left zeroed (no partial decryption
 * leaks).
 *
 * AuthFailed is the common-case failure: wrong password (wrong master
 * key) or wallet-DB corruption.
 *
 * **Secret-handling discipline:** callers MUST wrap the decrypted seed
 * in `SecureSeed` (below) or an equivalent RAII zeroizer before it can
 * live in memory across a non-trivial scope. Plain `AeadSeed` is fine
 * for short-lived on-the-stack use inside a signing function; anything
 * longer-lived must be wrapped.
 */
AeadOpenResult OpenSeed(const AeadCiphertext& ciphertext,
                        const AeadNonce&      nonce,
                        const AeadTag&        tag,
                        const AeadKey&        key,
                        AeadSeed*             out);

/**
 * RAII owner for a decrypted 32-byte seed.
 *
 * Guarantees OPENSSL_cleanse on the plaintext bytes on every path out of
 * scope — destruction, exception, or move. Move zeros the source so only
 * one live copy of a decrypted seed exists at any time.
 *
 * Usage:
 *     SecureSeed s;
 *     auto rc = OpenSeedSecure(ct, nonce, tag, key, &s);
 *     if (rc != AeadOpenResult::Ok) { ... }
 *     // s.bytes() holds the plaintext here.
 *     auto kp = ml_dsa_65::KeygenFromSeed(s.bytes());
 *     // s goes out of scope; plaintext is scrubbed.
 *
 * No copy. Move semantics only.
 */
class SecureSeed {
public:
    SecureSeed() noexcept { bytes_.fill(0); }
    ~SecureSeed() { Wipe(); }

    SecureSeed(const SecureSeed&) = delete;
    SecureSeed& operator=(const SecureSeed&) = delete;

    SecureSeed(SecureSeed&& other) noexcept : bytes_(other.bytes_) {
        other.Wipe();
    }
    SecureSeed& operator=(SecureSeed&& other) noexcept {
        if (this != &other) {
            Wipe();
            bytes_ = other.bytes_;
            other.Wipe();
        }
        return *this;
    }

    /** Read-only access to the plaintext seed. */
    const AeadSeed& bytes() const noexcept { return bytes_; }

    /** Writable mutable access used only by OpenSeedSecure to populate. */
    AeadSeed& mutable_bytes() noexcept { return bytes_; }

    /** Explicit early wipe. After calling, bytes() reads as all zero. */
    void Wipe() noexcept;

    /** Convert to an ml_dsa_65::Seed for the KeygenFromSeed call path.
     *  The returned array is a *copy* — caller is responsible for
     *  scrubbing it (typically via std::array going out of scope of a
     *  short signing function). */
    dinero::consensus::pq::ml_dsa_65::Seed ToMlDsaSeed() const noexcept;

private:
    AeadSeed bytes_;
};

/**
 * Same as OpenSeed, but writes the decrypted plaintext into a SecureSeed
 * so the caller cannot forget to scrub it.
 */
AeadOpenResult OpenSeedSecure(const AeadCiphertext& ciphertext,
                              const AeadNonce&      nonce,
                              const AeadTag&        tag,
                              const AeadKey&        key,
                              SecureSeed*           out);

} // namespace dinero::wallet
