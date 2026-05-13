#pragma once

#include "../framework/mining_trace.h"
#include "../persistence/deterministic_persistence_store.h"
#include <string>
#include <vector>
#include <cstdint>

// Ring 4 Phase 4g.2: Persistence Oracle Base Class

namespace mining_test {

/**
 * PersistenceViolation
 *
 * Describes a detected persistence property violation.
 */
struct PersistenceViolation {
    std::string property;      // Property name (e.g., "MR1", "MR2")
    std::string message;       // Human-readable description
    uint64_t event_index{0};   // Event index where violation occurred
};

/**
 * MiningPersistenceOracle
 *
 * Base class for all persistence property checkers (MR1-MR5).
 *
 * Design pattern (mirrors Phase 4f MiningDeterminismOracle):
 * - Each property has its own oracle subclass
 * - Oracle observes trace + persistence store
 * - Returns violations (empty = property holds)
 *
 * Properties checked:
 * - MR1: State Survives Restart Correctly
 * - MR2: No State Duplication After Crash
 * - MR3: Partial Persistence Recovers Safely
 * - MR4: Restart Converges to Valid State
 * - MR5: Persistence Does Not Break Determinism
 *
 * Implementation strategy:
 * - Pure virtual interface
 * - No logic in base class
 * - All property-specific logic in MR1-MR5 implementations
 */
class MiningPersistenceOracle {
public:
    virtual ~MiningPersistenceOracle() = default;

    /**
     * Human-readable property name
     * Example: "MR1: State Survives Restart Correctly"
     */
    virtual std::string name() const = 0;

    /**
     * Check persistence property
     *
     * Observes:
     * - Full mining trace (pre-crash + post-restart)
     * - Persistence store state
     *
     * Returns:
     * - Empty vector if property holds
     * - Non-empty vector of violations if property violated
     *
     * Note: Oracle may mutate store (e.g., inject faults)
     */
    virtual std::vector<PersistenceViolation> check(
        const MiningTrace& trace,
        DeterministicPersistenceStore& store
    ) const = 0;

protected:
    /**
     * Helper: construct violation
     */
    PersistenceViolation violation(
        const std::string& property,
        const std::string& message,
        uint64_t event_index
    ) const {
        return PersistenceViolation{property, message, event_index};
    }
};

}  // namespace mining_test
