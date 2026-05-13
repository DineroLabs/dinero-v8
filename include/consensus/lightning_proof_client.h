#pragma once

#include <optional>
#include <functional>
#include <mutex>
#include <memory>
#include "primitives/uint256.h"
#include "primitives/block.h"
#include "consensus/proof_cache.h"
#include "consensus/proof_router.h"
#include "consensus/proof_gossip.h"
#include "consensus/proof_compression.h"

namespace dinero {
namespace consensus {

/**
 * Proof query result
 *
 * Phase 9.5: Result of proof query with metadata
 */
struct ProofQueryResult {
    BlockUtreexoData proof;
    bool from_cache = false;
    bool compressed = false;
    uint64_t peer_id = 0;  // 0 = local cache/provider

    ProofQueryResult() = default;
    ProofQueryResult(const BlockUtreexoData& p) : proof(p) {}
};

/**
 * Proof query statistics
 *
 * Phase 9.5: Track Lightning proof query performance
 */
struct ProofQueryStats {
    uint64_t queries_total = 0;
    uint64_t queries_cache_hit = 0;
    uint64_t queries_cache_miss = 0;
    uint64_t queries_network_success = 0;
    uint64_t queries_network_fail = 0;
    uint64_t queries_local_provider = 0;

    /**
     * Calculate cache hit rate
     */
    double CacheHitRate() const {
        if (queries_total == 0) return 0.0;
        return static_cast<double>(queries_cache_hit) / queries_total;
    }

    /**
     * Calculate network success rate
     */
    double NetworkSuccessRate() const {
        uint64_t network_queries = queries_cache_miss;
        if (network_queries == 0) return 0.0;
        return static_cast<double>(queries_network_success) / network_queries;
    }
};

/**
 * Proof query interface
 *
 * Phase 9.5: Abstract interface for proof queries
 *
 * Allows different implementations:
 * - Lightning daemon queries
 * - Direct bridge node queries
 * - Testing mock implementations
 */
class ProofQueryInterface {
public:
    virtual ~ProofQueryInterface() = default;

    /**
     * Query proof for a block
     *
     * @param block_hash Block hash to query proof for
     * @param expected_root Optional expected accumulator root (for validation)
     * @return Proof if available, nullopt if not found
     */
    virtual std::optional<ProofQueryResult> QueryProof(
        const uint256& block_hash,
        const uint256& expected_root = uint256()) = 0;

    /**
     * Get query statistics
     */
    virtual ProofQueryStats GetStats() const = 0;

    /**
     * Clear statistics (for testing)
     */
    virtual void ClearStats() = 0;
};

/**
 * Lightning proof client
 *
 * Phase 9.5: Read-only proof query client for Lightning daemon
 *
 * Integration with Phase 9 components:
 * 1. ProofCache: Check cache first (fast path)
 * 2. ProofRouter: Select best peer if cache miss
 * 3. ProofGossipManager: Request proof from peer
 * 4. ProofCompressionManager: Handle compressed proofs
 *
 * Read-only guarantees:
 * - Lightning daemon never generates proofs (bridge node responsibility)
 * - Lightning daemon never announces proofs (passive consumer)
 * - Query failure never blocks Lightning operation (graceful degradation)
 *
 * Use case: Lightning needs proofs to validate channel/HTLC states
 * Example: Verifying a channel funding output is unspent
 */
class LightningProofClient : public ProofQueryInterface {
public:
    LightningProofClient();
    ~LightningProofClient() override = default;

    /**
     * Set proof cache (optional, for fast lookups)
     *
     * @param cache Shared cache instance
     */
    void SetCache(std::shared_ptr<ProofCache> cache);

    /**
     * Set proof router (for peer selection)
     *
     * @param router Shared router instance
     */
    void SetRouter(std::shared_ptr<ProofRouter> router);

