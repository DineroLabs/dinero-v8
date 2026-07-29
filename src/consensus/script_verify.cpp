#include "consensus/script_verify.h"
#include "consensus/tx_parser.h"
#include "consensus/tapscript_interpreter.h"
#include "consensus/script_interpreter.h"  // Phase L0.3: For SCRIPT_VERIFY_STANDARD
#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "crypto/ripemd160.h"
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>  // BIP340 Schnorr signatures for Taproot
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace dinero {
namespace consensus {

// Hex utilities
std::vector<uint8_t> ScriptVerifier::HexDecode(const std::string& hex) {
    std::vector<uint8_t> bytes;
    if (hex.length() % 2 != 0) return bytes;
    
    bytes.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

std::string ScriptVerifier::HexEncode(const std::vector<uint8_t>& data) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t byte : data) {
        ss << std::setw(2) << static_cast<int>(byte);
    }
    return ss.str();
}

// Hash160 = RIPEMD160(SHA256(data))
std::vector<uint8_t> ScriptVerifier::Hash160(const uint8_t* data, size_t len) {
    // SHA256
    uint8_t sha_hash[32];
    dinero::crypto::CSHA256().Write(data, len).Finalize(sha_hash);
    
    // RIPEMD160
    auto ripemd_result = dinero::RIPEMD160(sha_hash, 32);
    
    return std::vector<uint8_t>(ripemd_result.begin(), ripemd_result.end());
}

// Double SHA256
std::vector<uint8_t> ScriptVerifier::DoubleSHA256(const uint8_t* data, size_t len) {
    uint8_t hash1[32];
    dinero::crypto::CSHA256().Write(data, len).Finalize(hash1);
    
    uint8_t hash2[32];
    dinero::crypto::CSHA256().Write(hash1, 32).Finalize(hash2);
    
    return std::vector<uint8_t>(hash2, hash2 + 32);
}

bool ScriptVerifier::IsP2WPKH(const std::vector<uint8_t>& script_pubkey) {
    // P2WPKH: OP_0 OP_PUSH20 <20-byte-hash>
    // Size: 22 bytes (1 + 1 + 20)
    if (script_pubkey.size() != 22) {
        return false;
    }
    
    if (script_pubkey[0] != 0x00) {  // OP_0
        return false;
    }
    
    if (script_pubkey[1] != 0x14) {  // OP_PUSH20 (20 bytes)
        return false;
    }
    
    return true;
}

std::vector<uint8_t> ScriptVerifier::ExtractPubkeyHash(const std::vector<uint8_t>& script_pubkey) {
    if (!IsP2WPKH(script_pubkey)) {
        return std::vector<uint8_t>();
    }
    
    // Skip OP_0 and OP_PUSH20, return the 20-byte hash
    return std::vector<uint8_t>(script_pubkey.begin() + 2, script_pubkey.end());
}

std::vector<uint8_t> ScriptVerifier::ComputeSignatureHash(
    const Transaction& tx,
    size_t input_index,
    const std::vector<uint8_t>& script_code,
    uint64_t value,
    uint32_t hash_type) {
    
    // BIP143 signature hash for SegWit
    // https://github.com/bitcoin/bips/blob/master/bip-0143.mediawiki
    
    std::vector<uint8_t> data;
    
    // 1. nVersion (4 bytes)
    for (int i = 0; i < 4; i++) {
        data.push_back((tx.version >> (i * 8)) & 0xff);
    }
    
    // 2. hashPrevouts (32 bytes)
    {
        std::vector<uint8_t> prevouts_data;
        for (const auto& input : tx.vin) {
            // Previous txid (reversed for serialization) - Phase M.4.3-B: Unwrap TxId
            const auto& txid_u256 = input.prevout.txid.AsUint256();
            std::vector<uint8_t> txid_bytes(txid_u256.data, txid_u256.data + 32);
            std::reverse(txid_bytes.begin(), txid_bytes.end());
            prevouts_data.insert(prevouts_data.end(), txid_bytes.begin(), txid_bytes.end());
            
            // Previous vout (4 bytes LE)
            for (int i = 0; i < 4; i++) {
                prevouts_data.push_back((input.prevout.vout >> (i * 8)) & 0xff);
            }
        }
        auto hash_prevouts = DoubleSHA256(prevouts_data.data(), prevouts_data.size());
        data.insert(data.end(), hash_prevouts.begin(), hash_prevouts.end());
    }
    
    // 3. hashSequence (32 bytes)
    {
        std::vector<uint8_t> sequence_data;
        for (const auto& input : tx.vin) {
            for (int i = 0; i < 4; i++) {
                sequence_data.push_back((input.sequence >> (i * 8)) & 0xff);
            }
        }
        auto hash_sequence = DoubleSHA256(sequence_data.data(), sequence_data.size());
        data.insert(data.end(), hash_sequence.begin(), hash_sequence.end());
    }
    
    // 4. outpoint (36 bytes)
    {
        const auto& input = tx.vin[input_index];
        // Previous txid (reversed) - Phase M.4.3-B: Unwrap TxId
        const auto& txid_u256 = input.prevout.txid.AsUint256();
        std::vector<uint8_t> txid_bytes(txid_u256.data, txid_u256.data + 32);
        std::reverse(txid_bytes.begin(), txid_bytes.end());
        data.insert(data.end(), txid_bytes.begin(), txid_bytes.end());
        
        // Previous vout
        for (int i = 0; i < 4; i++) {
            data.push_back((input.prevout.vout >> (i * 8)) & 0xff);
        }
    }
    
    // 5. scriptCode (variable)
    {
        // Length as compact size
        data.push_back(static_cast<uint8_t>(script_code.size()));
        data.insert(data.end(), script_code.begin(), script_code.end());
    }
    
    // 6. value (8 bytes)
    for (int i = 0; i < 8; i++) {
        data.push_back((value >> (i * 8)) & 0xff);
    }
    
    // 7. nSequence (4 bytes)
    {
        const auto& input = tx.vin[input_index];
        for (int i = 0; i < 4; i++) {
            data.push_back((input.sequence >> (i * 8)) & 0xff);
        }
    }
    
    // 8. hashOutputs (32 bytes)
    {
        std::vector<uint8_t> outputs_data;
        for (const auto& output : tx.vout) {
            // Value (8 bytes LE)
            // Phase M.6.1: Extract raw value for serialization
            uint64_t value_raw = output.value.GetUna();
            for (int i = 0; i < 8; i++) {
                outputs_data.push_back((value_raw >> (i * 8)) & 0xff);
            }
            
            // ScriptPubKey (convert from hex string to bytes)
            std::vector<uint8_t> spk_bytes(output.scriptPubKey.begin(), output.scriptPubKey.end());
            outputs_data.push_back(static_cast<uint8_t>(spk_bytes.size()));
            outputs_data.insert(outputs_data.end(), spk_bytes.begin(), spk_bytes.end());
        }
        auto hash_outputs = DoubleSHA256(outputs_data.data(), outputs_data.size());
        data.insert(data.end(), hash_outputs.begin(), hash_outputs.end());
    }
    
    // 9. nLockTime (4 bytes)
    for (int i = 0; i < 4; i++) {
        data.push_back((tx.lockTime >> (i * 8)) & 0xff);
    }
    
    // 10. nHashType (4 bytes)
    for (int i = 0; i < 4; i++) {
        data.push_back((hash_type >> (i * 8)) & 0xff);
    }
    
    // Double SHA256 of everything
    return DoubleSHA256(data.data(), data.size());
}

bool ScriptVerifier::VerifyP2WPKH(const Transaction& tx, size_t input_index,
                                  const UTXOEntry& utxo, std::string& error) {
    // 1. Check scriptPubKey is P2WPKH
    if (!IsP2WPKH(utxo.scriptPubKey)) {
        error = "Not a P2WPKH output";
        return false;
    }
    
    // 2. Extract pubkey hash from scriptPubKey
    std::vector<uint8_t> expected_pubkey_hash = ExtractPubkeyHash(utxo.scriptPubKey);
    
    // 3. Check witness data format
    const auto& input = tx.vin[input_index];
    if (input.witness.size() != 2) {
        error = "Invalid witness size for P2WPKH (expected 2: signature, pubkey)";
        return false;
    }
    
    // 4. Parse signature and pubkey from witness
    const std::vector<uint8_t>& signature_bytes = input.witness[0];
    const std::vector<uint8_t>& pubkey_bytes = input.witness[1];
    
    if (signature_bytes.empty() || pubkey_bytes.empty()) {
        error = "Empty witness data";
        return false;
    }
    
    // 5. Verify pubkey hash matches
    std::vector<uint8_t> computed_pubkey_hash = Hash160(pubkey_bytes.data(), pubkey_bytes.size());
    if (computed_pubkey_hash != expected_pubkey_hash) {
        error = "Pubkey hash mismatch";
        return false;
    }
    
    // 6. Build scriptCode for P2WPKH
    // scriptCode = OP_DUP OP_HASH160 <20-byte-hash> OP_EQUALVERIFY OP_CHECKSIG
    std::vector<uint8_t> script_code;
    script_code.push_back(0x76);  // OP_DUP
    script_code.push_back(0xa9);  // OP_HASH160
    script_code.push_back(0x14);  // Push 20 bytes
    script_code.insert(script_code.end(), expected_pubkey_hash.begin(), expected_pubkey_hash.end());
    script_code.push_back(0x88);  // OP_EQUALVERIFY
    script_code.push_back(0xac);  // OP_CHECKSIG
    
    // 7. Compute signature hash (BIP143)
    // Phase M.6.2: Extract raw value for signature hashing
    std::vector<uint8_t> sighash = ComputeSignatureHash(tx, input_index, script_code, utxo.value.GetUna(), 1);
    
    // 8. Verify ECDSA signature with libsecp256k1
    secp256k1_context* ctx = dinero::crypto::GetSecp256k1ContextVerify();
    
    // Parse pubkey
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_parse(ctx, &pubkey, pubkey_bytes.data(), pubkey_bytes.size())) {
        error = "Failed to parse public key";
        return false;
    }
    
    // Parse signature (DER format, strip last byte which is hashtype)
    secp256k1_ecdsa_signature sig;
    size_t sig_len = signature_bytes.size();
    if (sig_len > 0 && signature_bytes[sig_len - 1] == 0x01) {
        sig_len--;  // Remove SIGHASH_ALL byte
    }
    
    if (!secp256k1_ecdsa_signature_parse_der(ctx, &sig, signature_bytes.data(), sig_len)) {
        error = "Failed to parse signature";
        return false;
    }
    
    // Verify signature
    int verify_result = secp256k1_ecdsa_verify(ctx, &sig, sighash.data(), &pubkey);
    
    if (verify_result != 1) {
        error = "Signature verification failed";
        return false;
    }
    
    return true;
}

