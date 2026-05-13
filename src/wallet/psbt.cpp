#include "wallet/psbt.h"
#include "wallet/bip143_signer.h"
#include "crypto/hash160.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <secp256k1.h>

// OpenSSL for base64 encoding
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>

namespace dinero {

// PSBT magic bytes
static const uint8_t PSBT_MAGIC[5] = {0x70, 0x73, 0x62, 0x74, 0xff}; // "psbt" + 0xff separator

// ========================================================================
// DEBUG-ONLY PSBT INVARIANT VALIDATION
// ========================================================================
#ifndef NDEBUG

static void ValidatePSBTInvariants(const PSBT& psbt) {
    // 1. Structural consistency: input/output counts must match transaction
    assert(psbt.inputs.size() == psbt.tx.vin.size() &&
           "PSBT inputs count must match transaction vin count");
    assert(psbt.outputs.size() == psbt.tx.vout.size() &&
           "PSBT outputs count must match transaction vout count");

    // 2. Per-input invariants
    for (size_t i = 0; i < psbt.inputs.size(); i++) {
        const PSBTInput& input = psbt.inputs[i];

        // State consistency: can't have partial_sigs AND final witness
        assert(!(input.partial_sigs.size() > 0 && !input.final_script_witness.empty()) &&
               "Input has both partial_sigs and final_script_witness");

        // Validate partial signature pubkey sizes (33 = compressed, 65 = uncompressed)
        for (const auto& [pubkey, sig] : input.partial_sigs) {
            assert((pubkey.size() == 33 || pubkey.size() == 65) &&
                   "Partial sig pubkey must be 33 (compressed) or 65 (uncompressed) bytes");
            // DER signature: min ~8 bytes, max 73 bytes (72 DER + 1 sighash)
            assert(sig.size() >= 8 && sig.size() <= 73 &&
                   "Partial sig must be valid DER+sighash (8-73 bytes)");
        }

        // Validate witness UTXO script format if present
        if (!input.witness_utxo_script.empty()) {
            size_t len = input.witness_utxo_script.size();
            // P2WPKH: OP_0 <20 bytes> = 22 bytes
            // P2WSH:  OP_0 <32 bytes> = 34 bytes
            // P2TR:   OP_1 <32 bytes> = 34 bytes
            assert((len == 22 || len == 34) &&
                   "Witness UTXO script must be P2WPKH (22), P2WSH (34), or P2TR (34)");
        }

        // BIP371 Taproot field validation
        if (!input.tap_key_sig.empty()) {
            // Schnorr sig: 64 bytes, or 65 with non-default sighash
            assert((input.tap_key_sig.size() == 64 || input.tap_key_sig.size() == 65) &&
                   "Taproot key sig must be 64 or 65 bytes");
        }

        if (!input.tap_internal_key.empty()) {
            // x-only pubkey: 32 bytes
            assert(input.tap_internal_key.size() == 32 &&
                   "Taproot internal key must be 32 bytes (x-only)");
        }

        // BIP32 derivation pubkey validation
        for (const auto& [pubkey, path] : input.bip32_derivation) {
            assert((pubkey.size() == 33 || pubkey.size() == 65) &&
                   "BIP32 derivation pubkey must be 33 or 65 bytes");
        }
    }

    // 3. Per-output invariants
    for (size_t i = 0; i < psbt.outputs.size(); i++) {
        const PSBTOutput& output = psbt.outputs[i];

        // BIP32 derivation pubkey validation
        for (const auto& [pubkey, path] : output.bip32_derivation) {
            assert((pubkey.size() == 33 || pubkey.size() == 65) &&
                   "Output BIP32 derivation pubkey must be 33 or 65 bytes");
        }
    }
}

#define VALIDATE_PSBT_INVARIANTS(psbt) ValidatePSBTInvariants(psbt)
#else
#define VALIDATE_PSBT_INVARIANTS(psbt) ((void)0)
#endif

PSBT::PSBT() {
    version = 0;
}

// ========================================================================
// SERIALIZATION HELPERS
// ========================================================================

void PSBT::WriteVarInt(std::vector<uint8_t>& data, uint64_t n) {
    if (n < 0xFD) {
        data.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xFFFF) {
        data.push_back(0xFD);
        data.push_back(n & 0xFF);
        data.push_back((n >> 8) & 0xFF);
    } else if (n <= 0xFFFFFFFF) {
        data.push_back(0xFE);
        for (int i = 0; i < 4; i++) {
            data.push_back((n >> (8 * i)) & 0xFF);
        }
    } else {
        data.push_back(0xFF);
        for (int i = 0; i < 8; i++) {
            data.push_back((n >> (8 * i)) & 0xFF);
        }
    }
}

uint64_t PSBT::ReadVarInt(const uint8_t*& ptr, const uint8_t* end) {
    if (ptr >= end) throw std::runtime_error("ReadVarInt: unexpected end");
    
    uint8_t first = *ptr++;
    if (first < 0xFD) {
        return first;
    } else if (first == 0xFD) {
        if (ptr + 2 > end) throw std::runtime_error("ReadVarInt: unexpected end");
        uint64_t n = ptr[0] | (ptr[1] << 8);
        ptr += 2;
        return n;
    } else if (first == 0xFE) {
        if (ptr + 4 > end) throw std::runtime_error("ReadVarInt: unexpected end");
        uint64_t n = 0;
        for (int i = 0; i < 4; i++) {
            n |= (static_cast<uint64_t>(ptr[i]) << (8 * i));
        }
        ptr += 4;
        return n;
    } else { // 0xFF
        if (ptr + 8 > end) throw std::runtime_error("ReadVarInt: unexpected end");
        uint64_t n = 0;
        for (int i = 0; i < 8; i++) {
            n |= (static_cast<uint64_t>(ptr[i]) << (8 * i));
        }
        ptr += 8;
        return n;
    }
}

void PSBT::WriteCompactSize(std::vector<uint8_t>& data, uint64_t n) {
    WriteVarInt(data, n);
}

uint64_t PSBT::ReadCompactSize(const uint8_t*& ptr, const uint8_t* end) {
    return ReadVarInt(ptr, end);
}

void PSBT::SerializeKV(std::vector<uint8_t>& data,
                       const std::vector<uint8_t>& key,
                       const std::vector<uint8_t>& value) {
    // Write key length and key
    WriteCompactSize(data, key.size());
    data.insert(data.end(), key.begin(), key.end());
    
    // Write value length and value
    WriteCompactSize(data, value.size());
    data.insert(data.end(), value.begin(), value.end());
}

bool PSBT::DeserializeKV(const uint8_t*& ptr, const uint8_t* end,
                         std::vector<uint8_t>& key,
                         std::vector<uint8_t>& value) {
    // Check for separator (0x00)
    if (ptr >= end) return false;
    if (*ptr == 0x00) {
        ptr++;
        return false; // End of map
    }
    
    // Read key
    uint64_t key_len = ReadCompactSize(ptr, end);
    if (ptr + key_len > end) throw std::runtime_error("Key extends past end");
    key.assign(ptr, ptr + key_len);
    ptr += key_len;
    
    // Read value
    uint64_t value_len = ReadCompactSize(ptr, end);
    if (ptr + value_len > end) throw std::runtime_error("Value extends past end");
    value.assign(ptr, ptr + value_len);
    ptr += value_len;
    
    return true;
}

// ========================================================================
// BASE64 ENCODING/DECODING
// ========================================================================

std::string Base64Encode(const std::vector<uint8_t>& data) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data.data(), data.size());
    BIO_flush(b64);
    
    BUF_MEM* bptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string result(bptr->data, bptr->length);
    
    BIO_free_all(b64);
    return result;
}

