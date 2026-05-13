#pragma once

#include "tests/mining/framework/mining_types.h"
#include <optional>
#include <cstdint>
#include <string>
#include <memory>

// Ring 4 — Phase 4h.2: Production Persistence Skeleton
//
// STATUS: 🧱 SKELETON ONLY — NO BEHAVIOR
//
// This is a structural specification, not an implementation.
// Phase 4h.2 proves API compatibility with Phase 4g abstract model.
//
// Rules:
// - MUST mirror DeterministicPersistenceStore API exactly
// - MUST compile
// - MUST NOT persist data
// - MUST NOT include RocksDB headers
// - MUST NOT perform file I/O
//
// Implementation comes in Phase 4h.3+

namespace mining {

/**
 * PersistenceConfig
 *
 * Configuration for ProductionPersistenceStore.
 * Phase 4h.2: Fields defined but unused (skeleton only).
 */
struct PersistenceConfig {
    std::string data_directory;

    // Future flags (unused in Phase 4h.2)
    bool enable_checksums = true;
    bool conservative_recovery = true;
};

/**
 * ProductionPersistenceStore
 *
 * Production-grade persistence backend for mining state.
 *
 * Phase 4h.2 Skeleton Contract:
 * - API mirrors DeterministicPersistenceStore exactly
 * - All methods compile but return safe defaults
 * - NO storage backend linked
 * - NO file I/O
 * - NO RocksDB includes
 *
 * Phase 4h.3+ will add:
 * - RocksDB integration
 * - Atomic write batches
 * - Conservative recovery
 * - Checksum validation
 *
 * Must satisfy: MR1-MR5 (unchanged from Phase 4g)
 */
class ProductionPersistenceStore {
public:
    // ═══════════════════════════════════════════════════════════
    // Lifecycle
    // ═══════════════════════════════════════════════════════════

    /**
     * Constructor
     *
     * Phase 4h.2: Accepts config but performs NO initialization.
     * Phase 4h.3+: Will open RocksDB, validate directory, etc.
     */
    explicit ProductionPersistenceStore(const PersistenceConfig& config);

    /**
     * Destructor
     *
     * Phase 4h.2: NO-OP
     * Phase 4h.3+: Will close RocksDB, flush pending writes
     */
    ~ProductionPersistenceStore();

    // ═══════════════════════════════════════════════════════════
    // Core Persistence API (mirrors Phase 4g abstract model)
    // ═══════════════════════════════════════════════════════════

    /**
     * Persist a mining state snapshot
     *
     * Phase 4h.2: NO-OP (does not write anything)
     * Phase 4h.3+: Will write atomic batch to RocksDB
     *
     * Required semantics (enforced by MR1-MR5):
     * - Atomic (all-or-nothing)
     * - Deterministic serialization
     * - Crash-safe
     * - No partial writes exposed
     */
    void persist(const mining_test::MiningState& state);

    /**
     * Recover the most recent persisted snapshot
     *
     * Phase 4h.2: Always returns std::nullopt
     * Phase 4h.3+: Will read from RocksDB, validate checksum
     *
     * Returns nullopt if:
     * - No snapshot exists
     * - Snapshot is corrupted
     * - Snapshot was partially written
     * - Recovery validation fails
     *
     * Conservative recovery (MR3/MR4): Never returns partial state
     */
    std::optional<mining_test::MiningState> recover() const;

    // ═══════════════════════════════════════════════════════════
    // Introspection (required by MR property tests)
    // ═══════════════════════════════════════════════════════════

    /**
     * Check if a valid snapshot exists
     *
     * Phase 4h.2: Always returns false
     * Phase 4h.3+: Checks RocksDB for valid snapshot key
     */
    bool hasSnapshot() const;

    /**
     * Get snapshot version (monotonic counter)
     *
     * Phase 4h.2: Always returns 0
     * Phase 4h.3+: Returns persisted checkpoint version
     */
    uint64_t snapshotVersion() const;

    // ═══════════════════════════════════════════════════════════
    // Fault Injection (NO-OP in production, required by MR tests)
    // ═══════════════════════════════════════════════════════════

    /**
     * Simulate torn write (testing only)
     *
     * Phase 4h.2: NO-OP
     * Phase 4h.3+: NO-OP (production has no fault injection)
     *
     * Note: These methods exist for API compatibility with
     * DeterministicPersistenceStore. Production code never calls them.
     * MR tests may call them but expect no effect.
     */
    void injectPartialWrite();

    /**
     * Simulate corruption (testing only)
     *
     * Phase 4h.2: NO-OP
     * Phase 4h.3+: NO-OP (production has no fault injection)
     */
    void injectCorruption();

    /**
     * Clear all persisted state (testing only)
     *
     * Phase 4h.2: NO-OP
     * Phase 4h.3+: NO-OP or controlled wipe for test environments
     */
    void clearStore();

private:
    // ═══════════════════════════════════════════════════════════
    // Internal Helpers (no implementation in Phase 4h.2)
    // ═══════════════════════════════════════════════════════════

    /**
     * Validate state before persist
     * Phase 4h.3+: Checks consensus invariants
     */
    void validateState(const mining_test::MiningState& state) const;

    /**
     * Ensure deterministic serialization
     * Phase 4h.3+: Enforces canonical field ordering
     */
    void ensureDeterministic() const;

private:
    // ═══════════════════════════════════════════════════════════
    // Private State (opaque pImpl pattern)
    // ═══════════════════════════════════════════════════════════

    /**
     * Phase 4h.2: Empty struct
     * Phase 4h.3+: Will contain RocksDB instance, config, version counter
     */
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mining
