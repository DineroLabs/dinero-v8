// Transaction implementation - consensus-critical serialization methods
// Phase M.1: Moved from wallet layer to primitives layer
// Consensus library (libdinero_consensus.a) requires these methods

#include "primitives/transaction.h"
#include <sstream>
#include <iomanip>
#include <cstring>

namespace dinero {

// ============================================================================
// Transaction Serialization (Consensus-Critical)
// ============================================================================
// ⚠️  Phase 11a.3: DO NOT use for merkle tree computation!
//     Use consensus::ComputeMerkleRoot(vtx) instead.
//     See include/primitives/transaction.h for full explanation.
// ============================================================================

// Phase 11d: Explicit serialization mode (recommended API)
std::vector<uint8_t> Transaction::Serialize(TxSerializationMode mode) const {
    return Serialize(mode == TxSerializationMode::WithWitness);
}

// Legacy API (deprecated - use TxSerializationMode instead)
std::vector<uint8_t> Transaction::Serialize(bool include_witness) const {
    std::vector<uint8_t> result;

    // Version (4 bytes, little-endian)
    TransactionSerializer::WriteUint32(result, static_cast<uint32_t>(version));

    // SegWit marker and flag (if witness data present)
    // Support all witness versions (v0, v1/Taproot, v2+)
    if (include_witness && HasWitness()) {
        result.push_back(0x00);  // Marker
        result.push_back(0x01);  // Flag
    }

    // Input count
    TransactionSerializer::WriteVarint(result, vin.size());

    // Inputs
    for (const auto& input : vin) {
        // Previous output (txid + vout)
        // Phase M.4.3-B: Unwrap TxId → uint256 for serialization (explicit boundary)
        // uint256 is already in little-endian wire format
        const auto& txid_bytes = input.prevout.txid.AsUint256();
        result.insert(result.end(), txid_bytes.begin(), txid_bytes.end());
        TransactionSerializer::WriteUint32(result, input.prevout.vout);

        // ScriptSig (empty for SegWit)
        TransactionSerializer::WriteBytes(result, input.scriptSig);

        // Sequence
        TransactionSerializer::WriteUint32(result, input.sequence);
    }

    // Output count
    TransactionSerializer::WriteVarint(result, vout.size());

    // Outputs
    for (const auto& output : vout) {
        // Check if this is a confidential output
        if (output.is_confidential) {
            // Confidential output format (Phase I):
            // - Value (8 bytes) = 0 (confidential marker)
            // - ScriptPubKey (variable length)
            // - Commitment (33 bytes)
            // - Range proof (variable length)
            // - Nonce (65 bytes for ECDH)

            TransactionSerializer::WriteUint64(result, 0);  // Confidential marker
            TransactionSerializer::WriteBytes(result, output.scriptPubKey);
            TransactionSerializer::WriteBytes(result, output.commitment);
            TransactionSerializer::WriteBytes(result, output.range_proof);
            TransactionSerializer::WriteBytes(result, output.nonce);
        } else {
            // Transparent output format:
            // - Value (8 bytes, little-endian)
            // - ScriptPubKey (variable length)

            // Phase M.6.1: Extract raw value for serialization
            TransactionSerializer::WriteUint64(result, output.value.GetUna());
            TransactionSerializer::WriteBytes(result, output.scriptPubKey);
        }
    }

    // Explicit fee field (Phase G.2 / Phase I / v7 shielded).
    // Serialized for any tx that has CT outputs OR shielded value semantics.
    // For shielded txs, transparent_in - transparent_out alone is insufficient
    // because value can move into/out of the shielded pool via
    // bundle.value_balance, so replay/reindex must carry the same explicit fee
    // value the live validator saw.
    // Transparent-only txs (v1/v2, no shielded bundle) use Bitcoin wire format:
    // no fee field.
    if (HasConfidentialOutputs() || IsShielded()) {
        // Serialized after outputs, before witness data.
        // Format: 1-byte flag + 8-byte fee (if flag = 1)
        if (has_explicit_fee) {
            result.push_back(0x01);  // Explicit fee marker
            // Phase M.6.1: Extract raw value for serialization
            TransactionSerializer::WriteUint64(result, explicit_fee.GetUna());
        } else {
            result.push_back(0x00);  // No explicit fee
        }
    }
    // Transparent-only transactions (v1/v2): no explicit fee field (Bitcoin-compatible)

    // Witness data (if witness present and including witness)
    // Compatible with all witness versions: v0 (SegWit), v1 (Taproot), v2+
    if (include_witness && HasWitness()) {
        for (const auto& input : vin) {
            // Witness stack count
            TransactionSerializer::WriteVarint(result, input.witness.size());

            // Each witness element
            for (const auto& witness_element : input.witness) {
                TransactionSerializer::WriteBytes(result, witness_element);
            }
        }
    }

    // Shielded bundle bytes are already canonical (SerializeShieldedBundle).
    // v5 is legacy mainnet behavior: witness-only, excluded from txid.
    // v6 is the forward shielded format: included even in non-witness
    // serialization so the txid commits to spends/outputs.
    const bool write_shielded_bundle =
        Transaction::IsShieldedVersion(version) &&
        !shielded_bundle_bytes.empty() &&
        (include_witness || version == TX_VERSION_SHIELDED_V2);
    if (write_shielded_bundle) {
        TransactionSerializer::WriteBytes(result, shielded_bundle_bytes);
    }

    // Locktime (4 bytes, little-endian)
    TransactionSerializer::WriteUint32(result, lockTime);

    return result;
}

// Phase 11d: Explicit serialization mode (recommended API)
std::string Transaction::SerializeHex(TxSerializationMode mode) const {
    return SerializeHex(mode == TxSerializationMode::WithWitness);
}

// Legacy API (deprecated - use TxSerializationMode instead)
std::string Transaction::SerializeHex(bool include_witness) const {
    // Use the existing Serialize() method
    std::vector<uint8_t> serialized = Serialize(include_witness);

    // Convert to hex string
    std::ostringstream hex;
    for (unsigned char byte : serialized) {
        hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return hex.str();
}

// ============================================================================
// Transaction Identity (Consensus-Critical)
// ============================================================================

TxId Transaction::GetTxid() const {
    // Txid is hash of non-witness serialization - Phase M.4.3-A: returns TxId
    auto bytes = Serialize(false);
    auto hash_bytes = TransactionSerializer::DoubleSHA256Bytes(bytes);
    uint256 result;
    // Phase M.0: Direct memcpy of raw SHA256 output (little-endian internal identity)
    // DoubleSHA256Bytes() returns little-endian bytes for uint256 storage
    std::memcpy(result.data, hash_bytes.data(), 32);
    return TxId(result);  // Wrap in TxId domain type
}

// ============================================================================
// Witness Transaction ID (Phase 11b.3: Safety Guard)
// ============================================================================
// ⚠️  Phase 11b.3: wtxid is NOT consensus-active yet!
//     Used only by consensus::ComputeWitnessMerkleRoot() (groundwork).
//     See include/primitives/transaction.h for full explanation.
// ============================================================================
WTxId Transaction::GetWtxid() const {
    // Wtxid is hash of full serialization (with witness) - Phase M.4.3-A: returns WTxId
    auto bytes = Serialize(true);
    auto hash_bytes = TransactionSerializer::DoubleSHA256Bytes(bytes);
    uint256 result;
    // Phase M.0: Direct memcpy of raw SHA256 output (little-endian internal identity)
    std::memcpy(result.data, hash_bytes.data(), 32);
    return WTxId(result);  // Wrap in WTxId domain type
}

// ============================================================================
// Transaction Size Calculation (Consensus-Critical)
// ============================================================================

size_t Transaction::GetSize() const {
    // Total size including witness data
    return Serialize(true).size();
}

size_t Transaction::GetBaseSize() const {
    // Size without witness data
    return Serialize(false).size();
}

size_t Transaction::GetWeight() const {
    // BIP141 weight calculation: base_size * 3 + total_size
    return GetBaseSize() * 3 + GetSize();
}

} // namespace dinero
