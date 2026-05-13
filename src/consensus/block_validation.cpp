#include "consensus/block_validation.h"
#include "consensus/consensus_write_batch.h"
#include "dinero/compat/int128.hpp"
#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/shielded_tx.h"
#include "consensus/shielded/shielded_serialization.h"
#include "consensus/shielded/shielded_block_validation.h"
#include "consensus/shielded/shielded_validation.h"
extern "C" {
#include <secp256k1.h>
#include <secp256k1_generator.h>
}
#include "consensus/limits.h"              // MAX_BLOCK_SIZE, MAX_BLOCK_WEIGHT, MAX_BLOCK_SIGOPS_COST
#include "consensus/outpoint.h"            // Phase M.2: Binary OutPoint comparison
#include "consensus/utreexo_activation.h"   // Phase 3: Utreexo activation rule
#include "consensus/utreexo_canonical_roots_activation.h"  // Apr 13 2026 Stage 3 fork
#include "consensus/utreexo_phase_guard.h"  // Phase 3: Safety guards (prevent prod deployment)
#include "consensus/utreexo_delta.h"        // Phase 4: Delta-based undo for efficient reorgs
#include "consensus/header_consensus.h"     // Phase 3: Header size enforcement
#include "consensus/witness_commitment.h"   // Phase 11: Witness commitment guardrail
// Wallet header removed - consensus uses only IUTXOProvider interface with consensus types
#include "consensus/tx_parser.h"
#include "consensus/script_verify.h"
#include "consensus/script_interpreter.h"  // Phase L0.1: For VerifyScript and consensus flags
#include "consensus/script_validation.h"   // Phase F.11: Minimal script validation engine
#include "consensus/covenants.h"           // Phase L0.1: For SCRIPT_VERIFY_COVENANTS
#include "consensus/script.h"              // Phase L0.1: For Script class
#include "consensus/cpu_budget_monitor.h"  // Phase E.3: CPU budget monitoring
#include "consensus/subsidy.h"  // Canonical monetary policy
#include "consensus/utreexo_stump.h"       // Transition proof cross-check
#include <algorithm>
#include <sstream>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <iomanip>  // v0.14.0.4: For std::setw, std::setfill in Utreexo hex conversion
#include <iostream>  // Phase 2: Shadow verification logging
#include <limits>
#include <ctime>     // Priority 5: D4 timestamp validation
#include <functional>  // Phase 2: For std::function in rollback guard
#include <typeinfo>