    /**
     * Set gossip manager (for proof requests)
     *
     * @param gossip Shared gossip manager instance
     */
    void SetGossipManager(std::shared_ptr<ProofGossipManager> gossip);

    /**
     * Set compression manager (for handling compressed proofs)
     *
     * @param compression Shared compression manager instance
     */
    void SetCompressionManager(std::shared_ptr<ProofCompressionManager> compression);

    /**
     * Set local proof provider (for bridge nodes)
     *
     * Callback to retrieve proofs locally (if running bridge node)
     *
     * @param provider Callback: (block_hash) -> optional<BlockUtreexoData>
     */
    void SetLocalProvider(std::function<std::optional<BlockUtreexoData>(const uint256&)> provider);

    /**
     * Query proof for a block (implements ProofQueryInterface)
     *
     * Query strategy:
     * 1. Check cache (if configured)
     * 2. Check local provider (if configured)
     * 3. Request from network via gossip (if configured)
     * 4. Return nullopt if all fail (graceful degradation)
     *
     * @param block_hash Block hash to query proof for
     * @param expected_root Optional expected accumulator root
     * @return Proof if available, nullopt if not found
     */
    std::optional<ProofQueryResult> QueryProof(
        const uint256& block_hash,
        const uint256& expected_root = uint256()) override;

    /**
     * Get query statistics
     */
    ProofQueryStats GetStats() const override;

    /**
     * Clear statistics (for testing)
     */
    void ClearStats() override;

    /**
     * Configuration: Set query timeout (milliseconds)
     * Default: 5000ms (5 seconds)
     */
    void SetQueryTimeout(uint64_t timeout_ms);

    /**
     * Configuration: Enable/disable cache writes
     * If enabled, successful network queries are cached
     * Default: true
     */
    void SetCacheWrites(bool enabled);

private:
    // Phase 9 component integrations
    std::shared_ptr<ProofCache> cache_;
    std::shared_ptr<ProofRouter> router_;
    std::shared_ptr<ProofGossipManager> gossip_;
    std::shared_ptr<ProofCompressionManager> compression_;

    // Local proof provider (for bridge nodes)
    std::function<std::optional<BlockUtreexoData>(const uint256&)> local_provider_;

    // Statistics
    mutable ProofQueryStats stats_;

    // Configuration
    uint64_t query_timeout_ms_ = 5000;  // 5 seconds default
    bool cache_writes_enabled_ = true;

    // Thread safety
    mutable std::mutex mutex_;

    /**
     * Try to get proof from cache
     */
    std::optional<ProofQueryResult> TryCache(const uint256& block_hash);

    /**
     * Try to get proof from local provider
     */
    std::optional<ProofQueryResult> TryLocalProvider(const uint256& block_hash);

    /**
     * Try to get proof from network
     */
    std::optional<ProofQueryResult> TryNetwork(
        const uint256& block_hash,
        const uint256& expected_root);

    /**
     * Store proof in cache (if cache writes enabled)
     */
    void MaybeStoreInCache(const uint256& block_hash, const BlockUtreexoData& proof);
};

/**
 * Mock proof provider for testing
 *
 * Phase 9.5: In-memory proof storage for testing
 */
class MockProofProvider {
public:
    MockProofProvider() = default;
    ~MockProofProvider() = default;

    /**
     * Add proof to mock storage
     */
    void AddProof(const uint256& block_hash, const BlockUtreexoData& proof);

    /**
     * Get proof from mock storage
     */
    std::optional<BlockUtreexoData> GetProof(const uint256& block_hash);

    /**
     * Clear all stored proofs
     */
    void Clear();

    /**
     * Get provider callback (for use with SetLocalProvider)
     */
    std::function<std::optional<BlockUtreexoData>(const uint256&)> GetCallback();

private:
    std::unordered_map<uint256, BlockUtreexoData> proofs_;
    mutable std::mutex mutex_;
};

} // namespace consensus
} // namespace dinero
