#pragma once
/**
 * Shielded pool consensus validation.
 *
 * THE BOUNDARY RULE (non-negotiable, enforced here):
 *
 *   Shielded outputs → CommitmentTree ONLY (never Utreexo)
 *   Transparent outputs → Utreexo ONLY (never CommitmentTree)
 *   Nullifiers → NullifierSet ONLY
 *
 * This file is the single place where the two state domains interact.
 * Block validation calls ValidateShieldedBundle() for every v5 tx.
 * The function checks:
 *
 *   1. All nullifiers are unique (not in NullifierSet, not duplicated in block)
 *   2. All spend anchors reference a recent commitment tree root
 *   3. All ZK proofs verify
 *   4. Value balance is consistent with transparent inputs/outputs
 *   5. Binding signature is valid
 *
 * If validation passes, the block connector:
 *   - Inserts nullifiers into NullifierSet
 *   - Appends commitments to CommitmentTree
 *   - Handles transparent side via Utreexo (existing code, unchanged)
 */

#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/shielded/shielded_tx.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dinero { struct Transaction; }

namespace dinero::consensus::shielded {

// ─────────────────────────────────────────────────────────────────────
// Supply-integrity rules:
//
// Each spend and output value is constrained to 64 bits by its shielded
// circuit and is represented by a secp256k1 Pedersen value commitment.
// Per-cv Borromean range proofs reject negative/out-of-range commitments.
// A BIP340 binding signature proves the cross-bundle conservation equation
// between spend cvs, output cvs, and `value_balance`. At and above the
// public-input-binding activation height these checks are mandatory and
// generator initialization failure is rejected rather than skipped.
//
// At and above the cv-binding activation height, the shielded circuit also
// constrains each published cv to the same private note value used by the
// note commitment. Earlier proof versions remain only for historical block
// validation; mainnet resets the legacy shielded epoch at the cv-binding
// activation boundary.
// ─────────────────────────────────────────────────────────────────────

enum class ShieldedValidationError : uint8_t {
    Ok                    = 0,
    NullifierDuplicate    = 1,   ///< nullifier already spent
    AnchorInvalid         = 2,   ///< spend anchor not a recent tree root
    ProofInvalid          = 3,   ///< ZK proof failed verification
    ValueBalanceMismatch  = 4,   ///< transparent + shielded don't balance
    BindingSigInvalid     = 5,   ///< binding signature check failed
    BundleMalformed       = 6,   ///< structural issues (empty proof, etc.)
    NotActive             = 7,   ///< shielded pool not yet active at this height
    BundleTooLarge        = 8,   ///< exceeds kMaxSpendsPerBundle / kMaxOutputsPerBundle
    RangeProofInvalid     = 9,   ///< per-cv range proof failed (Phase 3 wave 1B)
};

/**
 * Context passed to bundle validation — the consensus state at the
 * point the block is being connected.
 *
 * Every consensus input is constructor-mandatory. Aggregate init with a
 * too-short brace list (or default construction) is a compile error
 * rather than silently defaulting trailing fields. See the
 * `ShieldedValidationContext` regression test for the bug that made
 * this enforcement load-bearing: a missing `tx_sighash` on the reindex
 * caller defaulted to `Hash{}`, the binding-signature verifier rejected
 * every post-activation shielded block during replay, and the fleet
 * fragmented because reindex and live ConnectTip disagreed on validity.
 *
 * Tests that need a pre-activation / structural-only configuration use
 * `ValidationContext::ForPreActivationTests(...)` — explicitly named so
 * reviewers see at the call site that wrap-attack-guard / anchor checks
 * are NOT exercised there.
 */
struct ValidationContext {
    const NullifierSet*     nullifier_set;    ///< current nullifier set
    const CommitmentTree*   commitment_tree;  ///< current tree (for anchor check)
    uint32_t                block_height;
    int64_t                 transparent_value_delta;  ///< transparent_in - transparent_out - fee

    /// Phase 1 activation gate. Block heights below this value reject
    /// every non-empty shielded bundle with NotActive. The caller (block
    /// validation) supplies this from chainparams.shielded_activation_height.
    uint32_t                shielded_activation_height;

