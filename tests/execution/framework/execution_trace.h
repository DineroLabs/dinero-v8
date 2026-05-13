#pragma once

#include "execution_types.h"
#include <vector>
#include <string>
#include <cstdint>
#include <optional>

namespace dinero {
namespace execution {
namespace test {

/**
 * ExecutionTrace
 *
 * Observable-facts-only execution record.
 * Captures everything that happened during script/Taproot/covenant execution,
 * enabling semantic verification via oracles.
 *
 * Design principles:
 * 1. Observable facts only - no internal interpreter state
 * 2. Deterministic - same inputs → same trace
 * 3. Complete - sufficient for semantic verification
 * 4. Immutable - trace is append-only during execution
 */
struct ExecutionTrace {
    //=========================================================================
    // Input (What we're executing)
    //=========================================================================

    uint64_t rng_seed;                          // For deterministic generation
    std::string scenario_name;                  // Human-readable test name

    std::vector<uint8_t> script;                // Script being executed
    WitnessStack witness;                       // Witness data
    std::optional<TaprootPath> taproot_path;    // Taproot path (if any)
    std::optional<CovenantSpec> covenant;       // Covenant spec (if any)

    //=========================================================================
    // Execution (What happened)
    //=========================================================================

    std::vector<Operation> operations;          // Opcode-level trace
    std::vector<StackSnapshot> stack_states;    // Stack at each step
    std::vector<PathActivation> path_reveals;   // Taproot path revelations
    std::vector<ExecutionEvent> events;         // High-level events

    //=========================================================================
    // Output (Result)
    //=========================================================================

    bool success;                               // Did execution succeed?
    std::optional<std::string> error;           // Error message (if failed)
    ExecutionState final_state;                 // Final execution state

    //=========================================================================
    // Determinism Verification
    //=========================================================================

    uint64_t final_hash;                        // Hash of entire trace
    uint64_t operation_count;                   // Total operations executed
    uint64_t stack_depth_max;                   // Maximum stack depth

    //=========================================================================
    // Metadata
    //=========================================================================

    uint64_t execution_time_us;                 // Execution duration (microseconds)
    uint64_t timestamp_ms;                      // When execution occurred

    //=========================================================================
    // Constructors
    //=========================================================================

    ExecutionTrace()
        : rng_seed(0)
        , success(false)
        , final_hash(0)
        , operation_count(0)
        , stack_depth_max(0)
        , execution_time_us(0)
        , timestamp_ms(0)
    {}

    explicit ExecutionTrace(uint64_t seed, const std::string& name)
        : rng_seed(seed)
        , scenario_name(name)
        , success(false)
        , final_hash(0)
        , operation_count(0)
        , stack_depth_max(0)
        , execution_time_us(0)
        , timestamp_ms(0)
    {}

    //=========================================================================
    // Trace Recording (Append-only)
    //=========================================================================

    void recordOperation(const Operation& op) {
        operations.push_back(op);
        operation_count++;
    }

    void recordStackState(const StackSnapshot& snapshot) {
        stack_states.push_back(snapshot);
        if (snapshot.depth > stack_depth_max) {
            stack_depth_max = snapshot.depth;
        }
    }

    void recordPathReveal(const PathActivation& activation) {
        path_reveals.push_back(activation);
    }

    void recordEvent(const ExecutionEvent& event) {
        events.push_back(event);
    }

    //=========================================================================
    // Determinism Verification
    //=========================================================================

    /**
     * Compute deterministic hash of entire trace.
     * Same inputs → same hash.
     */
    uint64_t computeHash() const;

    /**
     * Verify trace is well-formed.
     * Returns true if trace passes basic consistency checks.
     */
    bool isWellFormed() const;

    //=========================================================================
    // Query Helpers
    //=========================================================================

    size_t getOperationCount() const { return operations.size(); }
    size_t getStackSnapshotCount() const { return stack_states.size(); }
    size_t getPathRevealCount() const { return path_reveals.size(); }
    size_t getEventCount() const { return events.size(); }

    bool usedTaproot() const { return taproot_path.has_value(); }
    bool usedCovenant() const { return covenant.has_value(); }

    bool wasSuccessful() const { return success; }
    bool hasFailed() const { return !success && error.has_value(); }

    //=========================================================================
    // Comparison (for determinism testing)
    //=========================================================================

    /**
     * Check if two traces are semantically equivalent.
     * Used for S1 (Script Determinism) verification.
     */
    bool isEquivalentTo(const ExecutionTrace& other) const;

    /**
     * Check if two traces represent different execution paths.
     * Used for S2 (No Alternate Witness Equivalence) verification.
     */
    bool hasDifferentPathFrom(const ExecutionTrace& other) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
