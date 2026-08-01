#pragma once
/**
 * Dinero v7 wallet-identity HKDF derivation test vector.
 *
 * The wallet schema (docs/consensus/V7_WALLET_SCHEMA.md §1) derives a
 * 32-byte PQ seed from a BIP-32 extended key via:
 *
 *   pq_seed = HKDF-SHA256(
 *       ikm  = bip32_private_key_bytes || chain_code_bytes,   // 64 bytes
 *       salt = "dinero-v7-ml-dsa-65",                          // 19 bytes
 *       info = LE32(leaf_index),                              // 4 bytes
 *       L    = 32
 *   )
 *   keypair = KeygenFromSeed(pq_seed)
 *
 * Every field above is consensus-portability-critical and is LOCKED in
 * the spec. This test vector pins the HKDF step so any wallet
 * implementation (Qt, mobile, third-party) can verify its derivation
 * pipeline independent of BIP-32.
 *
 * How a conforming wallet uses this vector:
 *   1. Feed `ikm` and `info` through any standard HKDF-SHA256 library.
 *   2. Confirm the output equals `expected_pq_seed`.
 *   3. Pass `expected_pq_seed` into `ml_dsa_65::KeygenFromSeed`.
 *   4. Confirm the resulting pubkey matches `expected_pubkey_prefix_32`.
 *
 * If either step diverges, the wallet cannot produce keys interoperable
 * with the rest of v7.
 *
 * Computed 2026-04-16 using Python's hmac + hashlib reference and
 * PQClean ML-DSA-65 pinned upstream commit
 * 3730b32aa50ba9e712592c1476bdd048f5f6ed7e.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dinero::consensus::pq::test_vectors {

/**
 * Canonical IKM (64 bytes): first 32 = 0x11 repeated, second 32 = 0x22
 * repeated. Distinctive so a debugger spots it; not a real BIP-32 output.
 * For spec conformance, every wallet implementation must produce the
 * same HKDF output when fed this exact IKM + salt + info.
 */
inline constexpr std::array<uint8_t, 64> kHkdfVector_Ikm = {
    0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
    0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
    0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
    0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
};

/** Locked salt: ASCII bytes of "dinero-v7-ml-dsa-65", no null, no newline. */
inline constexpr std::string_view kHkdfVector_Salt = "dinero-v7-ml-dsa-65";

/** leaf_index = 0 encoded little-endian (4 bytes). */
inline constexpr std::array<uint8_t, 4> kHkdfVector_Info = {
    0x00, 0x00, 0x00, 0x00,
};

/**
 * Expected 32-byte PQ seed from HKDF-SHA256(ikm, salt, info, L=32).
 *
 * Any wallet implementation feeding `kHkdfVector_Ikm` / `kHkdfVector_Salt` /
 * `kHkdfVector_Info` into a standard HKDF-SHA256 MUST produce exactly
 * these 32 bytes.
 */
inline constexpr std::array<uint8_t, 32> kHkdfVector_ExpectedPqSeed = {
    0x32, 0x62, 0x7c, 0x28, 0xac, 0x21, 0x29, 0xaa,
    0xcf, 0x74, 0xe0, 0x42, 0xa8, 0xa4, 0xb4, 0x3a,
    0x44, 0x7d, 0xdd, 0x9c, 0xac, 0x4b, 0x27, 0x5c,
    0x4b, 0xae, 0xac, 0x9b, 0x02, 0x81, 0x35, 0x89,
};

/**
 * Expected first 32 bytes of the ML-DSA-65 pubkey derived from
 * kHkdfVector_ExpectedPqSeed via `KeygenFromSeed`.
 *
 * This is the end-to-end anchor: a wallet whose BIP-32 → HKDF → KeygenFromSeed
 * pipeline is correct will produce this pubkey prefix after feeding a real
 * BIP-32 extended key whose (priv || chain) equals kHkdfVector_Ikm.
 *
 * Confirmed on Apple Silicon 2026-04-16.
 */
inline constexpr std::array<uint8_t, 32> kHkdfVector_ExpectedPubkeyPrefix = {
    0x02, 0xea, 0x27, 0x52, 0x3f, 0xf7, 0xae, 0x68,
    0xf9, 0x62, 0x49, 0xca, 0xb0, 0x06, 0x1d, 0xa6,
    0xac, 0x55, 0x42, 0xb8, 0x01, 0x12, 0x30, 0x7c,
    0xd8, 0xcd, 0xd7, 0x70, 0xcc, 0x68, 0x1b, 0x3a,
};

} // namespace dinero::consensus::pq::test_vectors
