#include "deterministic_persistence_store.h"

// Ring 4 Phase 4g.1: Deterministic Persistence Model Implementation

namespace mining_test {

// ============================================================================
// Constructor
// ============================================================================

DeterministicPersistenceStore::DeterministicPersistenceStore(uint64_t seed)
    : seed_(seed) {
    // Seed is stored for future use (Phase 4g.2+)
    // For now, persistence is simple and deterministic
}

// ============================================================================
// Persistence Operations
// ============================================================================

void DeterministicPersistenceStore::persist(const MiningState& state) {
    // Increment version (monotonic counter)
    version_++;

    // Store snapshot (in-memory copy)
    snapshot_ = state;

    // Clear fault flags (successful persist)
    partially_written_ = false;
    corrupted_ = false;
}

std::optional<MiningState> DeterministicPersistenceStore::recover() const {
    // No snapshot exists
    if (!snapshot_) {
        return std::nullopt;
    }

    // Snapshot is corrupted
    if (corrupted_) {
        return std::nullopt;
    }

    // Snapshot was partially written (torn write)
    if (partially_written_) {
        // Conservative recovery: drop snapshot
        // This simulates detecting torn write via checksum
        return std::nullopt;
    }

    // Clean recovery: return snapshot
    return snapshot_;
}

// ============================================================================
// Fault Injection
// ============================================================================

void DeterministicPersistenceStore::injectPartialWrite() {
    partially_written_ = true;
}

void DeterministicPersistenceStore::injectCorruption() {
    corrupted_ = true;
}

void DeterministicPersistenceStore::clearStore() {
    snapshot_.reset();
    partially_written_ = false;
    corrupted_ = false;
    // Version is NOT reset (simulates persistent metadata)
}

// ============================================================================
// Introspection
// ============================================================================

bool DeterministicPersistenceStore::hasSnapshot() const {
    return snapshot_.has_value();
}

uint64_t DeterministicPersistenceStore::snapshotVersion() const {
    return version_;
}

}  // namespace mining_test
