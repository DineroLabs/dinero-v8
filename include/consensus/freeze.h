/**
 * Phase D.4: Consensus Freeze Header
 *
 * CRITICAL CONSENSUS LOCK: This file establishes the consensus version and freeze status.
 * Once consensus is frozen, ANY changes to consensus rules require:
 * 1. Hard fork planning
 * 2. Network coordination
 * 3. Version bump with explicit changelog
 *
 * Philosophy:
 * - Consensus rules are IMMUTABLE after freeze
 * - Breaking changes require explicit version increment
 * - Compile-time guards prevent accidental modifications
 * - Runtime verification ensures integrity
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DINERO_CONSENSUS_FREEZE_H
#define DINERO_CONSENSUS_FREEZE_H

#include <cstdint>
#include <string>
#include <array>

namespace dinero {
namespace consensus {

//==============================================================================
// CONSENSUS VERSION
//==============================================================================

/**
 * CONSENSUS_VERSION: Semantic version for consensus rules
 *
 * Format: MAJOR.MINOR.PATCH
 * - MAJOR: Hard fork (incompatible consensus changes)
 * - MINOR: Soft fork (backward-compatible restrictions)
 * - PATCH: Bug fixes that don't change validation
 *
 * Current: 1.0.0 (Initial consensus freeze after Phase D)
 */
constexpr uint32_t CONSENSUS_VERSION_MAJOR = 1;
constexpr uint32_t CONSENSUS_VERSION_MINOR = 0;
constexpr uint32_t CONSENSUS_VERSION_PATCH = 0;

// Compile-time lock: Prevent version changes without explicit intent
static_assert(CONSENSUS_VERSION_MAJOR == 1,
    "🔒 CONSENSUS VERSION CHANGE: Hard fork detected! "
    "Update docs/consensus_map.md with migration plan.");

static_assert(CONSENSUS_VERSION_MINOR == 0,
    "🔒 CONSENSUS VERSION CHANGE: Soft fork detected! "
    "Document backward-compatibility in docs/consensus_map.md.");

static_assert(CONSENSUS_VERSION_PATCH == 0,
    "🔒 CONSENSUS VERSION CHANGE: Patch detected! "
    "Verify no validation logic changed.");

/**
 * Get consensus version as string (e.g., "1.0.0")
 */
inline std::string GetConsensusVersion() {
    return std::to_string(CONSENSUS_VERSION_MAJOR) + "." +
           std::to_string(CONSENSUS_VERSION_MINOR) + "." +
           std::to_string(CONSENSUS_VERSION_PATCH);
}

//==============================================================================
// CONSENSUS FREEZE STATUS
//==============================================================================

/**
 * CONSENSUS_FROZEN: Flag indicating consensus is locked
 *
 * When true:
 * - All consensus rules are immutable
 * - Changes require hard/soft fork
 * - Tests and fuzzers enforce invariants
 * - Runtime verification is enabled
 *
 * When false (pre-freeze):
 * - Development mode, consensus may evolve
 * - Tests may be adjusted
 * - No network deployment yet
 */
constexpr bool CONSENSUS_FROZEN = true;

// Compile-time lock: Prevent unfreezing without explicit intent
static_assert(CONSENSUS_FROZEN == true,
    "🔒 CONSENSUS UNFREEZE DETECTED: "
    "This should ONLY happen during controlled hard fork development! "
    "Document the reason in docs/consensus_map.md.");

/**
 * CONSENSUS_FREEZE_DATE: When consensus was frozen (ISO 8601)
 */
constexpr const char* CONSENSUS_FREEZE_DATE = "2025-12-30";

/**
 * CONSENSUS_FREEZE_PHASE: Which phase completed the freeze
 */
constexpr const char* CONSENSUS_FREEZE_PHASE = "Phase D.4";

//==============================================================================
// CONSENSUS INTEGRITY MANIFEST
//==============================================================================

/**
 * Critical consensus files and their expected SHA256 checksums
 *
 * This manifest allows runtime verification that consensus code hasn't been
 * tampered with or accidentally modified.
 *
 * Format: { filename, expected_sha256_hex }
 *
 * CRITICAL: Update this manifest whenever consensus version changes!
 */
struct ConsensusFileChecksum {
    const char* filename;
    const char* sha256_hex;
};

/**
 * Consensus integrity manifest
 *
 * SHA256 checksums computed for consensus-critical files.
 * These are verified at runtime to detect tampering or corruption.
 *
 * NOTE: freeze.h uses SELF_REFERENCE marker because a file cannot
 * contain its own correct hash (circular dependency).
 */
constexpr std::array<ConsensusFileChecksum, 4> CONSENSUS_MANIFEST = {{
    // Phase D.1 files
    {"include/consensus/limits.h",          "70b37036ab23f7c83ebf5e756bb6303aa90fdf3cdcb91dba4a2111fa276cd7d2"},
    {"include/consensus/tx_validation.h",   "b8ef985b850e1133901208da70c1c770e4dcb9ce97a4c62d941542ca6f03c865"},
    {"include/consensus/subsidy.h",         "3463567f22140b6cd58d23e39ebb75c21b4b4d3d92d67feadf04f149e37f1f90"},

    // Phase D.4 file (self-reference - cannot contain own hash)
    {"include/consensus/freeze.h",          "SELF_REFERENCE"},
}};

//==============================================================================
// RUNTIME VERIFICATION
//==============================================================================

/**
 * Verify consensus integrity at runtime
 *
 * Checks:
 * 1. All consensus files exist
 * 2. SHA256 checksums match expected values (if not placeholder)
 * 3. No unauthorized modifications
 *
 * Returns:
 * - true: Consensus integrity verified
 * - false: Consensus corruption detected (critical error!)
 *
 * Usage:
 *   Should be called at daemon startup to ensure consensus code integrity.
 */
bool VerifyConsensusIntegrity(std::string& error_msg);

/**
 * Get human-readable consensus freeze status report
 *
 * Returns a multi-line string with:
 * - Consensus version
 * - Freeze status
 * - Freeze date
 * - File integrity status
 */
std::string GetConsensusFreezeReport();

//==============================================================================
// COMPILE-TIME SANITY CHECKS
//==============================================================================

// Verify freeze.h is consistent with other consensus modules
static_assert(CONSENSUS_VERSION_MAJOR >= 1,
    "Consensus version must be at least 1.0.0 after Phase D");

static_assert(CONSENSUS_MANIFEST.size() >= 4,
    "Consensus manifest must track all critical files");

//==============================================================================
// PHASE D COMPLETION MARKER
//==============================================================================

/**
 * PHASE_D_COMPLETE: Marker that all Phase D tasks finished
 *
 * Phase D checklist:
 * ✅ D.1.a: Consensus surface mapping
 * ✅ D.1.b: Extraction and naming
 * ✅ D.1.c: Compile-time guards
 * ✅ D.2:   Invariant tests (correctness)
 * ✅ D.3:   Safety fuzzers
 * ✅ D.4:   Consensus freeze + documentation
 */
constexpr bool PHASE_D_COMPLETE = true;

static_assert(PHASE_D_COMPLETE == true,
    "Phase D completion marker must not be reverted");

} // namespace consensus
} // namespace dinero

#endif // DINERO_CONSENSUS_FREEZE_H
