#pragma once
#include <cstdint>
#include "consensus/chainparams.h"  // For Chain enum and GetActiveChain()

namespace dinero {
namespace consensus {

// ═════════════════════════════════════════════════════════════════════════════
// Utreexo Canonical-Roots Fork Activation (Apr 13 2026 — Bug #4 stage 3)
// ═════════════════════════════════════════════════════════════════════════════
// BACKGROUND
// ----------
// `UtreexoForest::recomputePath()` sets `roots_[h] = nullopt` when the sole
// leaf in an h=0 tree is deleted, even though bit h of `numLeaves_` remains
// set (Utreexo position indices never reuse). The next `add()` sees the
// empty slot, takes the "place" branch instead of "merge", and the resulting
// tree structure cascades into the state that made `proof.verify` fail on
// every covenant spend. See project_tx_soak_apr13.md for the full post-mortem.
//
// Stage 1 (`54057cfd4`): normalized confidential leaf values.
// Stage 2 (`7de08dbf4`): bypassed proof.verify in trusted internal callers
//                        (removeAtKnownPosition). Keeps user flow working.
// Stage 3 (this header):  THE ACTUAL FIX — `recomputePath()` returns canonical
//                         zero-sentinel hashes instead of nullopt, preserving
//                         the invariant `roots_[h].has_value() ⟺ bit h of
//                         numLeaves_`. Subsequent `add()` merges correctly.
//                         Stateless verification (verifyBatchProofStateless,
//                         mining.generatetoaddress self-check, CSN clients)
//                         also start working.
//
// WHY THIS IS A CONSENSUS CHANGE
// ------------------------------
// Pre-fix `getCommitment()` folds `roots_` non-null entries into the utreexo
// commitment hash. Empty trees (all leaves deleted) were filtered out; with
// the fix they contribute a canonical zero-sentinel hash. The commitment
// value therefore changes at the first block where any subtree is fully
// deleted. Any node running one version rejects blocks produced by the
// other. This is a hard fork and needs a flag day.
//
// ACTIVATION BEHAVIOR
// -------------------
// At the activation block exactly:
//   1. `UtreexoForest::setCanonicalEmptyRoots(true)` is flipped on
//   2. `UtreexoForest::rebuildRoots()` is called once to recompute `roots_`
//      from `nodes_` using the canonical zero-sentinel logic
//   3. Block N's utreexo_root header is computed with canonical behavior
// From block N+1 onward, the canonical behavior is maintained by every
// subsequent add/remove.
// ═════════════════════════════════════════════════════════════════════════════

// Mainnet: activate at block 2870 (fleet tip 2861 + 9-block buffer). The
// earlier UINT32_MAX placeholder was flipped once the single-gate
// `cloneForHeight()` architecture was validated end-to-end by
// `tests/regtest/test_canonical_roots_fork.sh` (Apr 13 2026 late evening —
// regtest mines through the activation boundary at height 10 with all
// post-fork invariants PASSing). Fleet + Mac miner will all land on the
// same binary before height 2870 is mined.
constexpr uint32_t UTREEXO_CANONICAL_ROOTS_HEIGHT_MAINNET = 2870;

// Testnet / Regtest: activate at small finite heights so regtest integration
// tests can exercise both pre-fork and post-fork semantics in a single run.
constexpr uint32_t UTREEXO_CANONICAL_ROOTS_HEIGHT_TESTNET = 0;
constexpr uint32_t UTREEXO_CANONICAL_ROOTS_HEIGHT_REGTEST = 10;

/**
 * @brief Is the canonical-roots fork active at `height`?
 */
inline bool IsUtreexoCanonicalRootsActive(uint32_t height) {
    switch (GetActiveChain()) {
        case Chain::REGTEST: return height >= UTREEXO_CANONICAL_ROOTS_HEIGHT_REGTEST;
        case Chain::TESTNET: return height >= UTREEXO_CANONICAL_ROOTS_HEIGHT_TESTNET;
        case Chain::MAINNET: return height >= UTREEXO_CANONICAL_ROOTS_HEIGHT_MAINNET;
    }
    return false;
}

/**
 * @brief Network-specific activation height.
 */
inline uint32_t GetUtreexoCanonicalRootsActivationHeight() {
    switch (GetActiveChain()) {
        case Chain::REGTEST: return UTREEXO_CANONICAL_ROOTS_HEIGHT_REGTEST;
        case Chain::TESTNET: return UTREEXO_CANONICAL_ROOTS_HEIGHT_TESTNET;
        case Chain::MAINNET: return UTREEXO_CANONICAL_ROOTS_HEIGHT_MAINNET;
    }
    return UINT32_MAX;
}

} // namespace consensus
} // namespace dinero
