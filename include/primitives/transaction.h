#pragma once

#include <array>
#include <vector>
#include <string>
#include <cstdint>
#include "primitives/uint256.h"
#include "primitives/hash_domains.h"  // Phase M.4: Semantic hash domain types
#include "primitives/amount.h"        // Phase M.6: Monetary type safety

namespace dinero {

// ═══════════════════════════════════════════════════════════════════════════
// Transaction Serialization Policy (Phase 11d: Explicit Mode Selection)
// ═══════════════════════════════════════════════════════════════════════════
// Forbids implicit boolean parameters in consensus-critical serialization.
// Each mode has a specific, documented purpose per BIP141.
//
// Usage:
//   tx.Serialize(TxSerializationMode::WithWitness)    // Blocks, P2P, storage
//   tx.Serialize(TxSerializationMode::WithoutWitness) // Txid, base size, weight
// ═══════════════════════════════════════════════════════════════════════════
enum class TxSerializationMode {
    WithWitness,      // Full serialization including witness data (SegWit format)
    WithoutWitness    // Legacy serialization (txid computation, base size, weight)
};

// Bitcoin-compatible transaction structures

struct TxOutPoint {
    TxId txid;         // Transaction hash (Phase M.4.3-B: TxId semantic type)
    uint32_t vout;     // Output index

    TxOutPoint() : vout(0) {}
    TxOutPoint(const TxId& txid_, uint32_t vout_) : txid(txid_), vout(vout_) {}

    // Phase M.0: Equality for duplicate detection (consensus-critical)
    bool operator==(const TxOutPoint& other) const {
        return txid == other.txid && vout == other.vout;
    }

    bool operator!=(const TxOutPoint& other) const {
        return !(*this == other);
    }

    // Phase M.4: Ordering for std::set usage (validation duplicate detection)
    bool operator<(const TxOutPoint& other) const {
        if (txid.AsUint256() != other.txid.AsUint256()) {
            return txid.AsUint256() < other.txid.AsUint256();
        }
        return vout < other.vout;
    }
};

// Minimal UTXO structure for transaction signing (stateless primitive)
// Contains only cryptographic data needed for signature generation
// NO wallet state, NO derivation paths, NO labels
// Phase M.0-pre: Renamed from UTXO to SigningUTXO to avoid namespace pollution
// Phase M.6.1: value converted to AmountUna for type safety
struct SigningUTXO {
    AmountUna value;                      // Output value in una
    std::vector<uint8_t> scriptPubKey;     // Locking script

    SigningUTXO() : value(AmountUna::Zero()) {}
    SigningUTXO(AmountUna val, const std::vector<uint8_t>& spk)
        : value(val), scriptPubKey(spk) {}
};

struct TxInput {
    TxOutPoint prevout;
    std::vector<uint8_t> scriptSig;  // Empty for SegWit
    uint32_t sequence;

    // SegWit witness data
    std::vector<std::vector<uint8_t>> witness;

    // ═══════════════════════════════════════════════════════════════
    // Ring signature data (Phase R.4: Private pool spends)
    // ═══════════════════════════════════════════════════════════════
    // Present only for private-pool inputs (tx version 3).
    // Transparent inputs leave these empty.

    // Mandatory ring size for all private pool spends (CLSAG_RING_SIZE).
    static constexpr size_t MANDATORY_RING_SIZE = 16;

    // 16 global CT output indices forming the anonymity set.
    // Serialized with delta encoding (first absolute, rest as deltas).
    std::vector<uint64_t> ring_members;

    // 33-byte compressed secp256k1 key image (prevents double-spend).
    std::vector<uint8_t> key_image;

    // Serialized CLSAG signature (see zk/clsag.h CLSAGSignature::Serialize).
    std::vector<uint8_t> clsag_signature;

    // 33-byte pseudo-output commitment C' (proves amount balance without revealing it).
    // The CLSAG signature proves that C_real - C' commits to zero.
    std::vector<uint8_t> pseudo_commitment;

    // ═══════════════════════════════════════════════════════════════
    // Ring-covenant data (ZKVM: v4 transactions)
    // ═══════════════════════════════════════════════════════════════
    // Present only for ring-covenant inputs (tx version 4).
    // Current verifier plumbing: CLSAG key ownership + ZK Tapscript
    // satisfaction + compact hidden-member proof + transitional public
    // TapLeaf/Taproot-key binding.

    // Serialized ZK Tapscript proof (Nova IVC + IPA)
    std::vector<uint8_t> tapscript_zk_proof;

    // 32-byte BIP341 TapLeaf hash binding CLSAG to ZK proof
    std::vector<uint8_t> tapleaf_hash;