std::vector<uint8_t> Base64Decode(const std::string& str) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new_mem_buf(str.data(), str.size());
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    
    std::vector<uint8_t> result(str.size());
    int decoded_size = BIO_read(b64, result.data(), str.size());
    if (decoded_size < 0) {
        BIO_free_all(b64);
        throw std::runtime_error("Base64 decode failed");
    }
    
    result.resize(decoded_size);
    BIO_free_all(b64);
    return result;
}

// ========================================================================
// PSBT SERIALIZATION
// ========================================================================

std::vector<uint8_t> PSBT::Serialize() const {
    VALIDATE_PSBT_INVARIANTS(*this);

    std::vector<uint8_t> data;

    // Write magic bytes
    data.insert(data.end(), PSBT_MAGIC, PSBT_MAGIC + 5);
    
    // === GLOBAL MAP ===
    
    // Write unsigned transaction (key type 0x00)
    // BIP174: The unsigned transaction MUST NOT have scriptSig/witness data
    std::vector<uint8_t> tx_key = {static_cast<uint8_t>(PSBTGlobalType::UNSIGNED_TX)};
    std::vector<uint8_t> tx_data = tx.Serialize(TxSerializationMode::WithoutWitness);
    SerializeKV(data, tx_key, tx_data);
    
    // Write global unknown fields
    for (const auto& [key, value] : unknown) {
        SerializeKV(data, key, value);
    }
    
    // End of global map
    data.push_back(0x00);
    
    // === INPUT MAPS ===
    for (const auto& input : inputs) {
        // Witness UTXO
        if (!input.witness_utxo_script.empty()) {
            std::vector<uint8_t> key = {static_cast<uint8_t>(PSBTInputType::WITNESS_UTXO)};
            std::vector<uint8_t> value;
            
            // Amount (8 bytes, little-endian)
            for (int i = 0; i < 8; i++) {
                value.push_back((input.witness_utxo_amount >> (8 * i)) & 0xFF);
            }
            
            // Script
            value.insert(value.end(), input.witness_utxo_script.begin(), input.witness_utxo_script.end());
            
            SerializeKV(data, key, value);
        }
        
        // Partial signatures
        for (const auto& [pubkey, sig] : input.partial_sigs) {
            std::vector<uint8_t> key = {static_cast<uint8_t>(PSBTInputType::PARTIAL_SIG)};
            key.insert(key.end(), pubkey.begin(), pubkey.end());
            SerializeKV(data, key, sig);
        }
        
        // Sighash type
        if (input.sighash_type != 1) {
            std::vector<uint8_t> key = {static_cast<uint8_t>(PSBTInputType::SIGHASH_TYPE)};
            std::vector<uint8_t> value(4);
            for (int i = 0; i < 4; i++) {
                value[i] = (input.sighash_type >> (8 * i)) & 0xFF;
            }
            SerializeKV(data, key, value);
        }
        
        // Final scriptSig
        if (!input.final_script_sig.empty()) {
            std::vector<uint8_t> key = {static_cast<uint8_t>(PSBTInputType::FINAL_SCRIPTSIG)};
            SerializeKV(data, key, input.final_script_sig);
        }
        
        // Final scriptWitness
        if (!input.final_script_witness.empty()) {
            std::vector<uint8_t> key = {static_cast<uint8_t>(PSBTInputType::FINAL_SCRIPTWITNESS)};
            std::vector<uint8_t> value;
            WriteCompactSize(value, input.final_script_witness.size());
            for (const auto& item : input.final_script_witness) {
                WriteCompactSize(value, item.size());
                value.insert(value.end(), item.begin(), item.end());
            }
            SerializeKV(data, key, value);
        }
        
        // Unknown fields
        for (const auto& [key, value] : input.unknown) {
            SerializeKV(data, key, value);
        }
        
        // End of input map
        data.push_back(0x00);
    }
    
    // === OUTPUT MAPS ===
    for (const auto& output : outputs) {
        // Unknown fields
        for (const auto& [key, value] : output.unknown) {
            SerializeKV(data, key, value);
        }
        
        // End of output map
        data.push_back(0x00);
    }
    
    return data;
}

