#pragma once

/**
 * @file compact_proof_blocks.h
 * @brief Phase 34.7: Compact-Proof Block Relay
 *
 * Extends BIP152 compact blocks with Utreexo proofs for stateless validation.
 * This enables:
 * - 90%+ bandwidth reduction (from compact blocks)
 * - Stateless block validation (from Utreexo proofs)
 * - Instant sync (no UTXO set download needed)
 *
 * Protocol:
 * 1. Peer sends cmpctblock with short tx IDs
 * 2. Node requests missing txs via getblocktxn
 * 3. Peer responds with blocktxnproofs (txs + Utreexo proofs)
 * 4. Node reconstructs full block with proofs
 * 5. Node validates block statelessly using proofs
 *
 * Wire format (blocktxnproofs):
 *   block_hash  : 32 bytes
 *   txcount     : CompactSize
 *   transactions: tx[]
 *   proof_data  : BlockUtreexoProofs (serialized)
 */

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <chrono>
#include <atomic>
#include <optional>
#include <functional>

// Plan-A: Use binary compact block implementation
#include "p2p/compact_block.h"
#include "consensus/utreexo_proof_relay.h"
#include "primitives/block.h"

namespace dinero {
namespace p2p {

// Import binary types from dinero namespace
using dinero::CompactBlock;
using dinero::BlockTransactionsRequest;

// ═══════════════════════════════════════════════════════════════════════════
// Compact-Proof Block: Compact block extended with Utreexo proofs
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Compact block with Utreexo proofs for stateless validation
 *
 * Combines:
 * - BIP152 compact block (short tx IDs, prefilled txs)
 * - Utreexo proofs (for stateless UTXO validation)
 *
 * This allows nodes without UTXO set to validate blocks.
 */
struct CompactProofBlock {
    // Standard compact block fields (binary format)
    CompactBlock compact_block;

    // Utreexo proofs for all non-coinbase inputs
    consensus::BlockUtreexoProofs utreexo_proofs;

    // Convenience methods
    uint256 getHash() const { return compact_block.header.GetHash(); }
    bool hasProofs() const { return !utreexo_proofs.empty(); }

    // Serialization
    std::vector<uint8_t> serialize() const;
    static CompactProofBlock deserialize(const std::vector<uint8_t>& data);
};

// ═══════════════════════════════════════════════════════════════════════════
// Block Transactions with Proofs Response
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Response to getblocktxn with Utreexo proofs
 *
 * Format matches blocktxnproofs P2P message:
 * - Block hash
 * - Missing transactions
 * - Utreexo proofs for those transactions
 */
struct BlockTransactionsWithProofs {
    std::string block_hash;
    std::vector<dinero::Transaction> transactions;
    consensus::BlockUtreexoProofs proofs;

    // Serialization (matches blocktxnproofs wire format)
    std::vector<uint8_t> serialize() const;
    static BlockTransactionsWithProofs deserialize(const std::vector<uint8_t>& data);
};

// ═══════════════════════════════════════════════════════════════════════════
// Compact-Proof Block Manager
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Manager for compact-proof block relay
 *
 * Extends compact blocks (BIP152) with:
 * - Utreexo proof storage and relay
 * - Stateless block validation
 * - Proof-aware reconstruction
 *
 * Integrates with:
 * - CompactBlockCodec: For block reconstruction (binary format)
 * - GlobalUTXOSet: For proof generation
 * - Mempool: For transaction proofs (Phase 34.6)
 *
 * Plan-A: Uses binary wire format for compact blocks.
 */
class CompactProofBlockManager {
public:
    CompactProofBlockManager();
    ~CompactProofBlockManager();

    // ───────────────────────────────────────────────────────────────────────
    // Compact-Proof Block Processing
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Process incoming cmpctblock with optional proofs
     *
     * @param peer_id Peer that sent the block
     * @param compact_block The compact block
     * @param proofs Optional Utreexo proofs (may be empty)
     * @return true if block was processed/queued successfully
     */
    bool processCompactProofBlock(const std::string& peer_id,
                                   const CompactBlock& compact_block,
                                   const consensus::BlockUtreexoProofs& proofs = {});

    /**
     * @brief Process blocktxnproofs response
     *
     * Called when we receive missing transactions with their proofs.
     *
     * @param peer_id Peer that sent the response
     * @param response The transactions and proofs
     * @param full_block_out Output: reconstructed block with proofs
     * @return true if block was fully reconstructed
     */
    bool processBlockTxnProofs(const std::string& peer_id,
                                const BlockTransactionsWithProofs& response,
                                dinero::Block& full_block_out);

