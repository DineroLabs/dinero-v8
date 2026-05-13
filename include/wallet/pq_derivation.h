#pragma once
/**
 * V7 wallet-side PQ key derivation (BIP-32 → HKDF → ML-DSA-65).
 *
 * Spec: docs/consensus/V7_WALLET_SCHEMA.md §1.
 *
 * This library handles the middle of the wallet key pipeline:
 *
 *                     ┌──────────────────────────────────────────┐
 *   BIP-39 mnemonic ──┤ BIP-32 walk m/88'/1448'/acct'/chg/index   │
 *                     └────────────────┬─────────────────────────┘
 *                                      │  (32-byte priv_key, 32-byte chain_code)
 *                                      ▼
 *                     ┌──────────────────────────────────────────┐
 *                     │ THIS LIBRARY:                            │
 *                     │   HKDF-SHA256(priv||chain, salt, info)   │
 *                     │       ↓ 32-byte pq_seed                  │
 *                     │   ml_dsa_65::KeygenFromSeed(pq_seed)     │
 *                     └────────────────┬─────────────────────────┘
 *                                      │  (1952-byte pubkey, 4032-byte secret)
 *                                      ▼
 *                     Wallet storage / address encoding / signing
 *
 * BIP-32 walk is NOT in this file — that's existing v5 wallet code and
 * lives in `src/crypto/hd_keychain.cpp`. Callers feed the 32-byte priv
 * and 32-byte chain_code from any BIP-32 library.
 *
 * Zeroization:
 *   - The HKDF input buffer (priv||chain, 64 bytes) is wiped with
 *     OPENSSL_cleanse immediately after HKDF returns.
 *   - The derived 32-byte pq_seed is also wiped after KeygenFromSeed
 *     consumes it.
 *   - DerivePQKeypair returns SecureKeypair; it adopts the 4032-byte
 *     ML-DSA secret into an RAII owner and scrubs the local source keypair.
 *
 * Not consensus-critical. Wallet-layer only.
 */

#include "consensus/pq/ml_dsa_65.h"
#include "wallet/secure_keypair.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace dinero::wallet::pq {

/** Size of a BIP-32 extended private key component (private scalar or chain code). */
constexpr std::size_t BIP32_PRIVKEY_BYTES   = 32;
constexpr std::size_t BIP32_CHAINCODE_BYTES = 32;

using Bip32PrivKey   = std::array<uint8_t, BIP32_PRIVKEY_BYTES>;
using Bip32ChainCode = std::array<uint8_t, BIP32_CHAINCODE_BYTES>;

/**
 * Derive the 32-byte pq_seed for scheme_id = 0x01 (ML-DSA-65).
 *
 * Implements the exact construction pinned in
 * docs/consensus/V7_WALLET_SCHEMA.md §1 and anchored by the vector at
 * include/consensus/pq/test_vectors/wallet_hkdf_vectors.h:
 *
 *   pq_seed = HKDF-SHA256(
 *       ikm  = priv_key || chain_code,          // 64 bytes, fixed order
 *       salt = "dinero-v7-ml-dsa-65",            // 19 bytes, ASCII, locked
 *       info = LE32(leaf_index),                // 4 bytes
 *       L    = 32
 *   )
 *
 * Inputs are consumed by value so the caller doesn't have to manage
 * lifetime; the copies are zeroized before this function returns.
 */
std::array<uint8_t, 32> DerivePQSeed(Bip32PrivKey priv_key,
                                     Bip32ChainCode chain_code,
                                     uint32_t leaf_index);

/**
 * End-to-end convenience: BIP-32 material → SecureKeypair.
 *
 * Equivalent to:
 *   auto seed = DerivePQSeed(priv, chain, leaf);
 *   auto kp   = ml_dsa_65::KeygenFromSeed(ml_dsa_65::Seed{seed});
 *   // seed auto-wiped
 *   return SecureKeypair(std::move(kp));
 * with all intermediates (ikm, pq_seed, ml_seed) scrubbed via
 * OPENSSL_cleanse before this function returns. SecureKeypair also scrubs
 * the raw Keypair source during adoption, then zeroizes its owned
 * 4032-byte secret on every path out of scope.
 */
SecureKeypair DerivePQKeypair(Bip32PrivKey priv_key,
                              Bip32ChainCode chain_code,
                              uint32_t leaf_index);

/**
 * Compute the v7 P2MR Merkle root for a single-leaf tree, which is the
 * default at v7 genesis.
 *
 *   merkle_root = SHA256( scheme_id || pubkey )
 *
 * where scheme_id is the 1-byte registry index (0x01 for ML-DSA-65) and
 * pubkey is the full pubkey bytes.
 *
 * Callers pass this root into wallet::EncodeP2MRAddress to produce the
 * final bech32m address string.
 *
 * Multi-leaf Merkle trees (future extension — see V7_WALLET_SCHEMA §3)
 * will get their own helper; this one is single-leaf only.
 */
std::array<uint8_t, 32> ComputeSingleLeafMerkleRoot(
    uint8_t scheme_id,
    const dinero::consensus::pq::ml_dsa_65::PublicKey& pubkey);

} // namespace dinero::wallet::pq