// ============================================================================
// Taproot (BIP341) Sighash Computation
// ============================================================================

std::vector<uint8_t> ScriptVerifier::ComputeTaprootSighash(
    const Transaction& tx,
    size_t input_index,
    const std::vector<uint64_t>& prevout_values,
    const std::vector<std::vector<uint8_t>>& prevout_scripts,
    uint32_t hash_type,
    const std::vector<uint8_t>& tapleaf_hash,
    const std::vector<uint8_t>& annex
) {
    // BIP341: Taproot signature hash
    // https://github.com/bitcoin/bips/blob/master/bip-0341.mediawiki

    if (input_index >= tx.vin.size()) {
        return std::vector<uint8_t>(32, 0);
    }

    if (prevout_values.size() != tx.vin.size() || prevout_scripts.size() != tx.vin.size()) {
        return std::vector<uint8_t>(32, 0);
    }

    // Epoch (1 byte) - 0x00 for BIP341
    std::vector<uint8_t> data;
    data.push_back(0x00);

    // Hash type (1 byte)
    data.push_back(static_cast<uint8_t>(hash_type));

    // nVersion (4 bytes)
    for (int i = 0; i < 4; i++) {
        data.push_back((tx.version >> (i * 8)) & 0xff);
    }

    // nLockTime (4 bytes)
    for (int i = 0; i < 4; i++) {
        data.push_back((tx.lockTime >> (i * 8)) & 0xff);
    }

    // If hash_type is not ANYONECANPAY:
    // - sha_prevouts (32 bytes)
    // - sha_amounts (32 bytes)
    // - sha_scriptpubkeys (32 bytes)
    // - sha_sequences (32 bytes)

    bool anyonecanpay = (hash_type & 0x80) != 0;

    if (!anyonecanpay) {
        // Hash all prevouts
        {
            std::vector<uint8_t> prevouts_data;
            for (const auto& input : tx.vin) {
                // Phase M.4.3-B: Unwrap TxId for serialization
                const auto& txid_u256 = input.prevout.txid.AsUint256();
                std::vector<uint8_t> txid_bytes(txid_u256.data, txid_u256.data + 32);
                std::reverse(txid_bytes.begin(), txid_bytes.end());
                prevouts_data.insert(prevouts_data.end(), txid_bytes.begin(), txid_bytes.end());

                for (int i = 0; i < 4; i++) {
                    prevouts_data.push_back((input.prevout.vout >> (i * 8)) & 0xff);
                }
            }
            auto hash = DoubleSHA256(prevouts_data.data(), prevouts_data.size());
            data.insert(data.end(), hash.begin(), hash.end());
        }

        // Hash all amounts
        {
            std::vector<uint8_t> amounts_data;
            for (uint64_t value : prevout_values) {
                for (int i = 0; i < 8; i++) {
                    amounts_data.push_back((value >> (i * 8)) & 0xff);
                }
            }
            auto hash = DoubleSHA256(amounts_data.data(), amounts_data.size());
            data.insert(data.end(), hash.begin(), hash.end());
        }

        // Hash all scriptPubKeys
        {
            std::vector<uint8_t> scripts_data;
            for (const auto& script : prevout_scripts) {
                // Compact size length
                scripts_data.push_back(static_cast<uint8_t>(script.size()));
                scripts_data.insert(scripts_data.end(), script.begin(), script.end());
            }
            auto hash = DoubleSHA256(scripts_data.data(), scripts_data.size());
            data.insert(data.end(), hash.begin(), hash.end());
        }

        // Hash all sequences
        {
            std::vector<uint8_t> sequences_data;
            for (const auto& input : tx.vin) {
                for (int i = 0; i < 4; i++) {
                    sequences_data.push_back((input.sequence >> (i * 8)) & 0xff);
                }
            }
            auto hash = DoubleSHA256(sequences_data.data(), sequences_data.size());
            data.insert(data.end(), hash.begin(), hash.end());
        }
    }

    // If hash_type is neither NONE nor SINGLE:
    // - sha_outputs (32 bytes)

    uint8_t base_type = hash_type & 0x1f;
    if (base_type != 0x02 && base_type != 0x03) {  // Not NONE and not SINGLE
        std::vector<uint8_t> outputs_data;
        for (const auto& output : tx.vout) {
            // Value (8 bytes)
            // Phase M.6.1: Extract raw value for serialization
            uint64_t value_raw = output.value.GetUna();
            for (int i = 0; i < 8; i++) {
                outputs_data.push_back((value_raw >> (i * 8)) & 0xff);
            }

            // ScriptPubKey
            std::vector<uint8_t> spk_bytes(output.scriptPubKey.begin(), output.scriptPubKey.end());
            outputs_data.push_back(static_cast<uint8_t>(spk_bytes.size()));
            outputs_data.insert(outputs_data.end(), spk_bytes.begin(), spk_bytes.end());
        }
        auto hash = DoubleSHA256(outputs_data.data(), outputs_data.size());
        data.insert(data.end(), hash.begin(), hash.end());
    }

    // spend_type (1 byte)
    // BIP341: spend_type = 2*ext_flag + annex_present
    // ext_flag: 0 for key path, 1 for script path
    // annex_present: 1 if annex is provided
    uint8_t ext_flag = tapleaf_hash.empty() ? 0 : 1;
    uint8_t annex_present = annex.empty() ? 0 : 1;
    uint8_t spend_type = 2 * ext_flag + annex_present;
    data.push_back(spend_type);

    // If ANYONECANPAY:
    // - outpoint (36 bytes)
    // - amount (8 bytes)
    // - scriptPubKey (variable)
    // - nSequence (4 bytes)

    if (anyonecanpay) {
        const auto& input = tx.vin[input_index];

        // Outpoint - Phase M.4.3-B: Unwrap TxId for serialization
        const auto& txid_u256 = input.prevout.txid.AsUint256();
        std::vector<uint8_t> txid_bytes(txid_u256.data, txid_u256.data + 32);
        std::reverse(txid_bytes.begin(), txid_bytes.end());
        data.insert(data.end(), txid_bytes.begin(), txid_bytes.end());

        for (int i = 0; i < 4; i++) {
            data.push_back((input.prevout.vout >> (i * 8)) & 0xff);
        }

        // Amount
        uint64_t value = prevout_values[input_index];
        for (int i = 0; i < 8; i++) {
            data.push_back((value >> (i * 8)) & 0xff);
        }

        // ScriptPubKey
        const auto& script = prevout_scripts[input_index];
        data.push_back(static_cast<uint8_t>(script.size()));
        data.insert(data.end(), script.begin(), script.end());

        // Sequence
        for (int i = 0; i < 4; i++) {
            data.push_back((input.sequence >> (i * 8)) & 0xff);
        }
    } else {
        // Input index (4 bytes)
        for (int i = 0; i < 4; i++) {
            data.push_back((input_index >> (i * 8)) & 0xff);
        }
    }

    // BIP341: If annex is present, add sha_annex
    // sha_annex = SHA256(compact_size(len(annex)) || annex)
    if (!annex.empty()) {
        std::vector<uint8_t> annex_serialized;
        // Compact size length prefix
        if (annex.size() < 253) {
            annex_serialized.push_back(static_cast<uint8_t>(annex.size()));
        } else if (annex.size() <= 0xFFFF) {
            annex_serialized.push_back(253);
            annex_serialized.push_back(annex.size() & 0xFF);
            annex_serialized.push_back((annex.size() >> 8) & 0xFF);
        } else {
            annex_serialized.push_back(254);
            annex_serialized.push_back(annex.size() & 0xFF);
            annex_serialized.push_back((annex.size() >> 8) & 0xFF);
            annex_serialized.push_back((annex.size() >> 16) & 0xFF);
            annex_serialized.push_back((annex.size() >> 24) & 0xFF);
        }
        annex_serialized.insert(annex_serialized.end(), annex.begin(), annex.end());

        // SHA256 of serialized annex
        uint8_t annex_hash[32];
        dinero::crypto::CSHA256().Write(annex_serialized.data(), annex_serialized.size()).Finalize(annex_hash);
        data.insert(data.end(), annex_hash, annex_hash + 32);
    }

    // If script path spending, add tapleaf hash
    if (!tapleaf_hash.empty()) {
        data.insert(data.end(), tapleaf_hash.begin(), tapleaf_hash.end());
    }

    // Key version (1 byte) - always 0x00 for BIP341
    data.push_back(0x00);

    // Code separator position (4 bytes) - 0xFFFFFFFF for key path
    for (int i = 0; i < 4; i++) {
        data.push_back(0xFF);
    }

    // Hash with "TapSighash" tag (BIP340 tagged hash)
    // Tagged hash: SHA256(SHA256("TapSighash") || SHA256("TapSighash") || data)
    std::string tag = "TapSighash";
    std::vector<uint8_t> tag_bytes(tag.begin(), tag.end());

    // Compute SHA256(tag)
    uint8_t tag_hash[32];
    dinero::crypto::CSHA256().Write(tag_bytes.data(), tag_bytes.size()).Finalize(tag_hash);

    // Compute tagged hash: SHA256(tag_hash || tag_hash || data)
    dinero::crypto::CSHA256 hasher;
    hasher.Write(tag_hash, 32);
    hasher.Write(tag_hash, 32);
    hasher.Write(data.data(), data.size());

    uint8_t result[32];
    hasher.Finalize(result);

    return std::vector<uint8_t>(result, result + 32);
}

