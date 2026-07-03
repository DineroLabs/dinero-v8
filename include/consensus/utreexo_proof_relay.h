#pragma once

/**
 * @file utreexo_proof_relay.h
 * @brief Phase 34.4: Utreexo Proof Relay Data Structures
 *
 * This file defines the canonical data structures for Utreexo proof relay:
 * - TxInProof: Associates a transaction input with its Utreexo proof
 * - BlockUtreexoProofs: Batch of proofs for all inputs in a block
 *
 * Design principles:
 * 1. Deterministic serialization (network transmission, on-disk)
 * 2. CompactSize encoding for variable-length fields
 * 3. Version field for future upgrades
 * 4. Efficient batch verification
 *
 * Wire format:
 * - All integers are little-endian
 * - All variable-length arrays use CompactSize prefix
 * - Hashes are 32 bytes in natural byte order
 */

#include <vector>
#include <cstdint>
#include <string>
#include "consensus/utreexo_accumulator.h"
#include "primitives/transaction.h"

namespace dinero {
namespace consensus {

// ═══════════════════════════════════════════════════════════════════════════
// Version Constants
// ═══════════════════════════════════════════════════════════════════════════

constexpr uint8_t UTREEXO_PROOF_VERSION = 1;

// ═══════════════════════════════════════════════════════════════════════════
// TxInProof: Single input proof
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Proof for a single transaction input
 *
 * Associates a TxOutPoint (txid:vout) with its Utreexo proof.
 * This proves the UTXO exists in the accumulator without needing
 * the full UTXO set.
 *
 * Wire format (deterministic):
 *   txid         : 32 bytes (natural order, not reversed)
 *   vout         : 4 bytes (little-endian uint32)
 *   position     : 8 bytes (little-endian uint64)
 *   numLeaves    : 8 bytes (little-endian uint64)
 *   numSiblings  : CompactSize
 *   siblings     : 32 * numSiblings bytes
 *
 * Total: 52 + CompactSize(n) + 32*n bytes
 */
struct TxInProof {
    // The UTXO being spent
    TxOutPoint outpoint;

    // Utreexo proof for this UTXO
    UtreexoProof proof;

    // Cached leaf hash (optional, for verification efficiency)
    // Not serialized - computed on demand
    mutable UtreexoHash leafHash;

    TxInProof() = default;
    TxInProof(const TxOutPoint& op, const UtreexoProof& p)
        : outpoint(op), proof(p) {}

    // ───────────────────────────────────────────────────────────────────────
    // Serialization
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Serialize to bytes (deterministic wire format)
     */
    std::vector<uint8_t> Serialize() const;

    /**
     * @brief Deserialize from bytes
     * @return true on success, false on parse error
     */
    static bool Deserialize(const uint8_t* data, size_t size, TxInProof& out, size_t& bytesRead);

    /**
     * @brief Get serialized size in bytes
     */
    size_t GetSerializeSize() const;

    // ───────────────────────────────────────────────────────────────────────
    // Validation
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Verify this proof against the current forest roots
     *
     * @param roots Current Utreexo forest roots
     * @param amount UTXO amount (for leaf hash)
     * @param scriptPubKey UTXO script (for leaf hash)
     * @return true if proof is valid
     */
    bool Verify(const std::vector<UtreexoHash>& roots,
                uint64_t amount,
                const std::vector<uint8_t>& scriptPubKey) const;

    bool Verify(const std::vector<UtreexoHash>& roots,
                uint64_t amount,
                const std::vector<uint8_t>& scriptPubKey,
                uint32_t created_height,
                bool is_coinbase) const;
};

// ═══════════════════════════════════════════════════════════════════════════
// BlockUtreexoProofs: Batch proofs for entire block
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief All Utreexo proofs for a block
 *
 * Contains proofs for every non-coinbase input in the block.
 * Coinbase inputs have no previous output, so they need no proof.
 *
 * Wire format (deterministic):
 *   version     : 1 byte
 *   blockHash   : 32 bytes (for correlation)
 *   numProofs   : CompactSize
 *   proofs      : TxInProof[]
 *
 * Features:
 * - Version field for future format upgrades
 * - Block hash for correlation with block data
 * - Batch verification support
 *
 * Future optimizations (Phase 34.8):
 * - Proof aggregation (combine sibling paths)
 * - Compressed position encoding
 * - Delta encoding for sequential positions
 */
struct BlockUtreexoProofs {
    // Version for future format changes
    uint8_t version;

