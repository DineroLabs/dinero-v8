#include "script_version_dispatcher.h"
#include <stdexcept>

namespace dinero {
namespace governance {
namespace test {

ScriptVersionDispatcher::ScriptVersionDispatcher(std::shared_ptr<ExtensionRegistry> registry)
    : registry_(registry)
{
    // Create Ring 7 executor (frozen, VERSION_0 only)
    ring7_executor_ = std::make_unique<execution::test::ExecutionSimulator>(42);
}

execution::test::ExecutionTrace ScriptVersionDispatcher::executeScript(
    ScriptVersion version,
    const std::vector<uint8_t>& script,
    const execution::test::WitnessStack& witness,
    uint64_t block_height,
    const std::string& scenario_name)
{
    // EG2: Check version isolation BEFORE execution
    auto violations = checkVersionIsolation(version, script);
    if (!violations.empty()) {
        throw std::runtime_error("EG2 violation: " + violations[0].description);
    }

    // VERSION_0: Use Ring 7 executor (frozen)
    if (version == ScriptVersion::VERSION_0) {
        return ring7_executor_->executeScript(script, witness, scenario_name);
    }

    // VERSION_1+: Check if version is activated
    if (!isVersionActive(version, block_height)) {
        throw std::runtime_error("Script version not activated at height " +
                               std::to_string(block_height));
    }

    // For testing Phase 8b, extension executors would go here
    // For now, just validate opcodes are in allowed set
    auto allowed_opcodes = getAvailableOpcodes(version);
    for (uint8_t opcode : script) {
        if (allowed_opcodes.find(opcode) == allowed_opcodes.end()) {
            throw std::runtime_error("Opcode 0x" + std::to_string(opcode) +
                                   " not allowed in version " + std::to_string(static_cast<int>(version)));
        }
    }

    // Execute with Ring 7 executor (extension executors not implemented yet)
    // This is a placeholder for Phase 8b testing
    return ring7_executor_->executeScript(script, witness, scenario_name);
}

std::vector<IsolationViolation> ScriptVersionDispatcher::checkVersionIsolation(
    ScriptVersion version,
    const std::vector<uint8_t>& script) const
{
    std::vector<IsolationViolation> violations;

    // Get allowed opcodes for this version
    auto allowed_opcodes = getAvailableOpcodes(version);

    // Check each opcode in script
    for (uint8_t opcode : script) {
        if (allowed_opcodes.find(opcode) == allowed_opcodes.end()) {
            IsolationViolation v("EG2", "Opcode not allowed in script version");
            v.script_version = version;
            v.opcode_byte = opcode;
            v.details = "Opcode 0x" + std::to_string(opcode) +
                       " not in allowed set for version " +
                       std::to_string(static_cast<int>(version));
            violations.push_back(v);
        }
    }

    return violations;
}

bool ScriptVersionDispatcher::isVersionActive(ScriptVersion version, uint64_t block_height) const {
    // VERSION_0 is always active (Ring 7)
    if (version == ScriptVersion::VERSION_0) {
        return true;
    }

    // VERSION_1+: Check if extension is activated
    // For Phase 8b, we check registry for version activation
    // This is a simplified check - real implementation would query registry
    return false;  // VERSION_1+ not activated yet in Phase 8b
}

std::set<uint8_t> ScriptVersionDispatcher::getAvailableOpcodes(ScriptVersion version) const {
    if (version == ScriptVersion::VERSION_0) {
        return getRing7Opcodes();
    }

    // VERSION_1+: Ring 7 opcodes + extension opcodes
    // For Phase 8b testing, VERSION_1+ get no additional opcodes yet
    return getRing7Opcodes();
}

bool ScriptVersionDispatcher::isOpcodeAllowed(ScriptVersion version, uint8_t opcode) const {
    auto allowed = getAvailableOpcodes(version);
    return allowed.find(opcode) != allowed.end();
}

std::set<uint8_t> ScriptVersionDispatcher::getRing7Opcodes() {
    // Ring 7 (VERSION_0) frozen opcodes
    // These correspond to Ring 7 S1-S25 properties
    return {
        0x51,  // OP_1
        0x52,  // OP_2
        0x53,  // OP_3
        0x54,  // OP_4
        0x55,  // OP_5
        0x56,  // OP_6
        0x57,  // OP_7
        0x58,  // OP_8
        0x59,  // OP_9
        0x5a,  // OP_10
        0x76,  // OP_DUP
        0x87,  // OP_EQUAL
        0x88,  // OP_EQUALVERIFY
        0x93,  // OP_ADD
        0x94,  // OP_SUB
        0xac,  // OP_CHECKSIG
        0xad,  // OP_CHECKSIGVERIFY
        // Additional Ring 7 opcodes...
    };
}

} // namespace test
} // namespace governance
} // namespace dinero
