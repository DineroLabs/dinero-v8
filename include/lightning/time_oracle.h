#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Phase 8.5: Deterministic Time Oracle (Block-Height-Based Time)
// ═══════════════════════════════════════════════════════════════════════════
// Lightning MUST NOT use wall clock time (std::time, std::chrono::system_clock).
// Instead, time is derived ONLY from block height.
//
// ARCHITECTURE:
// - Core state machine: Uses block heights directly (CLTVs, expiry heights)
// - Protocol compatibility: Converts block height → Unix timestamp (approximate)
// - Deterministic: Same block height → same timestamp
// - Testable: MockTimeOracle allows full control
//
// Replaces forbidden patterns:
//   ❌ std::time(nullptr)
//   ❌ std::chrono::system_clock::now()
//   ✅ time_oracle->getCurrentBlockHeight()
//   ✅ time_oracle->blockHeightToTimestamp(height)
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include "lightning/chain_oracle.h"  // Phase M.4: Need full definition for ProductionTimeOracle inline methods

namespace lightning {

// Forward declaration (kept for documentation, but full definition included above)
class IChainOracle;

/**
 * Deterministic time source based on blockchain block height.
 *
 * Phase 8.5 Invariant:
 * - Lightning has NO access to wall clock time
 * - Time exists ONLY as block height
 * - Timestamps are derived from block height (for BOLT-11 compatibility)
 *
 * Implementation:
 * - Production: Queries IChainOracle for current block height
 * - Test: MockTimeOracle with controlled block height
 */
class ITimeOracle {
public:
    virtual ~ITimeOracle() = default;

    /**
     * Get current block height (canonical time source).
     * This is the ONLY time source Lightning uses.
     */
    virtual uint64_t getCurrentBlockHeight() const = 0;

    /**
     * Convert block height to approximate Unix timestamp.
     * Used for BOLT-11 invoice timestamps (protocol requirement).
     *
     * Formula (Dinero):
     *   timestamp = genesis_time + (height * block_interval_seconds)
     *   genesis_time = 1609459200 (2021-01-01 00:00:00 UTC)
     *   block_interval = 300 seconds (5 minutes)
     *
     * Note: This is approximate - actual block times vary. But it's
     * deterministic: same height always gives same timestamp.
     */
    virtual uint64_t blockHeightToTimestamp(uint64_t height) const = 0;

    /**
     * Convert Unix timestamp to approximate block height.
     * Used for converting BOLT-11 expiry timestamps to expiry heights.
     */
    virtual uint64_t timestampToBlockHeight(uint64_t timestamp) const = 0;

    /**
     * Get current time as Unix timestamp (derived from block height).
     * Convenience method for protocol compatibility.
     * Equivalent to: blockHeightToTimestamp(getCurrentBlockHeight())
     */
    virtual uint64_t getCurrentTimestamp() const = 0;
};

/**
 * Mock implementation for testing.
 * Allows tests to control block height and verify determinism.
 */
class MockTimeOracle : public ITimeOracle {
public:
    MockTimeOracle() : m_block_height(0), m_genesis_time(1609459200), m_block_interval(300) {}

    // Test configuration
    void setBlockHeight(uint64_t height) { m_block_height = height; }
    void setGenesisTime(uint64_t genesis) { m_genesis_time = genesis; }
    void setBlockInterval(uint64_t interval) { m_block_interval = interval; }

    // ITimeOracle implementation
    uint64_t getCurrentBlockHeight() const override {
        return m_block_height;
    }

    uint64_t blockHeightToTimestamp(uint64_t height) const override {
        return m_genesis_time + (height * m_block_interval);
    }

    uint64_t timestampToBlockHeight(uint64_t timestamp) const override {
        if (timestamp < m_genesis_time) return 0;
        return (timestamp - m_genesis_time) / m_block_interval;
    }

    uint64_t getCurrentTimestamp() const override {
        return blockHeightToTimestamp(m_block_height);
    }

private:
    uint64_t m_block_height;
    uint64_t m_genesis_time;
    uint64_t m_block_interval;
};

/**
 * Production implementation using IChainOracle.
 * Derives time from current blockchain height.
 */
class ProductionTimeOracle : public ITimeOracle {
public:
    explicit ProductionTimeOracle(const IChainOracle* chain_oracle)
        : m_chain_oracle(chain_oracle)
        , m_genesis_time(1609459200)  // 2021-01-01 00:00:00 UTC
        , m_block_interval(300)        // 5 minutes
    {}

    uint64_t getCurrentBlockHeight() const override {
        return m_chain_oracle->getBlockHeight();
    }

    uint64_t blockHeightToTimestamp(uint64_t height) const override {
        return m_genesis_time + (height * m_block_interval);
    }

    uint64_t timestampToBlockHeight(uint64_t timestamp) const override {
        if (timestamp < m_genesis_time) return 0;
        return (timestamp - m_genesis_time) / m_block_interval;
    }

    uint64_t getCurrentTimestamp() const override {
        return blockHeightToTimestamp(getCurrentBlockHeight());
    }

private:
    const IChainOracle* m_chain_oracle;  // NOT owned
    const uint64_t m_genesis_time;
    const uint64_t m_block_interval;
};

} // namespace lightning
