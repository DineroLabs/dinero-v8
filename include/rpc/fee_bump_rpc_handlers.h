#pragma once

// SPDX-License-Identifier: MIT
// Phase W.4.4: Fee Bump RPC Handlers

#include "compat/jsoncpp_compat.h"
#include "primitives/uint256.h"

namespace dinero {

/**
 * @brief Get transaction inclusion status
 *
 * Returns comprehensive analysis of whether and when a transaction
 * is likely to be included in a block.
 *
 * RPC: wallet.gettxinclusion <txid>
 *
 * Arguments:
 * 1. txid (string, required) - Transaction hash
 *
 * Returns:
 * {
 *   "txid": "hash",
 *   "state": "likely|possible|stalled|blocked",
 *   "primary_reason": "none|low_feerate|...",
 *   "estimated_inclusion_prob": 0.0-1.0,
 *   "effective_feerate": sat/vB,
 *   "cutoff_feerate": sat/vB,
 *   "rbf_available": bool,
 *   "cpfp_available": bool,
 *   "suggested_bump_feerate": sat/vB (optional),
 *   "explanation": "human-readable explanation",
 *   "timestamp_ms": unix timestamp
 * }
 *
 * @param params [txid]
 * @return JSON result
 */
Json::Value wallet_gettxinclusion(
    const Json::Value& params
);

/**
 * @brief Get fee bump recommendation
 *
 * Returns actionable recommendation for bumping transaction fee
 * via RBF or CPFP, with specific fee calculations.
 *
 * RPC: wallet.gettxbumprecommendation <txid> [target_blocks]
 *
 * Arguments:
 * 1. txid (string, required) - Transaction hash
 * 2. target_blocks (number, optional, default=1) - Target confirmation time in blocks
 *
 * Returns:
 * {
 *   "txid": "hash",
 *   "strategy": "rbf|cpfp|both|wait|none",
 *   "rationale": "explanation of recommended strategy",
 *   "current_feerate": sat/vB,
 *   "target_feerate": sat/vB,
 *   "mempool_min_feerate": sat/vB,
 *   "estimated_blocks_current": blocks,
 *   "estimated_blocks_target": blocks,
 *   "rbf": {  // Optional, if RBF available
 *     "viable": bool,
 *     "original_fee": sats,
 *     "min_replacement_fee": sats,
 *     "recommended_fee": sats,
 *     "recommended_feerate": sat/vB,
 *     "additional_cost": sats,
 *     "explanation": "string"
 *   },
 *   "cpfp": {  // Optional, if CPFP available
 *     "viable": bool,
 *     "parent_txid": "hash",
 *     "output_index": index,
 *     "output_amount": sats,
 *     "parent_fee": sats,
 *     "parent_feerate": sat/vB,
 *     "recommended_child_fee": sats,
 *     "recommended_child_feerate": sat/vB,
 *     "package_feerate": sat/vB,
 *     "total_cost": sats,
 *     "explanation": "string"
 *   },
 *   "warnings": ["warning1", "warning2", ...],
 *   "timestamp_ms": unix timestamp
 * }
 *
 * @param params [txid, target_blocks]
 * @return JSON result
 */
Json::Value wallet_gettxbumprecommendation(
    const Json::Value& params
);

/**
 * Register Phase W.4.4 RPC handlers
 */
void RegisterFeeBumpRpcHandlers();

} // namespace dinero