    // Block hash this proof batch belongs to
    std::string blockHash;  // 64 hex chars

    // Proofs for all non-coinbase inputs
    // Order: tx[0].in[0], tx[0].in[1], ..., tx[1].in[0], ...
    // Note: tx[0] is coinbase (skipped), so starts at tx[1]
    std::vector<TxInProof> proofs;

    BlockUtreexoProofs() : version(UTREEXO_PROOF_VERSION) {}

    // ───────────────────────────────────────────────────────────────────────
    // Serialization
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Serialize to bytes (deterministic wire format)
     */
    std::vector<uint8_t> Serialize() const;

    /**
     * @brief Deserialize from bytes
     * @return true on success, false on parse error
     */
    static bool Deserialize(const uint8_t* data, size_t size, BlockUtreexoProofs& out);
    static bool Deserialize(const std::vector<uint8_t>& data, BlockUtreexoProofs& out);

    /**
     * @brief Get serialized size in bytes
     */
    size_t GetSerializeSize() const;

    // ───────────────────────────────────────────────────────────────────────
    // Batch Operations
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Verify all proofs against forest roots
     *
     * @param roots Current Utreexo forest roots
     * @param utxoLookup Function to get UTXO data (amount, scriptPubKey) for outpoint
     * @return true if ALL proofs are valid
     */
    template<typename UTXOLookup>
    bool VerifyAll(const std::vector<UtreexoHash>& roots, UTXOLookup utxoLookup) const {
        for (const auto& proof : proofs) {
            auto utxo = utxoLookup(proof.outpoint);
            if (!utxo.has_value()) {
                return false;  // UTXO data not available
            }
            if (!proof.Verify(roots, utxo->amount, utxo->scriptPubKey)) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Get number of proofs
     */
    size_t size() const { return proofs.size(); }

    /**
     * @brief Check if empty
     */
    bool empty() const { return proofs.empty(); }

    /**
     * @brief Add a proof
     */
    void AddProof(const TxInProof& proof) {
        proofs.push_back(proof);
    }

    /**
     * @brief Clear all proofs
     */
    void Clear() {
        proofs.clear();
        blockHash.clear();
    }

    // ───────────────────────────────────────────────────────────────────────
    // Statistics
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Get statistics about this proof batch
     */
    struct Stats {
        size_t numProofs;
        size_t totalSiblings;
        size_t serializedSize;
        double avgProofSize;
    };

    Stats GetStats() const;
};

// ═══════════════════════════════════════════════════════════════════════════
// UTXO Data (for proof verification lookup)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Minimal UTXO data needed for proof verification
 */
struct UTXOData {
    uint64_t amount;
    std::vector<uint8_t> scriptPubKey;

    UTXOData() : amount(0) {}
    UTXOData(uint64_t amt, const std::vector<uint8_t>& script)
        : amount(amt), scriptPubKey(script) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// Serialization Helpers (CompactSize encoding)
// ═══════════════════════════════════════════════════════════════════════════

namespace detail {

/**
 * @brief Write CompactSize to buffer
 * @return Number of bytes written (1, 3, 5, or 9)
 */
size_t WriteCompactSize(std::vector<uint8_t>& out, uint64_t value);

/**
 * @brief Read CompactSize from buffer
 * @return true on success
 */
bool ReadCompactSize(const uint8_t* data, size_t size, size_t& offset, uint64_t& value);

/**
 * @brief Write little-endian uint32
 */
void WriteUint32LE(std::vector<uint8_t>& out, uint32_t value);

/**
 * @brief Write little-endian uint64
 */
void WriteUint64LE(std::vector<uint8_t>& out, uint64_t value);

/**
 * @brief Read little-endian uint32
 */
bool ReadUint32LE(const uint8_t* data, size_t size, size_t& offset, uint32_t& value);

/**
 * @brief Read little-endian uint64
 */
bool ReadUint64LE(const uint8_t* data, size_t size, size_t& offset, uint64_t& value);

/**
 * @brief Get CompactSize encoding size
 */
size_t CompactSizeSize(uint64_t value);

} // namespace detail

} // namespace consensus
} // namespace dinero
