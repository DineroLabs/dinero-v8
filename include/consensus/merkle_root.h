#pragma once
/**
 * Phase 11a.2: Canonical Merkle Root Computation
 *
 * SINGLE SOURCE OF TRUTH for merkle tree construction.
 *
 * Invariants enforced:
 * 1. Single-TX blocks: merkle_root == txid
 * 2. Multi-TX blocks: standard Bitcoin double-SHA256 tree
 * 3. NO serialization dependency - merkle is computed from txids only
 * 4. NO endianness ambiguity - uses internal uint256 format consistently
 *
 * This is the ONLY function that should compute merkle roots.
 * All other merkle computation is deprecated and will be removed.
 */

#include <vector>
#include "primitives/transaction.h"
#include "primitives/uint256.h"

namespace dinero::consensus {

/**
 * Compute canonical merkle root from transaction vector
 *
 * Algorithm:
 * 1. Extract txid (as uint256) from each transaction
 * 2. Build merkle tree bottom-up using double-SHA256
 * 3. Return root as uint256 (internal format)
 *
 * This matches Bitcoin consensus and guarantees:
 * - merkle_root == txid for single-transaction blocks
 * - Consistent byte ordering across all paths
 * - No serialization dependency
 *
 * @param vtx Vector of transactions
 * @return Merkle root as uint256 (zero hash if empty)
 */
uint256 ComputeMerkleRoot(const std::vector<Transaction>& vtx);

/**
 * Compute the consensus witness merkle root for a DINW commitment.
 *
 * Algorithm:
 * 1. For each transaction, compute witness transaction ID (wtxid):
 *    - Coinbase: wtxid = 0x00...00 (Bitcoin convention)
 *    - Others: hash of serialization WITH witness data
 * 2. Build merkle tree bottom-up using double-SHA256
 * 3. Return root as uint256 (internal format)
 *
 * Difference from ComputeMerkleRoot():
 *   - ComputeMerkleRoot() uses txid (non-witness hash) ← consensus-active
 *   - ComputeWitnessMerkleRoot() uses wtxid (witness hash) for DINW
 *
 * Do not use this value as BlockHeader::merkle_root; the block header commits
 * to the non-witness transaction IDs returned by GetTxid().
 *
 * @param vtx Vector of transactions
 * @return Witness merkle root as uint256 (zero hash if empty)
 */
uint256 ComputeWitnessMerkleRoot(const std::vector<Transaction>& vtx);

} // namespace dinero::consensus
