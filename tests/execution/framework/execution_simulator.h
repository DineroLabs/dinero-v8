#pragma once

#include "execution_trace.h"
#include "execution_types.h"
#include "script_executor.h"
#include "../../p2p/property_test_framework.h" // Reuse PropertyTestRNG
#include <memory>
#include <vector>
#include <cstdint>

namespace dinero {
namespace execution {
namespace test {

/**
 * ExecutionSimulator
 *
 * Ring 7 execution orchestrator.
 * Generates execution scenarios, runs scripts, records traces.
 *
 * Design principles (same as Ring 6):
 * 1. Deterministic - PropertyTestRNG seeding
 * 2. Observable-facts-only - traces capture what happened
 * 3. Oracle-ready - traces fed to semantic oracles
 * 4. Zero flakiness - same seed → same trace
 */
class ExecutionSimulator {
public:
    /**
     * Create simulator with RNG seed.
     */
    explicit ExecutionSimulator(uint64_t seed = 0);
    ~ExecutionSimulator();

    /**
     * Execute script and return trace.
     *
     * @param script Script bytecode
     * @param witness Witness stack
     * @param scenario_name Human-readable test name
     * @return Execution trace
     */
    ExecutionTrace executeScript(
        const std::vector<uint8_t>& script,
        const WitnessStack& witness,
        const std::string& scenario_name = "default"
    );

    /**
     * Execute with Taproot path.
     * Phase 7b.
     */
    ExecutionTrace executeWithTaproot(
        const std::vector<uint8_t>& script,
        const WitnessStack& witness,
        const TaprootPath& taproot_path,
        const std::string& scenario_name = "taproot"
    );

    /**
     * Execute with covenant constraints.
     * Phase 7c.
     */
    ExecutionTrace executeWithCovenant(
        const std::vector<uint8_t>& script,
        const WitnessStack& witness,
        const CovenantSpec& covenant,
        const std::string& scenario_name = "covenant"
    );

    /**
     * Execute with specific evaluation order.
     * Phase 7f - S21 (Evaluation Order Determinism).
     *
     * @param script Script bytecode
     * @param witness Witness stack
     * @param order Evaluation order to use
     * @param scenario_name Human-readable test name
     * @return Execution trace
     */
    ExecutionTrace executeWithOrder(
        const std::vector<uint8_t>& script,
        const WitnessStack& witness,
        EvaluationOrder order,
        const std::string& scenario_name = "order_test"
    );

    /**
     * Execute with specific execution strategy.
     * Phase 7f - S23 (Strategy Independence).
     *
     * @param script Script bytecode
     * @param witness Witness stack
     * @param strategy Execution strategy to use
     * @param scenario_name Human-readable test name
     * @return Execution trace
     */
    ExecutionTrace executeWithStrategy(
        const std::vector<uint8_t>& script,
        const WitnessStack& witness,
        ExecutionStrategy strategy,
        const std::string& scenario_name = "strategy_test"
    );

    /**
     * Reset simulator state.
     */
    void reset();

    /**
     * Get RNG seed (for determinism verification).
     */
    uint64_t getSeed() const { return seed_; }

private:
    uint64_t seed_;
    dinero::p2p::test::PropertyTestRNG rng_;
    std::unique_ptr<ScriptExecutor> executor_;

    // Internal helpers
    void initializeTrace(ExecutionTrace& trace, const std::string& scenario_name);
    void finalizeTrace(ExecutionTrace& trace);
};

/**
 * ExecutionSequenceGenerator
 *
 * Generates test scenarios for Ring 7 properties.
 * Similar to Ring 6's EconomicSequenceGenerator.
 */
class ExecutionSequenceGenerator {
public:
    struct Config {
        size_t min_opcodes;
        size_t max_opcodes;
        size_t min_witness_elements;
        size_t max_witness_elements;
        bool allow_failures;

        Config()
            : min_opcodes(1)
            , max_opcodes(20)
            , min_witness_elements(0)
            , max_witness_elements(5)
            , allow_failures(true)
        {}
    };

    explicit ExecutionSequenceGenerator(dinero::p2p::test::PropertyTestRNG& rng);
    ExecutionSequenceGenerator(dinero::p2p::test::PropertyTestRNG& rng, const Config& config);

    /**
     * Generate simple valid script execution.
     * Example: OP_1 (pushes 1, script succeeds)
     */
    ExecutionTrace generateSimpleScript();

    /**
     * Generate script with specific complexity.
     */
    ExecutionTrace generateScriptExecution(ScriptComplexity complexity);

    /**
     * Generate two scripts that should produce same result.
     * For S1 (Script Determinism) testing.
     */
    std::pair<ExecutionTrace, ExecutionTrace> generateDeterministicPair();

    /**
     * Generate two witnesses for same script that take different paths.
     * For S2 (No Alternate Witness Equivalence) testing.
     */
    std::pair<ExecutionTrace, ExecutionTrace> generateAlternateWitnessPair();

    /**
     * Generate Taproot execution scenario.
     * Phase 7b.
     */
    ExecutionTrace generateTaprootExecution();

    /**
     * Generate covenant execution scenario.
     * Phase 7c.
     */
    ExecutionTrace generateCovenantExecution();

private:
    dinero::p2p::test::PropertyTestRNG& rng_;
    Config config_;
    ExecutionSimulator simulator_;

    // Generation helpers
    std::vector<uint8_t> generateRandomScript(size_t length);
    WitnessStack generateRandomWitness(size_t elements);
    std::vector<uint8_t> generateSimpleValidScript();
};

} // namespace test
} // namespace execution
} // namespace dinero