    // Legacy public BIP341 control block slot for the hidden script path.
    // Secure anonymous/hybrid ring-covenant proofs leave this empty; the field
    // remains for decoding legacy envelopes and test vectors.
    std::vector<uint8_t> taproot_control_block;

    // Serialized hidden-member binding proof wrapper.
    // Accepted consensus shapes are the anonymous/hybrid proof versions that
    // bind the CLSAG signer and the hidden Taproot path to the same member.
    // Legacy compact-ring envelopes remain parseable for compatibility but are
    // rejected by VerifyRingCovenant.
    std::vector<uint8_t> hidden_member_binding_proof;

    // Poseidon-2 privkey commitment: Poseidon2(privkey, nonce) over secp256k1 scalar field.
    // Included in the CLSAG signing message for cryptographic binding soundness.
    // Fixed 32 bytes; all-zero = legacy (no commitment binding).
    std::array<uint8_t, 32> privkey_commitment{};

    // Helper: true when this input spends from the private pool.
    bool IsPrivateInput() const { return !ring_members.empty(); }

    // Helper: true when this input has a covenant ZK proof.
    bool IsCovenantInput() const { return !tapscript_zk_proof.empty(); }

    TxInput() : sequence(0xfffffffe) {}  // Default: RBF-enabled
};

// Phase M.6.1: TxOutput value converted to AmountUna for type safety
struct TxOutput {
    AmountUna value;  // Amount in una (una) - transparent only
    std::vector<uint8_t> scriptPubKey;

    // Zero-Knowledge privacy (confidential transactions)
    bool is_confidential = false;  // Is this a confidential output?
    std::vector<uint8_t> commitment;      // 33-byte Pedersen commitment (if confidential)
    std::vector<uint8_t> range_proof;     // Bulletproof range proof (~5KB)
    std::vector<uint8_t> nonce;           // 32-byte nonce for receiver

    TxOutput() : value(AmountUna::Zero()), is_confidential(false) {}
    TxOutput(AmountUna val, const std::vector<uint8_t>& script)
        : value(val), scriptPubKey(script), is_confidential(false) {}

    // Parse witness version from scriptPubKey
    // Returns 0xFF for non-witness scripts, 0-16 for witness versions
    uint8_t GetWitnessVersion() const {
        // Witness scripts: OP_0 to OP_16 followed by push data
        // OP_0 = 0x00, OP_1 = 0x51, OP_2 = 0x52, ..., OP_16 = 0x60
        if (scriptPubKey.empty()) return 0xFF;

        uint8_t first_byte = scriptPubKey[0];

        // OP_0 (SegWit v0)
        if (first_byte == 0x00) {
            // P2WPKH: OP_0 <20 bytes>
            // P2WSH:  OP_0 <32 bytes>
            if (scriptPubKey.size() == 22 || scriptPubKey.size() == 34) {
                return 0;
            }
        }

        // OP_1 to OP_16 (Taproot = v1, future = v2-16)
        if (first_byte >= 0x51 && first_byte <= 0x60) {
            // Taproot: OP_1 <32 bytes>
            if (first_byte == 0x51 && scriptPubKey.size() == 34) {
                return 1;  // Taproot
            }
            // Future witness versions
            return first_byte - 0x50;
        }

        return 0xFF;  // Not a witness script
    }

    // Helper: Check if this is a witness output
    bool IsWitness() const { return GetWitnessVersion() != 0xFF; }
    bool IsSegWitV0() const { return GetWitnessVersion() == 0; }
    bool IsTaproot() const { return GetWitnessVersion() == 1; }

    // Helper: Check if this is a confidential output
    bool IsConfidential() const { return is_confidential; }

    // Helper: Get actual value (0 for confidential outputs)
    // Phase M.6.1: Returns uint64_t for serialization compatibility
    uint64_t GetValue() const { return is_confidential ? 0 : value.GetUna(); }

    // Helper: Get commitment size for serialization
    size_t GetConfidentialDataSize() const {
        if (!is_confidential) return 0;
        return 1 + commitment.size() + range_proof.size() + nonce.size();
    }
};

struct Transaction {
    // Transaction version constants
    static constexpr int32_t TX_VERSION_LEGACY  = 1;
    static constexpr int32_t TX_VERSION_SEGWIT  = 2;
    // Versions 3 (ring) and 4 (ring-covenant) were excised on Apr 17 2026.
    // The constants are kept out of the header so any stale reference fails to
    // compile; do NOT add them back.
    // v5 is the legacy shielded wire format already present on mainnet: the
    // shielded bundle is witness-only and therefore excluded from txid.
    static constexpr int32_t TX_VERSION_SHIELDED     = 5;
    // v6 is the forward format for newly-created shielded txs: the shielded
    // bundle commits into txid so shielded-only spends cannot collide.
    static constexpr int32_t TX_VERSION_SHIELDED_V2  = 6;

