// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

namespace dinero {
namespace consensus {

/**
 * @brief Validation mode for block validation (Phase 8)
 *
 * Determines how blocks are validated:
 * - STATEFUL: Uses UTXO database for input validation (default)
 * - STATELESS: Uses Utreexo proofs only (no database required)
 *
 * Phase 8: Both modes MUST accept/reject the same blocks post-activation.
 */
enum class ValidationMode {
    /**
     * Stateful validation (UTXO database)
     * - Uses UTXOIndex for input validation
     * - MAY verify proofs as sanity check (optional)
     * - Updates accumulator in parallel (for serving proofs)
     * - Default mode for full nodes
     */
    STATEFUL,

    /**
     * Stateless validation (proof-based)
     * - NO UTXO database required
     * - MUST validate every Utreexo proof
     * - Accumulator is only source of truth
     * - Requires bridge nodes for proof data
     */
    STATELESS
};

/**
 * @brief Convert ValidationMode to string for logging
 */
inline const char* ValidationModeToString(ValidationMode mode) {
    switch (mode) {
        case ValidationMode::STATEFUL:  return "STATEFUL";
        case ValidationMode::STATELESS: return "STATELESS";
        default: return "UNKNOWN";
    }
}

} // namespace consensus
} // namespace dinero