// ============================================================================
// Taproot (BIP341) Verification
// ============================================================================

bool ScriptVerifier::IsP2TR(const std::vector<uint8_t>& script_pubkey) {
    // P2TR: OP_1 OP_PUSH32 <32-byte-output-key>
    // Format: 0x51 0x20 <32 bytes>
    // Total: 34 bytes
    return (script_pubkey.size() == 34 &&
            script_pubkey[0] == 0x51 &&  // OP_1
            script_pubkey[1] == 0x20);    // Push 32 bytes
}

bool ScriptVerifier::IsOPCTCOMMIT(const std::vector<uint8_t>& script_pubkey) {
    // OP_CTCOMMIT: OP_2 OP_PUSH32 <32-byte-commitment-hash>
    // Format: 0x52 0x20 <32 bytes>
    // Total: 34 bytes
    // Witness version 2 for confidential transactions
    return (script_pubkey.size() == 34 &&
            script_pubkey[0] == 0x52 &&  // OP_2
            script_pubkey[1] == 0x20);    // Push 32 bytes
}

bool ScriptVerifier::VerifyOPCTCOMMIT(const Transaction& tx, size_t input_index,
                                       const UTXOEntry& utxo, std::string& error) {
    // Validate input_index
    if (input_index >= tx.vin.size()) {
        error = "Invalid input index";
        return false;
    }

    // OP_CTCOMMIT (witness v2) outputs are commitment-bearing outputs
    // They don't require traditional signature verification
    // The commitment validation is handled separately by ConfidentialTransactionValidator

    // Verify the UTXO scriptPubKey is valid OP_CTCOMMIT format
    if (!IsOPCTCOMMIT(utxo.scriptPubKey)) {
        error = "UTXO scriptPubKey is not OP_CTCOMMIT format";
        return false;
    }

    // Extract commitment hash from scriptPubKey (skip OP_2 and OP_PUSH32)
    std::vector<uint8_t> commitment_hash(utxo.scriptPubKey.begin() + 2, utxo.scriptPubKey.end());

    // Verify witness structure (should be empty for OP_CTCOMMIT)
    const auto& input = tx.vin[input_index];
    if (!input.witness.empty()) {
        error = "OP_CTCOMMIT inputs should have empty witness";
        return false;
    }

    // Note: Actual commitment validation (range proofs, balance equation) is performed
    // by ConfidentialTransactionValidator in block_validation.cpp
    // This function just ensures the script format is correct

    return true;
}