    static constexpr bool IsShieldedVersion(int32_t tx_version) {
        return tx_version == TX_VERSION_SHIELDED ||
               tx_version == TX_VERSION_SHIELDED_V2;
    }

    int32_t version;
    std::vector<TxInput> vin;
    std::vector<TxOutput> vout;
    uint32_t lockTime;

    // Witness version: 0xFF = legacy (no witness), 0 = v0 (SegWit), 1 = v1 (Taproot), etc.
    // This replaces the binary is_segwit flag for forward compatibility
    uint8_t witness_version;

    // Confidential transaction explicit fee (Phase G.1)
    // For confidential transactions, fee must be explicit since amounts are hidden
    // Set to 0 for transparent transactions (fee calculated from inputs/outputs)
    // Phase M.6.1: explicit_fee converted to AmountUna for type safety
    AmountUna explicit_fee;
    bool has_explicit_fee;

    // Shielded bundle — canonical bytes (SerializeShieldedBundle format).
    // v5: excluded from txid, included in wtxid for legacy chain compatibility.
    // v6: included in txid and wtxid so shielded-only txs have unique txids.
    // Parsed on demand via DeserializeShieldedBundle when needed.
    std::vector<uint8_t> shielded_bundle_bytes;

    bool IsShielded() const {
        return IsShieldedVersion(version) && !shielded_bundle_bytes.empty();
    }
    bool ShieldedBundleCommitsToTxid() const {
        return version == TX_VERSION_SHIELDED_V2 && !shielded_bundle_bytes.empty();
    }

    Transaction() : version(2), lockTime(0), witness_version(0), explicit_fee(AmountUna::Zero()), has_explicit_fee(false) {}  // Default to SegWit v0
    
    // Check if transaction is coinbase
    bool IsCoinbase() const {
        return vin.size() == 1 &&
               vin[0].prevout.txid.IsNull() &&
               vin[0].prevout.vout == 0xffffffff;
    }

    // Witness version helpers (Bitcoin Core v0.27 compatibility)
    bool IsLegacy() const { return witness_version == 0xFF; }
    bool HasWitness() const { return witness_version != 0xFF; }
    bool IsSegWitV0() const { return witness_version == 0; }
    bool IsTaproot() const { return witness_version == 1; }
    bool IsUnknownWitnessVersion() const { return witness_version >= 2 && witness_version <= 16; }

    // Confidential transaction helpers (Zero-Knowledge privacy)
    bool HasConfidentialOutputs() const {
        for (const auto& output : vout) {
            if (output.is_confidential) return true;
        }
        return false;
    }

    size_t CountConfidentialOutputs() const {
        size_t count = 0;
        for (const auto& output : vout) {
            if (output.is_confidential) ++count;
        }
        return count;
    }

    // Explicit fee support (Phase G.1)
    // Required for confidential transactions where fee can't be calculated
    // Phase M.6.1: Returns uint64_t for RPC/serialization compatibility
    bool HasExplicitFee() const {
        return has_explicit_fee;
    }

    uint64_t GetExplicitFee() const {
        return explicit_fee.GetUna();
    }

    void SetExplicitFee(uint64_t fee) {
        explicit_fee = AmountUna::Una(fee);
        has_explicit_fee = true;
    }

    // Detect witness version from transaction inputs
    // Scans first input's witness data to determine version
    void DetectWitnessVersion() {
        // Default to legacy if no witness data
        witness_version = 0xFF;

        // Check if any input has witness data
        bool has_witness = false;
        for (const auto& input : vin) {
            if (!input.witness.empty()) {
                has_witness = true;
                // Detect Taproot (v1) from witness structure:
                // Key-path: exactly 1 item, 64 or 65 bytes (Schnorr sig)
                // Script-path: 2+ items, last item starts with 0xc0/0xc1 (control block)
                if (input.witness.size() == 1 &&
                    (input.witness[0].size() == 64 || input.witness[0].size() == 65)) {
                    witness_version = 1; // Taproot key-path
                    return;
                }
                if (input.witness.size() >= 2) {
                    const auto& last = input.witness.back();
                    if (!last.empty() && (last[0] == 0xc0 || last[0] == 0xc1)) {
                        witness_version = 1; // Taproot script-path
                        return;
                    }
                }
                break;
            }
        }

        if (has_witness) {
            witness_version = 0; // SegWit v0 (P2WPKH/P2WSH)
        }
    }

