#pragma once

// ============================================================================
// CONSENSUS LAYER - STATELESS BLOCK VERIFICATION
// ============================================================================
//
// Phase 2.2: Stateless Block Verification
//
// PURE FUNCTION - NO SIDE EFFECTS
//
// This function is:
//   ✓ const-correct
//   ✓ no heap allocation (in hot path)
//   ✓ no globals
//   ✓ no logging
//   ✓ no timing dependency
//
// If this function is pure, then:
//   - iOS wallets can verify miners
//   - Browsers can verify servers
//   - Hardware wallets can verify payments
//   - RPC becomes optional
//
// This is Dinero's killer feature.
//
// ============================================================================

#include "consensus/utxo_snapshot_state.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include "primitives/block.h"
#include "primitives/uint256.h"
#include <cstdint>

namespace dinero {
namespace consensus {

// ============================================================================
// Verification Error Codes (no strings in hot path)
// ============================================================================

enum class VerifyError : uint8_t {
    OK = 0,
    INVALID_PROOF,           // Utreexo proof verification failed
    MISSING_INPUT,           // Input references non-existent UTXO
    DOUBLE_SPEND,            // Same UTXO spent twice in block
    COINBASE_IMMATURE,       // Spending immature coinbase
    INVALID_AMOUNT,          // Output value exceeds input
    INVALID_COINBASE,        // Invalid coinbase structure
    ROOT_MISMATCH,           // Computed root != header root
    INVALID_HEADER,          // Invalid block header
    OVERSIZED_BLOCK,         // Block exceeds size limit
};

// ============================================================================
// Stateless Verification Result (POD - no heap)
// ============================================================================

struct VerifyResult {
    VerifyError error;
    uint64_t fees;              // Total fees if valid
    uint32_t outputs_created;   // New UTXOs created
    uint32_t inputs_spent;      // UTXOs spent

    constexpr bool valid() const { return error == VerifyError::OK; }

    static constexpr VerifyResult Ok(uint64_t f, uint32_t out, uint32_t in) {
        return {VerifyError::OK, f, out, in};
    }

    static constexpr VerifyResult Fail(VerifyError e) {
        return {e, 0, 0, 0};
    }
};

// ============================================================================
// Minimal UTXO Context (for light clients)
// ============================================================================

/**
 * StatelessContext - Minimal data needed for stateless verification
 *
 * Light clients receive this alongside the block. Contains:
 * - Spent output data (value + scriptPubKey for each input)
 * - Utreexo roots before block
 * - Chain height
 *
 * This is a VIEW - does not own data, just references.
 */
struct StatelessContext {
    // Spent outputs data (parallel to block inputs)
    // Order: tx0.input0, tx0.input1, ..., tx1.input0, ...
    const SpentOutputData* spent_outputs;
    uint32_t spent_count;

    // Utreexo state before block
    const UtreexoHash* roots;
    uint8_t num_roots;
    uint64_t num_leaves;

    // Chain state
    uint32_t height;

