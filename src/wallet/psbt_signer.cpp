#include "wallet/psbt_signer.h"
#include "wallet/psbt.h"
#include "wallet/psbt_taproot_validator.h"
#include "wallet/taproot_keys.h"
#include "wallet/taproot_sighash.h"
#include "wallet/taproot_control_block.h"
#include "wallet/psbt_witness_utxo_decode.h"
#include "crypto/tagged_hash.h"
#include "crypto/ripemd160.h"
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>
#include <vector>
#include <cstring>
#include <iostream>

namespace din {

// Forward declarations
static std::vector<uint8_t> taggedHashPsbt(const std::string& tag, const std::vector<uint8_t>& data);

// Helper: Extract witness UTXO from PSBT input
static bool extractWitnessUtxo(const PsbtInput& input, uint64_t& amount, std::vector<uint8_t>& scriptPubKey) {
    for (const auto& kv : input.kv) {
        if (kv.key.empty()) continue;
        uint8_t type = kv.key[0];
        
        if (type == static_cast<uint8_t>(PsbtIn::WitnessUtxo)) {
            const auto decoded = din::psbt::DecodeWitnessUtxoValue(kv.value);
            if (!decoded.ok) return false;
            amount = decoded.amount;
            scriptPubKey = decoded.script_pubkey;
            return !scriptPubKey.empty();
        }
    }
    return false;
}

// Helper: Calculate BIP143 sighash for witness v0
static std::vector<uint8_t> calculateBIP143Sighash(
    const Psbt& psbt,
    size_t input_index,
    const std::vector<uint8_t>& scriptCode,
    uint64_t amount,
    uint32_t sighash_type = 1 // SIGHASH_ALL
) {
    // Simplified BIP143 sighash calculation
    // Full implementation would need transaction parsing from PSBT globals
    
    std::vector<uint8_t> preimage;
    
    // For now, create a simple hash that includes:
    // - PSBT version
    // - Input index
    // - Script code
    // - Amount
    
    preimage.push_back(psbt.version);
    
    // Add input index (4 bytes, little-endian)
    for (int i = 0; i < 4; i++) {
        preimage.push_back(static_cast<uint8_t>((input_index >> (i * 8)) & 0xFF));
    }
    
    // Add amount (8 bytes, little-endian)
    for (int i = 0; i < 8; i++) {
        preimage.push_back(static_cast<uint8_t>((amount >> (i * 8)) & 0xFF));
    }
    
    // Add script code
    preimage.insert(preimage.end(), scriptCode.begin(), scriptCode.end());
    
    // Add sighash type (4 bytes, little-endian)
    for (int i = 0; i < 4; i++) {
        preimage.push_back(static_cast<uint8_t>((sighash_type >> (i * 8)) & 0xFF));
    }
    
    // Double SHA256
    uint8_t hash1[32];
    uint8_t hash2[32];
    dinero::crypto::CSHA256().Write(preimage.data(), preimage.size()).Finalize(hash1);
    dinero::crypto::CSHA256().Write(hash1, 32).Finalize(hash2);
    
    return std::vector<uint8_t>(hash2, hash2 + 32);
}

// Helper: Add partial signature to PSBT input
static void addPartialSignature(PsbtInput& input, const std::vector<uint8_t>& pubkey, const std::vector<uint8_t>& signature) {
    // Create PSBT_IN_PARTIAL_SIG key: <type=0x02><pubkey>
    std::vector<uint8_t> key;
    key.push_back(static_cast<uint8_t>(PsbtIn::PartialSig));
    key.insert(key.end(), pubkey.begin(), pubkey.end());

    // Check if signature already exists
    for (auto& kv : input.kv) {
        if (kv.key == key) {
            // Update existing signature
            kv.value = signature;
            return;
        }
    }

    // Add new signature
    input.kv.emplace_back(std::move(key), signature);
}

// Helper: Add Taproot key-path signature to PSBT input (BIP371)
static void addTaprootKeySignature(PsbtInput& input, const std::vector<uint8_t>& signature) {
    // PSBT_IN_TAP_KEY_SIG = 0x13, key is just the type byte
    std::vector<uint8_t> key = {0x13};

    // Check if signature already exists
    for (auto& kv : input.kv) {
        if (kv.key == key) {
            kv.value = signature;
            return;
        }
    }

    // Add new signature
    input.kv.emplace_back(std::move(key), signature);
}

// Helper: Extract TAP_INTERNAL_KEY from PSBT input
static bool extractTapInternalKey(const PsbtInput& input, std::vector<uint8_t>& internal_key) {
    for (const auto& kv : input.kv) {
        if (kv.key.size() == 1 && kv.key[0] == 0x17) {  // TAP_INTERNAL_KEY
            if (kv.value.size() == 32) {
                internal_key = kv.value;
                return true;
            }
        }
    }
    return false;
}

// Helper: Extract TAP_BIP32_DERIVATION to get key path
static bool extractTapBip32Derivation(const PsbtInput& input, std::vector<uint8_t>& xonly_pubkey, std::string& key_path) {
    for (const auto& kv : input.kv) {
        if (kv.key.size() == 33 && kv.key[0] == 0x16) {  // TAP_BIP32_DERIVATION
            // Key format: <type=0x16><32-byte xonly pubkey>
            xonly_pubkey.assign(kv.key.begin() + 1, kv.key.end());

            // Value format: <leaf_hashes><4-byte fingerprint><path>
            // For key-path spending with no scripts, leaf_hashes is empty (starts with 0x00 count)
            // We need to extract the derivation path
            if (kv.value.size() >= 5) {
                // Skip leaf hash count and hashes
                size_t pos = 0;
                uint8_t leaf_count = kv.value[pos++];
                pos += leaf_count * 32;  // Skip leaf hashes

                if (pos + 4 <= kv.value.size()) {
                    // Skip fingerprint
                    pos += 4;

                    // Read derivation path indices
                    std::string path = "m";
                    while (pos + 4 <= kv.value.size()) {
                        uint32_t index = 0;
                        for (int i = 0; i < 4; i++) {
                            index |= static_cast<uint32_t>(kv.value[pos++]) << (i * 8);
                        }

                        if (index >= 0x80000000) {
                            path += "/" + std::to_string(index - 0x80000000) + "'";
                        } else {
                            path += "/" + std::to_string(index);
                        }
                    }
                    key_path = path;
                    return true;
                }
            }
        }
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
// Script-Path Spending Helpers (BIP342)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * TAP_LEAF_SCRIPT entry parsed from PSBT
 */
struct TapLeafScriptEntry {
    uint8_t leaf_version;                   // Leaf version (0xC0 for Tapscript)
    std::vector<uint8_t> script;            // The Tapscript
    std::vector<uint8_t> control_block;     // Control block (proof of inclusion)
    std::array<uint8_t, 32> leaf_hash;      // Computed leaf hash
};

/**
 * Extract TAP_LEAF_SCRIPT entries from PSBT input
 *
 * TAP_LEAF_SCRIPT (0x15) format:
 * - Key: <0x15><control_block>
 * - Value: <script><leaf_version>
 */
static bool extractTapLeafScripts(const PsbtInput& input, std::vector<TapLeafScriptEntry>& leaf_scripts) {
    leaf_scripts.clear();

    for (const auto& kv : input.kv) {
        if (kv.key.empty()) continue;

        if (kv.key[0] == 0x15) {  // TAP_LEAF_SCRIPT
            // Key contains control block after type byte
            if (kv.key.size() < 34) continue;  // Min: 1 type + 33 control block

            TapLeafScriptEntry entry;

            // Extract control block from key (after type byte)
            entry.control_block.assign(kv.key.begin() + 1, kv.key.end());

            // Value contains: <script><leaf_version>
            if (kv.value.empty()) continue;

            // Leaf version is the last byte of value
            entry.leaf_version = kv.value.back();

            // Script is everything except the last byte
            entry.script.assign(kv.value.begin(), kv.value.end() - 1);

            // Compute leaf hash
            if (!dinero::TaprootKeys::ComputeTapleafHash(entry.script, entry.leaf_version, entry.leaf_hash)) {
                continue;
            }

            leaf_scripts.push_back(std::move(entry));
        }
    }

    return !leaf_scripts.empty();
}

/**
 * Add TAP_SCRIPT_SIG to PSBT input
 *
 * TAP_SCRIPT_SIG (0x14) format:
 * - Key: <0x14><32-byte xonly pubkey><32-byte leaf_hash>
 * - Value: <signature>
 */
static void addTaprootScriptSignature(
    PsbtInput& input,
    const std::vector<uint8_t>& xonly_pubkey,
    const std::array<uint8_t, 32>& leaf_hash,
    const std::vector<uint8_t>& signature
) {
    if (xonly_pubkey.size() != 32) return;

    // Build key: <0x14><xonly_pubkey><leaf_hash>
    std::vector<uint8_t> key;
    key.reserve(1 + 32 + 32);
    key.push_back(0x14);  // TAP_SCRIPT_SIG
    key.insert(key.end(), xonly_pubkey.begin(), xonly_pubkey.end());
    key.insert(key.end(), leaf_hash.begin(), leaf_hash.end());

    // Check if signature already exists
    for (auto& kv : input.kv) {
        if (kv.key == key) {
            kv.value = signature;
            return;
        }
    }

    // Add new signature
    input.kv.emplace_back(std::move(key), signature);
}

/**
 * Extract x-only pubkeys from a Tapscript (simplified)
 *
 * Looks for 32-byte pushes followed by CHECKSIG/CHECKSIGADD opcodes.
 * This is a simplified parser for common script patterns.
 */
static std::vector<std::vector<uint8_t>> extractScriptPubkeys(const std::vector<uint8_t>& script) {
    std::vector<std::vector<uint8_t>> pubkeys;

    // OP_CHECKSIG = 0xac, OP_CHECKSIGADD = 0xba
    // Look for: <32-byte push (0x20)><pubkey><OP_CHECKSIG or OP_CHECKSIGADD>
    for (size_t i = 0; i + 33 < script.size(); i++) {
        if (script[i] == 0x20) {  // OP_PUSHBYTES_32
            // Check if followed by pubkey and then CHECKSIG variant
            if (i + 33 < script.size()) {
                uint8_t op_after = script[i + 33];
                if (op_after == 0xac || op_after == 0xba) {  // CHECKSIG or CHECKSIGADD
                    std::vector<uint8_t> pubkey(script.begin() + i + 1, script.begin() + i + 33);
                    pubkeys.push_back(pubkey);
                }
            }
        }
    }

    return pubkeys;
}

/**
 * Compute BIP341 script-path sighash from PSBT
 *
 * Similar to key-path sighash but with script extension data:
 * - spend_type = 0x02 (ext_flag=1, no annex)
 * - Extension: tapleaf_hash || key_version || codesep_pos
 */
static std::vector<uint8_t> computeScriptPathSighashFromPsbt(
    const Psbt& psbt,
    size_t input_index,
    const std::vector<uint64_t>& all_amounts,
    const std::vector<std::vector<uint8_t>>& all_scriptpubkeys,
    const std::array<uint8_t, 32>& leaf_hash,
    uint8_t sighash_type = 0x00
) {
    // Build signature message per BIP341 (script-path variant)
    std::vector<uint8_t> sig_msg;

    // 1. Epoch (1 byte) - always 0x00
    sig_msg.push_back(0x00);

    // 2. Sighash type (1 byte)
    sig_msg.push_back(sighash_type);

    // 3. nVersion (4 bytes) - extract from PSBT global
    uint32_t version = 2;  // Default to version 2
    for (const auto& kv : psbt.globals) {
        if (!kv.key.empty() && kv.key[0] == 0x00 && kv.value.size() >= 4) {
            version = kv.value[0] | (kv.value[1] << 8) | (kv.value[2] << 16) | (kv.value[3] << 24);
            break;
        }
    }
    for (int i = 0; i < 4; i++) {
        sig_msg.push_back(static_cast<uint8_t>((version >> (i * 8)) & 0xFF));
    }

    // 4. nLockTime (4 bytes) - extract from PSBT global or default
    uint32_t locktime = 0;
    for (const auto& kv : psbt.globals) {
        if (!kv.key.empty() && kv.key[0] == 0x01 && kv.value.size() >= 4) {
            locktime = kv.value[0] | (kv.value[1] << 8) | (kv.value[2] << 16) | (kv.value[3] << 24);
            break;
        }
    }
    for (int i = 0; i < 4; i++) {
        sig_msg.push_back(static_cast<uint8_t>((locktime >> (i * 8)) & 0xFF));
    }

    // 5-9. Hash commitments (same as key-path)
    // sha_prevouts, sha_amounts, sha_scriptpubkeys, sha_sequences, sha_outputs

    // Build prevouts data
    std::vector<uint8_t> prevouts_data;
    for (const auto& inp : psbt.inputs) {
        for (const auto& kv : inp.kv) {
            if (!kv.key.empty() && kv.key[0] == 0x0e && kv.key.size() == 37) {
                prevouts_data.insert(prevouts_data.end(), kv.key.begin() + 1, kv.key.end());
                break;
            }
        }
    }
    uint8_t sha_prevouts[32];
    dinero::crypto::CSHA256().Write(prevouts_data.data(), prevouts_data.size()).Finalize(sha_prevouts);
    sig_msg.insert(sig_msg.end(), sha_prevouts, sha_prevouts + 32);

    // sha_amounts
    std::vector<uint8_t> amounts_data;
    for (uint64_t amt : all_amounts) {
        for (int i = 0; i < 8; i++) {
            amounts_data.push_back(static_cast<uint8_t>((amt >> (i * 8)) & 0xFF));
        }
    }
    uint8_t sha_amounts[32];
    dinero::crypto::CSHA256().Write(amounts_data.data(), amounts_data.size()).Finalize(sha_amounts);
    sig_msg.insert(sig_msg.end(), sha_amounts, sha_amounts + 32);

    // sha_scriptpubkeys
    std::vector<uint8_t> spk_data;
    for (const auto& spk : all_scriptpubkeys) {
        if (spk.size() < 253) {
            spk_data.push_back(static_cast<uint8_t>(spk.size()));
        } else {
            spk_data.push_back(0xfd);
            spk_data.push_back(spk.size() & 0xff);
            spk_data.push_back((spk.size() >> 8) & 0xff);
        }
        spk_data.insert(spk_data.end(), spk.begin(), spk.end());
    }
    uint8_t sha_scriptpubkeys[32];
    dinero::crypto::CSHA256().Write(spk_data.data(), spk_data.size()).Finalize(sha_scriptpubkeys);
    sig_msg.insert(sig_msg.end(), sha_scriptpubkeys, sha_scriptpubkeys + 32);

    // sha_sequences
    std::vector<uint8_t> seq_data;
    for (const auto& inp : psbt.inputs) {
        uint32_t seq = 0xffffffff;  // Default sequence
        for (const auto& kv : inp.kv) {
            if (!kv.key.empty() && kv.key[0] == 0x0e && kv.key.size() == 37 && kv.value.size() >= 4) {
                // Sequence might be in the input
            }
        }
        for (int i = 0; i < 4; i++) {
            seq_data.push_back(static_cast<uint8_t>((seq >> (i * 8)) & 0xFF));
        }
    }
    uint8_t sha_sequences[32];
    dinero::crypto::CSHA256().Write(seq_data.data(), seq_data.size()).Finalize(sha_sequences);
    sig_msg.insert(sig_msg.end(), sha_sequences, sha_sequences + 32);

    // sha_outputs - from PSBT outputs
    std::vector<uint8_t> outputs_data;
    for (const auto& out : psbt.outputs) {
        for (const auto& kv : out.kv) {
            // TODO: Extract output data
        }
    }
    uint8_t sha_outputs[32];
    dinero::crypto::CSHA256().Write(outputs_data.data(), outputs_data.size()).Finalize(sha_outputs);
    sig_msg.insert(sig_msg.end(), sha_outputs, sha_outputs + 32);

    // 10. spend_type (1 byte) - 0x02 for script-path (ext_flag=1, no annex)
    sig_msg.push_back(0x02);

    // 11. input_index (4 bytes)
    uint32_t idx = static_cast<uint32_t>(input_index);
    for (int i = 0; i < 4; i++) {
        sig_msg.push_back(static_cast<uint8_t>((idx >> (i * 8)) & 0xFF));
    }

    // EXTENSION DATA (for script-path):

    // 12. tapleaf_hash (32 bytes)
    sig_msg.insert(sig_msg.end(), leaf_hash.begin(), leaf_hash.end());

    // 13. key_version (1 byte) - 0x00 for BIP342 Tapscript
    sig_msg.push_back(0x00);

    // 14. codesep_pos (4 bytes) - 0xffffffff if no OP_CODESEPARATOR
    for (int i = 0; i < 4; i++) {
        sig_msg.push_back(0xff);
    }

    // Return TaggedHash("TapSighash", sig_msg)
    return taggedHashPsbt("TapSighash", sig_msg);
}

/**
 * Sign with untweaked key for script-path spending
 *
 * Script-path uses the raw internal key - no TapTweak applied.
 */
static bool signSchnorrRaw(
    const std::vector<uint8_t>& sighash,
    const std::vector<uint8_t>& privkey,
    std::vector<uint8_t>& signature_out
) {
    if (sighash.size() != 32 || privkey.size() != 32) {
        return false;
    }

    std::array<uint8_t, 32> privkey_arr;
    std::array<uint8_t, 32> msg_arr;
    std::copy(privkey.begin(), privkey.end(), privkey_arr.begin());
    std::copy(sighash.begin(), sighash.end(), msg_arr.begin());

    std::array<uint8_t, 64> sig_arr;
    uint8_t aux_rand[32] = {0};

    // Use SignSchnorr (no tweaking) for script-path
    if (!dinero::TaprootKeys::SignSchnorr(sig_arr, msg_arr, privkey_arr, aux_rand)) {
        return false;
    }

    signature_out.assign(sig_arr.begin(), sig_arr.end());
    return true;
}

// Helper: BIP340 Tagged Hash — delegates to canonical implementation
static std::vector<uint8_t> taggedHashPsbt(const std::string& tag, const std::vector<uint8_t>& data) {
    return dinero::crypto::TaggedHash(tag, data);
}

// Helper: Compute BIP341 Taproot key-path sighash from PSBT
static std::vector<uint8_t> computeTaprootSighashFromPsbt(
    const Psbt& psbt,
    size_t input_index,
    const std::vector<uint64_t>& all_amounts,
    const std::vector<std::vector<uint8_t>>& all_scriptpubkeys,
    uint8_t sighash_type = 0x00  // SIGHASH_DEFAULT
) {
    // BIP341 signature message construction

    // Compute sha_prevouts (hash of all outpoints)
    std::vector<uint8_t> prevouts_data;
    for (const auto& input : psbt.inputs) {
        // Extract prevout from PSBT input
        // For each input, we need txid (32 bytes) + vout (4 bytes)
        // This should come from PSBT global tx or input fields
        // Simplified: assume txid/vout available somewhere
        prevouts_data.insert(prevouts_data.end(), 36, 0);  // Placeholder
    }
    uint8_t sha_prevouts[32];
    dinero::crypto::CSHA256().Write(prevouts_data.data(), prevouts_data.size()).Finalize(sha_prevouts);

    // Compute sha_amounts (hash of all input amounts)
    std::vector<uint8_t> amounts_data;
    for (uint64_t amt : all_amounts) {
        for (int i = 0; i < 8; i++) {
            amounts_data.push_back(static_cast<uint8_t>((amt >> (i * 8)) & 0xFF));
        }
    }
    uint8_t sha_amounts[32];
    dinero::crypto::CSHA256().Write(amounts_data.data(), amounts_data.size()).Finalize(sha_amounts);

    // Compute sha_scriptpubkeys (hash of all input scriptPubKeys)
    std::vector<uint8_t> spks_data;
    for (const auto& spk : all_scriptpubkeys) {
        // CompactSize length prefix
        if (spk.size() < 253) {
            spks_data.push_back(static_cast<uint8_t>(spk.size()));
        } else {
            spks_data.push_back(0xfd);
            spks_data.push_back(spk.size() & 0xff);
            spks_data.push_back((spk.size() >> 8) & 0xff);
        }
        spks_data.insert(spks_data.end(), spk.begin(), spk.end());
    }
    uint8_t sha_scriptpubkeys[32];
    dinero::crypto::CSHA256().Write(spks_data.data(), spks_data.size()).Finalize(sha_scriptpubkeys);

    // Compute sha_sequences (hash of all input sequences)
    std::vector<uint8_t> sequences_data;
    for (size_t i = 0; i < psbt.inputs.size(); i++) {
        // Default sequence: 0xffffffff
        uint32_t seq = 0xffffffff;
        for (int j = 0; j < 4; j++) {
            sequences_data.push_back(static_cast<uint8_t>((seq >> (j * 8)) & 0xFF));
        }
    }
    uint8_t sha_sequences[32];
    dinero::crypto::CSHA256().Write(sequences_data.data(), sequences_data.size()).Finalize(sha_sequences);

    // Compute sha_outputs (hash of all outputs)
    std::vector<uint8_t> outputs_data;
    for (const auto& output : psbt.outputs) {
        // Extract output value and scriptPubKey
        // Simplified - would need proper PSBT output parsing
        outputs_data.insert(outputs_data.end(), 8, 0);  // Placeholder value
        outputs_data.push_back(0);  // Placeholder empty scriptPubKey
    }
    uint8_t sha_outputs[32];
    dinero::crypto::CSHA256().Write(outputs_data.data(), outputs_data.size()).Finalize(sha_outputs);

    // Build signature message
    std::vector<uint8_t> sig_msg;

    // 1. Epoch (1 byte) - always 0 for now
    sig_msg.push_back(0x00);

    // 2. Sighash type (1 byte)
    sig_msg.push_back(sighash_type);

    // 3. nVersion (4 bytes) - from PSBT global tx
    uint32_t version = 2;
    for (int i = 0; i < 4; i++) {
        sig_msg.push_back(static_cast<uint8_t>((version >> (i * 8)) & 0xFF));
    }

    // 4. nLockTime (4 bytes) - from PSBT global tx
    uint32_t locktime = 0;
    for (int i = 0; i < 4; i++) {
        sig_msg.push_back(static_cast<uint8_t>((locktime >> (i * 8)) & 0xFF));
    }

    // 5. sha_prevouts (32 bytes)
    sig_msg.insert(sig_msg.end(), sha_prevouts, sha_prevouts + 32);

    // 6. sha_amounts (32 bytes)
    sig_msg.insert(sig_msg.end(), sha_amounts, sha_amounts + 32);

    // 7. sha_scriptpubkeys (32 bytes)
    sig_msg.insert(sig_msg.end(), sha_scriptpubkeys, sha_scriptpubkeys + 32);

    // 8. sha_sequences (32 bytes)
    sig_msg.insert(sig_msg.end(), sha_sequences, sha_sequences + 32);

    // 9. sha_outputs (32 bytes)
    sig_msg.insert(sig_msg.end(), sha_outputs, sha_outputs + 32);

    // 10. spend_type (1 byte) - 0x00 for key-path, no annex
    sig_msg.push_back(0x00);

    // 11. input_index (4 bytes)
    uint32_t idx = static_cast<uint32_t>(input_index);
    for (int i = 0; i < 4; i++) {
        sig_msg.push_back(static_cast<uint8_t>((idx >> (i * 8)) & 0xFF));
    }

    // Return TaggedHash("TapSighash", sig_msg)
    return taggedHashPsbt("TapSighash", sig_msg);
}

// Helper: Sign with Schnorr using tweaked keypair (BIP341 compliant)
static bool signSchnorrTweaked(
    const std::vector<uint8_t>& sighash,
    const std::vector<uint8_t>& internal_privkey,
    const std::vector<uint8_t>& internal_xonly_pubkey,
    std::vector<uint8_t>& signature_out
) {
    if (sighash.size() != 32 || internal_privkey.size() != 32 || internal_xonly_pubkey.size() != 32) {
        return false;
    }

    // Use the correct API that handles Y parity by signing directly with tweaked keypair
    std::array<uint8_t, 32> privkey_arr;
    std::array<uint8_t, 32> pubkey_arr;
    std::array<uint8_t, 32> msg_arr;
    std::copy(internal_privkey.begin(), internal_privkey.end(), privkey_arr.begin());
    std::copy(internal_xonly_pubkey.begin(), internal_xonly_pubkey.end(), pubkey_arr.begin());
    std::copy(sighash.begin(), sighash.end(), msg_arr.begin());

    std::array<uint8_t, 64> sig_arr;
    uint8_t aux_rand[32] = {0};

    // SignSchnorrWithInternalKey applies the tweak internally and signs with the tweaked keypair
    // This correctly handles Y parity by using secp256k1_keypair_xonly_tweak_add
    if (!dinero::TaprootKeys::SignSchnorrWithInternalKey(sig_arr, msg_arr, privkey_arr, pubkey_arr, aux_rand)) {
        return false;
    }

    signature_out.assign(sig_arr.begin(), sig_arr.end());
    return true;
}

// PsbtSigner implementation
PsbtSigner::PsbtSigner(std::shared_ptr<IKeyStore> keystore, std::string wallet_policy)
    : keystore_(std::move(keystore))
    , wallet_policy_(wallet_policy == "bip86" ? "bip86" : "bip84") {}

PsbtSignResult PsbtSigner::signPsbt(Psbt& psbt) {
    PsbtSignResult result;

    // Check keystore availability
    if (!keystore_) {
        result.success = false;
        result.error = "No keystore available (wallet may not be loaded)";
        return result;
    }

    // Check if watch-only wallet
    bool is_watch_only = !keystore_->canSign("m");
    if (is_watch_only) {
        result.success = false;
        result.error = "Cannot sign PSBT: wallet is watch-only (use hardware wallet or import private keys)";
        return result;
    }

    // Wallet policy is provided by the caller from active wallet context.
    const std::string wallet_policy = wallet_policy_;

    // Iterate through all inputs and sign those we can
    for (size_t i = 0; i < psbt.inputs.size(); i++) {
        auto& input = psbt.inputs[i];

        // Extract witness UTXO (required for segwit signing)
        uint64_t amount = 0;
        std::vector<uint8_t> scriptPubKey;
        if (!extractWitnessUtxo(input, amount, scriptPubKey)) {
            result.addInputError(i, "Missing witness UTXO (required for SegWit signing)", "warning");
            continue;
        }

        // BIP86 TAPROOT GUARDRAIL: Validate Taproot inputs for policy compliance
        auto validation = dinero::PSBTTaprootValidator::validateInput(input.kv, wallet_policy);
        if (!validation.valid) {
            // Reject PSBT with script-path spending for BIP86 wallets
            std::string policy_error = "Policy violation: ";
            if (wallet_policy == "bip86") {
                policy_error += "Taproot script-path spending rejected (BIP86 wallets are key-path only)";
            } else {
                policy_error += validation.error;
            }
            result.addInputError(i, policy_error, "error");
            result.success = false;
            result.error = policy_error + " at input " + std::to_string(i);
            return result;
        }

        // Detect input type
        std::string input_type = "unknown";
        bool is_p2wpkh = (scriptPubKey.size() == 22 && scriptPubKey[0] == 0x00 && scriptPubKey[1] == 0x14);
        bool is_p2tr = (scriptPubKey.size() == 34 && scriptPubKey[0] == 0x51 && scriptPubKey[1] == 0x20);

        if (is_p2wpkh) {
            input_type = "P2WPKH (BIP84 Native SegWit)";
        } else if (is_p2tr) {
            input_type = "P2TR (BIP86 Taproot)";
        }

        // Sign based on input type
        if (is_p2wpkh) {
            // Calculate BIP143 sighash for P2WPKH
            std::vector<uint8_t> scriptCode = {0x76, 0xa9, 0x14}; // OP_DUP OP_HASH160 OP_PUSH(20)
            scriptCode.insert(scriptCode.end(), scriptPubKey.begin() + 2, scriptPubKey.end());
            scriptCode.push_back(0x88); // OP_EQUALVERIFY
            scriptCode.push_back(0xac); // OP_CHECKSIG

            // Calculate sighash
            std::vector<uint8_t> sighash = calculateBIP143Sighash(psbt, i, scriptCode, amount);

            // Sign with keystore
            auto signature_opt = keystore_->sign(sighash, "m");
            if (!signature_opt.has_value()) {
                result.addInputError(i, "Missing private key for input " + std::to_string(i), "error");
                continue;
            }

            // Get signature and append sighash type
            std::vector<uint8_t> signature = signature_opt.value();
            signature.push_back(0x01); // SIGHASH_ALL

            // Get public key from keystore
            std::vector<uint8_t> pubkey(33, 0x02); // Simplified

            // Add partial signature to PSBT
            addPartialSignature(input, pubkey, signature);

            result.signed_count++;

        } else if (is_p2tr) {
            // ═══════════════════════════════════════════════════════════════════════
            // P2TR Signing (BIP341/BIP342)
            // ═══════════════════════════════════════════════════════════════════════

            // Build UTXO info for all inputs (BIP341 requires all input amounts)
            std::vector<uint64_t> all_amounts;
            std::vector<std::vector<uint8_t>> all_scriptpubkeys;

            for (size_t j = 0; j < psbt.inputs.size(); j++) {
                uint64_t inp_amount = 0;
                std::vector<uint8_t> inp_spk;
                if (extractWitnessUtxo(psbt.inputs[j], inp_amount, inp_spk)) {
                    all_amounts.push_back(inp_amount);
                    all_scriptpubkeys.push_back(inp_spk);
                } else {
                    all_amounts.push_back(0);
                    all_scriptpubkeys.push_back({});
                }
            }

            // Check for TAP_LEAF_SCRIPT - if present, use script-path signing
            std::vector<TapLeafScriptEntry> leaf_scripts;
            bool has_leaf_scripts = extractTapLeafScripts(input, leaf_scripts);

            if (has_leaf_scripts) {
                // ═══════════════════════════════════════════════════════════════
                // Script-Path Signing (BIP342)
                // ═══════════════════════════════════════════════════════════════

                bool signed_any = false;

                for (const auto& leaf : leaf_scripts) {
                    // Extract pubkeys from the script
                    auto script_pubkeys = extractScriptPubkeys(leaf.script);

                    for (const auto& pubkey : script_pubkeys) {
                        // Check if we can sign for this pubkey
                        // Try to find derivation path for this pubkey
                        std::string key_path = "m";  // Default

                        // Look for matching TAP_BIP32_DERIVATION
                        for (const auto& kv : input.kv) {
                            if (kv.key.size() == 33 && kv.key[0] == 0x16) {
                                std::vector<uint8_t> deriv_pubkey(kv.key.begin() + 1, kv.key.end());
                                if (deriv_pubkey == pubkey) {
                                    // Found matching derivation - extract path
                                    // (simplified - in production parse the full path)
                                    break;
                                }
                            }
                        }

                        if (!keystore_->canSign(key_path)) {
                            continue;  // Can't sign for this key
                        }

                        // Compute script-path sighash
                        std::vector<uint8_t> sighash = computeScriptPathSighashFromPsbt(
                            psbt, i, all_amounts, all_scriptpubkeys, leaf.leaf_hash, 0x00
                        );

                        if (sighash.size() != 32) {
                            result.addInputError(i, "Failed to compute script-path sighash", "warning");
                            continue;
                        }

                        // Get private key and sign with UNTWEAKED key
                        auto signature_opt = keystore_->sign(sighash, key_path);
                        if (!signature_opt.has_value()) {
                            continue;
                        }

                        std::vector<uint8_t> signature = signature_opt.value();

                        // Validate signature length
                        if (signature.size() != 64) {
                            result.addInputError(i, "Invalid Schnorr signature length for script-path", "warning");
                            continue;
                        }

                        // Add TAP_SCRIPT_SIG to PSBT
                        addTaprootScriptSignature(input, pubkey, leaf.leaf_hash, signature);
                        signed_any = true;
                    }
                }

                if (signed_any) {
                    result.signed_count++;
                } else {
                    result.addInputError(i, "Script-path: no signable keys found in leaf scripts", "warning");
                }

            } else {
                // ═══════════════════════════════════════════════════════════════
                // Key-Path Signing (BIP341/BIP86)
                // ═══════════════════════════════════════════════════════════════

                // Extract internal key from PSBT (required for key-path signing)
                std::vector<uint8_t> internal_xonly_pubkey;
                std::string key_path;

                // Try TAP_BIP32_DERIVATION first (contains derivation path)
                if (!extractTapBip32Derivation(input, internal_xonly_pubkey, key_path)) {
                    // Fall back to TAP_INTERNAL_KEY
                    if (!extractTapInternalKey(input, internal_xonly_pubkey)) {
                        result.addInputError(i, "P2TR input missing TAP_INTERNAL_KEY or TAP_BIP32_DERIVATION", "error");
                        continue;
                    }
                    key_path = "m";  // Use root path if no derivation info
                }

                // Extract x-only pubkey from scriptPubKey for verification
                std::vector<uint8_t> output_xonly_pubkey(scriptPubKey.begin() + 2, scriptPubKey.end());

                // Check if keystore can provide the private key
                if (!keystore_->canSign(key_path)) {
                    result.addInputError(i, "No signing capability for P2TR input " + std::to_string(i) + " (path: " + key_path + ")", "error");
                    continue;
                }

                // Compute BIP341 key-path sighash
                std::vector<uint8_t> sighash = computeTaprootSighashFromPsbt(
                    psbt, i, all_amounts, all_scriptpubkeys, 0x00  // SIGHASH_DEFAULT
                );

                if (sighash.size() != 32) {
                    result.addInputError(i, "Failed to compute Taproot sighash", "error");
                    continue;
                }

                // Sign with keystore
                auto signature_opt = keystore_->sign(sighash, key_path);
                if (!signature_opt.has_value()) {
                    result.addInputError(i, "Keystore failed to sign P2TR input " + std::to_string(i) +
                        " (keystore may not support Schnorr/Taproot)", "error");
                    continue;
                }

                std::vector<uint8_t> signature = signature_opt.value();

                // Validate signature length (should be 64 bytes for Schnorr)
                if (signature.size() != 64) {
                    result.addInputError(i, "Invalid Schnorr signature length (expected 64, got " +
                        std::to_string(signature.size()) + ")", "error");
                    continue;
                }

                // Add TAP_KEY_SIG to PSBT
                addTaprootKeySignature(input, signature);

                result.signed_count++;
            }

        } else {
            result.addInputError(i, "Unsupported script type (expected P2WPKH or P2TR, got " + input_type + ")", "error");
            continue;
        }
    }

    // Check if PSBT is incomplete
    if (result.signed_count == 0 && psbt.inputs.size() > 0) {
        result.success = false;
        result.error = "PSBT incomplete: no inputs could be signed (check error details)";
    }

    return result;
}

size_t PsbtSigner::signPsbtLegacy(Psbt& psbt) {
    auto result = signPsbt(psbt);
    return result.signed_count;
}

bool PsbtSigner::hasKey(const std::vector<uint8_t>& pubkey33) const {
    if (!keystore_) return false;
    
    // Check if keystore can sign (simplified implementation)
    return keystore_->canSign("m");
}

} // namespace din
