#pragma once

#include "extension_types.h"
#include <map>
#include <set>
#include <vector>

namespace dinero {
namespace governance {
namespace test {

/**
 * Opcode Namespace Manager (Ring 8b: EG1 - Namespace Isolation)
 *
 * Manages opcode namespaces to prevent collisions and ensure Ring 7 immutability.
 *
 * Key invariants:
 * - CORE namespace (Ring 7) is frozen - no modifications allowed
 * - Extension namespaces are isolated from CORE
 * - No opcode collisions across namespaces
 */
class OpcodeNamespaceManager {
public:
    OpcodeNamespaceManager();

    /**
     * Register an opcode in a namespace
     * Returns: true if registered, false if collision detected
     */
    bool registerOpcode(OpcodeNamespace ns, uint8_t opcode, const std::string& name);

    /**
     * Check if opcode is registered in namespace
     */
    bool isOpcodeRegistered(OpcodeNamespace ns, uint8_t opcode) const;

    /**
     * Get opcode name in namespace
     */
    std::optional<std::string> getOpcodeName(OpcodeNamespace ns, uint8_t opcode) const;

    /**
     * Get all opcodes in namespace
     */
    std::set<uint8_t> getNamespaceOpcodes(OpcodeNamespace ns) const;

    /**
     * EG1 Property Check: Namespace Isolation
     * Returns: empty vector if isolated, violations if not
     */
    std::vector<IsolationViolation> checkNamespaceIsolation() const;

    /**
     * EG1 Property Check: CORE namespace immutability
     * Returns: true if CORE namespace is frozen (no modifications)
     */
    bool checkCoreNamespaceFrozen() const;

    /**
     * Check for opcode collisions across namespaces
     * Returns: empty vector if no collisions, violations if detected
     */
    std::vector<std::string> checkOpcodeCollisions() const;

private:
    // Namespace -> (Opcode -> Name)
    std::map<OpcodeNamespace, std::map<uint8_t, std::string>> namespaces_;

    // Track which opcodes are in CORE (Ring 7 - frozen)
    std::set<uint8_t> core_opcodes_;

    // Track namespace creation order (for collision detection)
    std::map<OpcodeNamespace, uint64_t> namespace_creation_order_;
    uint64_t next_namespace_id_;

    void initializeCoreNamespace();
};

} // namespace test
} // namespace governance
} // namespace dinero
