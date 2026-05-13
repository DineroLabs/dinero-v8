#pragma once

#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include <vector>
#include <string>
#include <optional>
#include <cstdint>

// Ring 2 Phase 4: Validation Trace (State Transition History)
// Purpose: Complete record of validation state transitions
// Pattern: Follows Ring 4 trace design (actions → events → snapshots)

namespace dinero::consensus::test {

// ============================================================================
// ValidationEvent - Record of a single validation event
// ============================================================================

enum class ValidationEventType {
    BLOCK_CONNECTED,      // Block successfully connected to chain
    BLOCK_DISCONNECTED,   // Block disconnected during reorg
    TX_VALIDATED,         // Transaction validated
    UTXO_ADDED,          // UTXO added to set
    UTXO_SPENT           // UTXO spent (removed from set)
};

struct ValidationEvent {
    ValidationEventType type;
    uint64_t timestamp{0};
    uint64_t sequence_number{0};

    // Event-specific data
    std::optional<Block> block;
    std::optional<Transaction> transaction;
    std::optional<OutPoint> outpoint;
    std::optional<UTXOEntry> coin;

    bool success{true};
    std::string error_message;

    ValidationEvent() = default;

    ValidationEvent(ValidationEventType t)
        : type(t), timestamp(0), sequence_number(0) {}
};

// ============================================================================
// ValidationState - Snapshot of validation state
// ============================================================================

struct ValidationState {
    uint32_t height{0};
    std::string best_block_hash;
    uint64_t total_utxos{0};
    uint64_t total_value{0};  // Sum of all UTXO values

    // State hash for determinism checking
    uint64_t state_hash{0};

    ValidationState() = default;

    // Compute simple hash of state
    uint64_t computeHash() const {
        uint64_t hash = height;
        hash ^= total_utxos;
        hash ^= total_value;
        return hash;
    }

    void updateHash() {
        state_hash = computeHash();
    }
};

// ============================================================================
// ValidationTrace - Complete validation execution history
// ============================================================================

struct ValidationTrace {
    // Configuration
    uint64_t rng_seed{0};
    std::string scenario_name;

    // Execution history
    std::vector<ValidationEvent> events;
    std::vector<ValidationState> snapshots;

    // Determinism verification
    uint64_t final_hash{0};

    // Metadata
    bool completed_successfully{false};
    std::optional<std::string> failure_reason;

    ValidationTrace() = default;

    explicit ValidationTrace(uint64_t seed, std::string name = "")
        : rng_seed(seed), scenario_name(std::move(name)) {}

    // Compute hash of entire trace
    uint64_t computeHash() const {
        uint64_t hash = rng_seed;

        // Hash events
        for (const auto& event : events) {
            hash ^= static_cast<uint64_t>(event.type);
            hash ^= event.timestamp;
            hash ^= event.sequence_number;
        }

        // Hash final state
        if (!snapshots.empty()) {
            hash ^= snapshots.back().state_hash;
        }

        return hash;
    }

    void updateHash() {
        final_hash = computeHash();
    }

    bool operator==(const ValidationTrace& other) const {
        return rng_seed == other.rng_seed &&
               events.size() == other.events.size() &&
               snapshots.size() == other.snapshots.size();
    }

    bool hasSameHashAs(const ValidationTrace& other) const {
        return final_hash == other.final_hash;
    }
};

} // namespace dinero::consensus::test