namespace dinero {
namespace consensus {

namespace {

std::string ActiveUtxoBackendName(const IConsensusUTXOSet* utxo_set) {
    if (!utxo_set) {
        return "null";
    }
    return typeid(*utxo_set).name();
}

bool HasConfidentialInputs(const std::vector<UTXOEntry>& input_utxos) {
    return std::any_of(input_utxos.begin(), input_utxos.end(), [](const UTXOEntry& utxo) {
        return utxo.is_confidential;
    });
}

bool UsesConfidentialValueSemantics(const Transaction& tx, const std::vector<UTXOEntry>& input_utxos) {
    return tx.HasConfidentialOutputs() || HasConfidentialInputs(input_utxos);
}

bool UsesShieldedValueSemantics(const Transaction& tx) {
    return Transaction::IsShieldedVersion(tx.version) ||
           !tx.shielded_bundle_bytes.empty();
}

bool ComputeValidatedTransactionFee(const Transaction& tx,
                                    const std::vector<UTXOEntry>& input_utxos,
                                    uint64_t total_input_value,
                                    uint64_t total_output_value,
                                    uint64_t& fee,
                                    std::string& error) {
    if (UsesConfidentialValueSemantics(tx, input_utxos) ||
        (UsesShieldedValueSemantics(tx) && tx.HasExplicitFee())) {
        if (!tx.HasExplicitFee()) {
            error = UsesShieldedValueSemantics(tx)
                ? "Shielded transaction missing explicit fee"
                : "Confidential transaction missing explicit fee";
            return false;
        }
        fee = tx.GetExplicitFee();
        return true;
    }

    if (total_output_value > total_input_value) {
        error = "Outputs exceed inputs (negative fee)";
        return false;
    }

    fee = total_input_value - total_output_value;
    return true;
}

bool ComputeTransparentValueDelta(uint64_t total_input_value,
                                  uint64_t total_output_value,
                                  uint64_t fee,
                                  int64_t& delta,
                                  std::string& error) {
    using dinero::compat::i128;
    using dinero::compat::i128_zext_u64;
    // Inputs are uint64_t. The original code did `(__int128)uint64_t_value`
    // which zero-extends. i128_zext_u64 mirrors that semantics; using
    // i128(uint64_t) directly would route through i128(int64_t) on the
    // struct backend and sign-extend high-bit-set values (an off-by-2^65
    // bug under adversarial inputs).
    const i128 signed_delta =
        i128_zext_u64(total_input_value) -
        i128_zext_u64(total_output_value) -
        i128_zext_u64(fee);
    if (signed_delta < i128(std::numeric_limits<int64_t>::min()) ||
        signed_delta > i128(std::numeric_limits<int64_t>::max())) {
        error = "Shielded transparent value delta out of range";
        return false;
    }
    delta = static_cast<int64_t>(signed_delta);
    return true;
}

const char* ShieldedValidationErrorToString(shielded::ShieldedValidationError err) {
    using shielded::ShieldedValidationError;
    switch (err) {
        case ShieldedValidationError::Ok:                   return "ok";
        case ShieldedValidationError::NullifierDuplicate:   return "nullifier-duplicate";
        case ShieldedValidationError::AnchorInvalid:        return "anchor-invalid";
        case ShieldedValidationError::ProofInvalid:         return "proof-invalid";
        case ShieldedValidationError::ValueBalanceMismatch: return "value-balance-mismatch";
        case ShieldedValidationError::BindingSigInvalid:    return "binding-sig-invalid";
        case ShieldedValidationError::BundleMalformed:      return "bundle-malformed";
        case ShieldedValidationError::NotActive:            return "shielded-not-active";
        case ShieldedValidationError::BundleTooLarge:       return "bundle-too-large";
        case ShieldedValidationError::RangeProofInvalid:    return "range-proof-invalid";
    }
    return "unknown";
}

bool ValidateShieldedTransactionBundle(
    const Transaction& tx,
    uint32_t height,
    uint64_t total_input_value,
    uint64_t total_output_value,
    uint64_t fee,
    const shielded::CommitmentTree* tree,
    const shielded::NullifierSet* nullifiers,
    const shielded::AnchorHistory* anchor_history,
    std::string& error,
    int64_t* transparent_delta_out = nullptr) {
    if (!UsesShieldedValueSemantics(tx)) {
        if (transparent_delta_out) {
            *transparent_delta_out = 0;
        }
        return true;
    }

    if (!Transaction::IsShieldedVersion(tx.version)) {
        error = "Non-shielded transaction carries shielded bundle";
        return false;
    }

    if (tx.shielded_bundle_bytes.empty()) {
        error = "Shielded transaction missing shielded bundle";
        return false;
    }

    if (!tree || !nullifiers) {
        error = "Shielded state unavailable";
        return false;
    }

    shielded::ShieldedBundle bundle;
    auto decode = shielded::DeserializeShieldedBundle(tx.shielded_bundle_bytes, &bundle);
    if (decode != shielded::BundleDecodeError::Ok) {
        error = "Shielded bundle decode failed (code " +
                std::to_string(static_cast<int>(decode)) + ")";
        return false;
    }

    int64_t transparent_delta = 0;
    if (!ComputeTransparentValueDelta(total_input_value, total_output_value, fee,
                                      transparent_delta, error)) {
        return false;
    }

    // Single-helper construction. Both this caller and the reindex
    // replay path call BuildShieldedValidationContext so they cannot
    // diverge on field shape (the Apr 30 fleet split was a missing
    // tx_sighash on the reindex side; with the helper, neither caller
    // can omit it).
    auto ctx = shielded::BuildShieldedValidationContext(
        tx,
        nullifiers,
        tree,
        height,
        transparent_delta,
        Params().shielded_activation_height,
        anchor_history);
    const auto validation = shielded::ValidateShieldedBundle(bundle, ctx);
    if (validation != shielded::ShieldedValidationError::Ok) {
        error = "Shielded validation failed: " +
                std::string(ShieldedValidationErrorToString(validation));
        return false;
    }

    if (transparent_delta_out) {
        *transparent_delta_out = transparent_delta;
    }
    return true;
}

uint64_t GetUtreexoLeafAmount(const UTXOEntry& utxo) {
    return utxo.is_confidential ? 0 : utxo.value.GetUna();
}

uint64_t GetUtreexoLeafAmount(const TxOutput& output) {
    return output.is_confidential ? 0 : output.value.GetUna();
}

uint64_t GetUtreexoLeafAmount(const SpentOutputData& spent_output) {
    return spent_output.is_confidential ? 0 : spent_output.value;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Phase 2: Pure Consensus Architecture
// ═══════════════════════════════════════════════════════════════════════════
// BlockValidator operates on IConsensusUTXOSet (pure in-memory).
// The UTXO set OWNS the Utreexo forest - no separate initialization.
//
// Key changes:
// - IConsensusUTXOSet* instead of IUTXOProvider*
// - Forest access via consensus_utxo_set_->GetForest()
// - Snapshot-first validation (failed block = restore snapshot)
// ═══════════════════════════════════════════════════════════════════════════

BlockValidator::BlockValidator(IConsensusUTXOSet* utxo_set)
    : consensus_utxo_set_(utxo_set) {
    if (!consensus_utxo_set_) {
        throw std::runtime_error("BlockValidator: Consensus UTXO set cannot be null");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 11a: PURE CONSENSUS - Apply Block (Mining + Validation)
// ═══════════════════════════════════════════════════════════════════════════
// This is the single source of truth for consensus state transitions.
// Used by BOTH mining and validation - difference is in enforcement, not computation.
// ═══════════════════════════════════════════════════════════════════════════
bool BlockValidator::ApplyBlock(const Block& block, uint32_t height, const uint256& block_hash,
                                BlockUndo& undo, uint256& computed_utreexo_root,
                                std::string& error, CPUBudgetMonitor* cpu_monitor) {
    // Delegate to the existing ConnectBlock implementation
    // We'll extract the computed root and return it
    bool result = ConnectBlockInternal(block, height, block_hash, undo, computed_utreexo_root, false, error, cpu_monitor);
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 11a: CONSENSUS + ENFORCEMENT - Validate And Apply (Validation Only)
// ═══════════════════════════════════════════════════════════════════════════
// Wraps ApplyBlock() and adds Utreexo root verification.
// Used by sync/validation paths only - mining should use ApplyBlock() directly.
// ═══════════════════════════════════════════════════════════════════════════
bool BlockValidator::ValidateAndApplyBlock(const Block& block, uint32_t height, const uint256& block_hash,
                                           BlockUndo& undo, std::string& error,
                                           CPUBudgetMonitor* cpu_monitor) {
    uint256 computed_utreexo_root;
    bool result = ConnectBlockInternal(block, height, block_hash, undo, computed_utreexo_root, true, error, cpu_monitor);
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 11a: Pure Utreexo Root Computation (NO STATE MUTATION)
// ═══════════════════════════════════════════════════════════════════════════
// Used by mining to compute utreexo_root without mutating chainstate.
// Equivalent to Bitcoin Core's TestBlockValidity().
bool BlockValidator::ComputeUtreexoRootPure(const Block& block, uint32_t height,
                                            uint256& computed_utreexo_root,
                                            std::string& error) {
    std::cout << "\n🔍 [ComputeUtreexoRootPure] ENTRY" << std::endl;
    std::cout << "   height=" << height << std::endl;
    std::cout << "   block.vtx.size()=" << block.vtx.size() << std::endl;

    // Check if Utreexo is active at this height
    if (!IsUtreexoActive(height)) {
        std::cout << "   Utreexo NOT active at height " << height << std::endl;
        computed_utreexo_root.SetNull();
        return true;  // Empty root when Utreexo not active
    }

    // Check if forest exists
    if (!consensus_utxo_set_) {
        std::cout << "   consensus_utxo_set_ is NULL" << std::endl;
        computed_utreexo_root.SetNull();
        return true;  // No forest = empty root
    }

    std::cout << "   Forest leaves before: " << consensus_utxo_set_->GetForest().getNumLeaves() << std::endl;

    // Clone current forest promoted to the semantics at `height`. This is
    // the single-source-of-truth factory (Apr 13 2026 Stage 3 — the old
    // scattered IsUtreexoCanonicalRootsActive+setCanonicalEmptyRoots+
    // rebuildRoots triples are gone).
    UtreexoForest snapshot = consensus_utxo_set_->GetForest().cloneForHeight(height);
    std::cout << "   Cloned forest, snapshot leaves: " << snapshot.getNumLeaves() << std::endl;

    // ═══════════════════════════════════════════════════════════════════════════
    // UTREEXO CANONICAL ORDER: REMOVE ALL → ADD ALL
    // ═══════════════════════════════════════════════════════════════════════════
    // Utreexo has ONE legal order per block:
    //   1. REMOVE all spent UTXOs (from previous state)
    //   2. ADD all new outputs (including coinbase)
    //   3. Commit root
    //
    // This is NOT per-transaction interleaved - it's two separate passes.
    //
    // INTRA-BLOCK EPHEMERAL UTXOS:
    // When transactions chain within a single block (tx2 spends change from tx1),
    // the intermediate outputs never enter or leave the Utreexo forest. They are
    // "ephemeral" - created and consumed in the same block. We must identify
    // these and skip them in both the REMOVE and ADD passes.
    // ═══════════════════════════════════════════════════════════════════════════

    // PRE-SCAN: Identify all outputs created within this block
    std::unordered_map<OutPoint, size_t> intra_block_outputs;  // outpoint -> tx index
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); tx_idx++) {
        const auto& tx = block.vtx[tx_idx];
        TxId txid = tx.GetTxid();
        for (uint32_t n = 0; n < tx.vout.size(); n++) {
            intra_block_outputs[OutPoint(txid, n)] = tx_idx;
        }
    }

    // Identify intra-block spends: inputs that reference outputs from this block
    std::unordered_set<OutPoint> intra_block_spends;
    for (const auto& tx : block.vtx) {
        if (tx.IsCoinbase()) continue;
        for (const auto& input : tx.vin) {
            OutPoint outpoint(input.prevout.txid, input.prevout.vout);
            if (intra_block_outputs.count(outpoint)) {
                intra_block_spends.insert(outpoint);
            }
        }
    }

    if (!intra_block_spends.empty()) {
        std::cout << "   [Pure] " << intra_block_spends.size()
                  << " intra-block ephemeral UTXOs (skipped in forest)" << std::endl;
    }

    // PASS 1: REMOVE ALL spent UTXOs (entire block)
    // Skip intra-block spends — those UTXOs were never in the forest
    for (const auto& tx : block.vtx) {
        bool is_coinbase = tx.IsCoinbase();

        // Skip coinbase (no inputs to spend)
        if (is_coinbase) continue;

        for (const auto& input : tx.vin) {
            OutPoint outpoint(input.prevout.txid, input.prevout.vout);

            // Skip intra-block spends (ephemeral UTXOs never enter the forest)
            if (intra_block_spends.count(outpoint)) {
                continue;
            }

            // Look up UTXO from consensus set (Phase 2: pure in-memory)
            const UTXOEntry* utxo_ptr = consensus_utxo_set_->GetCoin(outpoint);
            if (!utxo_ptr) {
                error = "UTXO not found for input: " + outpoint.ToString();
                return false;
            }

            const auto& utxo = *utxo_ptr;
            const uint64_t leaf_value = GetUtreexoLeafAmount(utxo);

            // Hash the UTXO being spent
            UtreexoHash leafHash = HashUTXO(
                input.prevout.txid.AsUint256(),
                input.prevout.vout,
                leaf_value,
                utxo.scriptPubKey
            );

            // Find leaf position
            auto position_opt = snapshot.findLeafPosition(leafHash);
            if (!position_opt.has_value()) {
                error = "utreexo-leaf-missing-in-pure: " + outpoint.ToString();
                return false;
            }

            // Remove from snapshot at the known position.
            //
            // The snapshot is a clone of our own forest; there is no
            // adversarial proof to verify and no external input to trust.
            // The proof-based variant re-verified the proof against the
            // cached `roots_` vector, which can be out of sync with the
            // node tree in the presence of partial deletions — that
            // staleness caused every covenant spend to fail with
            // "utreexo-remove-failed-in-pure" on Apr 13 2026 and was a
            // release blocker for the privacy stack.
            if (!snapshot.removeAtKnownPosition(position_opt.value(), leafHash)) {
                error = "utreexo-remove-failed-in-pure: " + outpoint.ToString();
                return false;
            }
        }
    }

    // PASS 2: ADD ALL new outputs (entire block, including coinbase)
    // Skip intra-block spent outputs — they are ephemeral and never enter the forest
    for (const auto& tx : block.vtx) {
        TxId txid = tx.GetTxid();

        for (size_t n = 0; n < tx.vout.size(); n++) {
            OutPoint out(txid, static_cast<uint32_t>(n));

            // Skip outputs consumed within this block (ephemeral)
            if (intra_block_spends.count(out)) {
                std::cout << "   [Pure] Skipping ephemeral output " << n << std::endl;
                continue;
            }

            const auto& output = tx.vout[n];
            const uint64_t leaf_value = GetUtreexoLeafAmount(output);

            // Hash the new UTXO
            UtreexoHash leafHash = HashUTXO(
                txid.AsUint256(),
                static_cast<uint32_t>(n),
                leaf_value,
                std::vector<uint8_t>(output.scriptPubKey.begin(), output.scriptPubKey.end())
            );

            {
                std::ostringstream lh;
                for (size_t b = 0; b < std::min(leafHash.size(), size_t(8)); b++)
                    lh << std::hex << std::setfill('0') << std::setw(2) << (int)leafHash[b];
                std::cout << "   [Pure] Adding output " << n << ": "
                          << "value=" << leaf_value
                          << ", spk_size=" << output.scriptPubKey.size()
                          << ", is_ct=" << output.is_confidential
                          << ", txid=" << txid.AsUint256().GetHex().substr(0, 16)
                          << ", leaf=" << lh.str()
                          << std::endl;
            }

            // Add to snapshot
            uint64_t pos = snapshot.add(leafHash);
            if (pos == UINT64_MAX) {
                error = "utreexo-add-failed-in-pure: " +
                        snapshot.describeAddFailure(leafHash);
                return false;
            }
            std::cout << "   [Pure] Added at position: " << pos << std::endl;
        }
    }

    // Extract AFTER-state root from snapshot
    std::cout << "   [Pure] Snapshot leaves after: " << snapshot.getNumLeaves() << std::endl;
    UtreexoHash temp_root = snapshot.getCommitment();

    std::ostringstream root_hex;
    for (size_t i = 0; i < std::min(temp_root.size(), size_t(32)); ++i) {
        root_hex << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(temp_root[i]);
    }
    std::cout << "   [Pure] Computed root: " << root_hex.str() << std::endl;

    if (temp_root.size() == 32) {
        std::memcpy(computed_utreexo_root.data, temp_root.data(), 32);
    } else {
        computed_utreexo_root.SetNull();
    }

    std::cout << "🔍 [ComputeUtreexoRootPure] EXIT (success)" << std::endl;
    // temp_forest goes out of scope and is discarded (no mutation of real forest)
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Pure Utreexo Root Computation — FROM SUPPLIED FOREST + LOOKUP
// ═══════════════════════════════════════════════════════════════════════════
// Variant of ComputeUtreexoRootPure that uses caller-provided
// `starting_forest` and `utxo_lookup` instead of the live
// `consensus_utxo_set_`. Enables accept-time validation of
// side-chain blocks whose pre-block UTXO/forest state is NOT the
// current main-chain state.
//
// Caller is responsible for the fork-aware walk that builds the
// UTXO overlay backing `utxo_lookup` — typically: load the utreexo
// checkpoint at the fork point for `starting_forest`, walk main
// chain's undo records back to the fork point to roll back spends,
// then walk the side chain forward. The coinbase-only simplification
// is just "no inputs to resolve" → a no-op lookup that returns nullptr.
// ═══════════════════════════════════════════════════════════════════════════
bool BlockValidator::ComputeUtreexoRootPureFromForest(
    const Block& block, uint32_t height,
    UtreexoForest starting_forest,
    const std::function<const UTXOEntry*(const OutPoint&)>& utxo_lookup,
    uint256& computed_utreexo_root,
    std::string& error) {
    if (!IsUtreexoActive(height)) {
        computed_utreexo_root.SetNull();
        return true;
    }
    if (block.vtx.empty()) {
        error = "no-coinbase: block has no transactions";
        return false;
    }

    UtreexoForest snapshot = std::move(starting_forest);

    // Canonical-roots fork may need to flip on if the height is
    // at/after activation and the checkpoint predates it.
    if (consensus::IsUtreexoCanonicalRootsActive(height) &&
        !snapshot.isCanonicalEmptyRoots()) {
        snapshot.setCanonicalEmptyRoots(true);
        snapshot.rebuildRoots();
    }

    // Pre-scan intra-block outputs / ephemeral spends (same shape
    // as ComputeUtreexoRootPure).
    std::unordered_map<OutPoint, size_t> intra_block_outputs;
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
        const auto& tx = block.vtx[tx_idx];
        TxId txid = tx.GetTxid();
        for (uint32_t n = 0; n < tx.vout.size(); ++n) {
            intra_block_outputs[OutPoint(txid, n)] = tx_idx;
        }
    }
    std::unordered_set<OutPoint> intra_block_spends;
    for (const auto& tx : block.vtx) {
        if (tx.IsCoinbase()) continue;
        for (const auto& input : tx.vin) {
            OutPoint op(input.prevout.txid, input.prevout.vout);
            if (intra_block_outputs.count(op)) {
                intra_block_spends.insert(op);
            }
        }
    }

    // PASS 1: remove all spent UTXOs from the snapshot.
    for (const auto& tx : block.vtx) {
        if (tx.IsCoinbase()) continue;
        for (const auto& input : tx.vin) {
            OutPoint op(input.prevout.txid, input.prevout.vout);
            if (intra_block_spends.count(op)) continue;

            const UTXOEntry* utxo = utxo_lookup(op);
            if (!utxo) {
                error = "utreexo-input-not-found-in-fork-view: " + op.ToString();
                return false;
            }
            const uint64_t leaf_value = GetUtreexoLeafAmount(*utxo);
            UtreexoHash leafHash = HashUTXO(
                input.prevout.txid.AsUint256(),
                input.prevout.vout,
                leaf_value,
                utxo->scriptPubKey);
            auto pos_opt = snapshot.findLeafPosition(leafHash);
            if (!pos_opt.has_value()) {
                error = "utreexo-leaf-missing-in-fork-view: " + op.ToString();
                return false;
            }
            if (!snapshot.removeAtKnownPosition(pos_opt.value(), leafHash)) {
                error = "utreexo-remove-failed-in-fork-view: " + op.ToString();
                return false;
            }
        }
    }

    // PASS 2: add all new outputs (including coinbase).
    for (const auto& tx : block.vtx) {
        TxId txid = tx.GetTxid();
        for (size_t n = 0; n < tx.vout.size(); ++n) {
            OutPoint op(txid, static_cast<uint32_t>(n));
            if (intra_block_spends.count(op)) continue;

            const auto& output = tx.vout[n];
            const uint64_t leaf_value = GetUtreexoLeafAmount(output);
            UtreexoHash leafHash = HashUTXO(
                txid.AsUint256(),
                static_cast<uint32_t>(n),
                leaf_value,
                std::vector<uint8_t>(output.scriptPubKey.begin(),
                                     output.scriptPubKey.end()));
            uint64_t pos = snapshot.add(leafHash);
            if (pos == UINT64_MAX) {
                error = "utreexo-add-failed-from-forest: capacity or duplicate";
                return false;
            }
        }
    }

    UtreexoHash temp_root = snapshot.getCommitment();
    if (temp_root.size() == 32) {
        std::memcpy(computed_utreexo_root.data, temp_root.data(), 32);
    } else {
        computed_utreexo_root.SetNull();
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// LEGACY WRAPPER - Backward Compatibility
// ═══════════════════════════════════════════════════════════════════════════
bool BlockValidator::ConnectBlock(const Block& block, uint32_t height, const uint256& block_hash, BlockUndo& undo, std::string& error, CPUBudgetMonitor* cpu_monitor) {
    return ValidateAndApplyBlock(block, height, block_hash, undo, error, cpu_monitor);
}

// ═══════════════════════════════════════════════════════════════════════════
// INTERNAL IMPLEMENTATION - Shared by ApplyBlock and ValidateAndApplyBlock
// ═══════════════════════════════════════════════════════════════════════════
bool BlockValidator::ConnectBlockInternal(const Block& block, uint32_t height, const uint256& block_hash,
                                          BlockUndo& undo, uint256& computed_utreexo_root,
                                          bool verify_root, std::string& error,
                                          CPUBudgetMonitor* cpu_monitor) {
    // Phase E.3: Block validation CPU budget tracking
    ScopedCPUBudget cpu_budget(cpu_monitor, ScopedCPUBudget::Operation::BLOCK_VALIDATION);

    // Initialize undo data before any validation exits so callers always get
    // a height/hash-consistent container and rollback can capture genesis-state snapshots.
    undo = BlockUndo(height, block_hash);

    // ═════════════════════════════════════════════════════════════════════════
    // Apr 13 2026 Stage 3 — Utreexo canonical-roots fork activation.
    //
    // At `UTREEXO_CANONICAL_ROOTS_HEIGHT_MAINNET` (3000 on mainnet):
    //   1. Flip `canonical_empty_roots_` on in the live consensus forest.
    //   2. Call `rebuildRoots()` so `roots_` is recomputed from `nodes_`
    //      using the new canonical-zero-sentinel logic. Any pre-activation
    //      ghost slots get overwritten with the correct values.
    //   3. From this block forward every add/remove maintains the
    //      canonical-roots invariant and `proof.verify` stops failing on
    //      covenant spends.
    //
    // This runs unconditionally on the forest the first time
    // ConnectBlockInternal sees the activation height. `setCanonicalEmptyRoots`
    // is idempotent — calling it again on subsequent blocks is a no-op.
    // The block being connected (height == activation) will have its
    // utreexo_root computed with the canonical logic, so all nodes on the
    // network produce the same commitment at the fork boundary.
    // ═════════════════════════════════════════════════════════════════════════
    if (consensus_utxo_set_ && consensus::IsUtreexoCanonicalRootsActive(height)) {
        auto& live_forest = consensus_utxo_set_->GetForest();
        if (!live_forest.isCanonicalEmptyRoots()) {
            std::cout << "🪐 [Canonical Roots Fork] Activating at height "
                      << height << " — rebuilding roots_ from nodes_"
                      << std::endl;
            live_forest.setCanonicalEmptyRoots(true);
            live_forest.rebuildRoots();
        }
    }

    // Phase 2 snapshot/rollback semantics apply to all failure paths, including
    // structural rejects that happen before any deeper consensus work.
    UTXOSnapshot pre_block_snapshot;
    const bool snapshot_supported =
        consensus_utxo_set_ && consensus_utxo_set_->SupportsSnapshotRestore();
    if (snapshot_supported) {
        pre_block_snapshot = consensus_utxo_set_->Snapshot();
    }

    std::vector<uint8_t> pre_block_shielded_frontier;
    if (shielded_tree_) {
        auto* tree = static_cast<shielded::CommitmentTree*>(shielded_tree_);
        pre_block_shielded_frontier = tree->SerializeFrontier();
    }

    auto restore_on_failure = [&]() {
        if (snapshot_supported) {
            std::cout << "🔄 [Phase 2] Restoring snapshot after block failure" << std::endl;
            consensus_utxo_set_->Restore(pre_block_snapshot);
        }
        if (shielded_tree_ && !pre_block_shielded_frontier.empty()) {
            auto* tree = static_cast<shielded::CommitmentTree*>(shielded_tree_);
            auto* nullifiers = static_cast<shielded::NullifierSet*>(shielded_nullifiers_);
            tree->DeserializeFrontier(pre_block_shielded_frontier.data(),
                                      pre_block_shielded_frontier.size());
            if (nullifiers && height > 0) {
                nullifiers->RollbackAbove(height - 1);
            }
        }
    };

    bool block_connect_success = false;
    struct RollbackGuard {
        std::function<void()> rollback;
        bool* success;
        RollbackGuard(std::function<void()> f, bool& s) : rollback(std::move(f)), success(&s) {}
        ~RollbackGuard() { if (!*success) rollback(); }
    };
    RollbackGuard rollback_guard(restore_on_failure, block_connect_success);

    // ═════════════════════════════════════════════════════════════════════════
    // STRUCTURAL CHECKS: Reject obviously invalid blocks before any state work
    // ═════════════════════════════════════════════════════════════════════════
    if (block.vtx.empty()) {
        error = "Block has no transactions (missing coinbase)";
        return false;
    }

    // ═════════════════════════════════════════════════════════════════════════
    // CONSENSUS-CRITICAL: Utreexo Forest Requirement
    // ═════════════════════════════════════════════════════════════════════════
    // In Dinero, Utreexo is consensus-critical from genesis.
    // If Utreexo is active at this height, forest MUST be initialized.
    // A missing forest is a FATAL initialization error - abort immediately.
    // ═════════════════════════════════════════════════════════════════════════
    if (IsUtreexoActive(height) && !consensus_utxo_set_) {
        std::cerr << "❌ [FATAL] Utreexo forest is NULL at active height " << height << std::endl;
        std::cerr << "❌ [FATAL] BlockValidator::setUtreexoForest() was never called" << std::endl;
        std::cerr << "❌ [FATAL] This is a consensus violation - aborting" << std::endl;
        std::abort();
    }

    // ═════════════════════════════════════════════════════════════════════════
    // EXPLICIT CONSENSUS GATE: Full Rules Activation Check
    // ═════════════════════════════════════════════════════════════════════════
    // Blocks before full-rules activation are PRE-ACTIVATION blocks.
    // Blocks at/after full-rules activation enforce Utreexo + witness commitment rules.
    //
    // This is the SINGLE, EXPLICIT gate for all consensus features.
    // ═════════════════════════════════════════════════════════════════════════

    if (FullRulesActive(height)) {
        // ─────────────────────────────────────────────────────────────────────
        // FULL RULES PATH: Utreexo + witness commitment enforced
        // ─────────────────────────────────────────────────────────────────────

        // Validate witness commitment (if present, must be correct)
        std::string witness_error;
        if (!ValidateWitnessCommitment(block.vtx, witness_error)) {
            error = "bad-witness-commitment: " + witness_error;
            return false;
        }
        // MANDATORY witness commitment for new blocks (activation height 10670)
        // Blocks 1-10669 were mined before the assembler added commitments.
        // After 10670, every block has witness data and MUST include DINW commitment.
        {
            constexpr uint32_t WITNESS_COMMITMENT_MANDATORY_HEIGHT = 10670;
            if (height >= WITNESS_COMMITMENT_MANDATORY_HEIGHT) {
                bool block_has_witness = false;
                for (const auto& tx : block.vtx) {
                    if (tx.HasWitness()) {
                        block_has_witness = true;
                        break;
                    }
                }
                if (block_has_witness && !block.vtx.empty()) {
                    auto commitment_idx = FindWitnessCommitmentIndex(block.vtx[0]);
                    if (!commitment_idx.has_value()) {
                        error = "missing-witness-commitment (required at height " +
                                std::to_string(height) + ", mandatory since height " +
                                std::to_string(WITNESS_COMMITMENT_MANDATORY_HEIGHT) + ")";
                        return false;
                    }
                }
            }
        }

        // Validate Utreexo data presence (if block spends UTXOs)
        // CONSENSUS-CRITICAL: No bypass, always enforced
        bool has_utxo_spends = false;
        for (const auto& tx : block.vtx) {
            if (!tx.IsCoinbase()) {
                has_utxo_spends = true;
                break;
            }
        }
        if (has_utxo_spends && !block.utreexo.has_value() && validation_mode_ == ValidationMode::STATELESS) {
            error = "missing-utreexo-data (height " + std::to_string(height) + " requires Utreexo proofs)";
            return false;
        }

    } else {
        // ─────────────────────────────────────────────────────────────────────
        // PRE-ACTIVATION PATH: legacy bootstrap window before full rules
        // ─────────────────────────────────────────────────────────────────────
        // These special blocks MUST NOT have witness commitments.
        // This is a hard invariant - if violated, something is seriously wrong.
        // ─────────────────────────────────────────────────────────────────────

        if (!block.vtx.empty()) {
            auto commitment_index = FindWitnessCommitmentIndex(block.vtx[0]);
            if (commitment_index.has_value()) {
                error = "bad-witness-commitment-preactivation (height " + std::to_string(height) + " must not have witness commitment)";
                return false;
            }
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // VALIDATION-BEFORE-MUTATION: Pending State Changes
    // ═════════════════════════════════════════════════════════════════════════
    // Collect all state mutations in memory buffers BEFORE committing.
    // This ensures that if root verification fails, NO state is mutated.
    // ═════════════════════════════════════════════════════════════════════════
    std::vector<std::pair<OutPoint, UTXOEntry>> pending_utxo_additions;  // UTXOs to add (deferred until validation passes)

    // ═════════════════════════════════════════════════════════════════════════
    // INV-2: DEFERRED SPENDS - No UTXO state mutation before proof verification
    // ═════════════════════════════════════════════════════════════════════════
    // Collect all spends in memory. Only commit after Utreexo proof passes.
    // This eliminates reliance on snapshot-restore for the normal validation path.
    // ═════════════════════════════════════════════════════════════════════════
    std::vector<std::pair<OutPoint, UndoEntry>> pending_utxo_spends;  // UTXOs to spend (deferred until proof passes)
    std::vector<int64_t> pending_shielded_deltas;  // Transparent deltas for shielded txs

    // ═════════════════════════════════════════════════════════════════════════
    // CONSENSUS ENFORCEMENT: Block Header Size (All Heights)
    // ═════════════════════════════════════════════════════════════════════════

    // 🔒 CONSENSUS RULE: Block Header Size Enforcement (Phase 3: BlockHeader v1)
    // ─────────────────────────────────────────────────────────────────────────
    // All blocks MUST use 128-byte headers (BlockHeader v1).
    // This prevents silent acceptance of legacy 80-byte or transitional 112-byte headers.
    // ─────────────────────────────────────────────────────────────────────────
    {
        auto serialized_header = block.header.SerializeForHash();
        size_t header_size = serialized_header.size();

        // Enforce 128-byte header size (BlockHeader v1)
        if (!IsValidHeaderSize(header_size, block.header.version, height)) {
            error = "bad-header-size (expected 128 bytes, got " + std::to_string(header_size) + ")";
            return false;  // REJECT BLOCK
        }

        // 🧪 TEMPORARY PHASE 3 ASSERTION: Verify header size is exactly 128 bytes
        // TODO (post-Phase-3): Remove this assertion after genesis is finalized and tested
        if (header_size != 128) {
            error = "FATAL: Header size validation bug (size=" + std::to_string(header_size) + ", expected 128)";
            return false;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // Priority 5 FIX: D4 - Timestamp Validation (max 2 hours in future)
    // ═══════════════════════════════════════════════════════════════════════════
    {
        constexpr uint32_t MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60;  // 2 hours
        uint32_t current_time = static_cast<uint32_t>(std::time(nullptr));
        if (block.header.timestamp > current_time + MAX_FUTURE_BLOCK_TIME) {
            error = "bad-timestamp: block too far in future (" +
                    std::to_string(block.header.timestamp) + " > " +
                    std::to_string(current_time + MAX_FUTURE_BLOCK_TIME) + ")";
            return false;
        }
    }

    // Phase 8: Stateless Validation - REQUIRE Utreexo proofs (for non-coinbase blocks)
    // ─────────────────────────────────────────────────────────────────────────
    // Stateless nodes CANNOT validate UTXO spends without proofs (no UTXO DB)
    // Coinbase-only blocks can be validated without proofs (no spends to check)
    // ─────────────────────────────────────────────────────────────────────────
    if (validation_mode_ == ValidationMode::STATELESS) {
        // Check if block has any non-coinbase inputs
        bool has_utxo_spends = false;
        for (const auto& tx : block.vtx) {
            if (!tx.IsCoinbase()) {
                has_utxo_spends = true;
                break;
            }
        }

        if (has_utxo_spends && !block.utreexo.has_value()) {
            error = "stateless-validation-requires-proofs (PROOF_MISSING)";
            return false;  // REJECT - stateless validation impossible without proofs for UTXO spends
        }

        if (!consensus_utxo_set_) {
            error = "stateless-validation-requires-accumulator";
            return false;  // REJECT - accumulator not initialized
        }

        std::cout << "🔍 [Stateless Validation] Block " << height << " validation (proof-based)" << std::endl;
    }

    // Phase 5: Stateless IBD - REQUIRE Utreexo proofs for non-coinbase blocks
    // ─────────────────────────────────────────────────────────────────────────
    // This check remains for backward compatibility with Phase 5 IBD mode
    // Coinbase-only blocks don't need proofs
    // ─────────────────────────────────────────────────────────────────────────
    if (validation_mode_ == ValidationMode::STATELESS) {
        // Check if block has any non-coinbase inputs
        bool has_utxo_spends = false;
        for (const auto& tx : block.vtx) {
            if (!tx.IsCoinbase()) {
                has_utxo_spends = true;
                break;
            }
        }

        if (has_utxo_spends && !block.utreexo.has_value()) {
            error = "stateless-ibd-requires-utreexo (PROOF_MISSING)";
            return false;  // REJECT - stateless validation impossible without proofs for UTXO spends
        }

        if (!consensus_utxo_set_) {
            error = "stateless-ibd-requires-utreexo-forest";
            return false;  // REJECT - accumulator not initialized
        }

        std::cout << "🔍 [Stateless IBD] Block " << height << " validation (proof-based)" << std::endl;
    }

    // Phase 5: Accumulator Root Continuity Check (Critical for Stateless IBD)
    // ═════════════════════════════════════════════════════════════════════════
    // Verify that block's root_before matches our current accumulator state.
    // This is analogous to header chain verification (prev_block_hash matching).
    //
    // **Why This Matters:**
    // - Prevents accumulator chain breaks
    // - Ensures proofs are for the correct forest state
    // - Detects reorgs immediately
    // - Makes accumulator tampering impossible
    //
    // **Security Model:**
    // - PoW secures header chain
    // - Root continuity secures accumulator chain
    // - Together = full validation without UTXO DB
    // ═════════════════════════════════════════════════════════════════════════
    // Root continuity check: Handled by StatelessNode::ValidateBlock.
    // ConnectBlockInternal cannot check this because the canonical forest is
    // maintained by StatelessNode and may be ahead of ActivateBestChain.

    // Phase 4/6: Delta-based Utreexo undo (10-20x more efficient than Phase 3)
    // ═════════════════════════════════════════════════════════════════════════
    // History:
    // - Phase 3: Full forest snapshot (~2-10 KB per block)
    // - Phase 4: Delta only (~100-500 bytes per block)
    // - Phase 6: Snapshot fallback removed (delta is mandatory)
    //
    // Delta tracks:
    // - Deleted leaves (position + hash) - restored during disconnect
    // - Added leaves (hash) - removed during disconnect
    //
    // Proven correct by:
    // - Phase 4 tests: 6/6 passing (roundtrip, order safety, size reduction)
    // - Phase 5 tests: 4/4 passing (stateless IBD, root continuity)
    // ═════════════════════════════════════════════════════════════════════════
    // ═════════════════════════════════════════════════════════════════════════
    // DUAL VALIDATION: UTXO DB + Utreexo Accumulator
    // ═════════════════════════════════════════════════════════════════════════
    // Both systems run in parallel:
    // 1. UTXO DB validates (stateful)
    // 2. Utreexo accumulator validates (stateless commitment)
    // 3. STRICT MODE (default): Reject on mismatch
    // 4. Proven equivalent by test_block_stateless_equivalence
    // ═════════════════════════════════════════════════════════════════════════
    UtreexoDelta delta;
    bool utreexo_validation_active = false;

    std::cout << "🔍 [UTREEXO] Height=" << height
              << " | Forest=" << (consensus_utxo_set_ ? "EXISTS" : "NULL")
              << " | Active=" << IsUtreexoActive(height) << std::endl;

    if (consensus_utxo_set_) {
        if (IsUtreexoActive(height)) {
            std::cout << "✅ [UTREEXO] Active - dual validation enabled" << std::endl;

            // ═══════════════════════════════════════════════════════════════════════
            // PRE-VALIDATION INVARIANT: Forest state must match expected root_before
            // ═══════════════════════════════════════════════════════════════════════
            // This catches blocks arriving out of order or forest corruption.
            // The block's accumulator_root_before is what the prover claims the
            // forest looked like BEFORE this block. Our forest MUST match.
            // ═══════════════════════════════════════════════════════════════════════
            // Statefully validating nodes do not need prover-supplied root_before metadata
            // to be authoritative; the local UTXO+forest state is the source of truth.
            // Pre-validation invariant: Handled by StatelessNode in STATELESS mode.
            // ConnectBlockInternal cannot check forest state because StatelessNode
            // maintains the canonical forest and may be ahead of ActivateBestChain.
            // Stateful nodes: local forest is authoritative. Block's
            // accumulator_root_before is prover metadata — mismatch is
            // informational only (e.g. stale relay cache). ConnectBlock
            // validates the forest transition directly.
            // STATELESS nodes enforce this in StatelessNode instead.
            if (validation_mode_ != ValidationMode::STATELESS &&
                block.utreexo.has_value() &&
                !block.utreexo->accumulator_root_before.empty()) {
                UtreexoHash current_root = consensus_utxo_set_->GetForest().getCommitment();
                if (current_root != block.utreexo->accumulator_root_before) {
                    std::cout << "[UTREEXO] root_before metadata mismatch (ignored — local forest authoritative)" << std::endl;
                }
            }

            // Enforce Phase 3 height limit (prevents accidental production deployment)
            bool snapshot_allowed = IsUtreexoSnapshotAllowed(height);
            std::cout << "🔍 [UTREEXO] Snapshot allowed: " << snapshot_allowed << std::endl;

            if (!snapshot_allowed) {
                std::cout << "❌ [UTREEXO] BLOCKED by height limit!" << std::endl;
                error = "utreexo-phase3-height-limit-exceeded";
                return false;
            }

            // Initialize delta with current state
            delta.numLeavesBefore = consensus_utxo_set_->GetForest().getNumLeaves();
            utreexo_validation_active = true;
            std::cout << "📊 [UTREEXO] Delta initialized | Leaves: "
                      << delta.numLeavesBefore << std::endl;
        } else {
            std::cout << "⏳ [UTREEXO] Not active yet - accumulator tracking only" << std::endl;
            // Still track leaves even if not enforcing yet
            delta.numLeavesBefore = consensus_utxo_set_->GetForest().getNumLeaves();
            utreexo_validation_active = true;  // Track but don't enforce
        }
        (void)utreexo_validation_active; // flag tracked for future enforcement
    } else {
        std::cout << "⚠️  [UTREEXO] WARNING: Forest is NULL!" << std::endl;
        std::cout << "⚠️  [UTREEXO] Validation SKIPPED - UTXO DB only" << std::endl;
        std::cout << "⚠️  [UTREEXO] Check ChainstateService initialization logs" << std::endl;
        // Continue with UTXO DB validation only - don't fail here
    }

    // Coinbase must be first transaction
    if (block.vtx.empty()) {
        error = "Block has no transactions";
        return false;
    }

    // Use coinbase transaction directly (block.vtx contains Transaction objects)
    const Transaction& coinbase_tx = block.vtx[0];

    uint64_t total_fees = 0;

    // Phase 8: Build spent_outputs index for stateless validation
    size_t global_spent_output_index = 0;

    // ═════════════════════════════════════════════════════════════════════════
    // INV-3: Double-spend tracking for stateless mode
    // ═════════════════════════════════════════════════════════════════════════
    // Stateful mode catches double-spends via SpendCoin returning nullptr.
    // Stateless mode relies solely on Utreexo proofs — this adds defense in depth.
    // ═════════════════════════════════════════════════════════════════════════
    std::unordered_set<OutPoint> spent_in_block;

    // Clear intra-block UTXO overlay for this block (enables tx chaining)
    intra_block_utxos_.clear();

    // Process all non-coinbase transactions
    for (size_t i = 1; i < block.vtx.size(); i++) {
        const Transaction& tx = block.vtx[i];

        // Phase E.3: Check CPU budget timeout (every 10 transactions for performance)
        if (i % 10 == 1 && cpu_budget.isTimedOut()) {
            error = "Block validation timeout exceeded";
            return false;
        }

        // ═════════════════════════════════════════════════════════════════════════
        // V5 Freeze Fork: Three consensus gates for non-coinbase txs at/above
        // FREEZE_FORK_ACTIVATION_HEIGHT. Spec: docs/consensus/V5_FREEZE_FORK_SPEC.md
        //
        // These gates stop new CT / ring / ring-covenant / non-Taproot outputs
        // from entering the chain. Existing pre-activation UTXOs remain spendable
        // under their original rules because the legacy validators stay wired in;
        // the freeze only blocks creation of new such outputs.
        // ═════════════════════════════════════════════════════════════════════════
        // v7: freeze-fork gates removed along with ring/CT stack.

        // Phase 8: Stateless validation path - validate using proof data
        // ─────────────────────────────────────────────────────────────────────────
        // In stateless mode, we get UTXO values from block.utreexo.spent_outputs
        // instead of querying the UTXO database. This enables full validation
        // without storing the entire UTXO set.
        // ─────────────────────────────────────────────────────────────────────────
        uint64_t total_input_value = 0;
        std::vector<UTXOEntry> fee_input_utxos;

        if (validation_mode_ == ValidationMode::STATELESS) {
            // STATELESS PATH: Validate using proof data only
            if (!block.utreexo.has_value()) {
                error = "stateless-validation-missing-proofs (PROOF_MISSING)";
                return false;
            }

            const auto& utreexo_data = block.utreexo.value();

            // Build vector of all UTXOs for this transaction (needed for BIP341 Taproot sighash)
            fee_input_utxos.reserve(tx.vin.size());

            size_t temp_spent_idx = global_spent_output_index;
            for (size_t input_idx = 0; input_idx < tx.vin.size(); input_idx++) {
                if (temp_spent_idx >= utreexo_data.spent_outputs.size()) {
                    error = "stateless-validation-insufficient-spent-outputs (PROOF_OUTPOINT_MISMATCH)";
                    return false;
                }

                const auto& spent_output = utreexo_data.spent_outputs[temp_spent_idx];
                temp_spent_idx++;

                fee_input_utxos.emplace_back(
                    AmountUna::Una(spent_output.value),
                    spent_output.scriptPubKey,
                    0,      // height unknown in stateless mode
                    false,  // assume not coinbase (maturity validated by proof)
                    spent_output.is_confidential,
                    spent_output.commitment
                );
            }

            // Phase 8: Validate each input using spent_outputs data
            // This includes both value checking AND script validation
            for (size_t input_idx = 0; input_idx < tx.vin.size(); input_idx++) {
                const auto& input = tx.vin[input_idx];

                // INV-3: Double-spend tracking in stateless mode
                OutPoint outpoint(input.prevout.txid, input.prevout.vout);
                if (!spent_in_block.insert(outpoint).second) {
                    error = "double-spend-in-block: outpoint " +
                            input.prevout.txid.AsUint256().GetHex() + ":" +
                            std::to_string(input.prevout.vout) +
                            " spent twice in same block (stateless)";
                    return false;
                }

                // Check we have spent_outputs for this input
                if (global_spent_output_index >= utreexo_data.spent_outputs.size()) {
                    error = "stateless-validation-insufficient-spent-outputs (PROOF_OUTPOINT_MISMATCH)";
                    return false;
                }

                const auto& spent_output = utreexo_data.spent_outputs[global_spent_output_index];
                global_spent_output_index++;

                // Accumulate input value
                total_input_value += spent_output.value;

                const auto& utxo = fee_input_utxos[input_idx];

                // Phase 8: CRITICAL - Script validation for stateless mode
                // This validates signatures and script execution
                // Pass all_utxos for BIP341 Taproot sighash computation
                ScriptValidationResult script_result = ValidateSpend(tx, input_idx, utxo, height, fee_input_utxos);

                if (script_result != ScriptValidationResult::OK) {
                    // Map validation result to error message
                    const char* reason = "unknown";
                    switch (script_result) {
                        case ScriptValidationResult::INVALID_SIGNATURE:
                            reason = "invalid signature";
                            break;
                        case ScriptValidationResult::INVALID_SCRIPT:
                            reason = "invalid script format";
                            break;
                        case ScriptValidationResult::UNSUPPORTED_SCRIPT:
                            reason = "unsupported script type";
                            break;
                        case ScriptValidationResult::EXTRACT_FAILED:
                            reason = "failed to extract sig/pubkey";
                            break;
                        default:
                            break;
                    }

                    error = "Stateless validation: Script validation failed for input " +
                            std::to_string(input_idx) + ": " + reason + " (SCRIPT_VERIFY_FAILED)";
                    return false;
                }
            }

        } else if (ibd_config_.isStateful()) {
            // STATEFUL PATH: Validate using UTXO database

            // Validate transaction
            if (!ValidateTransaction(tx, height, false, total_input_value, error)) {
                return false;
            }
        } else {
            // Fallback: Legacy Phase 5 IBD stateless path
            if (!ValidateTransaction(tx, height, false, total_input_value, error)) {
                return false;
            }
        }

        // Phase M.4: GetTxid() returns TxId
        const TxId txid = tx.GetTxid();

        // INV-2: DEFERRED SPENDS - validate UTXO exists, save to undo, but don't erase yet
        // Phase 8: Skip UTXO DB operations in stateless mode
        if (validation_mode_ == ValidationMode::STATEFUL && ibd_config_.isStateful()) {
            for (const auto& input : tx.vin) {
                // Look up UTXO from consensus set (Phase 2: pure in-memory)
                OutPoint outpoint(input.prevout.txid, input.prevout.vout);
                const UTXOEntry* utxo_ptr = consensus_utxo_set_->GetCoin(outpoint);

                // Fallback: check intra-block overlay (UTXOs created by earlier txs in same block)
                if (!utxo_ptr) {
                    auto it = intra_block_utxos_.find(outpoint);
                    if (it != intra_block_utxos_.end()) {
                        utxo_ptr = &it->second;
                    }
                }

                if (!utxo_ptr) {
                    // Phase M.0: .GetHex() only for error messages (presentation boundary)
                    error = "Input UTXO not found: " + input.prevout.txid.AsUint256().GetHex() + ":" + std::to_string(input.prevout.vout);
                    std::cerr << "❌ [UTXO-LOOKUP] " << error
                              << " | height=" << height
                              << " | block=" << block_hash.GetHex().substr(0, 16) << "..."
                              << " | mode=" << ValidationModeToString(validation_mode_)
                              << " | backend=" << ActiveUtxoBackendName(consensus_utxo_set_)
                              << std::endl;
                    return false;
                }

                // Get consensus UTXOEntry directly from interface
                const UTXOEntry& utxo = *utxo_ptr;
                fee_input_utxos.push_back(utxo);

                // Save to undo data (Phase M.4: input.prevout.txid is TxId)
                undo.AddSpentCoin(input.prevout.txid.AsUint256(), input.prevout.vout, utxo);

                // DEFERRED: Collect spend for later commitment (after proof verification)
                // INV-2: No UTXO state mutation before proof verification
                pending_utxo_spends.emplace_back(outpoint, UndoEntry{input.prevout.txid.AsUint256(), input.prevout.vout, utxo});
            }

            // Erase spent intra-block UTXOs from overlay to prevent double-spend
            for (const auto& input : tx.vin) {
                OutPoint outpoint(input.prevout.txid, input.prevout.vout);
                intra_block_utxos_.erase(outpoint);
            }
        }
        // Phase 5: Stateless mode - UTXO spent validation happens via Utreexo proofs below

        // Create new outputs
        // TODO: BlockValidator should use consensus::UTXOSet, not wallet::UTXOIndex
        // Phase 8: Skip UTXO DB operations in stateless mode
        if (validation_mode_ == ValidationMode::STATEFUL && ibd_config_.isStateful()) {
            for (size_t n = 0; n < tx.vout.size(); n++) {
                const auto& output = tx.vout[n];

                // Create consensus OutPoint + UTXOEntry
                OutPoint outpoint(txid, static_cast<uint32_t>(n));
                UTXOEntry entry(
                    output.value,
                    std::vector<uint8_t>(output.scriptPubKey.begin(), output.scriptPubKey.end()),
                    height,
                    false,  // is_coinbase = false
                    output.is_confidential,
                    output.commitment
                );

                // DEFERRED: Collect UTXO for later addition (after root verification)
                // This prevents state mutation before validation
                pending_utxo_additions.emplace_back(outpoint, entry);

                // Populate intra-block overlay so subsequent txs can reference this output
                intra_block_utxos_[outpoint] = entry;
            }
        }
        // Phase 5: Stateless mode - outputs tracked only via Utreexo accumulator
        
        uint64_t total_output_value = SumOutputs(tx);
        uint64_t tx_fee = 0;
        if (!ComputeValidatedTransactionFee(tx, fee_input_utxos, total_input_value, total_output_value, tx_fee, error)) {
            return false;
        }
        if (UsesShieldedValueSemantics(tx)) {
            int64_t tx_delta = 0;
            if (validation_mode_ == ValidationMode::STATELESS) {
                if (!ValidateShieldedTransactionBundle(
                        tx,
                        height,
                        total_input_value,
                        total_output_value,
                        tx_fee,
                        static_cast<shielded::CommitmentTree*>(shielded_tree_),
                        static_cast<shielded::NullifierSet*>(shielded_nullifiers_),
                        static_cast<shielded::AnchorHistory*>(shielded_anchor_history_),
                        error,
                        &tx_delta)) {
                    return false;
                }
            } else {
                if (!ComputeTransparentValueDelta(total_input_value, total_output_value,
                                                  tx_fee, tx_delta, error)) {
                    return false;
                }
            }
            pending_shielded_deltas.push_back(tx_delta);
        }
        total_fees += tx_fee;
    }

    // Phase 8: Verify all spent_outputs were consumed (stateless validation sanity check)
    if (validation_mode_ == ValidationMode::STATELESS && block.utreexo.has_value()) {
        const auto& utreexo_data = block.utreexo.value();
        if (global_spent_output_index != utreexo_data.spent_outputs.size()) {
            error = "stateless-validation-spent-outputs-mismatch: used " +
                    std::to_string(global_spent_output_index) + ", provided " +
                    std::to_string(utreexo_data.spent_outputs.size()) +
                    " (PROOF_OUTPOINT_MISMATCH)";
            return false;
        }
    }

    // Validate coinbase reward
    // Subsidy is purely height-based per Dinero monetary policy (no total_issued dependency)
    uint64_t subsidy = GetBlockSubsidy(height);
    uint64_t expected_reward = subsidy + total_fees;
    
    uint64_t coinbase_output_value = SumOutputs(coinbase_tx);
    
    if (coinbase_output_value > expected_reward) {
        error = "Coinbase pays too much: " + std::to_string(coinbase_output_value) + 
               " > " + std::to_string(expected_reward);
        return false;
    }
    
    // Add coinbase outputs to UTXO set
    // Phase M.4: GetTxid() returns TxId
    // Phase 8: Skip UTXO DB operations in stateless mode
    const TxId coinbase_txid = coinbase_tx.GetTxid();
    if (validation_mode_ == ValidationMode::STATEFUL && ibd_config_.isStateful()) {
        for (size_t n = 0; n < coinbase_tx.vout.size(); n++) {
            const auto& output = coinbase_tx.vout[n];

            // Create consensus OutPoint + UTXOEntry
            OutPoint outpoint(coinbase_txid, static_cast<uint32_t>(n));
            UTXOEntry entry(
                output.value,
                std::vector<uint8_t>(output.scriptPubKey.begin(), output.scriptPubKey.end()),
                height,
                true,  // is_coinbase = true (important for maturity checking)
                output.is_confidential,
                output.commitment
            );

            // DEFERRED: Collect coinbase UTXO for later addition (after root verification)
            // This prevents state mutation before validation
            pending_utxo_additions.emplace_back(outpoint, entry);
        }
    }
    // Phase 5: Stateless mode - coinbase outputs tracked only via Utreexo accumulator

    // ═════════════════════════════════════════════════════════════════════════
    // Phase 3: Utreexo Proof Enforcement (Consensus-Critical)
    // ═════════════════════════════════════════════════════════════════════════
    // Verify Utreexo proofs if present.
    // - Log results for diagnostics
    // - REJECT blocks if IsUtreexoActive(height) and proof invalid
    // ═════════════════════════════════════════════════════════════════════════

    // Proof verification in STATELESS mode is handled by StatelessNode::ValidateBlock.
    // ConnectBlockInternal cannot verify proofs against the forest because StatelessNode
    // maintains the canonical forest and may be ahead of ActivateBestChain.
    // In STATEFUL mode, this section is skipped (validation_mode_ != STATELESS).
    if (false && validation_mode_ == ValidationMode::STATELESS &&
        block.utreexo.has_value() &&
        consensus_utxo_set_) {
        std::cout << "🔍 [Utreexo] Block " << height << " has Utreexo proof data" << std::endl;

        const auto& utreexo_data = block.utreexo.value();

        // 1. Get current accumulator state (BEFORE applying block)
        UtreexoHash current_root = consensus_utxo_set_->GetForest().getCommitment();
        UtreexoHash expected_root_before = utreexo_data.accumulator_root_before;

        // 2. Verify root_before matches current state
        bool root_matches = (current_root == expected_root_before);
        if (!root_matches) {
            std::cout << "⚠️  [Utreexo] Root mismatch!" << std::endl;
            std::cout << "   Current root:  ";
            for (auto b : current_root) std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)b;
            std::cout << std::endl << "   Expected root: ";
            for (auto b : expected_root_before) std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)b;
            std::cout << std::dec << std::endl;

            // Phase 3: Enforce if Utreexo active
            if (IsUtreexoActive(height)) {
                error = "utreexo-root-before-mismatch (ROOT_MISMATCH)";
                return false;
            }
        } else {
            std::cout << "✅ [Utreexo] Root before matches current state" << std::endl;
        }

        // 3. Build expected targets from spent inputs (STATELESS VALIDATION)
        // ─────────────────────────────────────────────────────────────────────────
        // ✅ STATELESS: Compute leaf hashes using block-provided spent_outputs
        // ─────────────────────────────────────────────────────────────────────────
        // The block carries spent output metadata (value + scriptPubKey) for each
        // spent input. This enables validation WITHOUT accessing local UTXO database.
        //
        // This is the core benefit of Utreexo: stateless validation.
        // ─────────────────────────────────────────────────────────────────────────

        std::vector<UtreexoHash> expected_targets;
        size_t spend_count = 0;
        size_t spent_outputs_index = 0;

        // Iterate through transactions and their inputs
        for (size_t i = 0; i < block.vtx.size(); i++) {
            const Transaction& tx = block.vtx[i];
            bool is_coinbase = (i == 0);

            if (!is_coinbase) {
                for (const auto& input : tx.vin) {
                    // Verify spent_outputs array has enough entries
                    if (spent_outputs_index >= utreexo_data.spent_outputs.size()) {
                        std::cout << "❌ [Utreexo] Insufficient spent_outputs: expected at least "
                                  << (spent_outputs_index + 1) << ", got "
                                  << utreexo_data.spent_outputs.size() << std::endl;

                        if (IsUtreexoActive(height)) {
                            error = "utreexo-insufficient-spent-outputs (PROOF_OUTPOINT_MISMATCH)";
                            return false;
                        }
                        break;
                    }

                    // Get spent output data from block (stateless!)
                    const auto& spent_output = utreexo_data.spent_outputs[spent_outputs_index];
                    spent_outputs_index++;
                    const uint64_t leaf_value = GetUtreexoLeafAmount(spent_output);

                    // Compute leaf hash for this spent UTXO
                    // Phase M.4: input.prevout.txid is TxId, extract uint256 for HashUTXO
                    UtreexoHash leaf_hash = HashUTXO(
                        input.prevout.txid.AsUint256(),
                        input.prevout.vout,
                        leaf_value,
                        spent_output.scriptPubKey
                    );

                    expected_targets.push_back(leaf_hash);
                    spend_count++;
                }
            }
        }

        // Verify we consumed exactly the right number of spent_outputs
        if (spent_outputs_index != utreexo_data.spent_outputs.size()) {
            std::cout << "❌ [Utreexo] Spent outputs count mismatch: used "
                      << spent_outputs_index << ", provided "
                      << utreexo_data.spent_outputs.size() << std::endl;

            if (IsUtreexoActive(height)) {
                error = "utreexo-spent-outputs-count-mismatch (PROOF_OUTPOINT_MISMATCH)";
                return false;
            }
        }

        std::cout << "📊 [Utreexo] Block spends: " << spend_count << " UTXOs" << std::endl;
        std::cout << "📊 [Utreexo] Proof targets: " << utreexo_data.spend_proof.targets.size() << std::endl;
        std::cout << "📊 [Utreexo] Proof hashes:  " << utreexo_data.spend_proof.proof_hashes.size() << std::endl;
        std::cout << "📊 [Utreexo] Spent outputs: " << utreexo_data.spent_outputs.size() << std::endl;

        // 4. Compare proof targets with expected targets
        // ─────────────────────────────────────────────────────────────────────────
        // NOTE: Target order comparison is DIAGNOSTIC-ONLY.
        // Real Utreexo proofs do not guarantee ordering.
        // Batched proofs may reorder targets internally.
        // Final proof verification must be order-independent (treat targets as a set).
        // ─────────────────────────────────────────────────────────────────────────

        bool targets_match = (expected_targets.size() == utreexo_data.spend_proof.targets.size());
        if (targets_match) {
            for (size_t i = 0; i < expected_targets.size(); i++) {
                if (expected_targets[i] != utreexo_data.spend_proof.targets[i]) {
                    targets_match = false;
                    std::cout << "❌ [Utreexo] Target mismatch at index " << i << std::endl;
                    break;
                }
            }
        } else {
            std::cout << "❌ [Utreexo] Target count mismatch (expected: "
                     << expected_targets.size() << ", got: "
                     << utreexo_data.spend_proof.targets.size() << ")" << std::endl;
        }

        if (targets_match && !expected_targets.empty()) {
            std::cout << "✅ [Utreexo] All targets match expected values" << std::endl;
        } else if (expected_targets.empty()) {
            std::cout << "ℹ️  [Utreexo] No spends to verify (coinbase-only block)" << std::endl;
        }

        // Phase 3: Enforce target matching if Utreexo active
        if (!targets_match && !expected_targets.empty() && IsUtreexoActive(height)) {
            error = "utreexo-proof-target-mismatch (PROOF_OUTPOINT_MISMATCH)";
            return false;
        }

        // 5. Batched proof verification against accumulator
        // ─────────────────────────────────────────────────────────────────────────
        // Verify that the batched Merkle proof is cryptographically valid.
        // This is CONSENSUS-CRITICAL when IsUtreexoActive(height).
        // ─────────────────────────────────────────────────────────────────────────

        bool proof_valid = consensus_utxo_set_->GetForest().verifyBatchProof(
            utreexo_data.spend_proof.targets,
            utreexo_data.spend_proof.proof_hashes
        );

        if (!proof_valid) {
            std::cout << "❌ [Utreexo] Batched proof verification FAILED" << std::endl;
            if (IsUtreexoActive(height)) {
                error = "utreexo-proof-invalid (PROOF_INVALID)";
                return false;
            }
        } else {
            std::cout << "✅ [Utreexo] Batched proof verification passed" << std::endl;
        }

        std::cout << "✅ [Utreexo] Verification complete" << std::endl;
    }

    // ═════════════════════════════════════════════════════════════════════════
    // STATELESS MODE: Early return — skip forest clone/mutation/root verification
    // ═════════════════════════════════════════════════════════════════════════
    // In STATELESS (CSN) mode, the forest is maintained by
    // StatelessNode::ApplyBlockToAccumulator, which directly mutates the
    // canonical forest after proof validation succeeds.
    //
    // ConnectBlockInternal's forest-clone path (below) would double-mutate the
    // forest: StatelessNode already applied block N's mutations, then this
    // function clones the mutated forest, applies block N again, and commits.
    // Result: ROOT_MISMATCH at the first block with spends.
    //
    // Proof-based validation (above) already verified:
    //   1. Root continuity (accumulator_root_before == current forest state)
    //   2. Proof targets match expected UTXO leaf hashes
    //   3. Batched Merkle proof is cryptographically valid
    //   4. Script validation via spent_outputs
    //   5. Coinbase reward <= subsidy + fees
    // ═════════════════════════════════════════════════════════════════════════
    if (validation_mode_ == ValidationMode::STATELESS) {
        // Set computed root from header (already proven valid by proof verification)
        computed_utreexo_root = block.header.utreexo_root;

        block_connect_success = true;
        std::cout << "✅ [STATELESS] Block " << height
                  << " validated via proofs — skipping forest-clone path" << std::endl;
        return true;
    }

    // ═════════════════════════════════════════════════════════════════════════
    // TRANSITION PROOF CROSS-CHECK (Defense in Depth)
    // ═════════════════════════════════════════════════════════════════════════
    // Generate and verify a stump-based transition proof alongside the
    // forest-clone path below. Both must agree. The transition proof provides:
    //   - Independent verification of state transition correctness
    //   - Cross-validation between forest and stump implementations
    //   - Foundation for future stateless-primary mode
    // ═════════════════════════════════════════════════════════════════════════
    if (block.utreexo.has_value() && consensus_utxo_set_ &&
        verify_root && IsUtreexoActive(height)) {
        const auto& utreexo_data = block.utreexo.value();

        auto transition = UtreexoTransitionProof::generate(
            consensus_utxo_set_->GetForest(),
            block,
            utreexo_data.spend_proof);

        if (!transition.verify()) {
            // Defense-in-depth cross-check: warn but don't block consensus.
            // Primary validation is the forest-clone path below.
            std::cerr << "⚠️  [UTREEXO] Transition proof cross-check FAILED at height "
                      << height << " (non-fatal, primary forest-clone will validate)" << std::endl;
        } else {
            std::cout << "✅ [UTREEXO] Transition proof cross-check passed (height "
                      << height << ")" << std::endl;
        }
    }

    // v0.14.0.4: Utreexo Commitment Enforcement (Consensus-Critical)
    // ═════════════════════════════════════════════════════════════════════════
    // This is the canonical enforcement point for Utreexo state commitments.
    // Utreexo validation happens EXACTLY like UTXO validation - never in mining,
    // never in RPC, never in mempool. Consensus layer only.
    // ═════════════════════════════════════════════════════════════════════════

    if (consensus_utxo_set_) {
        std::cout << "🔍 [DEBUG] Utreexo forest exists, IsUtreexoActive(" << height << ") = " << IsUtreexoActive(height) << std::endl;
        std::cout << "🔍 [DEBUG] Forest leaves before: " << consensus_utxo_set_->GetForest().getNumLeaves() << std::endl;

        // 1. Clone current accumulator state (non-destructive simulation)
        UtreexoForest snapshot = consensus_utxo_set_->GetForest().clone();

        // Phase 8: Reset spent_outputs index for accumulator update
        size_t accumulator_spent_index = 0;

        // ═══════════════════════════════════════════════════════════════════════════
        // UTREEXO CANONICAL ORDER: REMOVE ALL → ADD ALL
        // ═══════════════════════════════════════════════════════════════════════════
        // Utreexo has ONE legal order per block:
        //   1. REMOVE all spent UTXOs (from previous state)
        //   2. ADD all new outputs (including coinbase)
        //   3. Commit root
        //
        // This is NOT per-transaction interleaved - it's two separate passes.
        //
        // INTRA-BLOCK EPHEMERAL UTXOS:
        // When transactions chain within a block (tx2 spends change from tx1),
        // the intermediate outputs are ephemeral — they never enter or leave the
        // Utreexo forest. We skip them in both REMOVE and ADD passes.
        // ═══════════════════════════════════════════════════════════════════════════

        // PRE-SCAN: Identify intra-block ephemeral UTXOs
        std::unordered_map<OutPoint, size_t> val_intra_block_outputs;
        for (size_t tx_idx = 0; tx_idx < block.vtx.size(); tx_idx++) {
            TxId txid = block.vtx[tx_idx].GetTxid();
            for (uint32_t n = 0; n < block.vtx[tx_idx].vout.size(); n++) {
                val_intra_block_outputs[OutPoint(txid, n)] = tx_idx;
            }
        }
        std::unordered_set<OutPoint> val_intra_block_spends;
        for (size_t i = 0; i < block.vtx.size(); i++) {
            if (i == 0) continue;  // coinbase
            for (const auto& input : block.vtx[i].vin) {
                OutPoint outpoint(input.prevout.txid, input.prevout.vout);
                if (val_intra_block_outputs.count(outpoint)) {
                    val_intra_block_spends.insert(outpoint);
                }
            }
        }
        if (!val_intra_block_spends.empty()) {
            std::cout << "   [Internal] " << val_intra_block_spends.size()
                      << " intra-block ephemeral UTXOs (skipped in forest)" << std::endl;
        }

        // PASS 1: REMOVE ALL spent UTXOs (entire block)
        // Skip intra-block spends — those UTXOs were never in the forest
        for (size_t i = 0; i < block.vtx.size(); i++) {
            const Transaction& tx = block.vtx[i];
            bool is_coinbase = (i == 0);

            // Skip coinbase (no inputs to spend)
            if (is_coinbase) continue;

            for (const auto& input : tx.vin) {
                OutPoint outpoint(input.prevout.txid, input.prevout.vout);

                // Skip intra-block spends (ephemeral UTXOs never enter the forest)
                if (val_intra_block_spends.count(outpoint)) {
                    // Still advance accumulator_spent_index for stateless mode
                    if (validation_mode_ == ValidationMode::STATELESS && block.utreexo.has_value()) {
                        accumulator_spent_index++;
                    }
                    continue;
                }

                // Phase 8: Stateless mode - use spent_outputs from proof data
                UtreexoHash leafHash;

                if (validation_mode_ == ValidationMode::STATELESS && block.utreexo.has_value()) {
                    // STATELESS PATH: Get UTXO data from proof
                    const auto& utreexo_data = block.utreexo.value();
                    if (accumulator_spent_index >= utreexo_data.spent_outputs.size()) {
                        // Shouldn't happen - we validated this earlier
                        continue;
                    }

                    const auto& spent_output = utreexo_data.spent_outputs[accumulator_spent_index];
                    accumulator_spent_index++;
                    const uint64_t leaf_value = GetUtreexoLeafAmount(spent_output);

                    // Hash the UTXO being spent (using proof data)
                    // Phase M.4: input.prevout.txid is TxId, extract uint256 for HashUTXO
                    leafHash = HashUTXO(
                        input.prevout.txid.AsUint256(),
                        input.prevout.vout,
                        leaf_value,
                        spent_output.scriptPubKey
                    );
                } else {
                    // STATEFUL PATH: Look up UTXO from consensus set (Phase 2: pure in-memory)
                    const UTXOEntry* utxo_ptr = consensus_utxo_set_->GetCoin(outpoint);
                    if (!utxo_ptr) {
                        // This shouldn't happen - we already validated the transaction
                        continue;
                    }

                    const auto& utxo = *utxo_ptr;
                    const uint64_t leaf_value = GetUtreexoLeafAmount(utxo);

                    // Hash the UTXO being spent
                    // Phase M.4: input.prevout.txid is TxId, extract uint256 for HashUTXO
                    // Phase M.6.2: Extract raw value from AmountUna
                    leafHash = HashUTXO(
                        input.prevout.txid.AsUint256(),
                        input.prevout.vout,
                        leaf_value,
                        utxo.scriptPubKey
                    );
                }

                // Try to find this leaf in the accumulator
                auto position_opt = snapshot.findLeafPosition(leafHash);
                if (!position_opt.has_value()) {
                    if (IsUtreexoActive(height)) {
                        error = "utreexo-leaf-missing: " + outpoint.ToString();
                        return false;
                    }
                    continue;
                }

                // Phase 4: Record deletion in delta (for undo)
                if (IsUtreexoActive(height)) {
                    delta.recordDelete(position_opt.value(), leafHash);
                }

                // Remove the UTXO from accumulator. The snapshot is our own
                // forest clone — we trust the position returned by
                // findLeafPosition above, so there is no adversarial proof
                // to re-verify. The proof-based variant hit a stale cached
                // root for every covenant spend on Apr 13 2026 (the
                // release blocker for the privacy stack).
                bool removed = snapshot.removeAtKnownPosition(position_opt.value(), leafHash);
                if (!removed) {
                    if (IsUtreexoActive(height)) {
                        error = "utreexo-remove-failed: " + outpoint.ToString();
                        return false;
                    }
                    std::cerr << "⚠️  [Utreexo] Failed to remove UTXO from accumulator" << std::endl;
                }
            }
        }

        // PASS 2: ADD ALL new outputs (entire block, including coinbase)
        // Skip outputs consumed within this block (ephemeral)
        for (size_t i = 0; i < block.vtx.size(); i++) {
            const Transaction& tx = block.vtx[i];
            TxId txid = tx.GetTxid();  // Phase M.4: TxId semantic type

            for (size_t n = 0; n < tx.vout.size(); n++) {
                OutPoint out(txid, static_cast<uint32_t>(n));

                // Skip outputs consumed within this block (ephemeral)
                if (val_intra_block_spends.count(out)) {
                    std::cout << "   [Internal] Skipping ephemeral output " << n << std::endl;
                    continue;
                }

                const auto& output = tx.vout[n];
                const uint64_t leaf_value = GetUtreexoLeafAmount(output);

                // Hash the new UTXO
                // Phase M.4: txid is TxId, extract uint256 for HashUTXO
                // Phase M.6.2: Extract raw value from AmountUna
                UtreexoHash leafHash = HashUTXO(
                    txid.AsUint256(),
                    static_cast<uint32_t>(n),
                    leaf_value,
                    std::vector<uint8_t>(output.scriptPubKey.begin(), output.scriptPubKey.end())
                );

                {
                    std::ostringstream lh;
                    for (size_t b = 0; b < std::min(leafHash.size(), size_t(8)); b++)
                        lh << std::hex << std::setfill('0') << std::setw(2) << (int)leafHash[b];
                    std::cout << "   [Internal] Adding output " << n << ": "
                              << "value=" << leaf_value
                              << ", spk_size=" << output.scriptPubKey.size()
                              << ", is_ct=" << output.is_confidential
                              << ", txid=" << txid.AsUint256().GetHex().substr(0, 16)
                              << ", leaf=" << lh.str()
                              << std::endl;
                }

                // Phase 11a: Add to accumulator and capture assigned position
                uint64_t position = snapshot.add(leafHash);
                if (position == UINT64_MAX) {
                    // Diagnostic: dump txid, vout, and leaf hash for the failing output
                    std::cerr << "❌ [DIAG] utreexo-add-failed at height " << height
                              << " tx[" << i << "] vout=" << n << std::endl;
                    std::cerr << "   [DIAG] txid: " << txid.AsUint256().GetHex() << std::endl;
                    std::cerr << "   [DIAG] value: " << leaf_value
                              << " spk_size: " << output.scriptPubKey.size() << std::endl;
                    std::cerr << "   [DIAG] leaf_hash: ";
                    for (size_t b = 0; b < leafHash.size(); ++b)
                        std::cerr << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(leafHash[b]);
                    std::cerr << std::dec << std::endl;
                    error = "utreexo-add-failed: " +
                            snapshot.describeAddFailure(leafHash);
                    return false;
                }
                std::cout << "   [Internal] Added at position: " << position << std::endl;

                // Phase 4/11a: Record addition in delta with position (for undo + indexing)
                if (IsUtreexoActive(height)) {
                    delta.recordAdd(leafHash, position);
                }
            }
        }

        // 3. Compute AFTER-state root (PURE CONSENSUS)
        std::cout << "   [Internal] Snapshot leaves after: " << snapshot.getNumLeaves() << std::endl;
        UtreexoHash computed_root = snapshot.getCommitment();

        std::ostringstream computed_hex_internal;
        for (size_t i = 0; i < std::min(computed_root.size(), size_t(32)); ++i) {
            computed_hex_internal << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(computed_root[i]);
        }
        std::cout << "   [Internal] Computed root: " << computed_hex_internal.str() << std::endl;

        // 4. Store computed root in output parameter (for mining to use)
        if (computed_root.size() == 32) {
            std::memcpy(computed_utreexo_root.data, computed_root.data(), 32);
        } else {
            computed_utreexo_root.SetNull();
        }

        // 5. ENFORCEMENT (OPTIONAL) - Only if verify_root == true
        // Mining path: verify_root = false (computes root, doesn't verify)
        // Validation path: verify_root = true (computes + verifies root)
        //
        // SHADOW MODE: Compare but DON'T reject blocks based on Utreexo mismatches
        // Goal: Run both systems in parallel for testing/verification
        if (verify_root) {
            // Convert block header commitment (uint256) to UtreexoHash.
            // With v2 commitment, even an empty forest has a non-null root
            // (SHA256 of 2056-byte preimage). Always compare all 32 bytes.
            std::vector<uint8_t> expected_root_bytes(
                block.header.utreexo_root.begin(),
                block.header.utreexo_root.end()
            );

            // Compare computed root with header commitment
            bool roots_match = (computed_root.size() == expected_root_bytes.size() &&
                               std::equal(computed_root.begin(), computed_root.end(), expected_root_bytes.begin()));

            if (!roots_match) {
                // Convert to hex for logging
                std::ostringstream computed_hex, expected_hex;
                for (uint8_t byte : computed_root) {
                    computed_hex << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
                }
                for (uint8_t byte : expected_root_bytes) {
                    expected_hex << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
                }

                // ═════════════════════════════════════════════════════════════════
                // CONSENSUS-CRITICAL: Utreexo Root Mismatch = REJECT
                // ═════════════════════════════════════════════════════════════════
                // A root mismatch is a CONSENSUS FAILURE. In Dinero, Utreexo
                // is consensus-critical from genesis. There is NO shadow mode,
                // NO bypass flag, NO exceptions. Mismatched roots ALWAYS reject.
                // ═════════════════════════════════════════════════════════════════
                std::cerr << "❌ [UTREEXO] ROOT MISMATCH - BLOCK REJECTED" << std::endl;
                std::cerr << "❌ [UTREEXO]   Computed: " << computed_hex.str() << std::endl;
                std::cerr << "❌ [UTREEXO]   Expected: " << expected_hex.str() << std::endl;
                std::cerr << "❌ [UTREEXO]   Block height: " << height << std::endl;

                error = "bad-utreexo-root (ROOT_MISMATCH): computed=" + computed_hex.str() +
                        " expected=" + expected_hex.str();
                return false;
            } else {
                std::cout << "✅ [UTREEXO] Root matches header commitment" << std::endl;
            }
        }

        // 6. Store delta in undo data (Phase 4: for efficient rollback)
        if (IsUtreexoActive(height)) {
            undo.utreexo_delta = delta;
            size_t delta_size = delta.getSize();
            std::cout << "✅ [Phase 4 Delta] Stored delta: "
                      << delta.deletedLeaves.size() << " deletes, "
                      << delta.addedLeaves.size() << " adds, "
                      << delta_size << " bytes" << std::endl;
        }

        // ═════════════════════════════════════════════════════════════════════════
        // VALIDATION-BEFORE-MUTATION: Apply Pending UTXO Changes
        // ═════════════════════════════════════════════════════════════════════════
        // Root verification has PASSED (or was skipped for mining).
        // Now it's safe to commit irreversible state mutations.
        // ═════════════════════════════════════════════════════════════════════════

        // Apply all pending UTXO additions FIRST (before spends)
        // Order matters: intra-block ephemeral UTXOs must be added before they can be spent
        //
        // Phase 3a (gap #4 plan): when ChainstateService::ConnectTip
        // has set an active ConsensusWriteBatch via
        // setActiveConsensusWriteBatch, route additions and spends
        // onto the batch's working copy instead of mutating the live
        // consensus_utxo_set_ here. The batch's Commit() replays
        // them as one block-scoped step. With the batch pointer
        // null (legacy / flag-off path), behavior is byte-identical
        // to before this change.
        auto* active_batch = static_cast<consensus::ConsensusWriteBatch*>(
            active_consensus_write_batch_);
        if (validation_mode_ == ValidationMode::STATEFUL && ibd_config_.isStateful()) {
            std::cout << "✅ [COMMIT] Applying " << pending_utxo_additions.size() << " pending UTXO additions..." << std::endl;
            for (const auto& [outpoint, entry] : pending_utxo_additions) {
                if (active_batch != nullptr) {
                    active_batch->StageUTXOAddition(outpoint, entry);
                    continue;
                }
                if (!consensus_utxo_set_->AddCoin(outpoint, entry)) {
                    // UTXO may already exist in cache if loaded from ChainDB during
                    // validation (BlockAcceptor stores UTXOs before ConnectTip runs).
                    // This is expected for intra-block outputs — safe to continue.
                    if (!consensus_utxo_set_->HaveCoin(outpoint)) {
                        error = "Failed to commit UTXO addition: " + outpoint.ToString();
                        return false;
                    }
                }
            }
            std::cout << "✅ [COMMIT] All " << pending_utxo_additions.size() << " UTXOs committed successfully" << std::endl;
        }

        // INV-2: Commit all deferred UTXO spends (proof verification has passed)
        if (validation_mode_ == ValidationMode::STATEFUL && ibd_config_.isStateful()) {
            std::cout << "✅ [COMMIT] Applying " << pending_utxo_spends.size() << " pending UTXO spends..." << std::endl;
            for (const auto& [outpoint, undo_entry] : pending_utxo_spends) {
                if (active_batch != nullptr) {
                    active_batch->StageUTXOSpend(outpoint);
                    continue;
                }
                if (!consensus_utxo_set_->SpendCoin(outpoint)) {
                    error = "Failed to commit UTXO spend (not found): " +
                            outpoint.ToString();
                    return false;
                }
            }
            std::cout << "✅ [COMMIT] All " << pending_utxo_spends.size() << " spends committed successfully" << std::endl;
        }

        // INV-5: INVARIANT — All pending AddCoin calls completed above.
        // Forest commit MUST follow UTXO map commit. If we reach here,
        // the UTXO map is consistent with the pending state.

        // 7. Commit snapshot to canonical accumulator (AFTER-state becomes current state)
        consensus_utxo_set_->GetForest() = std::move(snapshot);
        std::cout << "🔍 [DEBUG] Forest leaves after commit: " << consensus_utxo_set_->GetForest().getNumLeaves() << std::endl;

        // ═════════════════════════════════════════════════════════════════════════
        // INV-1: Post-commit forest verification
        // ═════════════════════════════════════════════════════════════════════════
        // After move-assigning the snapshot, verify the committed forest root
        // matches what we computed. A mismatch here indicates memory corruption
        // or a move-semantic bug silently corrupting forest state.
        // ═════════════════════════════════════════════════════════════════════════
        {
            UtreexoHash post_commit_root = consensus_utxo_set_->GetForest().getCommitment();
            if (post_commit_root != computed_root) {
                std::cerr << "INVARIANT VIOLATION: forest commit produced wrong root" << std::endl;
                std::cerr << "  Post-commit root: ";
                for (uint8_t b : post_commit_root) std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)b;
                std::cerr << std::endl << "  Expected root:    ";
                for (uint8_t b : computed_root) std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)b;
                std::cerr << std::dec << std::endl;
                // Fatal — memory corruption or move-semantic bug
                std::abort();
            }
        }
    } else {
        // ═════════════════════════════════════════════════════════════════════════
        // CONSENSUS-CRITICAL: Null Forest = FATAL at Active Heights
        // ═════════════════════════════════════════════════════════════════════════
        // In Dinero, Utreexo is consensus-critical from genesis.
        // A null forest at Utreexo-active heights is a FATAL initialization error.
        // There is NO fallback, NO legacy mode, NO bypass.
        // ═════════════════════════════════════════════════════════════════════════
        if (IsUtreexoActive(height)) {
            std::cerr << "❌ [FATAL] Utreexo forest is NULL at active height " << height << std::endl;
            std::cerr << "❌ [FATAL] BlockValidator was not properly initialized" << std::endl;
            std::cerr << "❌ [FATAL] This is a consensus violation - aborting" << std::endl;
            std::abort();
        }

        // Pre-activation heights: Apply UTXO changes only
        // This is ONLY valid for heights before Utreexo activation (genesis bootstrap)
        if (validation_mode_ == ValidationMode::STATEFUL && ibd_config_.isStateful()) {
            // Additions first (intra-block ephemerals must exist before they can be spent)
            for (const auto& [outpoint, entry] : pending_utxo_additions) {
                if (!consensus_utxo_set_->AddCoin(outpoint, entry)) {
                    if (!consensus_utxo_set_->HaveCoin(outpoint)) {
                        error = "Failed to commit UTXO addition (pre-activation): " +
                                outpoint.ToString();
                        return false;
                    }
                }
            }
            // Then spends
            for (const auto& [outpoint, undo_entry] : pending_utxo_spends) {
                if (!consensus_utxo_set_->SpendCoin(outpoint)) {
                    error = "Failed to commit UTXO spend (pre-activation, not found): " +
                            outpoint.ToString();
                    return false;
                }
            }
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // v7 Shielded Pool: Block-level validation + atomic state apply
    // ═════════════════════════════════════════════════════════════════════════
    // Collects ShieldedBundles from shielded transactions (if any), validates
    // cross-tx nullifier uniqueness + global conservation, then applies
    // state changes (commitment tree appends + nullifier inserts) atomically
    // alongside the Utreexo commit above.
    //
    // This runs AFTER all per-tx transparent validation has passed and AFTER
    // the Utreexo state is committed. Shielded state mutations are the last
    // consensus-critical writes before block connect succeeds.
    // ═════════════════════════════════════════════════════════════════════════
    if (shielded_tree_ && shielded_nullifiers_) {
        namespace shld = dinero::consensus::shielded;
        auto* tree = static_cast<shld::CommitmentTree*>(shielded_tree_);
        auto* nullifiers = static_cast<shld::NullifierSet*>(shielded_nullifiers_);

        // Collect shielded bundles in block tx order.
        std::vector<shld::ShieldedBundle> bundles;
        std::vector<int64_t> deltas;
        size_t shielded_tx_index = 0;
        for (size_t i = 1; i < block.vtx.size(); ++i) {
            const auto& tx = block.vtx[i];
            if (tx.IsShielded()) {
                shld::ShieldedBundle bundle;
                auto dec = shld::DeserializeShieldedBundle(
                    tx.shielded_bundle_bytes, &bundle);
                if (dec != shld::BundleDecodeError::Ok) {
                    error = "shielded-bundle-decode-failed at tx " +
                        std::to_string(i) + " (code " +
                        std::to_string(static_cast<int>(dec)) + ")";
                    return false;
                }
                if (shielded_tx_index >= pending_shielded_deltas.size()) {
                    error = "shielded-delta-accounting-mismatch";
                    return false;
                }
                bundles.push_back(std::move(bundle));
                deltas.push_back(pending_shielded_deltas[shielded_tx_index++]);
            }
        }
        if (shielded_tx_index != pending_shielded_deltas.size()) {
            error = "shielded-delta-accounting-mismatch";
            return false;
        }

        if (!bundles.empty()) {
            shld::BlockShieldedContext bctx;
            bctx.existing_nullifiers = nullifiers;
            bctx.pre_block_tree = tree;
            bctx.block_height = height;

            auto berr = shld::ValidateBlockShielded(bundles, deltas, bctx);
            if (berr != shld::BlockValidationError::Ok) {
                error = "shielded-block-validation-failed (code " +
                    std::to_string(static_cast<int>(berr)) + ")";
                return false;
            }

            // Deterministic apply: commitments + nullifiers in block tx order.
            shld::ApplyBlockShielded(bundles, tree, nullifiers, height);
        }

        // Phase 1 of shielded reorg invertibility plan (gap #4): record
        // the post-block tree root in the AnchorHistory window once per
        // connected block, AFTER any of this block's shielded outputs
        // have been appended to the tree. Matches the contract spelled
        // out in include/consensus/shielded/anchor_history.h ("Caller
        // MUST invoke this exactly once per connected block") and the
        // behavior previously implemented by
        // ChainstateService::ReplayShieldedBlockForward (deleted in
        // phase 3b step 6 — option 1 made the recovery maze obsolete).
        // Without this, live-built chains
        // had an empty AnchorHistory and accepted only exact-tip-root
        // shielded spends, while reindexed/recovered chains accepted
        // anchors anywhere in the kDepth=100 window.
        //
        // Gate on shielded activation height — pre-activation blocks
        // can't carry shielded txs and recording a constant empty-tree
        // root for every pre-activation block would just churn the
        // window with redundant entries.
        if (shielded_anchor_history_ &&
            height >= dinero::Params().shielded_activation_height) {
            static_cast<shielded::AnchorHistory*>(shielded_anchor_history_)
                ->RecordRoot(height, tree->Root());
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Phase 2: Block Connect Success
    // ═════════════════════════════════════════════════════════════════════════
    // Store pre-block snapshot in undo data for DisconnectBlock.
    // Mark success to prevent rollback guard from restoring.
    // ═════════════════════════════════════════════════════════════════════════
    const bool has_pre_block_snapshot = snapshot_supported;
    if (has_pre_block_snapshot) {
        undo.pre_block_snapshot = std::move(pre_block_snapshot);
    } else {
        undo.pre_block_snapshot.reset();
    }
    if (!pre_block_shielded_frontier.empty()) {
        undo.pre_block_shielded_frontier = std::move(pre_block_shielded_frontier);
    } else {
        undo.pre_block_shielded_frontier.reset();
    }
    block_connect_success = true;
    std::cout << "✅ [Phase 2] Block " << height
              << " connected successfully, snapshot "
              << (has_pre_block_snapshot ? "stored" : "not available (backend unsupported)")
              << std::endl;

    return true;
}

bool BlockValidator::DisconnectBlock(const Block& block, uint32_t height, const BlockUndo& undo, std::string& error) {
    // Sanity check
    if (undo.height != height) {
        error = "Undo data height mismatch";
        return false;
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Phase 2: SNAPSHOT-RESTORE (Trivial Reorg)
    // ═════════════════════════════════════════════════════════════════════════
    // If we have a pre-block snapshot, just restore it.
    // This is the PRIMARY path for Phase 2.
    //
    // Benefits:
    //   - Guaranteed correctness (no partial state possible)
    //   - DisconnectBlock becomes trivial
    //   - No need to replay undo records
    // ═════════════════════════════════════════════════════════════════════════

    if (undo.pre_block_snapshot.has_value() &&
        consensus_utxo_set_ &&
        consensus_utxo_set_->SupportsSnapshotRestore()) {
        std::cout << "🔄 [Phase 2] DisconnectBlock: Restoring snapshot (trivial reorg)" << std::endl;
        std::cout << "   Block height: " << height << std::endl;
        std::cout << "   Snapshot UTXO count: " << undo.pre_block_snapshot->GetUTXOCount() << std::endl;

        consensus_utxo_set_->Restore(undo.pre_block_snapshot.value());

        // ═════════════════════════════════════════════════════════════════════════
        // INV-4: Post-restore verification — UTXO count must match snapshot
        // ═════════════════════════════════════════════════════════════════════════
        // Catches Restore() implementation bugs silently losing UTXOs.
        // ═════════════════════════════════════════════════════════════════════════
        if (consensus_utxo_set_->GetSetSize() != undo.pre_block_snapshot->GetUTXOCount()) {
            error = "INVARIANT VIOLATION: snapshot restore UTXO count mismatch (restored=" +
                    std::to_string(consensus_utxo_set_->GetSetSize()) + ", expected=" +
                    std::to_string(undo.pre_block_snapshot->GetUTXOCount()) + ")";
            return false;
        }

        if (shielded_tree_ && undo.pre_block_shielded_frontier.has_value()) {
            auto* tree = static_cast<shielded::CommitmentTree*>(shielded_tree_);
            auto* nullifiers = static_cast<shielded::NullifierSet*>(shielded_nullifiers_);
            const auto& frontier = *undo.pre_block_shielded_frontier;
            if (!tree->DeserializeFrontier(frontier.data(), frontier.size())) {
                error = "Failed to restore shielded frontier snapshot";
                return false;
            }
            if (nullifiers && height > 0) {
                nullifiers->RollbackAbove(height - 1);
            }
            // Apr 28 2026: anchor history was being rolled back ONLY on
            // startup recovery (chainstate_service.cpp), never on a normal
            // reorg path. A block's shielded txs that recorded anchors
            // would leave those anchors visible after DisconnectBlock,
            // letting a reorged-out anchor act as a valid spend reference
            // on the canonical chain. Roll it back symmetrically with the
            // nullifier set.
            if (shielded_anchor_history_ && height > 0) {
                static_cast<shielded::AnchorHistory*>(shielded_anchor_history_)
                    ->RollbackAbove(height - 1);
            }
        }

        std::cout << "✅ [Phase 2] Block " << height << " disconnected via snapshot restore" << std::endl;
        return true;
    }

    // ═════════════════════════════════════════════════════════════════════════
    // LEGACY PATH: Manual Undo (for blocks connected before Phase 2)
    // ═════════════════════════════════════════════════════════════════════════
    // This path handles blocks that were connected without snapshot storage.
    // Will be removed after migration is complete.
    // ═════════════════════════════════════════════════════════════════════════

    std::cout << "⚠️ [Phase 2] DisconnectBlock: Using legacy undo path (no snapshot)" << std::endl;

    // Audit gap #5 (full closure): the legacy path is a sequence of
    // mutations across the UTXO map and the Utreexo forest, each of
    // which can fail mid-way. Pre-fix, a failure on any step bailed
    // out leaving the daemon partially disconnected — UTXO map
    // half-rolled-back, forest still in pre-rollback state, no clean
    // way to recover. removeLastNLeaves was tightened in b520a3196
    // to fail loud, but that just exposes the partial-mutation
    // problem upstream: the FOREST stays clean on its failure but
    // the UTXO MAP changes from earlier in this function are
    // already on-the-books.
    //
    // Fix at the function level: take a snapshot of the consensus
    // state at entry, run all mutations, restore the snapshot on
    // any failure. The snapshot path (above) already does this; the
    // legacy path now does it too. Both paths therefore obey the
    // §1 atomic-unit law — DisconnectBlock either fully succeeds
    // or the live state is unchanged.
    //
    // The legacy path is slated for deletion in phase 5 once the
    // snapshot path covers every block, but it remains reachable
    // for blocks connected before Phase 2 snapshot support, so
    // hardening it is not optional. Cost: one Snapshot() call per
    // legacy disconnect, which already happens implicitly in the
    // snapshot path for every block.
    std::optional<UTXOSnapshot> rollback_snapshot;
    std::vector<uint8_t> rollback_shielded_frontier;
    if (consensus_utxo_set_ && consensus_utxo_set_->SupportsSnapshotRestore()) {
        rollback_snapshot = consensus_utxo_set_->Snapshot();
    }
    if (shielded_tree_) {
        auto* tree = static_cast<shielded::CommitmentTree*>(shielded_tree_);
        rollback_shielded_frontier = tree->SerializeFrontier();
    }

    auto restore_legacy_on_failure = [&]() {
        if (rollback_snapshot.has_value() && consensus_utxo_set_) {
            consensus_utxo_set_->Restore(rollback_snapshot.value());
        }
        if (shielded_tree_ && !rollback_shielded_frontier.empty()) {
            auto* tree = static_cast<shielded::CommitmentTree*>(shielded_tree_);
            tree->DeserializeFrontier(rollback_shielded_frontier.data(),
                                       rollback_shielded_frontier.size());
        }
    };

    // Remove all non-coinbase transaction outputs (in reverse order)
    for (size_t i = block.vtx.size(); i > 1; --i) {
        const Transaction& tx = block.vtx[i - 1];
        const TxId txid = tx.GetTxid();

        for (uint32_t n = 0; n < tx.vout.size(); ++n) {
            OutPoint outpoint(txid, n);
            if (!consensus_utxo_set_->DeleteCoin(outpoint)) {
                restore_legacy_on_failure();
                error = "Failed to delete tx output during disconnect";
                return false;
            }
        }
    }

    // Restore all spent UTXOs (in reverse order)
    for (auto it = undo.spent_coins.rbegin(); it != undo.spent_coins.rend(); ++it) {
        const auto& entry = *it;
        OutPoint outpoint(TxId(entry.txid), entry.vout);
        if (!consensus_utxo_set_->AddCoin(outpoint, entry.coin)) {
            restore_legacy_on_failure();
            error = "Failed to restore spent UTXO: " + entry.txid.GetHex();
            return false;
        }
    }

    // Remove coinbase outputs
    const Transaction& coinbase_tx = block.vtx[0];
    const TxId coinbase_txid = coinbase_tx.GetTxid();

    for (uint32_t n = 0; n < coinbase_tx.vout.size(); ++n) {
        OutPoint outpoint(coinbase_txid, n);
        if (!consensus_utxo_set_->DeleteCoin(outpoint)) {
            restore_legacy_on_failure();
            error = "Failed to delete coinbase output during disconnect";
            return false;
        }
    }

    // Legacy Utreexo delta undo (for pre-Phase-2 blocks)
    if (consensus_utxo_set_ && IsUtreexoActive(height)) {
        if (undo.utreexo_delta.has_value()) {
            const UtreexoDelta& delta = undo.utreexo_delta.value();

            // Remove added leaves. removeLastNLeaves itself is
            // transactional (b520a3196 + this commit's PASS-1
            // validate) so a failure here leaves the forest
            // unchanged — but the UTXO map is mutated above, so
            // rollback the snapshot.
            if (!delta.addedLeaves.empty()) {
                if (!consensus_utxo_set_->GetForest().removeLastNLeaves(delta.addedLeaves.size())) {
                    restore_legacy_on_failure();
                    error = "utreexo-delta-undo-remove-failed";
                    return false;
                }
            }

            // Restore deleted leaves (reverse order). Per-leaf
            // restore can fail and is NOT itself transactional,
            // but the snapshot covers it.
            for (auto it = delta.deletedLeaves.rbegin(); it != delta.deletedLeaves.rend(); ++it) {
                if (!consensus_utxo_set_->GetForest().restoreDeletedLeaf(it->position, it->leafHash)) {
                    restore_legacy_on_failure();
                    error = "utreexo-delta-undo-restore-failed";
                    return false;
                }
            }

            // Verify numLeaves
            if (consensus_utxo_set_->GetForest().getNumLeaves() != delta.numLeavesBefore) {
                restore_legacy_on_failure();
                error = "utreexo-delta-undo-numleaves-mismatch";
                return false;
            }
        } else {
            restore_legacy_on_failure();
            error = "missing-utreexo-delta-undo-data";
            return false;
        }
    }

    if (shielded_tree_ && undo.pre_block_shielded_frontier.has_value()) {
        auto* tree = static_cast<shielded::CommitmentTree*>(shielded_tree_);
        auto* nullifiers = static_cast<shielded::NullifierSet*>(shielded_nullifiers_);
        const auto& frontier = *undo.pre_block_shielded_frontier;
        if (!tree->DeserializeFrontier(frontier.data(), frontier.size())) {
            restore_legacy_on_failure();
            error = "Failed to restore shielded frontier during legacy disconnect";
            return false;
        }
        if (nullifiers && height > 0) {
            nullifiers->RollbackAbove(height - 1);
        }
        if (shielded_anchor_history_ && height > 0) {
            static_cast<shielded::AnchorHistory*>(shielded_anchor_history_)
                ->RollbackAbove(height - 1);
        }
    }

    return true;
}

bool BlockValidator::ValidateTransaction(const Transaction& tx, uint32_t height, 
                                        bool is_coinbase, uint64_t& total_input_value, 
                                        std::string& error) {
    total_input_value = 0;
    const bool has_shielded_bundle = UsesShieldedValueSemantics(tx);
    
    if (is_coinbase) {
        if (has_shielded_bundle) {
            error = "Coinbase transaction cannot carry a shielded bundle";
            return false;
        }
        // Coinbase validation
        if (tx.vin.empty()) {
            error = "Coinbase transaction has no inputs";
            return false;
        }
        
        // Coinbase input should have null prevout
        const auto& first_input = tx.vin[0];
        // Phase M.0: uint256 uses IsNull() instead of empty()
        if (!first_input.prevout.txid.IsNull() || first_input.prevout.vout != 0xFFFFFFFF) {
            error = "Invalid coinbase input";
            return false;
        }
        
        return true;  // Coinbase doesn't spend UTXOs
    }
    
    // Regular transaction validation
    if (!Transaction::IsShieldedVersion(tx.version) &&
        !tx.shielded_bundle_bytes.empty()) {
        error = "Non-shielded transaction carries shielded bundle";
        return false;
    }

    if (Transaction::IsShieldedVersion(tx.version) && tx.shielded_bundle_bytes.empty()) {
        error = "Shielded transaction missing shielded bundle";
        return false;
    }

    if (!has_shielded_bundle && tx.vin.empty()) {
        error = "Transaction has no inputs";
        return false;
    }
    
    if (!has_shielded_bundle && tx.vout.empty()) {
        error = "Transaction has no outputs";
        return false;
    }
    
    // Phase M.4: Use TxOutPoint for transaction context
    std::set<TxOutPoint> seen_outpoints;
    for (const auto& input : tx.vin) {
        if (!seen_outpoints.insert(input.prevout).second) {
            error = "Transaction has duplicate inputs";
            return false;
        }
    }

    // Phase 1: Collect ALL UTXOs for all inputs
    // This is required for proper BIP341 Taproot sighash computation
    std::vector<consensus::UTXOEntry> input_utxos;
    input_utxos.reserve(tx.vin.size());

    for (size_t i = 0; i < tx.vin.size(); i++) {
        const auto& input = tx.vin[i];

        // Look up UTXO from consensus set (Phase 2: pure in-memory)
        OutPoint outpoint(input.prevout.txid, input.prevout.vout);
        const UTXOEntry* utxo_ptr = consensus_utxo_set_->GetCoin(outpoint);

        // Fallback: check intra-block overlay (UTXOs created by earlier txs in same block)
        if (!utxo_ptr) {
            auto it = intra_block_utxos_.find(outpoint);
            if (it != intra_block_utxos_.end()) {
                utxo_ptr = &it->second;
            }
        }

        if (!utxo_ptr) {
            // UTXO not found = either doesn't exist or already spent
            // In consensus model, spent UTXOs are deleted from the set
            error = "Input UTXO not found (missing or already spent): " +
                    input.prevout.txid.AsUint256().GetHex() + ":" + std::to_string(input.prevout.vout);
            std::cerr << "❌ [UTXO-LOOKUP] " << error
                      << " | height=" << height
                      << " | tx=" << tx.GetTxid().AsUint256().GetHex().substr(0, 16) << "..."
                      << " | mode=" << ValidationModeToString(validation_mode_)
                      << " | backend=" << ActiveUtxoBackendName(consensus_utxo_set_)
                      << std::endl;
            return false;
        }

        // UTXOEntry returned directly from interface (no wallet conversion needed)
        const UTXOEntry& utxo = *utxo_ptr;

        // Check coinbase maturity
        if (utxo.isCoinbase) {
            uint32_t maturity = height - utxo.height;
            if (maturity < COINBASE_MATURITY) {
                error = "Coinbase UTXO not yet mature (need " + std::to_string(COINBASE_MATURITY) +
                       " blocks, have " + std::to_string(maturity) + ")";
                return false;
            }
        }

        input_utxos.push_back(utxo);
        // Phase M.6.2: Extract raw value for boundary type
        total_input_value += utxo.value.GetUna();
    }

    if (tx.HasConfidentialOutputs() || HasConfidentialInputs(input_utxos)) {
        error = "Legacy private lane removed";
        return false;
    }

    // Phase 2: Verify scripts with full UTXO set available
    // Phase F.11: Use minimal script validation engine (explicit, not VM-based)
    //
    // This replaces VerifyScript with ValidateSpend - no opcode VM, no hidden state.
    // Each script type has its own explicit validator.

    for (size_t i = 0; i < tx.vin.size(); i++) {
        const consensus::UTXOEntry& utxo = input_utxos[i];

        // Use minimal script engine (explicit validation per script type)
        // Pass all input UTXOs for BIP341 Taproot sighash computation
        ScriptValidationResult result = ValidateSpend(tx, i, utxo, height, input_utxos);

        if (result != ScriptValidationResult::OK) {
            // Map validation result to error message
            const char* reason = "unknown";
            switch (result) {
                case ScriptValidationResult::INVALID_SIGNATURE:
                    reason = "invalid signature";
                    break;
                case ScriptValidationResult::INVALID_SCRIPT:
                    reason = "invalid script format";
                    break;
                case ScriptValidationResult::UNSUPPORTED_SCRIPT:
                    reason = "unsupported script type";
                    break;
                case ScriptValidationResult::EXTRACT_FAILED:
                    reason = "failed to extract sig/pubkey";
                    break;
                default:
                    break;
            }

            error = "Script validation failed for input " + std::to_string(i) + ": " + reason;
            return false;
        }
    }
    
    // Check for overflow
    if (total_input_value < 0) {
        error = "Input value overflow";
        return false;
    }

    // Validate outputs
    uint64_t total_output_value = SumOutputs(tx);
    uint64_t ignored_fee = 0;
    if (!ComputeValidatedTransactionFee(tx, input_utxos, total_input_value, total_output_value, ignored_fee, error)) {
        return false;
    }

    if (!ValidateShieldedTransactionBundle(
            tx,
            height,
            total_input_value,
            total_output_value,
            ignored_fee,
            static_cast<shielded::CommitmentTree*>(shielded_tree_),
            static_cast<shielded::NullifierSet*>(shielded_nullifiers_),
            static_cast<shielded::AnchorHistory*>(shielded_anchor_history_),
            error)) {
        return false;
    }

    return true;
}

// NOTE: Removed old VerifyP2WPKH wrapper - now calling ScriptVerifier directly in ValidateTransaction()

uint64_t BlockValidator::GetBlockSubsidy(uint32_t height) {
    // Dinero monetary policy: subsidy is purely height-based
    // Height 0: 0 (genesis unspendable)
    // Height 1: 100 DIN (first PoW block)
    // Height 1+: 100 DIN initial, halving every 1,314,000 blocks, 1 DIN tail floor
    // Phase M.6.2: Extract raw value from AmountUna
    return dinero::ConsensusSubsidy::GetBlockSubsidy(height).GetUna();
}

uint64_t BlockValidator::SumOutputs(const Transaction& tx) const {
    uint64_t sum = 0;
    for (const auto& output : tx.vout) {
        // Phase M.6.2: Extract raw value from AmountUna
        uint64_t val = output.value.GetUna();
        sum += val;
        if (sum < val) {
            // Overflow
            return UINT64_MAX;
        }
    }
    return sum;
}

bool BlockValidator::IsStandardP2WPKH(const std::vector<uint8_t>& scriptPubKey) const {
    // P2WPKH: OP_0 OP_PUSH20 <20-byte-pubkey-hash>
    // Size: 22 bytes total (1 + 1 + 20)
    if (scriptPubKey.size() != 22) {
        return false;
    }
    
    if (scriptPubKey[0] != 0x00) {  // OP_0
        return false;
    }
    
    if (scriptPubKey[1] != 0x14) {  // OP_PUSH20 (20 bytes)
        return false;
    }
    
    return true;
}

} // namespace consensus
} // namespace dinero
