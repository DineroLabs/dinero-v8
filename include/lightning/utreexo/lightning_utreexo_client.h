// Copyright (c) 2026 The Dinero Core developers
// Distributed under the MIT software license

#pragma once

#include "consensus/lightning_proof_client.h"
#include "primitives/uint256.h"
#include "primitives/block.h"
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace lightning {
namespace utreexo {

/**
 * LightningProofCache - Lightning-specific proof cache
 *
 * Separate from Phase 9.1 core cache with different characteristics:
 * - Scope: Channel-specific (not global)
 * - TTL: Channel lifetime + 1 week (not short-lived)
 * - Eviction: Channel-based lifecycle (not just LRU)
 *
 * Cache ≠ trust: Always re-verify proofs before use
 */
class LightningProofCache {
public:
    LightningProofCache() = default;

    /**
     * Cache proof for channel
     * @param channel_id Channel identifier
     * @param utxo_hash UTXO hash (funding txid or HTLC txid)
     * @param proof Utreexo proof data
     */
    void CacheChannelProof(
        const std::string& channel_id,
        const dinero::uint256& utxo_hash,
        const dinero::consensus::BlockUtreexoData& proof
    );

    /**
     * Get cached proof
     * @param utxo_hash UTXO hash to lookup
     * @return Proof if cached and not expired, nullopt otherwise
     */
    std::optional<dinero::consensus::BlockUtreexoData> GetProof(
        const dinero::uint256& utxo_hash
    );

    /**
     * Evict all proofs for closed channel
     * @param channel_id Channel to evict
     * @return Number of proofs evicted
     */
    size_t EvictChannelProofs(const std::string& channel_id);

    /**
     * Clear entire cache (e.g., on reorg)
     */
    void Clear();

    /**
     * Get cache statistics
     */
    struct CacheStats {
        size_t total_proofs = 0;
        size_t channels_tracked = 0;
        size_t evicted_expired = 0;
    };
    CacheStats GetStats() const;

private:
    // channel_id → set of UTXO hashes
    std::unordered_map<std::string, std::unordered_set<dinero::uint256>> channel_proofs_;

    // UTXO hash → (proof, expiry_time)
    std::unordered_map<dinero::uint256, std::pair<dinero::consensus::BlockUtreexoData, uint64_t>> proof_cache_;

    // Statistics
    CacheStats stats_;

    mutable std::mutex mutex_;

    // Default TTL: 1 week (in seconds)
    static constexpr uint64_t DEFAULT_TTL = 7 * 24 * 60 * 60;

    // Max cached proofs (LRU eviction if exceeded)
    static constexpr size_t MAX_CACHED_PROOFS = 1000;

    // Check if proof expired
    bool IsExpired(uint64_t expiry_time) const;

    // Get current time (seconds since epoch)
    uint64_t GetCurrentTime() const;

    // Evict expired proofs
    void EvictExpired();
};

/**
 * LightningUtreexoClient - Lightning-specific proof query client
 *
 * Phase 11.1: Read-only wrapper around Phase 9.5 ProofQueryInterface
 *
 * Purpose: Provide Lightning components with proof query capability without
 * touching consensus or proof distribution logic.
 *
 * Key Properties:
 * - Read-only wrapper (never modifies Phase 9 infrastructure)
 * - Lightning-specific semantics (channels, HTLCs)
 * - Maintains Lightning-specific cache (separate from Phase 9.1)
 * - Tracks statistics for monitoring
 *
 * Usage:
 *   auto client = std::make_shared<LightningUtreexoClient>(proof_provider);
 *   auto proof = client->GetChannelFundingProof(txid, vout, root);
 *   if (proof) {
 *       // Validate channel funding
 *   }
 */
class LightningUtreexoClient {
public:
    /**
     * Construct client with Phase 9.5 proof provider
     * @param proof_provider ProofQueryInterface from Phase 9.5
     */
    explicit LightningUtreexoClient(
        std::shared_ptr<dinero::consensus::ProofQueryInterface> proof_provider
    );

    /**
     * Get proof for channel funding transaction
     *
     * Query flow:
     * 1. Try Lightning-specific cache (fast path)
     * 2. Try Phase 9.5 proof provider (cache → router → gossip)
     * 3. Cache result if found (for future queries)
     *
     * @param funding_txid Funding transaction ID
     * @param funding_vout Funding output index
     * @param expected_root Expected Utreexo root (for validation)
     * @return Proof data, or nullopt if unavailable
     */
    std::optional<dinero::consensus::BlockUtreexoData> GetChannelFundingProof(
        const dinero::uint256& funding_txid,
        uint32_t funding_vout,
        const dinero::uint256& expected_root
    );

    /**
     * Get proof for HTLC settlement
     *
     * Query flow: Same as channel funding (cache → provider → cache result)
     *
     * @param htlc_txid HTLC transaction ID
     * @param htlc_vout HTLC output index
     * @param expected_root Expected Utreexo root
     * @return Proof data, or nullopt if unavailable
     */
    std::optional<dinero::consensus::BlockUtreexoData> GetHTLCProof(
        const dinero::uint256& htlc_txid,
        uint32_t htlc_vout,
        const dinero::uint256& expected_root
    );

    /**
     * Prefetch proofs for channel monitoring
     *
     * Fetch proofs for all known UTXOs associated with a channel.
     * Useful for watchtowers to populate cache before monitoring.
     *
     * @param channel_id Channel to prefetch proofs for
     * @param utxo_hashes List of UTXO hashes to prefetch
     * @param expected_root Expected Utreexo root
     * @return Number of proofs successfully cached
     */
    size_t PrefetchChannelProofs(
        const std::string& channel_id,
        const std::vector<dinero::uint256>& utxo_hashes,
        const dinero::uint256& expected_root
    );

    /**
     * Evict proofs for closed channel
     * @param channel_id Channel that was closed
     * @return Number of proofs evicted
     */
    size_t EvictChannelProofs(const std::string& channel_id);

    /**
     * Clear all cached proofs (e.g., on reorg)
     */
    void ClearCache();

    /**
     * Get client statistics
     */
    struct Stats {
        uint64_t channel_funding_queries = 0;
        uint64_t htlc_queries = 0;
        uint64_t prefetch_requests = 0;
        uint64_t cache_hits = 0;
        uint64_t cache_misses = 0;
        uint64_t proof_unavailable = 0;
        uint64_t provider_queries = 0;
    };
    Stats GetStats() const;

private:
    // Phase 9.5 proof provider (read-only)
    std::shared_ptr<dinero::consensus::ProofQueryInterface> proof_provider_;

    // Lightning-specific proof cache
    LightningProofCache cache_;

    // Statistics
    Stats stats_;

    mutable std::mutex mutex_;

    // Helper: Query Phase 9.5 provider
    std::optional<dinero::consensus::BlockUtreexoData> QueryProvider(
        const dinero::uint256& txid,
        uint32_t vout,
        const dinero::uint256& expected_root
    );
};

} // namespace utreexo
} // namespace lightning
