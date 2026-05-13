#pragma once

#include "extension_types.h"
#include <map>
#include <set>
#include <memory>

namespace dinero {
namespace governance {
namespace test {

/**
 * Extension Registry (Ring 8b: EG3 - Activation Safety)
 *
 * Tracks registered extensions and their activation status.
 * Ensures extensions activate only when explicitly gated.
 *
 * Key invariants:
 * - No implicit activation (all extensions require explicit gating)
 * - No conflicting extensions can activate simultaneously
 * - Dependencies must be satisfied before activation
 */
class ExtensionRegistry {
public:
    ExtensionRegistry() = default;

    /**
     * Register a new extension proposal
     * Returns: true if registered, false if conflicts detected
     */
    bool registerExtension(const Extension& ext);

    /**
     * Activate an extension at given height
     * Returns: true if activated, false if not ready/conflicts
     */
    bool activateExtension(const std::string& name, uint64_t height);

    /**
     * Check if extension is active at given height
     */
    bool isActive(const std::string& name, uint64_t height) const;

    /**
     * Get extension status
     */
    std::optional<ExtensionStatus> getStatus(const std::string& name) const;

    /**
     * Get extension by name
     */
    std::optional<Extension> getExtension(const std::string& name) const;

    /**
     * List all extensions with given status
     */
    std::vector<Extension> listExtensions(ExtensionStatus status) const;

    /**
     * Validate extension proposal (check conflicts, dependencies)
     * Returns: empty vector if valid, violations if invalid
     */
    std::vector<std::string> validateProposal(const Extension& ext) const;

    /**
     * EG3 Property Check: No implicit activation
     * Returns: true if all extensions have explicit activation gates
     */
    bool checkNoImplicitActivation() const;

    /**
     * EG3 Property Check: No activation conflicts
     * Returns: empty vector if valid, conflicts if detected
     */
    std::vector<std::string> checkActivationConflicts() const;

private:
    // Extension name -> Extension data
    std::map<std::string, Extension> extensions_;

    // Track version assignments
    std::map<ScriptVersion, std::string> version_assignments_;

    // Track namespace assignments
    std::map<OpcodeNamespace, std::string> namespace_assignments_;

    bool hasConflict(const Extension& ext) const;
    bool dependenciesSatisfied(const Extension& ext) const;
};

} // namespace test
} // namespace governance
} // namespace dinero