bool PSBT::Deserialize(const std::vector<uint8_t>& data) {
    const uint8_t* ptr = data.data();
    const uint8_t* end = data.data() + data.size();
    
    try {
        // Check magic bytes
        if (ptr + 5 > end || std::memcmp(ptr, PSBT_MAGIC, 5) != 0) {
            error_ = "Invalid PSBT magic bytes";
  return false;
}
        ptr += 5;
        
        // Parse global map
        std::vector<uint8_t> key, value;
        while (DeserializeKV(ptr, end, key, value)) {
            if (key.empty()) continue;
            
            PSBTGlobalType type = static_cast<PSBTGlobalType>(key[0]);
            if (type == PSBTGlobalType::UNSIGNED_TX) {
                // BIP174: Deserialize the unsigned transaction
                if (!TransactionSerializer::Deserialize(tx, value)) {
                    error_ = "Failed to deserialize PSBT transaction";
                    return false;
                }
            } else {
                unknown[key] = value;
            }
        }
        
        // Parse input maps
        for (size_t i = 0; i < tx.vin.size(); i++) {
            PSBTInput input;
            
            while (DeserializeKV(ptr, end, key, value)) {
                if (key.empty()) continue;
                
                PSBTInputType type = static_cast<PSBTInputType>(key[0]);
                
                switch (type) {
                    case PSBTInputType::WITNESS_UTXO:
                        if (value.size() >= 8) {
                            input.witness_utxo_amount = 0;
                            for (int j = 0; j < 8; j++) {
                                input.witness_utxo_amount |= (static_cast<uint64_t>(value[j]) << (8 * j));
                            }
                            input.witness_utxo_script.assign(value.begin() + 8, value.end());
                        }
                        break;
                        
                    case PSBTInputType::PARTIAL_SIG:
                        if (key.size() > 1) {
                            std::vector<uint8_t> pubkey(key.begin() + 1, key.end());
                            input.partial_sigs[pubkey] = value;
                        }
                        break;
                        
                    case PSBTInputType::FINAL_SCRIPTSIG:
                        input.final_script_sig = value;
                        break;
                        
                    default:
                        input.unknown[key] = value;
        break;
      }
    }
            
            inputs.push_back(input);
        }
        
        // Parse output maps
        for (size_t i = 0; i < tx.vout.size(); i++) {
            PSBTOutput output;
            
            while (DeserializeKV(ptr, end, key, value)) {
                if (key.empty()) continue;
                output.unknown[key] = value;
            }
            
            outputs.push_back(output);
        }

        VALIDATE_PSBT_INVARIANTS(*this);
        return true;

    } catch (const std::exception& e) {
        error_ = std::string("Deserialization error: ") + e.what();
        return false;
    }
}
  
