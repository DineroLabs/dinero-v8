/**
 * ML-DSA-65 (scheme_id = 0x01) — thin C++ façade over PQClean.
 *
 * Rule for this file: it is the ONLY file in the Dinero tree that includes
 * PQClean headers. Every consensus caller goes through the narrow API in
 * include/consensus/pq/ml_dsa_65.h. If you feel tempted to add another
 * #include "api.h" anywhere in src/consensus/, stop and add a function
 * here instead.
 */

#include "consensus/pq/ml_dsa_65.h"

#include <cstring>
#include <stdexcept>
#include <string>

extern "C" {
#include "api.h"   // PQClean ML-DSA-65 clean API

// Explicit-seeded ML-DSA-65 keygen, implemented in
// src/consensus/pq/ml_dsa_65_keygen.c. Replicates the body of PQClean's
// crypto_sign_keypair but takes the 32-byte seed as a parameter instead
// of drawing it from randombytes. Bit-identical output given the same
// seed on any architecture (confirmed Apple Silicon + EPYC-Rome).
int dinero_pq_mldsa65_keypair_from_seed(uint8_t *pk,
                                        uint8_t *sk,
                                        const uint8_t seed[32]);
}

namespace dinero::consensus::pq::ml_dsa_65 {

// Defense-in-depth: if PQClean ever changes the byte sizes (e.g. upstream
// pin is bumped to an incompatible version), we want the build to fail, not
// silently sign/verify against a different parameter set.
static_assert(PUBKEY_BYTES    == PQCLEAN_MLDSA65_CLEAN_CRYPTO_PUBLICKEYBYTES,
              "ML-DSA-65 pubkey size drift vs PQClean");
static_assert(SECRETKEY_BYTES == PQCLEAN_MLDSA65_CLEAN_CRYPTO_SECRETKEYBYTES,
              "ML-DSA-65 secret size drift vs PQClean");
static_assert(SIGNATURE_BYTES == PQCLEAN_MLDSA65_CLEAN_CRYPTO_BYTES,
              "ML-DSA-65 signature size drift vs PQClean");

Keypair Keygen() {
    Keypair kp{};
    const int rc = PQCLEAN_MLDSA65_CLEAN_crypto_sign_keypair(
        kp.pubkey.data(), kp.secret.data());
    if (rc != 0) {
        throw std::runtime_error("ml_dsa_65::Keygen failed: PQClean rc=" +
                                 std::to_string(rc));
    }
    return kp;
}

Keypair KeygenFromSeed(const Seed& seed) {
    Keypair kp{};
    const int rc = dinero_pq_mldsa65_keypair_from_seed(
        kp.pubkey.data(), kp.secret.data(), seed.data());
    if (rc != 0) {
        throw std::runtime_error("ml_dsa_65::KeygenFromSeed failed: rc=" +
                                 std::to_string(rc));
    }
    return kp;
}

Signature Sign(const uint8_t* msg, std::size_t msg_len, const SecretKey& secret) {
    Signature sig{};
    std::size_t siglen = sig.size();
    const int rc = PQCLEAN_MLDSA65_CLEAN_crypto_sign_signature_ctx(
        sig.data(), &siglen,
        msg, msg_len,
        /*ctx=*/nullptr, /*ctxlen=*/0,
        secret.data());
    if (rc != 0) {
        throw std::runtime_error("ml_dsa_65::Sign failed: PQClean rc=" +
                                 std::to_string(rc));
    }
    // PQClean ML-DSA produces a fixed-length signature. If it ever writes
    // fewer bytes, refuse to return a truncated Signature rather than
    // silently paper over it.
    if (siglen != SIGNATURE_BYTES) {
        throw std::runtime_error("ml_dsa_65::Sign: unexpected siglen " +
                                 std::to_string(siglen));
    }
    return sig;
}

bool Verify(const uint8_t* msg, std::size_t msg_len,
            const uint8_t* sig, std::size_t sig_len,
            const uint8_t* pubkey, std::size_t pubkey_len) {
    // Length gates. These are consensus-observable decisions: if the caller
    // passed something that couldn't possibly be a valid ML-DSA-65 artifact,
    // we reject without even touching the library.
    if (sig_len    != SIGNATURE_BYTES) return false;
    if (pubkey_len != PUBKEY_BYTES)    return false;

    // PQClean returns 0 on valid signature, non-zero on any failure (incl.
    // malformed sig, mismatched message, wrong key). Translate to bool.
    const int rc = PQCLEAN_MLDSA65_CLEAN_crypto_sign_verify_ctx(
        sig, sig_len,
        msg, msg_len,
        /*ctx=*/nullptr, /*ctxlen=*/0,
        pubkey);
    return rc == 0;
}

} // namespace dinero::consensus::pq::ml_dsa_65
