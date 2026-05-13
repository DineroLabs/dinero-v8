#pragma once
/**
 * ML-DSA-65 (scheme_id = 0x01) — thin C++ façade over PQClean.
 *
 * Spec: docs/consensus/V7_GENESIS_SPEC.md.
 * Upstream: third_party/pqclean/crypto_sign/ml-dsa-65/clean/, pinned commit
 * 3730b32aa50ba9e712592c1476bdd048f5f6ed7e.
 *
 * Scope of this wrapper:
 *   - Provide a narrow C++ API that the rest of Dinero consumes.
 *   - Consensus code never #includes PQClean headers directly. All access
 *     goes through this interface, so we keep the boundary between vendored
 *     C code and our consensus-critical code small and auditable.
 *
 * NOT in this wrapper:
 *   - The P2MR Merkle-path check. That lives in the P2MR verifier
 *     (src/consensus/p2mr_verify.cpp, Phase 5).
 *   - Any consensus decision. Verify() returns a boolean; the caller decides
 *     what to do with it.
 *   - Scheme dispatch. The PQSchemeRegistry does dispatch by scheme_id; this
 *     wrapper is ML-DSA-65 only.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dinero::consensus::pq::ml_dsa_65 {

// Byte sizes per NIST FIPS 204. Must match PQClean's CRYPTO_* constants
// exactly — static_asserts in the .cpp verify this on every build.
constexpr std::size_t PUBKEY_BYTES    = 1952;
constexpr std::size_t SECRETKEY_BYTES = 4032;
constexpr std::size_t SIGNATURE_BYTES = 3309;
constexpr std::size_t SEED_BYTES      = 32;  ///< Input to KeygenFromSeed.

using PublicKey  = std::array<uint8_t, PUBKEY_BYTES>;
using SecretKey  = std::array<uint8_t, SECRETKEY_BYTES>;
using Signature  = std::array<uint8_t, SIGNATURE_BYTES>;
using Seed       = std::array<uint8_t, SEED_BYTES>;

struct Keypair {
    PublicKey pubkey;
    SecretKey secret;
};

/**
 * Generate a fresh ML-DSA-65 keypair from platform randomness.
 *
 * Wallet-side only. Not called from consensus.
 */
Keypair Keygen();

/**
 * Generate an ML-DSA-65 keypair deterministically from a 32-byte seed.
 *
 * Same seed on two hosts (any architecture, any OS) produces identical
 * `pubkey` and `secret` bytes. The implementation scopes a thread-local
 * "seeded mode" on the randombytes source for the duration of this call,
 * then clears it. Platform randomness is restored before the call returns
 * even if PQClean's keygen throws or returns a non-zero status.
 *
 * This is the wallet-side primitive the HD-wallet derivation path feeds
 * into: wallet seed + BIP-32-style derivation path → 32-byte PQ seed →
 * ML-DSA keypair.
 *
 * Not called from consensus.
 */
Keypair KeygenFromSeed(const Seed& seed);

/**
 * Sign `msg` with `secret`. Returns the canonical 3309-byte ML-DSA signature.
 *
 * Wallet-side only. Not called from consensus.
 */
Signature Sign(const uint8_t* msg, std::size_t msg_len, const SecretKey& secret);

inline Signature Sign(const std::vector<uint8_t>& msg, const SecretKey& secret) {
    return Sign(msg.data(), msg.size(), secret);
}

/**
 * Verify `sig` over `msg` under `pubkey`. Returns true iff the signature is
 * valid. Pure function — no I/O, no randomness, deterministic across hosts.
 *
 * THIS is the consensus-critical call path. Any divergence between two nodes'
 * Verify() results on the same (msg, sig, pubkey) is a consensus bug.
 */
bool Verify(const uint8_t* msg, std::size_t msg_len,
            const uint8_t* sig, std::size_t sig_len,
            const uint8_t* pubkey, std::size_t pubkey_len);

inline bool Verify(const std::vector<uint8_t>& msg,
                   const Signature& sig,
                   const PublicKey& pubkey) {
    return Verify(msg.data(), msg.size(),
                  sig.data(), sig.size(),
                  pubkey.data(), pubkey.size());
}

} // namespace dinero::consensus::pq::ml_dsa_65