std::string PSBT::ToBase64() const {
    return Base64Encode(Serialize());
}

PSBT PSBT::FromBase64(const std::string& base64_str) {
    PSBT psbt;
    std::vector<uint8_t> data = Base64Decode(base64_str);
    if (!psbt.Deserialize(data)) {
        throw std::runtime_error("Failed to deserialize PSBT: " + psbt.GetError());
    }
    return psbt;
}

// ========================================================================
// PSBT OPERATIONS
// ========================================================================

bool PSBT::Sign(const std::vector<uint8_t>& privkey, size_t input_index) {
    if (input_index >= inputs.size()) {
        error_ = "Input index out of range";
        return false;
    }

    if (privkey.size() != 32) {
        error_ = "Private key must be 32 bytes";
        return false;
    }

    PSBTInput& input = inputs[input_index];

    // Check if we have witness UTXO (P2WPKH)
    if (input.witness_utxo_script.empty()) {
        error_ = "Only P2WPKH signing is currently supported (need witness_utxo)";
        return false;
    }

    // Verify this is P2WPKH: OP_0 <20-byte-hash>
    if (input.witness_utxo_script.size() != 22 ||
        input.witness_utxo_script[0] != 0x00 ||
        input.witness_utxo_script[1] != 0x14) {
        error_ = "Only P2WPKH scripts are currently supported";
        return false;
    }

    // Get public key from private key
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) {
        error_ = "Failed to create secp256k1 context";
        return false;
    }

    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_create(ctx, &pubkey, privkey.data())) {
        secp256k1_context_destroy(ctx);
        error_ = "Invalid private key";
        return false;
    }

    // Serialize public key (compressed)
    std::vector<uint8_t> pubkey_bytes(33);
    size_t pubkey_len = 33;
    secp256k1_ec_pubkey_serialize(ctx, pubkey_bytes.data(), &pubkey_len,
                                   &pubkey, SECP256K1_EC_COMPRESSED);

    // Verify pubkey hash matches witness UTXO
    // HASH160 = RIPEMD160(SHA256(pubkey))
    auto pubkey_hash = Hash160(pubkey_bytes.data(), pubkey_bytes.size());

    // Compare with hash in witness_utxo_script (bytes 2-22)
    if (std::memcmp(pubkey_hash.data(), input.witness_utxo_script.data() + 2, 20) != 0) {
        secp256k1_context_destroy(ctx);
        error_ = "Public key does not match witness UTXO";
        return false;
    }

    // Build scriptCode for BIP143: OP_DUP OP_HASH160 <pubkey_hash> OP_EQUALVERIFY OP_CHECKSIG
    std::vector<uint8_t> script_code = {0x76, 0xa9, 0x14};  // OP_DUP OP_HASH160 PUSH20
    script_code.insert(script_code.end(), pubkey_hash.begin(), pubkey_hash.end());
    script_code.push_back(0x88);  // OP_EQUALVERIFY
    script_code.push_back(0xac);  // OP_CHECKSIG

    // Compute BIP143 sighash
    std::vector<uint8_t> sighash = BIP143Signer::ComputeSighash(
        tx, input_index, script_code, input.witness_utxo_amount, input.sighash_type);

    if (sighash.size() != 32) {
        secp256k1_context_destroy(ctx);
        error_ = "Failed to compute sighash";
        return false;
    }

    // Sign with ECDSA
    secp256k1_ecdsa_signature sig;
    if (!secp256k1_ecdsa_sign(ctx, &sig, sighash.data(), privkey.data(), nullptr, nullptr)) {
        secp256k1_context_destroy(ctx);
        error_ = "Failed to sign";
        return false;
    }

    // Normalize signature (low S)
    secp256k1_ecdsa_signature_normalize(ctx, &sig, &sig);

    // Serialize to DER
    uint8_t der_sig[72];
    size_t der_len = 72;
    if (!secp256k1_ecdsa_signature_serialize_der(ctx, der_sig, &der_len, &sig)) {
        secp256k1_context_destroy(ctx);
        error_ = "Failed to serialize signature";
        return false;
    }

    secp256k1_context_destroy(ctx);

    // Add sighash type byte
    std::vector<uint8_t> signature(der_sig, der_sig + der_len);
    signature.push_back(static_cast<uint8_t>(input.sighash_type));

    // Store in partial_sigs
    input.partial_sigs[pubkey_bytes] = signature;

    VALIDATE_PSBT_INVARIANTS(*this);
    return true;
}

