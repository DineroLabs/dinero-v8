/**
 * Shielded pool consensus validation + state application.
 * See include/consensus/shielded/shielded_validation.h.
 */

#include "consensus/shielded/shielded_validation.h"

#include <set>
#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/pedersen_generators.h"
#include "consensus/shielded/range_proof.h"
#include "consensus/shielded/shielded_circuit.h"

#include "crypto/evp_secp256k1.h"

#include <cstring>
#include <unordered_set>
#include <vector>

namespace dinero::consensus::shielded {

namespace {

struct HashHasher {
    size_t operator()(const Hash& h) const {
        size_t result = 0;
        for (size_t i = 0; i < 8 && i < HASH_BYTES; ++i) {
            result ^= static_cast<size_t>(h[i]) << (i * 8);
        }
        return result;
    }
};

bool VerifySpendProof(const ShieldedSpend& spend, const ValidationContext& ctx) {
    // CONFIRMED-CRIT-05: blocks at/above the binding-activation height require the
    // public-input-bound proof rule; older blocks use the pre-fix unbound rule.
    const bool bind = ctx.block_height >= ctx.shielded_input_binding_activation_height;
    // Audit Critical #1: blocks at/above the cv-binding height require cv-bound
    // proofs (circuit enforces cv == val·V + rcv·G). The spend's published cv is
    // bound as a public input.
    const bool cv_bound = ctx.block_height >= ctx.shielded_cv_binding_activation_height;
    const bool spend_auth =
        ctx.block_height >= ctx.shielded_spend_auth_activation_height;
    const SpendPublicInputs pub{spend.nullifier, spend.anchor, spend.cv};
    return VerifySpend(
        spend.zk_proof, pub, dinero::crypto::GetSecp256k1ContextSignVerify(),
        bind, cv_bound, spend_auth);
}

bool VerifyOutputProof(const ShieldedOutput& output, const ValidationContext& ctx) {
    const bool bind = ctx.block_height >= ctx.shielded_input_binding_activation_height;
    const bool cv_bound = ctx.block_height >= ctx.shielded_cv_binding_activation_height;
    const OutputPublicInputs pub{output.commitment, output.cv};
    return VerifyOutput(
        output.zk_proof, pub, dinero::crypto::GetSecp256k1ContextSignVerify(), bind, cv_bound);
}

} // namespace

ShieldedValidationError ValidateShieldedBundle(
    const ShieldedBundle&    bundle,
    const ValidationContext& ctx) {

    if (bundle.IsEmpty()) {
        return ShieldedValidationError::Ok;
    }

    // Phase 1 activation gate: reject every non-empty bundle below the
    // configured activation height. This is fail-closed — a context that
    // forgets to set `shielded_activation_height` defaults to UINT32_MAX
    // and rejects everything.
    if (ctx.block_height < ctx.shielded_activation_height) {
        return ShieldedValidationError::NotActive;
    }

    // Phase 2 size limits: bound worst-case verification cost. A single
    // bundle can carry at most kMaxSpendsPerBundle spends and
    // kMaxOutputsPerBundle outputs. Beyond that, reject outright.
    if (bundle.spends.size()  > kMaxSpendsPerBundle ||
        bundle.outputs.size() > kMaxOutputsPerBundle) {
        return ShieldedValidationError::BundleTooLarge;
    }

    // Structural checks
    for (const auto& spend : bundle.spends) {
        if (spend.zk_proof.empty()) {
            return ShieldedValidationError::BundleMalformed;
        }
    }
    for (const auto& output : bundle.outputs) {
        if (output.zk_proof.empty()) {
            return ShieldedValidationError::BundleMalformed;
        }
    }

    // 1. Nullifier uniqueness: no duplicate within this bundle, and
    //    none already in the global set.
    std::unordered_set<Hash, HashHasher> seen_nullifiers;
    for (const auto& spend : bundle.spends) {
        if (ctx.nullifier_set && ctx.nullifier_set->Contains(spend.nullifier)) {
            return ShieldedValidationError::NullifierDuplicate;
        }
        if (!seen_nullifiers.insert(spend.nullifier).second) {
            return ShieldedValidationError::NullifierDuplicate;
        }
    }

    // 2. Anchor validity: each spend's anchor must equal the current
    //    tree root OR appear in the AnchorHistory window (Phase 2,
    //    Sapling-style depth = AnchorHistory::kDepth = 100 blocks).
    //    Without an AnchorHistory plumbed in, we fall back to exact
    //    current-root match — same conservative behavior as Phase 1.
    if (ctx.commitment_tree) {
        const Hash current_root = ctx.commitment_tree->Root();
        for (const auto& spend : bundle.spends) {
            const bool current_match  = (spend.anchor == current_root);
            const bool history_match  = ctx.anchor_history &&
                                        ctx.anchor_history->Contains(spend.anchor);
            if (!current_match && !history_match) {
                return ShieldedValidationError::AnchorInvalid;
            }
        }
    }

    // 3. Phase 3 wave 1B: per-cv range proofs.
    //    Closes the negative-value attack: every cv must come with a
    //    range proof that v ∈ [0, 2^64). Skipped when the Pedersen
    //    generators haven't been initialized OR no aggregated range
    //    proof is present (test contexts that don't supply cv/proofs).
    //    The activation gate keeps shielded txs unreachable on
    //    mainnet/testnet until both this AND the binding sig below
    //    are mandatory at every site.
    // CONFIRMED-CRIT (shielded inflation): an EMPTY aggregated_range_proof must
    // NEVER bypass the range-proof check post-activation. At/above the
    // input-binding activation height (chainparams CONFIRMED-CRIT-05: the height
    // from which real shielded value exists and every legitimate bundle — built
    // by bundle_builder — carries a non-empty range proof + binding sig), a valid
    // NON-EMPTY range proof is MANDATORY: reject when absent/empty. Below that
    // height we preserve the EXACT pre-fix opportunistic behavior so no historical
    // block is retroactively rejected (consensus-safe; no real shielded value
    // existed there). PedersenGeneratorsReady() remains an operational precondition
    // for the cryptographic verify only — it does not gate the empty rejection.
    if (ctx.block_height >= ctx.shielded_input_binding_activation_height) {
        if (bundle.aggregated_range_proof.empty()) {
            return ShieldedValidationError::RangeProofInvalid;
        }
        // Consensus cryptography must fail closed. Generator initialization is
        // deterministic and mandatory; treating an unavailable generator as a
        // reason to skip verification would make block validity depend on a
        // node's runtime initialization state.
        if (!PedersenGeneratorsReady()) {
            return ShieldedValidationError::RangeProofInvalid;
        }
        const auto rc = VerifyBundleRangeProofs(bundle);
        if (rc != RangeProofResult::Ok) {
            return ShieldedValidationError::RangeProofInvalid;
        }
    } else if (PedersenGeneratorsReady() && !bundle.aggregated_range_proof.empty()) {
        const auto rc = VerifyBundleRangeProofs(bundle);
        if (rc != RangeProofResult::Ok) {
            return ShieldedValidationError::RangeProofInvalid;
        }
    }

    // 3b. Phase 3 wave 2: Schnorr binding signature.
    //     Closes the cross-bundle inflation hole. Reconstructs
    //     bvk = sum(cv_spend) - sum(cv_output) + value_balance · V
    //     and verifies a BIP340 Schnorr signature against bvk over a
    //     domain-separated sighash that includes value_balance, the
    //     transparent tx_sighash, and every cv. Any mutation of those
    //     fields yields a different bvk or sighash and verify fails.
    //
    //     Same gate logic as range proofs: skipped when generators
    //     aren't ready or when the bundle doesn't carry cvs (legacy
    //     test contexts). Once the activation height is set on a real
    //     network, every non-empty bundle will require this check —
    //     and the size-limits / structural / nullifier / anchor checks
    //     above prevent obviously-malformed bundles from reaching
    //     this far.
    // Same gate as the range proof above: bundles that supply
    // aggregated_range_proof are post-Wave-1B+2 bundles and must carry
    // a valid Schnorr binding sig. Bundles without it (legacy / unit
    // test fixtures) skip both new checks, preserving compatibility
    // until activation. The activation gate prevents pre-cv bundles
    // from existing on mainnet/testnet.
    // CONFIRMED-CRIT (shielded inflation): the Schnorr binding signature is
    // MANDATORY at/above the input-binding activation height. The empty range
    // proof (which previously gated BOTH checks) is already rejected above, so an
    // attacker can no longer skip this by sending an empty proof. Below the
    // activation height we preserve the exact pre-fix opportunistic behavior.
    if (ctx.block_height >= ctx.shielded_input_binding_activation_height) {
        // Do not weaken consensus verification when the generator is
        // unavailable. The range-proof branch above has already rejected that
        // state; retain this independent guard so future refactors cannot make
        // the binding check fail open.
        if (!PedersenGeneratorsReady()) {
            return ShieldedValidationError::BindingSigInvalid;
        }
        const auto rc = VerifyBinding(bundle, ctx.tx_sighash);
        if (rc != BindingSigResult::Ok) {
            return ShieldedValidationError::BindingSigInvalid;
        }
    } else if (PedersenGeneratorsReady() && !bundle.aggregated_range_proof.empty()) {
        const auto rc = VerifyBinding(bundle, ctx.tx_sighash);
        if (rc != BindingSigResult::Ok) {
            return ShieldedValidationError::BindingSigInvalid;
        }
    }

    // 4. ZK proof verification
    for (const auto& spend : bundle.spends) {
        if (!VerifySpendProof(spend, ctx)) {
            return ShieldedValidationError::ProofInvalid;
        }
    }
    for (const auto& output : bundle.outputs) {
        if (!VerifyOutputProof(output, ctx)) {
            return ShieldedValidationError::ProofInvalid;
        }
    }

    // 5. Value balance: transparent_value_delta must match bundle's value_balance.
    //    transparent_in - transparent_out - fee = value_balance
    //    (ZK proofs internally verify that shielded values balance.)
    if (bundle.value_balance != ctx.transparent_value_delta) {
        return ShieldedValidationError::ValueBalanceMismatch;
    }

    return ShieldedValidationError::Ok;
}

ValidationContext BuildShieldedValidationContext(
    const ::dinero::Transaction& tx,
    const NullifierSet*          nullifier_set,
    const CommitmentTree*        commitment_tree,
    uint32_t                     block_height,
    int64_t                      transparent_value_delta,
    uint32_t                     shielded_activation_height,
    const AnchorHistory*         anchor_history,
    uint32_t                     shielded_input_binding_activation_height,
    uint32_t                     shielded_cv_binding_activation_height,
    uint32_t                     shielded_spend_auth_activation_height) {
    ValidationContext ctx(
        nullifier_set,
        commitment_tree,
        block_height,
        transparent_value_delta,
        shielded_activation_height,
        anchor_history,
        ComputeShieldedTxSighash(tx));
    // CONFIRMED-CRIT-05: defaulted member (not a ctor param) — set it here.
    ctx.shielded_input_binding_activation_height = shielded_input_binding_activation_height;
    // Audit Critical #1: cv-binding activation (defaulted member, set here).
    ctx.shielded_cv_binding_activation_height = shielded_cv_binding_activation_height;
    ctx.shielded_spend_auth_activation_height =
        shielded_spend_auth_activation_height;
    return ctx;
}

bool ApplyShieldedBundle(const ShieldedBundle& bundle,
                         CommitmentTree*       tree,
                         NullifierSet*         nullifiers,
                         uint32_t              block_height) {
    // ── PHASE 1: validate. Nothing below this line mutates anything. ───────
    //
    // Two distinct duplicate classes, and checking only the first is a trap:
    //
    //   against PERSISTENT state   Contains() -- the nullifier was spent in an
    //                              earlier block, or this block is being
    //                              applied twice.
    //   WITHIN this bundle         Contains() cannot see it. On an empty set a
    //                              bundle spending [N, N] passes both preflight
    //                              lookups, and the failure only surfaces on the
    //                              second Insert -- after outputs are appended
    //                              and the first nullifier is already persisted.
    //
    // Measured before this change: such a bundle left the commitment tree
    // mutated AND a nullifier row written, on a path that returns false.
    if (nullifiers) {
        std::set<Hash> seen_in_bundle;
        for (const auto& spend : bundle.spends) {
            if (nullifiers->Contains(spend.nullifier)) {
                return false;  // already spent in persistent state
            }
            if (!seen_in_bundle.insert(spend.nullifier).second) {
                return false;  // repeated inside this bundle
            }
        }
    } else {
        // Even with no nullifier set to consult, a self-conflicting bundle is
        // malformed and must not be applied.
        std::set<Hash> seen_in_bundle;
        for (const auto& spend : bundle.spends) {
            if (!seen_in_bundle.insert(spend.nullifier).second) {
                return false;
            }
        }
    }

    // NOT checked: repeated output commitments. No consensus rule prohibits
    // them -- ValidateShieldedBundle has no such check and the error enum has
    // no code for it -- so rejecting here would invent a rule this function
    // does not own and could refuse a block that other nodes accept. Appending
    // the same leaf twice is well defined and deterministic. If the protocol
    // ever forbids it, that belongs in ValidateShieldedBundle with its own
    // error code, not here.

    // ── PHASE 2: mutate. Validation above means neither step should now fail
    // for a reason this function can foresee. ──────────────────────────────
    //
    // Nullifiers go FIRST, deliberately. Insert() is the only fallible step
    // (it touches sqlite), and CommitmentTree::Append() is in-memory and does
    // not report failure. Doing the fallible work first means an I/O failure
    // returns with the commitment tree completely untouched.
    //
    // The asymmetry matters: a stray nullifier row is fail-SAFE (it can only
    // refuse a spend) and is rebuilt from ChainDB at startup, whereas a stray
    // tree leaf changes the shielded root and every anchor derived from it.
    // If one of the two has to be left dirty by an I/O fault, it must be the
    // recoverable one.
    if (nullifiers) {
        for (const auto& spend : bundle.spends) {
            if (!nullifiers->Insert(spend.nullifier, block_height)) {
                // Unreachable via validation above; only an I/O fault reaches
                // here. Tree is untouched; the caller aborts the block.
                return false;
            }
        }
    }

    // This is the ONLY place shielded outputs enter consensus state.
    // They NEVER enter Utreexo.
    if (tree) {
        for (const auto& output : bundle.outputs) {
            tree->Append(output.commitment);
        }
    }

    return true;
}

} // namespace dinero::consensus::shielded