    /// Phase 2 anchor depth window. When set, spends may reference any
    /// recent root recorded here (Sapling-style 100-block window) in
    /// addition to the current commitment-tree root. When nullptr, the
    /// validator falls back to exact-current-root match.
    const AnchorHistory*    anchor_history;

    /// Phase 3 wave 2: custom fixed-layout digest of the transparent envelope
    /// this bundle rides in (not a BIP143 input sighash). Bound into the
    /// binding-sig sighash to defeat wrap-attacks. Production callers MUST supply the real
    /// sighash via `ComputeShieldedTxSighash(tx)`. Passing all-zeros
    /// disables wrap-attack protection (intended for unit tests that
    /// exercise pre-activation / structural code paths only — use
    /// `ForPreActivationTests` to make that intent explicit).
    Hash                    tx_sighash;

    /// CONFIRMED-CRIT-05: blocks at/above this height verify shielded proofs with the
    /// public-input-bound rule; below it, the pre-fix unbound rule. Defaulted member
    /// (NOT a constructor parameter) so existing construction sites are unaffected;
    /// consensus callers set it via BuildShieldedValidationContext from chainparams.
    /// Default 0 = "always bind" (the secure rule) — matches the prover's bind=true
    /// default, so direct-construction unit tests exercise the bound rule.
    uint32_t                shielded_input_binding_activation_height = 0;

    /// Audit Critical #1: blocks at/above this height require cv-bound shielded
    /// proofs (circuit enforces cv == val·V + rcv·G; version bytes 0x03/0x04).
    /// Below it, legacy proofs (0x01/0x02) verify under the old circuit so the
    /// pre-activation notes stay spendable. Defaulted member (NOT a ctor param)
    /// — default UINT32_MAX = "never cv-bind", so direct-construction unit tests
    /// (which build legacy proofs) are unaffected. Consensus callers set it via
    /// BuildShieldedValidationContext from chainparams.
    uint32_t                shielded_cv_binding_activation_height = UINT32_MAX;

    /// Recipient-only spend-authority proof rule (version 0x05). At/above this
    /// height spends must prove knowledge of the diversified recipient scalar.
    uint32_t                shielded_spend_auth_activation_height = UINT32_MAX;

    constexpr ValidationContext(
        const NullifierSet*    nullifier_set,
        const CommitmentTree*  commitment_tree,
        uint32_t               block_height,
        int64_t                transparent_value_delta,
        uint32_t               shielded_activation_height,
        const AnchorHistory*   anchor_history,
        Hash                   tx_sighash)
      : nullifier_set(nullifier_set),
        commitment_tree(commitment_tree),
        block_height(block_height),
        transparent_value_delta(transparent_value_delta),
        shielded_activation_height(shielded_activation_height),
        anchor_history(anchor_history),
        tx_sighash(tx_sighash) {}

    ValidationContext() = delete;