    // Constants
    static constexpr uint32_t COINBASE_MATURITY = 100;
};

// ============================================================================
// PURE STATELESS VERIFICATION FUNCTION
// ============================================================================

/**
 * VerifyBlockStateless - Pure stateless block verification
 *
 * INVARIANTS (enforced by design):
 *   - const-correct: all parameters are const
 *   - no heap allocation: result is POD, no strings
 *   - no globals: no static state accessed
 *   - no logging: returns error code only
 *   - no timing dependency: deterministic execution
 *
 * WHAT IT VERIFIES:
 *   1. Utreexo proof validity (all spent inputs exist)
 *   2. No double-spends within block
 *   3. Coinbase maturity (100 blocks)
 *   4. Input/output value balance (outputs ≤ inputs + subsidy)
 *   5. Computed Utreexo root matches header commitment
 *
 * WHAT IT DOES NOT VERIFY (left to caller):
 *   - Script execution (expensive, optional for light clients)
 *   - Proof-of-work (separate concern)
 *   - Block size/weight limits (network policy)
 *
 * @param block The block to verify
 * @param ctx Stateless context (spent outputs, roots, height)
 * @param proof Utreexo proof for spent inputs
 * @return VerifyResult with error code and stats
 */
[[nodiscard]]
VerifyResult VerifyBlockStateless(
    const Block& block,
    const StatelessContext& ctx,
    const BlockUtreexoProof& proof) noexcept;

// ============================================================================
// Simplified API with snapshot (convenience, allocates)
// ============================================================================

/**
 * VerifyBlockWithSnapshot - Convenience wrapper using full snapshot
 *
 * NOTE: This allocates (builds context from snapshot).
 * For hot paths, use VerifyBlockStateless directly.
 *
 * @param block The block to verify
 * @param snapshot Full UTXO snapshot before block
 * @param proof Utreexo proof
 * @return VerifyResult
 */
VerifyResult VerifyBlockWithSnapshot(
    const Block& block,
    const UTXOSnapshot& snapshot,
    const BlockUtreexoProof& proof);

// ============================================================================
// Helper: Verify Utreexo proof only
// ============================================================================

/**
 * VerifyUtreexoProofStateless - Verify just the proof
 *
 * Quick check to reject invalid proofs before full verification.
 * Pure function, no allocations.
 *
 * @param proof The proof to verify
 * @param targets Leaf hashes being proven
 * @param roots Expected forest roots
 * @param num_roots Number of roots
 * @param num_leaves Total leaves in forest
 * @return true if proof is valid
 */
[[nodiscard]]
bool VerifyUtreexoProofStateless(
    const BlockUtreexoProof& proof,
    const UtreexoHash* targets,
    uint32_t target_count,
    const UtreexoHash* roots,
    uint8_t num_roots,
    uint64_t num_leaves) noexcept;

// ============================================================================
// Helper: Compute leaf hash (pure)
// ============================================================================

/**
 * ComputeUTXOLeafHash - Compute Utreexo leaf hash for a UTXO
 *
 * leaf = SHA256(txid || vout || value || scriptPubKey)
 *
 * Pure function.
 */
UtreexoHash ComputeUTXOLeafHash(
    const uint256& txid,
    uint32_t vout,
    uint64_t value,
    const uint8_t* script,
    uint32_t script_len) noexcept;

// ============================================================================
// Helper: Get block subsidy (pure)
// ============================================================================

/**
 * GetBlockSubsidy - Compute block subsidy at height
 *
 * Dinero consensus rules (Fair Launch v3 — no premine):
 *   Height 0: 0 (genesis unspendable, OP_RETURN)
 *   Height 1+: 100 DIN initial, halving every 1,314,000 blocks
 *   Tail emission: max(halving_subsidy, 1 DIN) forever
 *
 * Pure function, no allocations.
 */
[[nodiscard]]
constexpr uint64_t GetBlockSubsidy(uint32_t height) noexcept {
    constexpr uint64_t UNA_PER_DIN = 100'000'000ULL;
    constexpr uint64_t INITIAL_SUBSIDY = 100ULL * UNA_PER_DIN;
    constexpr uint32_t HALVING_INTERVAL = 1'314'000;
    constexpr uint64_t TAIL_EMISSION_UNA = 1ULL * UNA_PER_DIN;

    if (height == 0) return 0;

    // PoW emission starts at height 1
    uint32_t pow_blocks = height - 1;
    uint32_t halvings = pow_blocks / HALVING_INTERVAL;

    // Compute halving subsidy (shifts to 0 after 64 halvings)
    uint64_t subsidy = (halvings >= 64) ? 0 : (INITIAL_SUBSIDY >> halvings);

    // Tail emission floor: never pay less than 1 DIN
    return (subsidy > TAIL_EMISSION_UNA) ? subsidy : TAIL_EMISSION_UNA;
}

} // namespace consensus
} // namespace dinero
