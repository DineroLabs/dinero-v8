#pragma once

#include "extension_types.h"
#include "extension_registry.h"
#include "../../execution/framework/execution_types.h"
#include "../../execution/framework/execution_simulator.h"
#include <memory>

namespace dinero {
namespace governance {
namespace test {

/**
 * Script Version Dispatcher (Ring 8b: EG2 - Version Isolation)
 *
 * Routes script execution to version-specific executors.
 * Ensures strict version isolation - v0 scripts cannot access v1+ features.
 *
 * Key invariants:
 * - VERSION_0 (Ring 7) always uses frozen Ring 7 executor
 * - VERSION_1+ routes to extension-aware executors
 * - No cross-version feature access
 */
class ScriptVersionDispatcher {
public:
    ScriptVersionDispatcher(std::shared_ptr<ExtensionRegistry> registry);

    /**
     * Execute script with version isolation
     *
     * Returns: ExecutionTrace with version context
     * Throws: if version isolation is violated
     */
    execution::test::ExecutionTrace executeScript(
        ScriptVersion version,
        const std::vector<uint8_t>& script,
        const execution::test::WitnessStack& witness,
        uint64_t block_height,
        const std::string& scenario_name = "version_dispatch"
    );

    /**
     * EG2 Property Check: Version Isolation
     * Returns: empty vector if isolated, violations if not
     */
    std::vector<IsolationViolation> checkVersionIsolation(
        ScriptVersion version,
        const std::vector<uint8_t>& script
    ) const;

    /**
     * Check if script version is valid/activated
     */
    bool isVersionActive(ScriptVersion version, uint64_t block_height) const;

    /**
     * Get available opcodes for script version
     * VERSION_0 gets only Ring 7 opcodes, VERSION_1+ get extension opcodes
     */
    std::set<uint8_t> getAvailableOpcodes(ScriptVersion version) const;

private:
    std::shared_ptr<ExtensionRegistry> registry_;

    // Ring 7 executor (VERSION_0 only)
    std::unique_ptr<execution::test::ExecutionSimulator> ring7_executor_;

    // Check if opcode is allowed in script version
    bool isOpcodeAllowed(ScriptVersion version, uint8_t opcode) const;

    // Get Ring 7 (frozen) opcodes
    static std::set<uint8_t> getRing7Opcodes();
};

} // namespace test
} // namespace governance
} // namespace dinero
