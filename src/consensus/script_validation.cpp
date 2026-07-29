/**
 * Minimal Script Validation Engine - Implementation
 *
 * Phase F.11: Explicit script validation for consensus
 *
 * This is the canonical script validation for blocks.
 * No VM. No opcodes. Just explicit validation per script type.
 */

#include "consensus/script_validation.h"
#include "consensus/script_interpreter.h"  // For SignatureHashLegacy
#include "consensus/script_verify.h"       // Phase C.1: ScriptVerifier::VerifyTaproot
#include "consensus/covenant_activation.h" // Phase C.2: Covenant activation check
#include "consensus/script.h"               // For Script class
#include "consensus/pq/p2mr_consensus.h"    // Phase 6: V7 PQ P2MR spend verifier
#include "crypto/evp_secp256k1.h"           // DineroSecpIllegalCallback (non-aborting)
#include "crypto/hash.h"
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>   // For secp256k1_xonly_pubkey
#include <secp256k1_schnorrsig.h>  // For secp256k1_schnorrsig_verify
#include <cstring>

namespace dinero {
namespace consensus {

// ============================================================================
// Script Type Detection
// ============================================================================

ScriptType DetectScriptType(const std::vector<uint8_t>& spk) {
    // P2PKH: OP_DUP OP_HASH160 <20> <pubkeyhash> OP_EQUALVERIFY OP_CHECKSIG
    // Length: 25 bytes
    // Format: 76 a9 14 [20 bytes] 88 ac
    if (spk.size() == 25 &&
        spk[0] == 0x76 &&  // OP_DUP
        spk[1] == 0xa9 &&  // OP_HASH160
        spk[2] == 0x14 &&  // PUSH 20 bytes
        spk[23] == 0x88 && // OP_EQUALVERIFY
        spk[24] == 0xac) { // OP_CHECKSIG
        return ScriptType::P2PKH;
    }

    // P2WPKH: OP_0 <20-byte-hash>
    // Length: 22 bytes
    // Format: 00 14 [20 bytes]
    if (spk.size() == 22 &&
        spk[0] == 0x00 &&  // OP_0 (witness v0)
        spk[1] == 0x14) {  // PUSH 20 bytes
        return ScriptType::P2WPKH;
    }

    // P2TR: OP_1 <32-byte-xonly-pubkey>
    // Length: 34 bytes
    // Format: 51 20 [32 bytes]
    if (spk.size() == 34 &&
        spk[0] == 0x51 &&  // OP_1 (witness v1)
        spk[1] == 0x20) {  // PUSH 32 bytes
        return ScriptType::P2TR;
    }

    // P2MR: OP_3 <32-byte-merkle-root>  (BIP-360, v7 PQ)
    // Length: 34 bytes
    // Format: 53 20 [32 bytes]
    if (spk.size() == 34 &&
        spk[0] == 0x53 &&  // OP_3 (witness v3)
        spk[1] == 0x20) {  // PUSH 32 bytes
        return ScriptType::P2MR;
    }

    return ScriptType::UNKNOWN;
}

// ============================================================================
// Helper: Extract signature and pubkey from P2PKH scriptSig
// ============================================================================

/**
 * Extract DER signature and compressed pubkey from P2PKH scriptSig
 *
 * P2PKH scriptSig format:
 * <sig_len> <DER_sig> <hashtype> <pubkey_len> <compressed_pubkey>
 *
 * @param scriptSig The scriptSig to parse
 * @param sig_out Output: DER signature (without hashtype byte)
 * @param pubkey_out Output: Compressed public key (33 bytes)
 * @return true if extraction succeeded
 */
static bool ExtractSigAndPubKey(
    const std::vector<uint8_t>& scriptSig,
    std::vector<uint8_t>& sig_out,
    std::vector<uint8_t>& pubkey_out
) {
    // Minimum: sig_len(1) + sig(~70) + hashtype(1) + pubkey_len(1) + pubkey(33) = ~106 bytes
    if (scriptSig.size() < 100) {
        return false;
    }

    size_t pos = 0;

    // Extract signature
    uint8_t sig_len = scriptSig[pos++];
    if (sig_len < 8 || sig_len > 73) {  // DER signature: 8-73 bytes + hashtype
        return false;
    }

    if (pos + sig_len > scriptSig.size()) {
        return false;
    }

    // sig_len includes hashtype byte at the end
    sig_out.assign(scriptSig.begin() + pos, scriptSig.begin() + pos + sig_len - 1);
    pos += sig_len;  // Skip sig + hashtype

    // Extract pubkey
    if (pos >= scriptSig.size()) {
        return false;
    }

    uint8_t pubkey_len = scriptSig[pos++];
    if (pubkey_len != 33) {  // Compressed pubkey must be 33 bytes
        return false;
    }

    if (pos + pubkey_len > scriptSig.size()) {
        return false;
    }

    pubkey_out.assign(scriptSig.begin() + pos, scriptSig.begin() + pos + pubkey_len);

    return true;
}

// ============================================================================
// Helper: Verify ECDSA signature using secp256k1
// ============================================================================

/**
 * Verify ECDSA signature
 *
 * @param pubkey Compressed public key (33 bytes)
 * @param sig DER-encoded signature (WITHOUT hashtype byte)
 * @param sighash 32-byte message hash
 * @return true if signature is valid
 */
static bool VerifyECDSASignature(
    const std::vector<uint8_t>& pubkey,
    const std::vector<uint8_t>& sig,
    const std::vector<uint8_t>& sighash
) {
    if (pubkey.size() != 33) return false;
    if (sighash.size() != 32) return false;
    if (sig.empty() || sig.size() > 72) return false;

    // Create secp256k1 context with non-aborting illegal_arg callback so
    // adversarial inputs hit a logged-rejection path instead of __fastfail.
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    if (!ctx) return false;
    secp256k1_context_set_illegal_callback(
        ctx, dinero::crypto::DineroSecpIllegalCallback, nullptr);

    // Parse DER signature
    secp256k1_ecdsa_signature sig_parsed;
    if (!secp256k1_ecdsa_signature_parse_der(ctx, &sig_parsed, sig.data(), sig.size())) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Parse compressed pubkey
    secp256k1_pubkey pubkey_parsed;
    if (!secp256k1_ec_pubkey_parse(ctx, &pubkey_parsed, pubkey.data(), 33)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Verify signature
    bool result = secp256k1_ecdsa_verify(ctx, &sig_parsed, sighash.data(), &pubkey_parsed);

    secp256k1_context_destroy(ctx);
    return result;
}

// ============================================================================
// Legacy P2PKH Validator
// ============================================================================

ScriptValidationResult ValidateLegacySpend(
    const Transaction& tx,
    size_t input_index,
    const UTXOEntry& utxo,
    uint32_t block_height
) {
    const TxInput& input = tx.vin[input_index];

    // Extract sig and pubkey from scriptSig
    std::vector<uint8_t> sig, pubkey;
    if (!ExtractSigAndPubKey(input.scriptSig, sig, pubkey)) {
        return ScriptValidationResult::EXTRACT_FAILED;
    }

    // Verify pubkey hashes to scriptPubKey hash
    // scriptPubKey: 76 a9 14 <20-byte-hash> 88 ac
    // Extract the 20-byte hash at offset 3
    std::vector<uint8_t> expected_hash(utxo.scriptPubKey.begin() + 3, utxo.scriptPubKey.begin() + 23);

    // Compute HASH160 of pubkey
    auto computed_hash = din::crypto::HASH160(pubkey.data(), pubkey.size());
    std::vector<uint8_t> computed_hash_vec(computed_hash.begin(), computed_hash.end());

    if (computed_hash_vec != expected_hash) {
        return ScriptValidationResult::INVALID_SCRIPT;
    }

    // Compute legacy sighash using production function
    Script script_code(utxo.scriptPubKey);
    ScriptExecutionContext ctx(&tx, input_index, 0, 0);
    std::vector<uint8_t> sighash = SignatureHashLegacy(script_code, ctx, 0x01);  // SIGHASH_ALL

    // Verify ECDSA signature
    if (!VerifyECDSASignature(pubkey, sig, sighash)) {
        return ScriptValidationResult::INVALID_SIGNATURE;
    }

    return ScriptValidationResult::OK;
}

// ============================================================================
// Helper: Verify Schnorr signature using secp256k1
// ============================================================================

/**
 * Verify BIP340 Schnorr signature
 *
 * @param xonly_pubkey 32-byte x-only public key
 * @param sig 64-byte Schnorr signature
 * @param sighash 32-byte message hash
 * @return true if signature is valid
 */
static bool VerifySchnorrSignature(
    const std::vector<uint8_t>& xonly_pubkey,
    const std::vector<uint8_t>& sig,
    const std::vector<uint8_t>& sighash
) {
    if (xonly_pubkey.size() != 32) return false;
    if (sig.size() != 64) return false;
    if (sighash.size() != 32) return false;

    // Create secp256k1 context with non-aborting illegal_arg callback so
    // adversarial inputs hit a logged-rejection path instead of __fastfail.
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    if (!ctx) return false;
    secp256k1_context_set_illegal_callback(
        ctx, dinero::crypto::DineroSecpIllegalCallback, nullptr);

    // Parse x-only pubkey
    secp256k1_xonly_pubkey pubkey_parsed;
    if (!secp256k1_xonly_pubkey_parse(ctx, &pubkey_parsed, xonly_pubkey.data())) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Verify Schnorr signature (BIP340)
    bool result = secp256k1_schnorrsig_verify(ctx, sig.data(), sighash.data(), 32, &pubkey_parsed);

    secp256k1_context_destroy(ctx);
    return result;
}

// ============================================================================
// Taproot Validator
// ============================================================================

ScriptValidationResult ValidateTaprootSpend(
    const Transaction& tx,
    size_t input_index,
    const UTXOEntry& utxo,
    uint32_t block_height,
    const std::vector<UTXOEntry>& all_utxos
) {
    // Taproot requires full input context (BIP341) - catch caller bugs
    if (!all_utxos.empty() && all_utxos.size() != tx.vin.size()) {
        return ScriptValidationResult::INVALID_SCRIPT;
    }

    const TxInput& input = tx.vin[input_index];

    // Taproot key-path spend: witness must have exactly 1 element (64-byte signature)
    if (input.witness.empty()) {
        return ScriptValidationResult::INVALID_SCRIPT;
    }

    if (input.witness.size() != 1) {
        // Script-path spend — check activation height, then delegate to full verifier
        if (!consensus::CovenantActivationParams::IsCovenantActive(block_height, dinero::GetActiveChain())) {
            return ScriptValidationResult::UNSUPPORTED_SCRIPT;
        }

        // Delegate to ScriptVerifier::VerifyTaproot (full BIP342 + covenants)
        uint32_t flags = SCRIPT_VERIFY_STANDARD;
        if (consensus::CcvSuccessorBindingActivationParams::IsActive(
                block_height, dinero::GetActiveChain())) {
            flags |= SCRIPT_VERIFY_CCV_SUCCESSOR_BINDING;
        }
        std::string script_error;
        bool valid = consensus::ScriptVerifier::VerifyTaproot(
            tx, input_index, all_utxos, script_error, flags);
        return valid ? ScriptValidationResult::OK : ScriptValidationResult::INVALID_SCRIPT;
    }

    const std::vector<uint8_t>& sig = input.witness[0];

    // BIP340 Schnorr signature must be 64 bytes (or 65 with sighash type, but default is implicit)
    if (sig.size() != 64 && sig.size() != 65) {
        return ScriptValidationResult::INVALID_SIGNATURE;
    }

    // Extract 64-byte signature (strip sighash type if present)
    std::vector<uint8_t> sig64 = (sig.size() == 65)
        ? std::vector<uint8_t>(sig.begin(), sig.begin() + 64)
        : sig;

    // Extract sighash type (default is 0x00 for SIGHASH_DEFAULT)
    uint8_t sighash_type = (sig.size() == 65) ? sig[64] : 0x00;

    // Extract x-only pubkey from scriptPubKey
    // P2TR scriptPubKey: 51 20 <32-byte-xonly-pubkey>
    // Offset: 0=OP_1, 1=0x20(PUSH32), 2-33=pubkey
    if (utxo.scriptPubKey.size() != 34) {
        return ScriptValidationResult::INVALID_SCRIPT;
    }

    std::vector<uint8_t> xonly_pubkey(
        utxo.scriptPubKey.begin() + 2,
        utxo.scriptPubKey.begin() + 34
    );

    // Create script execution context with ALL input amounts and scriptPubKeys (BIP341 requirement)
    ScriptExecutionContext ctx(&tx, input_index, utxo.value.GetUna(), 0);

    // Populate all_amounts and all_scriptpubkeys for BIP341 sighash
    if (!all_utxos.empty() && all_utxos.size() == tx.vin.size()) {
        ctx.all_amounts.reserve(all_utxos.size());
        ctx.all_scriptpubkeys.reserve(all_utxos.size());
        ctx.all_confidential_flags.reserve(all_utxos.size());
        ctx.all_input_commitments.reserve(all_utxos.size());

        for (const auto& u : all_utxos) {
            ctx.all_amounts.push_back(u.value.GetUna());
            ctx.all_scriptpubkeys.push_back(u.scriptPubKey);
            ctx.all_confidential_flags.push_back(u.is_confidential ? 1 : 0);
            ctx.all_input_commitments.push_back(u.commitment);
        }
    } else if (!all_utxos.empty()) {
        return ScriptValidationResult::INVALID_SCRIPT;
    }

    std::vector<uint8_t> leaf_hash;  // Empty for key-path spend
    std::vector<uint8_t> sighash = SignatureHashTaproot(ctx, sighash_type, leaf_hash);

    if (sighash.size() != 32) {
        return ScriptValidationResult::INVALID_SCRIPT;
    }

    // Verify Schnorr signature
    if (!VerifySchnorrSignature(xonly_pubkey, sig64, sighash)) {
        return ScriptValidationResult::INVALID_SIGNATURE;
    }

    return ScriptValidationResult::OK;
}

// ============================================================================
// P2WPKH (SegWit v0) Validator - BIP141/143
// ============================================================================

/**
 * Validate P2WPKH (Pay-to-Witness-Public-Key-Hash) spend
 *
 * P2WPKH scriptPubKey: OP_0 <20-byte-pubkey-hash>
 * Witness: <signature> <pubkey>
 *
 * BIP143 sighash is used for signature verification.
 */
ScriptValidationResult ValidateP2WPKHSpend(
    const Transaction& tx,
    size_t input_index,
    const UTXOEntry& utxo,
    uint32_t block_height
) {
    const TxInput& input = tx.vin[input_index];

    // P2WPKH requires exactly 2 witness elements: signature and pubkey
    if (input.witness.size() != 2) {
        return ScriptValidationResult::INVALID_SCRIPT;
    }

    const std::vector<uint8_t>& sig_with_hashtype = input.witness[0];
    const std::vector<uint8_t>& pubkey = input.witness[1];

    // Signature must be at least 1 byte (hashtype) + some DER signature
    if (sig_with_hashtype.size() < 9) {  // Minimum: ~8 byte DER + 1 hashtype
        return ScriptValidationResult::INVALID_SCRIPT;
    }

    // Pubkey must be 33 bytes (compressed)
    if (pubkey.size() != 33) {
        return ScriptValidationResult::INVALID_SCRIPT;
    }

    // Extract hashtype (last byte of signature)
    uint8_t hashtype = sig_with_hashtype.back();
    std::vector<uint8_t> sig(sig_with_hashtype.begin(), sig_with_hashtype.end() - 1);

    // Verify pubkey hashes to the 20-byte hash in scriptPubKey
    // P2WPKH scriptPubKey: 00 14 <20-byte-hash>
    if (utxo.scriptPubKey.size() != 22) {
        return ScriptValidationResult::INVALID_SCRIPT;
    }

    std::vector<uint8_t> expected_hash(utxo.scriptPubKey.begin() + 2, utxo.scriptPubKey.begin() + 22);

    // Compute HASH160 of pubkey
    auto computed_hash = din::crypto::HASH160(pubkey.data(), pubkey.size());
    std::vector<uint8_t> computed_hash_vec(computed_hash.begin(), computed_hash.end());

    if (computed_hash_vec != expected_hash) {
        return ScriptValidationResult::INVALID_SCRIPT;
    }

    // Construct script code for BIP143: OP_DUP OP_HASH160 <20-byte-hash> OP_EQUALVERIFY OP_CHECKSIG
    // This is the P2PKH-equivalent script that BIP143 uses for P2WPKH
    std::vector<uint8_t> script_code_bytes = {
        0x76,  // OP_DUP
        0xa9,  // OP_HASH160
        0x14   // PUSH 20 bytes
    };
    script_code_bytes.insert(script_code_bytes.end(), expected_hash.begin(), expected_hash.end());
    script_code_bytes.push_back(0x88);  // OP_EQUALVERIFY
    script_code_bytes.push_back(0xac);  // OP_CHECKSIG

    Script script_code(script_code_bytes);

    // Compute BIP143 sighash
    ScriptExecutionContext ctx(&tx, input_index, utxo.value.GetUna(), 0);
    std::vector<uint8_t> sighash = SignatureHashWitness(script_code, ctx, hashtype);

    if (sighash.size() != 32) {
        return ScriptValidationResult::INVALID_SCRIPT;
    }

    // Verify ECDSA signature
    if (!VerifyECDSASignature(pubkey, sig, sighash)) {
        return ScriptValidationResult::INVALID_SIGNATURE;
    }

    return ScriptValidationResult::OK;
}

// ============================================================================
// P2MR (Pay-to-Merkle-Root, BIP-360 v7 PQ) Validator
// ============================================================================

/**
 * Validate a P2MR spend (witness v3, ML-DSA-65 + Merkle commitment).
 *
 * The witness stack for a P2MR input holds exactly one element: the canonical
 * binary payload defined by dinero::consensus::pq::P2MRWitness (scheme_id |
 * pubkey | signature | merkle_depth | siblings | leaf_index). The sighash
 * is BIP-341-style, computed over the full transaction + all-input context,
 * same as the Taproot key-path spend above.
 */
ScriptValidationResult ValidateP2MRSpend(
    const Transaction& tx,
    size_t input_index,
    const UTXOEntry& utxo,
    uint32_t block_height,
    const std::vector<UTXOEntry>& all_utxos
) {
    // Full-input context is required for a BIP-341 sighash. Catch caller bugs.
    if (!all_utxos.empty() && all_utxos.size() != tx.vin.size()) {
        return ScriptValidationResult::INVALID_SCRIPT;
    }

    const TxInput& input = tx.vin[input_index];

    // P2MR witness stack must be a single canonical blob. Multi-stack layouts
    // are not part of the wire format and we reject them outright.
    if (input.witness.size() != 1) {
        return ScriptValidationResult::INVALID_SCRIPT;
    }
    const std::vector<uint8_t>& witness_blob = input.witness[0];
    if (witness_blob.empty()) {
        return ScriptValidationResult::INVALID_SCRIPT;
    }

    // Compute the BIP-341 sighash — the signer's wallet.signp2mr RPC signs
    // over this same 32-byte blob.
    ScriptExecutionContext ctx(&tx, input_index, utxo.value.GetUna(), 0);

    if (!all_utxos.empty() && all_utxos.size() == tx.vin.size()) {
        ctx.all_amounts.reserve(all_utxos.size());
        ctx.all_scriptpubkeys.reserve(all_utxos.size());
        ctx.all_confidential_flags.reserve(all_utxos.size());
        ctx.all_input_commitments.reserve(all_utxos.size());
        for (const auto& u : all_utxos) {
            ctx.all_amounts.push_back(u.value.GetUna());
            ctx.all_scriptpubkeys.push_back(u.scriptPubKey);
            ctx.all_confidential_flags.push_back(u.is_confidential ? 1 : 0);
            ctx.all_input_commitments.push_back(u.commitment);
        }
    }

    std::vector<uint8_t> leaf_hash;  // Key-path style; no tapleaf commitment.
    std::vector<uint8_t> sighash_vec = SignatureHashTaproot(ctx, /*hash_type=*/0x00, leaf_hash);
    if (sighash_vec.size() != 32) {
        return ScriptValidationResult::INVALID_SCRIPT;
    }
    std::array<uint8_t, 32> sighash{};
    std::memcpy(sighash.data(), sighash_vec.data(), 32);

    auto err = dinero::consensus::pq::VerifyP2MRSpend(
        utxo.scriptPubKey, witness_blob, sighash, block_height);

    switch (err) {
        case dinero::consensus::pq::P2MRVerifyError::Ok:
            return ScriptValidationResult::OK;
        case dinero::consensus::pq::P2MRVerifyError::SignatureInvalid:
            return ScriptValidationResult::INVALID_SIGNATURE;
        case dinero::consensus::pq::P2MRVerifyError::BadScriptShape:
        case dinero::consensus::pq::P2MRVerifyError::WitnessDecodeFailed:
        case dinero::consensus::pq::P2MRVerifyError::MerklePathMismatch:
            return ScriptValidationResult::INVALID_SCRIPT;
        case dinero::consensus::pq::P2MRVerifyError::SchemeNotAcceptedHere:
            return ScriptValidationResult::UNSUPPORTED_SCRIPT;
        case dinero::consensus::pq::P2MRVerifyError::InternalError:
        default:
            return ScriptValidationResult::INVALID_SCRIPT;
    }
}

// ============================================================================
// Main Entry Point
// ============================================================================

ScriptValidationResult ValidateSpend(
    const Transaction& tx,
    size_t input_index,
    const UTXOEntry& utxo,
    uint32_t block_height,
    const std::vector<UTXOEntry>& all_utxos
) {
    ScriptType type = DetectScriptType(utxo.scriptPubKey);

    switch (type) {
        case ScriptType::P2PKH:
            return ValidateLegacySpend(tx, input_index, utxo, block_height);

        case ScriptType::P2TR:
            return ValidateTaprootSpend(tx, input_index, utxo, block_height, all_utxos);

        case ScriptType::P2WPKH:
            return ValidateP2WPKHSpend(tx, input_index, utxo, block_height);

        case ScriptType::P2MR:
            return ValidateP2MRSpend(tx, input_index, utxo, block_height, all_utxos);

        default:
            return ScriptValidationResult::UNSUPPORTED_SCRIPT;
    }
}

} // namespace consensus
} // namespace dinero