bool ScriptVerifier::VerifyTaproot(const Transaction& tx, size_t input_index,
                                   const std::vector<UTXOEntry>& input_utxos,
                                   std::string& error,
                                   uint32_t flags) {
    // Validate input_index and UTXO vector
    if (input_index >= tx.vin.size() || input_index >= input_utxos.size()) {
        error = "Invalid input index";
        return false;
    }

    if (input_utxos.size() != tx.vin.size()) {
        error = "UTXO vector size mismatch (expected " + std::to_string(tx.vin.size()) +
               ", got " + std::to_string(input_utxos.size()) + ")";
        return false;
    }

    const UTXOEntry& utxo = input_utxos[input_index];

    // 1. Check scriptPubKey is P2TR
    if (!IsP2TR(utxo.scriptPubKey)) {
        error = "Not a P2TR (Taproot) output";
        return false;
    }

    // 2. Extract 32-byte x-only public key from scriptPubKey
    // P2TR format: OP_1 OP_PUSH32 <32-byte-pubkey>
    std::vector<uint8_t> output_key(utxo.scriptPubKey.begin() + 2, utxo.scriptPubKey.end());

    if (output_key.size() != 32) {
        error = "Invalid P2TR output key size";
        return false;
    }

    // 3. Check witness data format
    const auto& input = tx.vin[input_index];
    if (input.witness.empty()) {
        error = "Missing witness data for Taproot";
        return false;
    }

    // Historical Dinero parsing looked for an annex in the first script-stack
    // item. BIP341 places it at the end of the witness and removes it before
    // key-path/script-path classification. Correct that behavior together with
    // CCV successor binding while preserving old blocks before activation.
    std::vector<std::vector<uint8_t>> effective_witness = input.witness;
    std::vector<uint8_t> annex;
    if ((flags & SCRIPT_VERIFY_CCV_SUCCESSOR_BINDING) &&
        effective_witness.size() >= 2 &&
        !effective_witness.back().empty() &&
        effective_witness.back()[0] == 0x50) {
        annex = effective_witness.back();
        effective_witness.pop_back();
    }

    // Determine spending path:
    // - Key path: witness has exactly 1 element (signature)
    // - Script path: witness has 2+ elements (stack items + script + control_block)

    bool is_key_path = (effective_witness.size() == 1);

    if (is_key_path) {
        // ============================================================
        // KEY PATH SPENDING (BIP341)
        // ============================================================

        const std::vector<uint8_t>& signature = effective_witness[0];

        // Schnorr signatures are 64 bytes (or 65 with sighash type)
        if (signature.size() != 64 && signature.size() != 65) {
            error = "Invalid Schnorr signature size (expected 64 or 65 bytes)";
            return false;
        }

        // Extract hash type (default is 0x00 for Taproot)
        uint32_t hash_type = 0;  // SIGHASH_DEFAULT
        if (signature.size() == 65) {
            hash_type = signature[64];  // Last byte is sighash type
        }

        // Compute Taproot sighash (BIP341)
        std::vector<uint64_t> prevout_values;
        std::vector<std::vector<uint8_t>> prevout_scripts;

        prevout_values.reserve(input_utxos.size());
        prevout_scripts.reserve(input_utxos.size());

        for (const auto& u : input_utxos) {
            // Phase M.6.2: Extract raw value for signature hashing
            prevout_values.push_back(u.value.GetUna());
            prevout_scripts.push_back(u.scriptPubKey);
        }

        std::vector<uint8_t> tapleaf_hash;  // Empty for key path

        std::vector<uint8_t> sighash = ComputeTaprootSighash(
            tx, input_index, prevout_values, prevout_scripts, hash_type,
            tapleaf_hash, annex
        );

        // Verify Schnorr signature with libsecp256k1
        secp256k1_context* ctx = dinero::crypto::GetSecp256k1ContextVerify();

        secp256k1_xonly_pubkey pubkey;
        if (!secp256k1_xonly_pubkey_parse(ctx, &pubkey, output_key.data())) {
            error = "Failed to parse Taproot public key";
            return false;
        }

        const uint8_t* sig_data = signature.data();
        int verify_result = secp256k1_schnorrsig_verify(ctx, sig_data, sighash.data(), sighash.size(), &pubkey);

        if (verify_result != 1) {
            error = "Schnorr signature verification failed";
            return false;
        }

        return true;
    }
    else {
        // ============================================================
        // SCRIPT PATH SPENDING (BIP342)
        // ============================================================

        // Script path witness: <stack items...> <script> <control_block>
        // Minimum: 2 elements (script + control_block)
        if (effective_witness.size() < 2) {
            error = "Invalid script path witness (need at least script + control block)";
            return false;
        }

        // Last element is control block
        const std::vector<uint8_t>& control_block = effective_witness.back();

        // Control block format: <leaf_version|parity> <internal_key> [<merkle_proof>...]
        // Minimum size: 33 bytes (1 + 32)
        // Maximum size: 33 + 32*128 = 4129 bytes (proof depth limited to 128)
        if (control_block.size() < 33 || control_block.size() > 4129) {
            error = "Invalid control block size";
            return false;
        }

        if ((control_block.size() - 33) % 32 != 0) {
            error = "Control block Merkle proof size not multiple of 32";
            return false;
        }

        // Second-to-last element is the revealed script
        const std::vector<uint8_t>& script =
            effective_witness[effective_witness.size() - 2];

        if (script.empty()) {
            error = "Empty Tapscript";
            return false;
        }

        // Parse control block
        uint8_t leaf_version = control_block[0] & 0xfe;  // Clear parity bit
        uint8_t parity_bit = control_block[0] & 0x01;

        std::vector<uint8_t> internal_key(control_block.begin() + 1, control_block.begin() + 33);

        // BIP342: Only leaf version 0xC0 is defined
        if (leaf_version != 0xC0) {
            error = "Unsupported Tapscript leaf version (expected 0xC0)";
            return false;
        }

        // Compute tapleaf hash: Tagged hash of (leaf_version || compact_size(script) || script)
        std::vector<uint8_t> tapleaf_data;
        tapleaf_data.push_back(leaf_version);
        // Compact size encoding (simplified for scripts < 253 bytes)
        if (script.size() < 253) {
            tapleaf_data.push_back(static_cast<uint8_t>(script.size()));
        } else {
            // TODO: Handle larger scripts with proper compact size encoding
            error = "Tapscript larger than 252 bytes not yet supported";
            return false;
        }
        tapleaf_data.insert(tapleaf_data.end(), script.begin(), script.end());

        // Tagged hash with "TapLeaf"
        std::string tag = "TapLeaf";
        std::vector<uint8_t> tag_bytes(tag.begin(), tag.end());
        uint8_t tag_hash[32];
        dinero::crypto::CSHA256().Write(tag_bytes.data(), tag_bytes.size()).Finalize(tag_hash);

        uint8_t tapleaf_hash_raw[32];
        dinero::crypto::CSHA256 hasher;
        hasher.Write(tag_hash, 32);
        hasher.Write(tag_hash, 32);
        hasher.Write(tapleaf_data.data(), tapleaf_data.size());
        hasher.Finalize(tapleaf_hash_raw);

        std::vector<uint8_t> tapleaf_hash(tapleaf_hash_raw, tapleaf_hash_raw + 32);

        // Verify Merkle proof (if present)
        std::vector<uint8_t> computed_root = tapleaf_hash;
        size_t proof_len = (control_block.size() - 33) / 32;

        for (size_t i = 0; i < proof_len; i++) {
            std::vector<uint8_t> proof_element(
                control_block.begin() + 33 + (i * 32),
                control_block.begin() + 33 + ((i + 1) * 32)
            );

            // Lexicographic ordering for branch hashing
            std::vector<uint8_t> branch_data;
            if (computed_root < proof_element) {
                branch_data.insert(branch_data.end(), computed_root.begin(), computed_root.end());
                branch_data.insert(branch_data.end(), proof_element.begin(), proof_element.end());
            } else {
                branch_data.insert(branch_data.end(), proof_element.begin(), proof_element.end());
                branch_data.insert(branch_data.end(), computed_root.begin(), computed_root.end());
            }

            // Tagged hash with "TapBranch"
            std::string branch_tag = "TapBranch";
            std::vector<uint8_t> branch_tag_bytes(branch_tag.begin(), branch_tag.end());
            uint8_t branch_tag_hash[32];
            dinero::crypto::CSHA256().Write(branch_tag_bytes.data(), branch_tag_bytes.size()).Finalize(branch_tag_hash);

            uint8_t branch_hash[32];
            dinero::crypto::CSHA256 branch_hasher;
            branch_hasher.Write(branch_tag_hash, 32);
            branch_hasher.Write(branch_tag_hash, 32);
            branch_hasher.Write(branch_data.data(), branch_data.size());
            branch_hasher.Finalize(branch_hash);

            computed_root = std::vector<uint8_t>(branch_hash, branch_hash + 32);
        }

        // Verify output key matches (internal_key tweaked with Merkle root)
        // BIP341: P = Q + tagged_hash("TapTweak", Q || m) * G
        // Where:
        //   P = output_key (from scriptPubKey)
        //   Q = internal_key (from control block)
        //   m = computed_root (Merkle root)

        // Compute TapTweak = tagged_hash("TapTweak", internal_key || merkle_root)
        std::vector<uint8_t> tweak_data;
        tweak_data.insert(tweak_data.end(), internal_key.begin(), internal_key.end());
        tweak_data.insert(tweak_data.end(), computed_root.begin(), computed_root.end());

        // Tagged hash with "TapTweak"
        std::string tweak_tag = "TapTweak";
        std::vector<uint8_t> tweak_tag_bytes(tweak_tag.begin(), tweak_tag.end());
        uint8_t tweak_tag_hash[32];
        dinero::crypto::CSHA256().Write(tweak_tag_bytes.data(), tweak_tag_bytes.size()).Finalize(tweak_tag_hash);

        uint8_t tap_tweak[32];
        dinero::crypto::CSHA256 tweak_hasher;
        tweak_hasher.Write(tweak_tag_hash, 32);
        tweak_hasher.Write(tweak_tag_hash, 32);
        tweak_hasher.Write(tweak_data.data(), tweak_data.size());
        tweak_hasher.Finalize(tap_tweak);

        // Use secp256k1 to compute tweaked public key
        secp256k1_context* ctx_verify = dinero::crypto::GetSecp256k1ContextVerify();

        // Parse internal key as x-only pubkey
        secp256k1_xonly_pubkey internal_pubkey;
        if (!secp256k1_xonly_pubkey_parse(ctx_verify, &internal_pubkey, internal_key.data())) {
            error = "Invalid internal public key";
            return false;
        }

        // Parse expected output key for comparison
        secp256k1_xonly_pubkey expected_output_pubkey;
        if (!secp256k1_xonly_pubkey_parse(ctx_verify, &expected_output_pubkey, output_key.data())) {
            error = "Invalid output public key";
            return false;
        }

        // Compute tweaked pubkey: P = Q + tweak * G
        secp256k1_pubkey tweaked_pubkey;
        int parity_ignored;
        if (!secp256k1_xonly_pubkey_tweak_add(ctx_verify, &tweaked_pubkey, &internal_pubkey, tap_tweak)) {
            error = "Failed to compute tweaked public key";
            return false;
        }

        // Convert tweaked pubkey to x-only format
        secp256k1_xonly_pubkey computed_output_xonly;
        if (!secp256k1_xonly_pubkey_from_pubkey(ctx_verify, &computed_output_xonly, &parity_ignored, &tweaked_pubkey)) {
            error = "Failed to convert tweaked pubkey to x-only";
            return false;
        }

        // Serialize both and compare
        uint8_t computed_output_bytes[32];
        uint8_t expected_output_bytes[32];
        secp256k1_xonly_pubkey_serialize(ctx_verify, computed_output_bytes, &computed_output_xonly);
        secp256k1_xonly_pubkey_serialize(ctx_verify, expected_output_bytes, &expected_output_pubkey);

        if (memcmp(computed_output_bytes, expected_output_bytes, 32) != 0) {
            error = "Output key does not match tweaked internal key (script tree commitment mismatch)";
            return false;
        }

        // Execute Tapscript (BIP342)
        // Witness stack: <stack items...> <script> <control_block>
        // Extract witness stack (exclude script and control block)
        std::vector<std::vector<uint8_t>> witness_stack;
        if (effective_witness.size() > 2) {
            witness_stack.insert(witness_stack.end(),
                               effective_witness.begin(),
                               effective_witness.end() - 2);
        }

        // Preserve the historical parser before successor-binding activation.
        // Its first-item rule is intentionally not used by the activated path.
        if (!(flags & SCRIPT_VERIFY_CCV_SUCCESSOR_BINDING) &&
            !witness_stack.empty() &&
            !witness_stack[0].empty() &&
            witness_stack[0][0] == 0x50) {
            annex = witness_stack[0];
            witness_stack.erase(witness_stack.begin());
        }

        std::array<uint8_t, 32> control_internal_key{};
        std::copy(internal_key.begin(), internal_key.end(),
                  control_internal_key.begin());
        std::array<uint8_t, 32> taproot_merkle_root{};
        std::copy(computed_root.begin(), computed_root.end(),
                  taproot_merkle_root.begin());

        // Execute Tapscript
        // Phase L0.3: Pass SCRIPT_VERIFY_STANDARD flags to enable covenant enforcement
        // BIP341: Pass annex for inclusion in sighash computation
        std::string script_error;
        bool script_valid = TapscriptInterpreter::ExecuteTapscript(
            script,
            witness_stack,
            tx,
            input_index,
            input_utxos,
            tapleaf_hash,
            control_internal_key,
            taproot_merkle_root,
            parity_bit,
            flags,
            script_error,
            annex  // BIP341 annex for sighash
        );

        if (!script_valid) {
            error = "Tapscript execution failed: " + script_error;
            return false;
        }

        return true;
    }
}

} // namespace consensus
} // namespace dinero
