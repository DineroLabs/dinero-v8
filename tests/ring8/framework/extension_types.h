#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace dinero {
namespace governance {
namespace test {

//=============================================================================
// Script Version (Ring 8b: EG2 - Version Isolation)
//=============================================================================

enum class ScriptVersion : uint8_t {
    VERSION_0 = 0,      // Ring 7 (FROZEN - S1-S25)
    VERSION_1 = 1,      // Future extensions (gated)
    VERSION_2 = 2,      // Future extensions (gated)
    VERSION_3 = 3,      // Future extensions (gated)
    // ... extensible via soft forks
};

//=============================================================================
// Opcode Namespace (Ring 8b: EG1 - Namespace Isolation)
//=============================================================================

enum class OpcodeNamespace : uint8_t {
    CORE = 0,           // Ring 7 opcodes (FROZEN - BC2)
    EXTENSION_1 = 1,    // First extension namespace
    EXTENSION_2 = 2,    // Second extension namespace
    EXTENSION_3 = 3,    // Third extension namespace
    // ... extensible via soft forks
};

//=============================================================================
// Extension Status (Ring 8b: EG3 - Activation Safety)
//=============================================================================

enum class ExtensionStatus {
    PROPOSED,           // Extension proposed, not yet activated
    LOCKED_IN,          // Activation locked in, not yet active
    ACTIVATED,          // Extension active and usable
    REJECTED,           // Extension rejected by network
    EXPIRED             // Activation period expired without activation
};

//=============================================================================
// Extension Metadata
//=============================================================================

struct Extension {
    std::string name;                           // Extension name (e.g., "covenant_v2")
    std::string description;                    // Human-readable description

    // Gating parameters
    std::optional<ScriptVersion> target_version;     // Script version (if version-gated)
    std::optional<OpcodeNamespace> target_namespace; // Opcode namespace (if namespace-gated)

    // Activation parameters
    uint64_t activation_height;                 // Block height for activation
    ExtensionStatus status;                     // Current status

    // Compatibility
    std::vector<std::string> conflicts_with;    // Extensions that conflict
    std::vector<std::string> depends_on;        // Extensions required before this

    Extension(const std::string& n, const std::string& desc)
        : name(n), description(desc), activation_height(0), status(ExtensionStatus::PROPOSED) {}
};

//=============================================================================
// Extension Proposal (For testing activation safety)
//=============================================================================

struct ExtensionProposal {
    Extension extension;

    // Proposal metadata
    uint64_t proposed_at_height;                // Block height when proposed
    uint64_t activation_threshold;              // % of blocks signaling support
    uint64_t activation_window;                 // Blocks to reach threshold

    ExtensionProposal(const Extension& ext, uint64_t height)
        : extension(ext), proposed_at_height(height),
          activation_threshold(95), activation_window(2016) {}
};

//=============================================================================
// Isolation Violation (For EG1/EG2 testing)
//=============================================================================

struct IsolationViolation {
    std::string property_name;                  // EG1 or EG2
    std::string description;                    // What was violated

    // Context
    std::optional<ScriptVersion> script_version;
    std::optional<OpcodeNamespace> opcode_namespace;
    std::optional<uint8_t> opcode_byte;

    std::string details;                        // Additional details

    IsolationViolation(const std::string& prop, const std::string& desc)
        : property_name(prop), description(desc) {}
};

} // namespace test
} // namespace governance
} // namespace dinero
