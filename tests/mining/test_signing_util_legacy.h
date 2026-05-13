#pragma once

/**
 * Test Signing Utility - Legacy P2PKH (Minimal Real Signing for T23)
 *
 * Purpose:
 * - Provide minimal P2PKH ECDSA signing for consensus tests
 * - No wallet, no HD derivation - just pure signing
 * - Enable proper ConnectBlock/DisconnectBlock testing
 *
 * Why Legacy for T23:
 * - Simpler than Taproot (no tweaking, standard ECDSA)
 * - Battle-tested, well-understood
 * - Consensus-valid (legacy must be supported forever)
 * - Validates undo logic without Taproot complexity
 *
 * Policy Note:
 * - Dinero is Taproot-first for production
 * - Legacy remains supported for compatibility
 * - T23 uses legacy as a TOOL, not a compromise
 *
 * This is test-only infrastructure. It does NOT bypass consensus.
 */

#include "primitives/transaction.h"
#include "crypto/hash.h"
#include "consensus/script.h"
#include "consensus/script_interpreter.h"
#include <secp256k1.h>
#include <array>
#include <vector>
#include <cstring>
#include <stdexcept>

namespace dinero {
namespace test {

/**
 * Minimal test keypair for P2PKH spending
 */
struct LegacyTestKey {
    std::array<uint8_t, 32> privkey;
    std::array<uint8_t, 33> pubkey;  // Compressed pubkey

    LegacyTestKey() {
        privkey.fill(0);
        pubkey.fill(0);
    }
};

/**
 * Generate deterministic test keypair from seed (P2PKH)
 *
 * @param seed Simple seed value (0-255)
 * @return LegacyTestKey with privkey and compressed pubkey
 */
inline LegacyTestKey GenerateLegacyTestKey(uint8_t seed) {
    LegacyTestKey key;

    // Create deterministic "private key" from seed
    // WARNING: This is ONLY for testing! NOT cryptographically secure!
    std::vector<uint8_t> seed_data(32, seed);

    // Hash seed to get "private key" (double SHA256)
    auto hash_result = din::crypto::SHA256D(seed_data);
    std::memcpy(key.privkey.data(), hash_result.data(), 32);

    // Derive compressed pubkey from privkey using secp256k1
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    secp256k1_pubkey pubkey_obj;
    if (!secp256k1_ec_pubkey_create(ctx, &pubkey_obj, key.privkey.data())) {
        secp256k1_context_destroy(ctx);
        throw std::runtime_error("Failed to create pubkey");
    }

    size_t pubkey_len = 33;
    secp256k1_ec_pubkey_serialize(ctx, key.pubkey.data(), &pubkey_len, &pubkey_obj, SECP256K1_EC_COMPRESSED);

    secp256k1_context_destroy(ctx);
    return key;
}

/**
 * Create P2PKH scriptPubKey from compressed pubkey
 *
 * Format: OP_DUP OP_HASH160 <20-byte-hash> OP_EQUALVERIFY OP_CHECKSIG
 *
 * @param pubkey 33-byte compressed public key
 * @return 25-byte P2PKH scriptPubKey
 */
inline std::vector<uint8_t> CreateP2PKHScriptPubKey(const std::array<uint8_t, 33>& pubkey) {
    // Hash pubkey: SHA256 then RIPEMD160 (HASH160)
    std::vector<uint8_t> pubkey_vec(pubkey.begin(), pubkey.end());
    auto pubkey_hash = din::crypto::HASH160(pubkey_vec);

    // Build P2PKH scriptPubKey
    std::vector<uint8_t> scriptPubKey;
    scriptPubKey.push_back(0x76);  // OP_DUP
    scriptPubKey.push_back(0xa9);  // OP_HASH160
    scriptPubKey.push_back(0x14);  // PUSH 20 bytes
    scriptPubKey.insert(scriptPubKey.end(), pubkey_hash.begin(), pubkey_hash.end());
    scriptPubKey.push_back(0x88);  // OP_EQUALVERIFY
    scriptPubKey.push_back(0xac);  // OP_CHECKSIG

    return scriptPubKey;
}

/**
 * Compute legacy sighash for P2PKH input
 *
 * Uses production SignatureHashLegacy() function from consensus layer.
 * Uses SIGHASH_ALL (0x01)
 *
 * @param tx Transaction being signed
 * @param input_index Index of input being signed
 * @param script_pubkey ScriptPubKey of UTXO being spent
 * @return 32-byte sighash
 */
inline std::vector<uint8_t> ComputeLegacySighash(
    const Transaction& tx,
    size_t input_index,
    const std::vector<uint8_t>& script_pubkey
) {
    // Use production sighash implementation from consensus layer
    // This ensures correctness and consistency with block validation

    // Wrap scriptPubKey in Script object
    consensus::Script script_code(script_pubkey);

    // Create execution context
    consensus::ScriptExecutionContext ctx(
        &tx,                        // Transaction pointer
        input_index,                // Input index
        0,                          // Amount (not needed for legacy sighash)
        0                           // Flags (not needed for sighash computation)
    );

    // Compute legacy sighash (SIGHASH_ALL = 0x01)
    const uint8_t SIGHASH_ALL = 0x01;
    return consensus::SignatureHashLegacy(script_code, ctx, SIGHASH_ALL);
}

/**
 * Sign a P2PKH transaction input with ECDSA
 *
 * @param tx Transaction to sign (modified in-place)
 * @param input_index Index of input to sign
 * @param script_pubkey ScriptPubKey of UTXO being spent
 * @param key Test key for signing
 * @return true if signing succeeded
 */
inline bool SignP2PKHInput(
    Transaction& tx,
    size_t input_index,
    const std::vector<uint8_t>& script_pubkey,
    const LegacyTestKey& key
) {
    // Compute sighash
    std::vector<uint8_t> sighash = ComputeLegacySighash(tx, input_index, script_pubkey);

    // Sign with ECDSA
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);

    secp256k1_ecdsa_signature sig;
    if (!secp256k1_ecdsa_sign(ctx, &sig, sighash.data(), key.privkey.data(), nullptr, nullptr)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Serialize signature to DER
    std::array<uint8_t, 72> der_sig;
    size_t der_sig_len = 72;
    secp256k1_ecdsa_signature_serialize_der(ctx, der_sig.data(), &der_sig_len, &sig);

    // Build scriptSig: <sig> <pubkey>
    std::vector<uint8_t> scriptSig;

    // Push signature with SIGHASH_ALL byte
    scriptSig.push_back(static_cast<uint8_t>(der_sig_len + 1));  // sig length + sighash byte
    scriptSig.insert(scriptSig.end(), der_sig.begin(), der_sig.begin() + der_sig_len);
    scriptSig.push_back(0x01);  // SIGHASH_ALL

    // Push pubkey
    scriptSig.push_back(0x21);  // 33 bytes
    scriptSig.insert(scriptSig.end(), key.pubkey.begin(), key.pubkey.end());

    // Attach to input
    tx.vin[input_index].scriptSig = scriptSig;

    secp256k1_context_destroy(ctx);
    return true;
}

} // namespace test
} // namespace dinero
