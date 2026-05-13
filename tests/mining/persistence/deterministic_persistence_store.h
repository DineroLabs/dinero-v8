#pragma once

#include "../framework/mining_types.h"
#include <optional>
#include <cstdint>

// Ring 4 Phase 4g.1: Deterministic Persistence Model Foundation

namespace mining_test {

/**
 * DeterministicPersistenceStore
 *
 * In-memory, deterministic simulation of persistence.
 * Used ONLY in Phase 4g.
 *
 * Properties:
 * - Deterministic (seeded)
 * - In-memory only (no filesystem)
 * - Serializable state snapshot
 * - Supports fault injection (partial writes, corruption)
 *
 * NOT included:
 * - Real disk I/O
 * - RocksDB or SQLite
 * - OS persistence calls
 *
 * Phase 4h will replace this with real persistence.
 */
class DeterministicPersistenceStore {
public:
    explicit DeterministicPersistenceStore(uint64_t seed);

    /**
     * Persist a snapshot (atomic in the ideal case)
     */
    void persist(const MiningState& state);

    /**
     * Recover the most recent persisted snapshot
     * Returns nullopt if:
     * - No snapshot exists
     * - Snapshot is corrupted
     * - Snapshot was partially written
     */
    std::optional<MiningState> recover() const;

    /**
     * Fault injection: Simulate torn write
     * Recovery will fail after this call
     */
    void injectPartialWrite();

    /**
     * Fault injection: Simulate corrupted state
     * Recovery will fail after this call
     */
    void injectCorruption();

    /**
     * Fault injection: Simulate disk wipe
     * Clears all persisted state
     */
    void clearStore();

    /**
     * Introspection (for tests)
     */
    bool hasSnapshot() const;
    uint64_t snapshotVersion() const;

private:
    uint64_t seed_;
    uint64_t version_{0};

    std::optional<MiningState> snapshot_;
    bool partially_written_{false};
    bool corrupted_{false};
};

}  // namespace mining_test
