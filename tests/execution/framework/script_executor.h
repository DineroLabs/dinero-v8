#pragma once

#include "execution_trace.h"
#include "execution_types.h"
#include <vector>
#include <cstdint>
#include <optional>
#include <memory>

namespace dinero {
namespace execution {
namespace test {

/**
 * ScriptExecutor
 *
 * Simplified script interpreter with trace recording.
 * NOT production interpreter - only for Ring 7 semantic verification.
 *
 * Design:
 * - Executes basic opcodes (PUSH, DUP, ADD, etc.)
 * - Records every operation in execution trace
 * - Observable-facts-only (no internal state exposure)
 * - Deterministic execution
 *
 * Phase 7a: Basic opcodes only
 * Phase 7b: Add Taproot support
 * Phase 7c: Add covenant support
 */
class ScriptExecutor {
public:
    ScriptExecutor();
    ~ScriptExecutor();

    /**
     * Execute script with witness, record trace.
     *
     * @param script Script to execute
     * @param witness Witness stack
     * @param trace Output trace (will be populated)
     * @return true if execution succeeded
     */
    bool execute(
        const std::vector<uint8_t>& script,
        const WitnessStack& witness,
        ExecutionTrace& trace
    );

    /**
     * Execute with Taproot path.
     * Phase 7b implementation.
     */
    bool executeWithTaproot(
        const std::vector<uint8_t>& script,
        const WitnessStack& witness,
        const TaprootPath& taproot_path,
        ExecutionTrace& trace
    );

    /**
     * Execute with covenant constraints.
     * Phase 7c implementation.
     */
    bool executeWithCovenant(
        const std::vector<uint8_t>& script,
        const WitnessStack& witness,
        const CovenantSpec& covenant,
        ExecutionTrace& trace
    );

    /**
     * Reset executor state.
     */
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl;

    // Internal execution helpers (not exposed)
    bool executeOpcode(uint8_t opcode, std::vector<StackElement>& stack,
                       ExecutionTrace& trace, uint64_t step);

    void recordStackSnapshot(const std::vector<StackElement>& stack,
                             ExecutionTrace& trace, uint64_t step);
};

} // namespace test
} // namespace execution
} // namespace dinero
