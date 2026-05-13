#pragma once
#include <cstdint>
#include "consensus/chainparams.h"  // For Chain enum and GetActiveChain()
#include "consensus/features.h"     // For CONSENSUS_UTREEXO_PHASE4_DELTA_UNDO

namespace dinero {
namespace consensus {

// ═════════════════════════════════════════════════════════════════════════════
// Utreexo Activation Heights (Network-Specific)
// ═════════════════════════════════════════════════════════════════════════════
// These heights define when Utreexo proof enforcement becomes MANDATORY.
// Post-activation blocks that violate Utreexo rules will be REJECTED.
// ═════════════════════════════════════════════════════════════════════════════

// Regtest: Active from genesis (height 0)
// Regtest is for testing - Utreexo is mandatory from the start
constexpr uint32_t UTREEXO_ACTIVATION_HEIGHT_REGTEST = 0;

// Testnet: Active from height 0 (same as mainnet)
// Allows testing activation from genesis
constexpr uint32_t UTREEXO_ACTIVATION_HEIGHT_TESTNET = 0;

// Mainnet: Active from height 0 (true genesis-era activation)
// Block 0 = genesis (Utreexo root = v2 empty forest commitment, NOT all zeros)
// Block 1 = first PoW block (Utreexo root = v2 commitment of single-leaf forest)
// Block 2+ = normal blocks (Utreexo enforced)
//
// Genesis-era activation rationale:
// - New chain, no legacy compatibility needed
// - Utreexo commitment v2 active from the very first block
// - v2 commitment: SHA256(numLeaves_LE64 || 64×32-byte root slots)
constexpr uint32_t UTREEXO_ACTIVATION_HEIGHT_MAINNET = 0;

// ═════════════════════════════════════════════════════════════════════════════
// Layer 3: Irreversibility - Compile-Time Activation Enforcement
// ═════════════════════════════════════════════════════════════════════════════
// These assertions prevent consensus violations and configuration errors.
// ═════════════════════════════════════════════════════════════════════════════

// CRITICAL: Utreexo activation REQUIRES Phase 4 delta-based undo
// This is the ONLY gate. If Phase 4 is enabled, any activation height is safe.
static_assert(CONSENSUS_UTREEXO_PHASE4_DELTA_UNDO,
    "CONSENSUS VIOLATION: Utreexo activation requires Phase 4 delta-based undo. "
    "Enable CONSENSUS_UTREEXO_PHASE4_DELTA_UNDO in consensus/features.h before "
    "setting any activation height below 999999999.");

// SAFETY: Regtest must activate immediately (height 0) for testing
static_assert(UTREEXO_ACTIVATION_HEIGHT_REGTEST == 0,
    "SAFETY VIOLATION: Regtest must activate Utreexo from genesis for testing");

// Mainnet: Utreexo active from genesis (height 0)
// Genesis utreexo_root = v2 empty forest commitment (non-zero)
// Block 1 utreexo_root = v2 commitment of single-leaf forest
static_assert(UTREEXO_ACTIVATION_HEIGHT_MAINNET == 0,
    "CONSENSUS: Mainnet Utreexo must activate from genesis (height 0)");

// SAFETY: Testnet should match mainnet for realistic testing
static_assert(UTREEXO_ACTIVATION_HEIGHT_TESTNET == UTREEXO_ACTIVATION_HEIGHT_MAINNET,
    "CONFIGURATION: Testnet should match mainnet activation height for realistic testing");

// All networks activate Utreexo from genesis (height 0)
static_assert(UTREEXO_ACTIVATION_HEIGHT_REGTEST == UTREEXO_ACTIVATION_HEIGHT_MAINNET,
    "CONSENSUS: All networks should activate Utreexo from genesis for consistency");

/**
 * @brief Utreexo activation rule (Consensus Enforcement)
 *
 * Determines if Utreexo proof enforcement is active at given height.
 *
 * This function provides:
 * - Explicit activation abstraction
 * - Network-specific activation logic
 * - Auditable enforcement rules
 * - Testability (can simulate pre/post-Utreexo behavior)
 *
 * Consensus behavior:
 * - If active: Blocks MUST carry Utreexo data (enforced by ConnectBlock)
 * - If inactive: Utreexo data optional (backward compatibility)
 *
 * @param height Block height
 * @return true if Utreexo proofs must be enforced at this height
 */
inline bool IsUtreexoActive(uint32_t height) {
    // Get current network from global chain params
    Chain chain = GetActiveChain();

    // Network-specific activation logic
    switch (chain) {
        case Chain::REGTEST:
            // Regtest: Active from genesis (immediate activation)
            return height >= UTREEXO_ACTIVATION_HEIGHT_REGTEST;

        case Chain::TESTNET:
            // Testnet: Active from height 0 (matches mainnet)
            return height >= UTREEXO_ACTIVATION_HEIGHT_TESTNET;

        case Chain::MAINNET:
            // Mainnet: Active from height 0 (genesis-era activation)
            return height >= UTREEXO_ACTIVATION_HEIGHT_MAINNET;
    }

    // Defensive: Treat unknown networks as inactive
    return false;
}

/**
 * @brief Get Utreexo activation height for current network
 *
 * Returns the block height at which Utreexo becomes mandatory.
 * Useful for:
 * - UI display ("Utreexo activates at height X")
 * - RPC info commands
 * - Testing and validation
 *
 * @return Activation height for current network
 */
inline uint32_t GetUtreexoActivationHeight() {
    Chain chain = GetActiveChain();

    switch (chain) {
        case Chain::REGTEST:  return UTREEXO_ACTIVATION_HEIGHT_REGTEST;
        case Chain::TESTNET:  return UTREEXO_ACTIVATION_HEIGHT_TESTNET;
        case Chain::MAINNET:  return UTREEXO_ACTIVATION_HEIGHT_MAINNET;
    }

    // Defensive: Return max height for unknown networks
    return 999999999;
}

// ═════════════════════════════════════════════════════════════════════════════
// Full Rules Activation (Utreexo + Witness Commitment)
// ═════════════════════════════════════════════════════════════════════════════
// Both features activate at the same height for simplicity.
// This is the SINGLE SOURCE OF TRUTH for "are full consensus rules active?"
// ═════════════════════════════════════════════════════════════════════════════

// Full rules activation height (same as Utreexo)
constexpr uint32_t FULL_RULES_ACTIVATION_HEIGHT_REGTEST = UTREEXO_ACTIVATION_HEIGHT_REGTEST;  // 0
constexpr uint32_t FULL_RULES_ACTIVATION_HEIGHT_TESTNET = UTREEXO_ACTIVATION_HEIGHT_TESTNET;  // 0
constexpr uint32_t FULL_RULES_ACTIVATION_HEIGHT_MAINNET = UTREEXO_ACTIVATION_HEIGHT_MAINNET;  // 0

/**
 * @brief Check if full consensus rules are active at given height
 *
 * Full rules include:
 * - Utreexo accumulator enforcement
 * - Witness commitment enforcement (for blocks with witness txs)
 *
 * This is the EXPLICIT gate for all full-rules consensus features.
 * Use this instead of checking individual feature activations.
 *
 * @param height Block height
 * @return true if full rules (Utreexo + witness commitment) are enforced
 */
inline bool FullRulesActive(uint32_t height) {
    // Same as IsUtreexoActive - both features activate together
    return IsUtreexoActive(height);
}

/**
 * @brief Get full rules activation height for current network
 *
 * @return Activation height for current network
 */
inline uint32_t GetFullRulesActivationHeight() {
    return GetUtreexoActivationHeight();
}

/**
 * @brief Check if this is a pre-activation block
 *
 * With activation height 0 on all networks, this always returns false.
 * Kept for API compatibility — no blocks are pre-activation.
 *
 * @param height Block height
 * @return true if height < activation height (always false with height 0 activation)
 */
inline bool IsPreActivationBlock(uint32_t height) {
    return height < GetFullRulesActivationHeight();
}

} // namespace consensus
} // namespace dinero
