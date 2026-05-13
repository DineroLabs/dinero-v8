#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include "wallet/transaction.h"

namespace dinero {

// PSBT key types (BIP 174)
enum class PSBTInputType : uint8_t {
    NON_WITNESS_UTXO = 0x00,
    WITNESS_UTXO = 0x01,
    PARTIAL_SIG = 0x02,
    SIGHASH_TYPE = 0x03,
    REDEEM_SCRIPT = 0x04,
    WITNESS_SCRIPT = 0x05,
    BIP32_DERIVATION = 0x06,
    FINAL_SCRIPTSIG = 0x07,
    FINAL_SCRIPTWITNESS = 0x08
};

enum class PSBTOutputType : uint8_t {
    REDEEM_SCRIPT = 0x00,
    WITNESS_SCRIPT = 0x01,
    BIP32_DERIVATION = 0x02
};

enum class PSBTGlobalType : uint8_t {
    UNSIGNED_TX = 0x00,
    XPUB = 0x01,
    VERSION = 0xFB
};

// PSBT Input data
struct PSBTInput {
    // Non-witness UTXO (full previous transaction)
    std::vector<uint8_t> non_witness_utxo;

    // Witness UTXO (just the output being spent)
    uint64_t witness_utxo_amount = 0;
    std::vector<uint8_t> witness_utxo_script;

    // Partial signatures: pubkey -> signature
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> partial_sigs;

    // Sighash type
    uint32_t sighash_type = 1; // SIGHASH_ALL

    // Redeem script (for P2SH)
    std::vector<uint8_t> redeem_script;

    // Witness script (for P2WSH)
    std::vector<uint8_t> witness_script;

    // BIP32 derivation paths: pubkey -> (fingerprint, path)
    std::map<std::vector<uint8_t>, std::pair<uint32_t, std::vector<uint32_t>>> bip32_derivation;

    // Final scriptSig (when finalized)
    std::vector<uint8_t> final_script_sig;

    // Final scriptWitness (when finalized)
    std::vector<std::vector<uint8_t>> final_script_witness;

    // BIP371 Taproot fields (BIP86 key-path only)
    std::vector<uint8_t> tap_key_sig;           // 0x13 - Taproot key path signature (64 bytes Schnorr)
    std::vector<uint8_t> tap_internal_key;      // 0x17 - Taproot internal key (32 bytes)
    std::map<std::vector<uint8_t>, std::pair<std::vector<uint32_t>, uint32_t>> tap_bip32_derivation;  // 0x16 - xonly pubkey -> (path, leaf_hashes)

    // B3: Dinero proprietary fields for SigHash v1
    // Proprietary key prefix: 0xFC "dinero"
    std::optional<std::array<uint8_t, 32>> ext_commitment;     // V1 sighash extension commitment
    std::optional<std::vector<uint8_t>> intent_descriptor;      // Serialized IntentDescriptor

    // Unknown key-value pairs
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> unknown;
};

// PSBT Output data
struct PSBTOutput {
    // Redeem script
    std::vector<uint8_t> redeem_script;
    
    // Witness script
    std::vector<uint8_t> witness_script;
    
    // BIP32 derivation paths
    std::map<std::vector<uint8_t>, std::pair<uint32_t, std::vector<uint32_t>>> bip32_derivation;
    
    // Unknown key-value pairs
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> unknown;
};

// Main PSBT structure
class PSBT {
public:
    PSBT();
    ~PSBT() = default;
    
    // Unsigned transaction
    Transaction tx;
    
    // Per-input data
    std::vector<PSBTInput> inputs;
    
    // Per-output data
    std::vector<PSBTOutput> outputs;
    
    // Global unknown key-value pairs
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> unknown;
    
    // Version (optional, defaults to 0)
    uint32_t version = 0;
    
    // Serialization
    std::vector<uint8_t> Serialize() const;
    bool Deserialize(const std::vector<uint8_t>& data);
    
    // Base64 encoding/decoding
    std::string ToBase64() const;
    static PSBT FromBase64(const std::string& base64_str);
    
    // Signing
    bool Sign(const std::vector<uint8_t>& privkey, size_t input_index);
    
    // Finalization
    bool Finalize();
    bool IsComplete() const;
    
    // Extract final transaction
    Transaction ExtractTransaction() const;
    
    // Combine with another PSBT
    bool Combine(const PSBT& other);
    
    // Validation
    bool IsValid() const;
    std::string GetError() const { return error_; }
    
private:
    std::string error_;
    
    // Serialization helpers
    static void WriteVarInt(std::vector<uint8_t>& data, uint64_t n);
    static uint64_t ReadVarInt(const uint8_t*& ptr, const uint8_t* end);
    static void WriteCompactSize(std::vector<uint8_t>& data, uint64_t n);
    static uint64_t ReadCompactSize(const uint8_t*& ptr, const uint8_t* end);
    
    // Key-value pair serialization
    static void SerializeKV(std::vector<uint8_t>& data, 
                           const std::vector<uint8_t>& key, 
                           const std::vector<uint8_t>& value);
    static bool DeserializeKV(const uint8_t*& ptr, const uint8_t* end,
                             std::vector<uint8_t>& key,
                             std::vector<uint8_t>& value);
};

// Helper functions for PSBT creation
PSBT CreatePSBT(const std::vector<std::pair<std::string, uint32_t>>& inputs,
                const std::map<std::string, uint64_t>& outputs);

// Base64 encoding/decoding
std::string Base64Encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> Base64Decode(const std::string& str);

} // namespace dinero