bool PSBT::Finalize() {
    for (size_t i = 0; i < inputs.size(); i++) {
        PSBTInput& input = inputs[i];

        // Skip already finalized inputs
        if (!input.final_script_sig.empty() || !input.final_script_witness.empty()) {
            continue;
        }

        // Check for P2WPKH (witness UTXO with OP_0 <20-byte-hash>)
        if (!input.witness_utxo_script.empty() &&
            input.witness_utxo_script.size() == 22 &&
            input.witness_utxo_script[0] == 0x00 &&
            input.witness_utxo_script[1] == 0x14) {

            // Need exactly one partial signature for P2WPKH
            if (input.partial_sigs.size() != 1) {
                error_ = "P2WPKH requires exactly one signature, input " + std::to_string(i);
                return false;
            }

            // Get the signature and pubkey
            auto it = input.partial_sigs.begin();
            const std::vector<uint8_t>& pubkey = it->first;
            const std::vector<uint8_t>& signature = it->second;

            // Verify pubkey is compressed (33 bytes)
            if (pubkey.size() != 33) {
                error_ = "Invalid pubkey size in partial_sigs";
                return false;
            }

            // Build witness: [signature, pubkey]
            input.final_script_witness.clear();
            input.final_script_witness.push_back(signature);
            input.final_script_witness.push_back(pubkey);

            // P2WPKH has empty scriptSig
            input.final_script_sig.clear();

            // Clear partial sigs (no longer needed)
            input.partial_sigs.clear();
        }
        // Check for Taproot key-path (P2TR with tap_key_sig)
        else if (!input.tap_key_sig.empty()) {
            // Taproot key-path: witness is just the signature
            if (input.tap_key_sig.size() != 64 && input.tap_key_sig.size() != 65) {
                error_ = "Invalid Taproot key signature size";
                return false;
            }

            input.final_script_witness.clear();
            input.final_script_witness.push_back(input.tap_key_sig);
            input.final_script_sig.clear();

            // Clear tap fields
            input.tap_key_sig.clear();
        }
        else {
            error_ = "Cannot finalize input " + std::to_string(i) + ": unsupported script type";
            return false;
        }
    }

    VALIDATE_PSBT_INVARIANTS(*this);
    return true;
}

bool PSBT::IsComplete() const {
    for (const auto& input : inputs) {
        if (input.final_script_sig.empty() && input.final_script_witness.empty()) {
            return false;
        }
    }
    return true;
}

Transaction PSBT::ExtractTransaction() const {
    if (!IsComplete()) {
        throw std::runtime_error("Cannot extract transaction from incomplete PSBT");
    }
    
    Transaction result = tx;
    
    // Add final scripts to transaction
    for (size_t i = 0; i < inputs.size(); i++) {
        if (i < result.vin.size()) {
            result.vin[i].scriptSig = inputs[i].final_script_sig;
            result.vin[i].witness = inputs[i].final_script_witness;
        }
    }
    
    return result;
}

