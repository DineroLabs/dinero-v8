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

// ═══════════════════════════════════════════════════════════════
// Phase 11b.1: Witness Merkle Groundwork (NOT CONSENSUS-ACTIVE)
// ═══════════════════════════════════════════════════════════════
// ⚠️  CRITICAL: This function is GROUNDWORK ONLY.
//
// Status: NOT consensus-active, NOT referenced by mining/validation
//
// Purpose:
//   - Prepare infrastructure for future witness commitment support
//   - Lock invariants before activation
//   - Zero call sites until explicit consensus upgrade
//
// What this computes:
//   - Merkle tree of witness transaction IDs (wtxids)
//   - Coinbase witness hash = 0x00...00 (Bitcoin convention)
//   - Uses full transaction data (witness included)
//
// ❌ DO NOT use for:
//   - Block header validation
//   - BlockHeader::merkle_root comparison
//   - Mining block templates
//   - Consensus checks
//
// ✅ Future use (when activated):
//   - Segwit witness commitment (in coinbase)
//   - Fraud proof generation
//   - Light client verification
//
// Invariants locked by:
//   - tests/consensus/test_witness_merkle_isolation.cpp
//
// Activation requires:
//   - Explicit consensus upgrade (Phase 11c+)
//   - Network versioning
//   - Softfork deployment
// ═══════════════════════════════════════════════════════════════
/**
 * Compute witness merkle root from transaction vector (GROUNDWORK ONLY)
 *
 * ⚠️ NOT CONSENSUS-ACTIVE - do not use for validation or mining
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
 *   - ComputeWitnessMerkleRoot() uses wtxid (witness hash) ← NOT active
 *
 * @param vtx Vector of transactions
 * @return Witness merkle root as uint256 (zero hash if empty)
 */
uint256 ComputeWitnessMerkleRoot(const std::vector<Transaction>& vtx);

} // namespace dinero::consensus
