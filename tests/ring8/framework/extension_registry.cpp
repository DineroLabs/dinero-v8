#include "extension_registry.h"
#include <algorithm>

namespace dinero {
namespace governance {
namespace test {

bool ExtensionRegistry::registerExtension(const Extension& ext) {
    // Check if already registered
    if (extensions_.find(ext.name) != extensions_.end()) {
        return false;
    }

    // Validate proposal
    auto violations = validateProposal(ext);
    if (!violations.empty()) {
        return false;
    }

    // Check for conflicts
    if (hasConflict(ext)) {
        return false;
    }

    // Register the extension
    extensions_.insert({ext.name, ext});

    // Track version/namespace assignments
    if (ext.target_version.has_value()) {
        version_assignments_[ext.target_version.value()] = ext.name;
    }
    if (ext.target_namespace.has_value()) {
        namespace_assignments_[ext.target_namespace.value()] = ext.name;
    }

    return true;
}

bool ExtensionRegistry::activateExtension(const std::string& name, uint64_t height) {
    auto it = extensions_.find(name);
    if (it == extensions_.end()) {
        return false;  // Extension not registered
    }

    Extension& ext = it->second;

    // Check dependencies
    if (!dependenciesSatisfied(ext)) {
        return false;
    }

    // Check for conflicts
    if (hasConflict(ext)) {
        return false;
    }

    // Activate the extension
    ext.status = ExtensionStatus::ACTIVATED;
    ext.activation_height = height;

    return true;
}

bool ExtensionRegistry::isActive(const std::string& name, uint64_t height) const {
    auto it = extensions_.find(name);
    if (it == extensions_.end()) {
        return false;
    }

    const Extension& ext = it->second;
    return ext.status == ExtensionStatus::ACTIVATED &&
           height >= ext.activation_height;
}

std::optional<ExtensionStatus> ExtensionRegistry::getStatus(const std::string& name) const {
    auto it = extensions_.find(name);
    if (it == extensions_.end()) {
        return std::nullopt;
    }
    return it->second.status;
}

std::optional<Extension> ExtensionRegistry::getExtension(const std::string& name) const {
    auto it = extensions_.find(name);
    if (it == extensions_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<Extension> ExtensionRegistry::listExtensions(ExtensionStatus status) const {
    std::vector<Extension> result;
    for (const auto& [name, ext] : extensions_) {
        if (ext.status == status) {
            result.push_back(ext);
        }
    }
    return result;
}

std::vector<std::string> ExtensionRegistry::validateProposal(const Extension& ext) const {
    std::vector<std::string> violations;

    // EG1: Check namespace isolation
    if (ext.target_namespace.has_value()) {
        if (ext.target_namespace.value() == OpcodeNamespace::CORE) {
            violations.push_back("EG1 violation: Cannot modify CORE namespace (Ring 7 frozen)");
        }

        // Check if namespace already assigned
        auto ns_it = namespace_assignments_.find(ext.target_namespace.value());
        if (ns_it != namespace_assignments_.end() && ns_it->second != ext.name) {
            violations.push_back("EG1 violation: Namespace already assigned to " + ns_it->second);
        }
    }

    // EG2: Check version isolation
    if (ext.target_version.has_value()) {
        if (ext.target_version.value() == ScriptVersion::VERSION_0) {
            violations.push_back("EG2 violation: Cannot modify VERSION_0 (Ring 7 frozen)");
        }

        // Check if version already assigned
        auto ver_it = version_assignments_.find(ext.target_version.value());
        if (ver_it != version_assignments_.end() && ver_it->second != ext.name) {
            violations.push_back("EG2 violation: Version already assigned to " + ver_it->second);
        }
    }

    // EG3: Check activation safety (extension must have gating)
    if (!ext.target_version.has_value() && !ext.target_namespace.has_value()) {
        violations.push_back("EG3 violation: Extension must be version-gated or namespace-gated");
    }

    return violations;
}

bool ExtensionRegistry::checkNoImplicitActivation() const {
    // All extensions must have explicit gating (version or namespace)
    for (const auto& [name, ext] : extensions_) {
        if (!ext.target_version.has_value() && !ext.target_namespace.has_value()) {
            return false;  // Implicit activation detected
        }
    }
    return true;
}

std::vector<std::string> ExtensionRegistry::checkActivationConflicts() const {
    std::vector<std::string> conflicts;

    // Check for conflicting extensions that are both activated
    for (const auto& [name1, ext1] : extensions_) {
        if (ext1.status != ExtensionStatus::ACTIVATED) {
            continue;
        }

        for (const std::string& conflict_name : ext1.conflicts_with) {
            auto it = extensions_.find(conflict_name);
            if (it != extensions_.end() && it->second.status == ExtensionStatus::ACTIVATED) {
                conflicts.push_back(name1 + " conflicts with " + conflict_name);
            }
        }
    }

    return conflicts;
}

bool ExtensionRegistry::hasConflict(const Extension& ext) const {
    // Check if any conflicting extension is already activated
    for (const std::string& conflict_name : ext.conflicts_with) {
        auto it = extensions_.find(conflict_name);
        if (it != extensions_.end() && it->second.status == ExtensionStatus::ACTIVATED) {
            return true;
        }
    }

    // Check if we conflict with any activated extension
    for (const auto& [name, existing] : extensions_) {
        if (existing.status != ExtensionStatus::ACTIVATED) {
            continue;
        }

        auto it = std::find(existing.conflicts_with.begin(),
                           existing.conflicts_with.end(),
                           ext.name);
        if (it != existing.conflicts_with.end()) {
            return true;
        }
    }

    return false;
}

bool ExtensionRegistry::dependenciesSatisfied(const Extension& ext) const {
    // Check if all dependencies are activated
    for (const std::string& dep_name : ext.depends_on) {
        auto it = extensions_.find(dep_name);
        if (it == extensions_.end() || it->second.status != ExtensionStatus::ACTIVATED) {
            return false;  // Dependency not satisfied
        }
    }
    return true;
}

} // namespace test
} // namespace governance
} // namespace dinero