    // ───────────────────────────────────────────────────────────────────────
    // Block Reconstruction with Proofs
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Reconstruct full block with Utreexo proofs
     *
     * Combines compact block transactions with proofs for stateless validation.
     *
     * @param compact_block The compact block
     * @param proofs The Utreexo proofs
     * @param full_block_out Output: reconstructed block
     * @return true if reconstruction succeeded
     */
    bool reconstructBlockWithProofs(const CompactBlock& compact_block,
                                     const consensus::BlockUtreexoProofs& proofs,
                                     dinero::Block& full_block_out);

    /**
     * @brief Create getblocktxn request with proof requirement
     *
     * Indicates we also need Utreexo proofs for the missing transactions.
     *
     * @param compact_block Block we're missing transactions for
     * @return Request to send to peer
     */
    BlockTransactionsRequest createMissingTxProofRequest(const CompactBlock& compact_block);

    // ───────────────────────────────────────────────────────────────────────
    // Proof Management
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Cache proofs for a block
     *
     * @param block_hash Block hash
     * @param proofs The proofs to cache
     */
    void cacheProofs(const std::string& block_hash,
                     const consensus::BlockUtreexoProofs& proofs);

    /**
     * @brief Get cached proofs for a block
     *
     * @param block_hash Block hash
     * @return Proofs if cached, nullopt otherwise
     */
    std::optional<consensus::BlockUtreexoProofs> getCachedProofs(
        const std::string& block_hash) const;

    /**
     * @brief Check if we have proofs for a block
     */
    bool hasProofs(const std::string& block_hash) const;

    /**
     * @brief Remove proofs from cache
     */
    void removeProofs(const std::string& block_hash);

    // ───────────────────────────────────────────────────────────────────────
    // Relay and Request
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Create blocktxnproofs response
     *
     * @param request The getblocktxn request
     * @param block The full block (to extract transactions)
     * @param proofs The Utreexo proofs
     * @return Response to send to peer
     */
    BlockTransactionsWithProofs createBlockTxnProofsResponse(
        const BlockTransactionsRequest& request,
        const dinero::Block& block,
        const consensus::BlockUtreexoProofs& proofs);

    /**
     * @brief Set callback for requesting proofs from peer
     */
    using RequestProofsCallback = std::function<void(const std::string& peer_id,
                                                      const std::string& block_hash)>;
    void setRequestProofsCallback(RequestProofsCallback callback) {
        request_proofs_callback_ = callback;
    }

    // ───────────────────────────────────────────────────────────────────────
    // Configuration
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Enable/disable stateless mode
     *
     * In stateless mode, blocks REQUIRE proofs for validation.
     */
    void setStatelessMode(bool enabled) { stateless_mode_ = enabled; }
    bool isStatelessMode() const { return stateless_mode_; }

    /**
     * @brief Enable/disable proof caching
     */
    void setProofCaching(bool enabled) { proof_caching_ = enabled; }
    bool isProofCachingEnabled() const { return proof_caching_; }

    /**
     * @brief Set max proof cache size (in number of blocks)
     */
    void setMaxProofCacheSize(size_t max_size) { max_proof_cache_size_ = max_size; }
    size_t getMaxProofCacheSize() const { return max_proof_cache_size_; }

    // ───────────────────────────────────────────────────────────────────────
    // Statistics
    // ───────────────────────────────────────────────────────────────────────

    struct Stats {
        uint64_t compact_proof_blocks_received;
        uint64_t blocks_reconstructed_with_proofs;
        uint64_t proof_requests_sent;
        uint64_t blocktxnproofs_received;
        uint64_t blocktxnproofs_sent;
        uint64_t proof_cache_hits;
        uint64_t proof_cache_misses;
        uint64_t bandwidth_saved_bytes;
        size_t current_cache_size;
    };

    Stats getStats() const;
    std::string getStatsString() const;

private:
    mutable std::mutex mutex_;

    // Configuration
    bool stateless_mode_{false};
    bool proof_caching_{true};
    size_t max_proof_cache_size_{100};

    // Proof cache: block_hash -> proofs
    std::unordered_map<std::string, consensus::BlockUtreexoProofs> proof_cache_;
    std::vector<std::string> proof_cache_order_;  // LRU order

    // Pending blocks waiting for proofs (uses binary CompactBlock)
    std::unordered_map<uint256, CompactBlock> pending_proof_blocks_;

    // Integration
    RequestProofsCallback request_proofs_callback_;

    // Statistics
    mutable Stats stats_{};

    // Internal methods
    void evictOldestProofs();
    void updateCacheOrder(const std::string& block_hash);
};

} // namespace p2p
} // namespace dinero