bool PSBT::Combine(const PSBT& other) {
    // Verify transaction matches
    if (tx.vin.size() != other.tx.vin.size() ||
        tx.vout.size() != other.tx.vout.size()) {
        error_ = "Cannot combine PSBTs with different transactions";
        return false;
    }

    // Combine inputs
    for (size_t i = 0; i < inputs.size() && i < other.inputs.size(); i++) {
        PSBTInput& input = inputs[i];
        const PSBTInput& other_input = other.inputs[i];

        // Merge partial signatures
        for (const auto& [pubkey, sig] : other_input.partial_sigs) {
            if (input.partial_sigs.find(pubkey) == input.partial_sigs.end()) {
                input.partial_sigs[pubkey] = sig;
            }
        }

        // Copy witness UTXO if we don't have it
        if (input.witness_utxo_script.empty() && !other_input.witness_utxo_script.empty()) {
            input.witness_utxo_amount = other_input.witness_utxo_amount;
            input.witness_utxo_script = other_input.witness_utxo_script;
        }

        // Copy non-witness UTXO if we don't have it
        if (input.non_witness_utxo.empty() && !other_input.non_witness_utxo.empty()) {
            input.non_witness_utxo = other_input.non_witness_utxo;
        }

        // Merge BIP32 derivation
        for (const auto& [pubkey, deriv] : other_input.bip32_derivation) {
            if (input.bip32_derivation.find(pubkey) == input.bip32_derivation.end()) {
                input.bip32_derivation[pubkey] = deriv;
            }
        }

        // Copy Taproot fields if missing
        if (input.tap_key_sig.empty() && !other_input.tap_key_sig.empty()) {
            input.tap_key_sig = other_input.tap_key_sig;
        }
        if (input.tap_internal_key.empty() && !other_input.tap_internal_key.empty()) {
            input.tap_internal_key = other_input.tap_internal_key;
        }

        // Merge unknown fields
        for (const auto& [key, val] : other_input.unknown) {
            if (input.unknown.find(key) == input.unknown.end()) {
                input.unknown[key] = val;
            }
        }
    }

    // Combine outputs
    for (size_t i = 0; i < outputs.size() && i < other.outputs.size(); i++) {
        PSBTOutput& output = outputs[i];
        const PSBTOutput& other_output = other.outputs[i];

        // Merge BIP32 derivation
        for (const auto& [pubkey, deriv] : other_output.bip32_derivation) {
            if (output.bip32_derivation.find(pubkey) == output.bip32_derivation.end()) {
                output.bip32_derivation[pubkey] = deriv;
            }
        }

        // Merge unknown fields
        for (const auto& [key, val] : other_output.unknown) {
            if (output.unknown.find(key) == output.unknown.end()) {
                output.unknown[key] = val;
            }
        }
    }

    VALIDATE_PSBT_INVARIANTS(*this);
    return true;
}

bool PSBT::IsValid() const {
    if (tx.vin.size() != inputs.size()) {
        return false;
    }
    if (tx.vout.size() != outputs.size()) {
        return false;
    }
    return true;
}

PSBT CreatePSBT(const std::vector<std::pair<std::string, uint32_t>>& inputs,
                const std::map<std::string, uint64_t>& outputs) {
    PSBT psbt;

    // Build transaction from inputs/outputs
    psbt.tx.version = 2;
    psbt.tx.lockTime = 0;
    psbt.tx.witness_version = 0;  // Default to SegWit v0

    // Add inputs
    for (const auto& [txid_hex, vout] : inputs) {
        TxInput input;
        uint256 txid;
        if (uint256::FromHex(txid_hex, txid)) {
            input.prevout.txid = TxId(txid);
        }
        input.prevout.vout = vout;
        input.sequence = 0xfffffffe;  // RBF-enabled
        psbt.tx.vin.push_back(input);

        // Add empty PSBT input
        psbt.inputs.emplace_back();
    }

    // Add outputs
    for (const auto& [address, value] : outputs) {
        TxOutput output;
        output.value = AmountUna::Una(value);
        // Note: scriptPubKey should be derived from address
        // For now, leave empty - caller should set it
        psbt.tx.vout.push_back(output);

        // Add empty PSBT output
        psbt.outputs.emplace_back();
    }

    return psbt;
}

} // namespace dinero