    /// Detect witness version from the scriptPubKey being spent (most reliable)
    static uint8_t WitnessVersionFromScriptPubKey(const std::vector<uint8_t>& scriptPubKey) {
        if (scriptPubKey.size() == 34 && scriptPubKey[0] == 0x51 && scriptPubKey[1] == 0x20) {
            return 1; // OP_1 + PUSH32 = P2TR (Taproot v1)
        }
        if (scriptPubKey.size() == 22 && scriptPubKey[0] == 0x00 && scriptPubKey[1] == 0x14) {
            return 0; // OP_0 + PUSH20 = P2WPKH (SegWit v0)
        }
        if (scriptPubKey.size() == 34 && scriptPubKey[0] == 0x00 && scriptPubKey[1] == 0x20) {
            return 0; // OP_0 + PUSH32 = P2WSH (SegWit v0)
        }
        return 0xFF; // Legacy
    }

    // ═══════════════════════════════════════════════════════════════
    // Serialization (Phase 11a.3: Merkle Safety Guard)
    // ═══════════════════════════════════════════════════════════════
    // ⚠️  CRITICAL: DO NOT use Serialize() for merkle tree computation!
    //
    // Merkle roots MUST be computed from txids using:
    //   consensus::ComputeMerkleRoot(vtx)  // Canonical API
    //
    // This function is for:
    //   - Network transmission (P2P messages)
    //   - Storage/persistence (database, files)
    //   - RPC responses (hex-encoded transactions)
    //
    // Why this matters:
    //   - Merkle trees use txid (hash of non-witness data)
    //   - include_witness parameter creates ambiguity
    //   - Byte-order bugs from string conversion
    //   - Phase 11a.1 bug was caused by using Serialize() for merkle
    //
    // Locked by: tests/consensus/test_merkle_invariants.cpp
    // ═══════════════════════════════════════════════════════════════
    std::vector<uint8_t> Serialize(TxSerializationMode mode) const;
    std::vector<uint8_t> Serialize(bool include_witness = true) const;  // Deprecated: use TxSerializationMode
    std::string SerializeHex(TxSerializationMode mode) const;
    std::string SerializeHex(bool include_witness = true) const;  // Deprecated: use TxSerializationMode
    
    // Transaction ID (hash of non-witness data) - Phase M.4.3-A: TxId semantic type
    TxId GetTxid() const;

    // Witness transaction ID: hash of the complete serialization, including
    // witness data. Consensus::ComputeWitnessMerkleRoot() consumes this value
    // for the DINW coinbase commitment. Transaction indexing and the block
    // header merkle root continue to use GetTxid().
    //
    // Locked by: tests/consensus/test_witness_merkle_isolation.cpp
    WTxId GetWtxid() const;
    
    // Size calculations
    size_t GetSize() const;  // Total size with witness
    size_t GetBaseSize() const;  // Size without witness
    size_t GetWeight() const;  // Weight units (base*3 + total)
    
    // Helper: compute virtual size for fee calculation
    size_t GetVirtualSize() const { return (GetWeight() + 3) / 4; }
};

// Transaction builder helper
class TransactionSerializer {
public:
    // Serialize integer types (little-endian)
    static void WriteUint32(std::vector<uint8_t>& out, uint32_t value);
    static void WriteUint64(std::vector<uint8_t>& out, uint64_t value);
    static void WriteVarint(std::vector<uint8_t>& out, uint64_t value);
    
    // Serialize vectors
    static void WriteBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& data);
    static void WriteString(std::vector<uint8_t>& out, const std::string& str);
    
    // Hash functions
    static std::string DoubleSHA256(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> DoubleSHA256Bytes(const std::vector<uint8_t>& data);
    
    // Hex conversion
    static std::string ToHex(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> FromHex(const std::string& hex);
    
    // Deserialization
    static bool Deserialize(Transaction& tx, const std::vector<uint8_t>& data);
    static bool Deserialize(Transaction& tx, const std::vector<uint8_t>& data, size_t& consumed_out);
    static bool Deserialize(Transaction& tx, const std::string& hex);
};

} // namespace dinero

// Phase M.0: std::hash specialization for TxOutPoint (consensus-critical)
namespace std {
template<>
struct hash<dinero::TxOutPoint> {
    size_t operator()(const dinero::TxOutPoint& outpoint) const {
        // Combine txid hash with vout - Phase M.4.3-B: Uses TxId hash
        size_t h1 = std::hash<dinero::TxId>{}(outpoint.txid);
        size_t h2 = std::hash<uint32_t>{}(outpoint.vout);
        return h1 ^ (h2 << 1);
    }
};
} // namespace std
