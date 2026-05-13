#pragma once

/**
 * Test Signing Utility - Minimal Real Signing for T23
 *
 * Provides minimal Taproot signing capability for consensus tests.
 * No wallet, no HD derivation, no descriptors - just pure signing.
 *
 * Purpose:
 * - Generate deterministic test keypairs
 * - Create valid Taproot outputs
 * - Sign transactions with real BIP340 Schnorr signatures
 * - Enable proper ConnectBlock/DisconnectBlock testing
 *
 * Architectural Note:
 * This is test-only infrastructure. It does NOT bypass consensus.
 * It enables testing the REAL consensus path with REAL signatures.
 */

#include "wallet/taproot_keys.h"
#include "wallet/taproot_tx_signer.h"
#include "primitives/transaction.h"
#include "crypto/hash.h"
#include <array>
#include <vector>
#include <cstring>

namespace dinero {
namespace test {

/**
 * Minimal test keypair for Taproot key-path spending
 */
struct TestKey {
    std::array<uint8_t, 32> privkey;
    std::array<uint8_t, 32> xonly_pubkey;
    int parity;

    TestKey() : parity(0) {
        privkey.fill(0);
        xonly_pubkey.fill(0);
    }
};

/**
 * Generate deterministic test keypair from seed
 *
 * For test reproducibility, we use a simple seed-based generation.
 * In production, use proper random generation.
 *
 * @param seed Simple seed value (0-255)
 * @return TestKey with privkey, xonly_pubkey, parity
 */
inline TestKey GenerateTestKey(uint8_t seed) {
    TestKey key;

    // Create deterministic "private key" from seed
    // WARNING: This is ONLY for testing! NOT cryptographically secure!
    std::array<uint8_t, 32> seed_data;
    seed_data.fill(seed);

    // Hash seed to get "private key"
    dinero::Hash256 hasher;
    hasher.Write(seed_data.data(), 32);
    auto hash_result = hasher.Finalize();
    std::memcpy(key.privkey.data(), hash_result.data(), 32);

    // Derive x-only pubkey from privkey
    if (!TaprootKeys::DeriveXOnlyPubkey(key.privkey, key.xonly_pubkey, key.parity)) {
        throw std::runtime_error("Failed to derive x-only pubkey");
    }

    return key;
}

/**
 * Create Taproot scriptPubKey (P2TR) for key-path spending
 *
 * Format: OP_1 (0x51) + PUSH32 (0x20) + 32-byte x-only pubkey
 *
 * @param xonly_pubkey 32-byte x-only public key
 * @return 34-byte P2TR scriptPubKey
 */
inline std::vector<uint8_t> CreateTaprootScriptPubKey(const std::array<uint8_t, 32>& xonly_pubkey) {
    std::vector<uint8_t> scriptPubKey;
    scriptPubKey.push_back(0x51);  // OP_1 (witness v1)
    scriptPubKey.push_back(0x20);  // PUSH 32 bytes
    scriptPubKey.insert(scriptPubKey.end(), xonly_pubkey.begin(), xonly_pubkey.end());
    return scriptPubKey;
}

/**
 * Sign a Taproot transaction input
 *
 * Performs full BIP341 sighash computation and BIP340 Schnorr signing.
 * No shortcuts, no bypasses - this is the real signing path.
 *
 * @param tx Transaction to sign (modified in-place)
 * @param input_index Index of input to sign
 * @param utxos All UTXOs being spent (for sighash computation)
 * @param key Test key for signing
 * @return true if signing succeeded
 */
inline bool SignTaprootInput(
    Transaction& tx,
    size_t input_index,
    const std::vector<CanonicalWalletUTXO>& utxos,
    const TestKey& key
) {
    // Compute BIP341 Taproot sighash
    std::vector<uint8_t> sighash = TaprootTxSigner::ComputeTaprootSighash(
        tx, input_index, utxos, TaprootTxSigner::SIGHASH_DEFAULT
    );

    if (sighash.size() != 32) {
        return false;
    }

    // Convert to fixed-size array for signing
    std::array<uint8_t, 32> msg32;
    std::memcpy(msg32.data(), sighash.data(), 32);

    // Sign with BIP340 Schnorr
    std::array<uint8_t, 64> sig64;
    if (!TaprootKeys::SignSchnorr(sig64, msg32, key.privkey)) {
        return false;
    }

    // Attach witness (Taproot key-path: just the signature)
    tx.vin[input_index].witness.clear();
    tx.vin[input_index].witness.push_back(
        std::vector<uint8_t>(sig64.begin(), sig64.end())
    );

    return true;
}

} // namespace test
} // namespace dinero
