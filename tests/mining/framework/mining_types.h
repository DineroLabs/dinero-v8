#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Ring 4 Phase 4b: Core Mining Test Framework Abstractions
// Purpose: Pure data containers for mining property testing
// Rule: NO mining correctness logic, NO consensus code

namespace mining_test {

// ============================================================================
// MiningActionType - External events that affect mining
// ============================================================================

enum class MiningActionType {
    START_MINING,             // Begin mining
    STOP_MINING,              // Stop mining
    NEW_BLOCK_ARRIVED,        // Chain tip changed
    TX_ADDED_TO_MEMPOOL,      // Mempool updated
    TX_REMOVED_FROM_MEMPOOL,  // Transaction removed
    TIME_ADVANCED,            // Mock clock tick
    CRASH,                    // Simulate crash
    RESTART,                  // Simulate restart
    REORG                     // Chain reorganization
};

// ============================================================================
// MiningAction - Input event to simulator
// ============================================================================

struct MiningAction {
    MiningActionType type;
    uint64_t timestamp{0};  // Mock time when action occurred

    // Action-specific data (optional)
    std::optional<uint64_t> block_hash;      // For NEW_BLOCK_ARRIVED, REORG
    std::optional<uint64_t> tx_hash;         // For TX_ADDED, TX_REMOVED
    std::optional<uint32_t> reorg_depth;     // For REORG
    std::optional<uint32_t> new_height;      // For NEW_BLOCK_ARRIVED

    // Metadata for replay
    uint64_t sequence_number{0};
    std::string description;

    // Default constructor
    MiningAction() = default;

    // Helper constructor
    MiningAction(MiningActionType t, uint64_t ts = 0, uint64_t seq = 0)
        : type(t), timestamp(ts), sequence_number(seq) {}

    // Equality for determinism checks
    bool operator==(const MiningAction& other) const {
        return type == other.type &&
               timestamp == other.timestamp &&
               block_hash == other.block_hash &&
               tx_hash == other.tx_hash &&
               reorg_depth == other.reorg_depth &&
               new_height == other.new_height &&
               sequence_number == other.sequence_number;
    }
};

// ============================================================================
// MiningPhase - Observable mining states
// ============================================================================

enum class MiningPhase {
    STOPPED,      // No mining thread
    IDLE,         // Thread running, no work
    ASSEMBLING,   // Building block template
    MINING,       // Hashing
    SUBMITTING    // Found solution, submitting block
};

// ============================================================================
// MiningState - Observable system state snapshot
// ============================================================================

struct MiningState {
    MiningPhase phase{MiningPhase::STOPPED};
    uint64_t timestamp{0};

    // Chain state
    uint64_t current_tip{0};        // Placeholder hash
    uint32_t current_height{0};

    // Mempool state
    uint32_t mempool_size{0};
    uint64_t mempool_total_fees{0};

    // Mining state
    std::optional<uint64_t> template_prev_hash;
    std::optional<uint32_t> template_height;
    std::optional<uint64_t> template_subsidy;  // Placeholder value
    std::optional<uint32_t> template_tx_count;

    // Statistics
    uint64_t hashes_computed{0};
    uint64_t blocks_found{0};
    uint64_t templates_created{0};

    // Lifecycle tracking
    bool has_crashed{false};
    uint32_t restart_count{0};

    // Default constructor
    MiningState() = default;

    // Equality for determinism checks
    bool operator==(const MiningState& other) const {
        return phase == other.phase &&
               timestamp == other.timestamp &&
               current_tip == other.current_tip &&
               current_height == other.current_height &&
               mempool_size == other.mempool_size &&
               mempool_total_fees == other.mempool_total_fees &&
               template_prev_hash == other.template_prev_hash &&
               template_height == other.template_height &&
               template_subsidy == other.template_subsidy &&
               template_tx_count == other.template_tx_count &&
               hashes_computed == other.hashes_computed &&
               blocks_found == other.blocks_found &&
               templates_created == other.templates_created &&
               has_crashed == other.has_crashed &&
               restart_count == other.restart_count;
    }
};

// ============================================================================
// MiningEventType - Observable outcomes
// ============================================================================

enum class MiningEventType {
    TEMPLATE_CREATED,      // Block template assembled
    TEMPLATE_DISCARDED,    // Old template abandoned
    POW_STARTED,           // Started hashing
    POW_STOPPED,           // Stopped hashing
    SOLUTION_FOUND,        // Valid PoW found (simulated)
    BLOCK_SUBMITTED,       // Block submitted to network (simulated)
    BLOCK_ACCEPTED,        // Block accepted by consensus (simulated)
    BLOCK_REJECTED,        // Block rejected by consensus (simulated)
    ERROR_OCCURRED         // Error in mining pipeline
};

// ============================================================================
// MiningEvent - Output event from simulator
// ============================================================================

struct MiningEvent {
    MiningEventType type;
    uint64_t timestamp{0};

    // Event-specific data (optional)
    std::optional<uint64_t> block_hash;
    std::optional<uint32_t> template_height;
    std::optional<uint64_t> subsidy_claimed;  // Placeholder value
    std::optional<std::string> error_message;

    // Metadata
    uint64_t sequence_number{0};
    std::string description;

    // Default constructor
    MiningEvent() = default;

    // Helper constructor
    MiningEvent(MiningEventType t, uint64_t ts = 0, uint64_t seq = 0)
        : type(t), timestamp(ts), sequence_number(seq) {}

    // Equality for determinism checks
    bool operator==(const MiningEvent& other) const {
        return type == other.type &&
               timestamp == other.timestamp &&
               block_hash == other.block_hash &&
               template_height == other.template_height &&
               subsidy_claimed == other.subsidy_claimed &&
               error_message == other.error_message &&
               sequence_number == other.sequence_number;
    }
};

}  // namespace mining_test