    /// Test-only factory: explicit shape for unit tests that exercise
    /// pre-activation / structural-only code paths and intentionally do
    /// not need wrap-attack-guard sighash or anchor-history. Callers
    /// of this factory acknowledge in source that binding-sig and
    /// anchor checks are NOT exercised. Activation height is set to
    /// UINT32_MAX so any non-empty bundle returns NotActive.
    static constexpr ValidationContext ForPreActivationTests(
        const NullifierSet* nullifier_set,
        const CommitmentTree* commitment_tree,
        uint32_t block_height,
        int64_t transparent_value_delta) {
        return ValidationContext(
            nullifier_set,
            commitment_tree,
            block_height,
            transparent_value_delta,
            /*shielded_activation_height=*/UINT32_MAX,
            /*anchor_history=*/nullptr,
            /*tx_sighash=*/Hash{});
    }
};

/**
 * Validate a shielded bundle within a v5 transaction.
 *
 * Pure function over the bundle + validation context. Does NOT mutate
 * any state — the caller (block connector) applies state changes only
 * after the full block validates.
 */
ShieldedValidationError ValidateShieldedBundle(
    const ShieldedBundle&    bundle,
    const ValidationContext& ctx);

/**
 * Build a production ValidationContext from the transaction + the
 * caller's consensus state.
 *
 * Single source of truth for context construction. Both live ConnectTip
 * (BlockValidator) and the reindex replay path call this helper rather
 * than hand-rolling the field list themselves. tx_sighash is computed
 * here from `tx` so no caller can forget it.
 *
 * Equivalence between live validation and reindex replay then reduces
 * to "they call the same helper" — the failure mode that caused the
 * Apr 30 fleet split (reindex passed Hash{} as tx_sighash while live
 * passed the real value) cannot recur as a missing-field bug.
 *
 * Pass `anchor_history = nullptr` for mempool-style validation that
 * only matches against the current commitment-tree root.
 */
ValidationContext BuildShieldedValidationContext(
    const ::dinero::Transaction& tx,
    const NullifierSet*          nullifier_set,
    const CommitmentTree*        commitment_tree,
    uint32_t                     block_height,
    int64_t                      transparent_value_delta,
    uint32_t                     shielded_activation_height,
    const AnchorHistory*         anchor_history,
    // CONFIRMED-CRIT-05: from chainparams.shielded_input_binding_activation_height.
    // Default UINT32_MAX (never bind) is a safe fallback for any caller that has not
    // yet been updated — such a caller would use the old rule, never silently accept a
    // forgery under the new rule. Real consensus callers pass the chainparams value.
    uint32_t                     shielded_input_binding_activation_height = UINT32_MAX,
    // Audit Critical #1: from chainparams.shielded_cv_binding_activation_height.
    // Default UINT32_MAX (never cv-bind) keeps un-updated callers on the legacy
    // circuit; real consensus callers pass the chainparams value.
    uint32_t                     shielded_cv_binding_activation_height = UINT32_MAX,
    uint32_t                     shielded_spend_auth_activation_height = UINT32_MAX);

// (Phase 2's ComputeBindingTag — SHA-256 structural tag — was replaced
// by the Phase 3 wave 2 Schnorr binding signature. See binding_sig.h
// for the new contract.)

/**
 * Apply a validated shielded bundle to consensus state.
 *
 * Called ONLY after ValidateShieldedBundle returns Ok for the entire block.
 *
 * Mutations:
 *   - Appends each ShieldedOutput::commitment to the commitment tree
 *   - Inserts each ShieldedSpend::nullifier into the nullifier set
 *
 * Does NOT touch Utreexo. The transparent side of the same tx is
 * handled by the existing block-connect path (Utreexo add/remove).
 */
// ─── CONTRACT ──────────────────────────────────────────────────────────────
//
// Returns false if the bundle is refused. The caller MUST abort the block
// rather than connect it with shielded state that is missing a spend.
//
// "false" guarantees the COMMITMENT TREE is untouched. It does NOT guarantee
// the nullifier set is untouched. Be precise about which:
//
//   validation refusal (already-spent, or a nullifier repeated inside the
//   bundle) — nothing at all was mutated. This is the ordinary refusal path
//   and it is fully atomic.
//
//   I/O failure during insertion — nullifiers are inserted sequentially, so a
//   failure on the Nth insert can leave the first N-1 rows written even though
//   this returns false. The tree is still untouched, because insertion runs
//   BEFORE any append precisely so that this is true.
//
// The residue is fail-SAFE, not fail-open: a surplus nullifier row can only
// refuse a spend, never admit one, so it cannot inflate supply. It can
// temporarily reject a VALID spend until startup rebuilds the set from
// ChainDB, which is the authoritative store.
//
// KNOWN LIMITATION, recorded rather than hidden: this violates a strict
// "false means no mutation" contract under SQLite failure. The durable fix is
// a transactional batch-nullifier API — begin, insert the whole batch, append
// the validated commitments, commit, with an in-memory tree undo available if
// the commit fails — instead of sequential standalone inserts. Until that
// exists, a caller that cannot tolerate the residue must supply its own undo
// transaction; do not use this function directly in such a context.
// Fault-injection coverage for the first/intermediate/final insert failures is
// a tracked follow-up and is NOT yet present.
bool ApplyShieldedBundle(const ShieldedBundle& bundle,
                         CommitmentTree*       tree,
                         NullifierSet*         nullifiers,
                         uint32_t              block_height);

} // namespace dinero::consensus::shielded
