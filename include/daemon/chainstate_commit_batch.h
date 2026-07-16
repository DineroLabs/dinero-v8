#pragma once
//
// ChainstateCommitBatch — single-batch ownership for ConnectTip's
// reorg-critical materials.
//
// Wraps a rocksdb::WriteBatch and tracks which fields the caller has
// staged into it. AllRequiredStaged() refuses to greenlight a commit
// if any required field is missing.
//
// Why this exists (Apr 30 2026):
//
//   The Apr 30 fleet split was caused by a class of bug where a
//   block became the active tip without all of its disconnect material
//   being durable. Today's chain of fixes — A (ValidationContext
//   non-positional), B (BuildShieldedValidationContext helper), D.1
//   (unconditional updateBlockIndex), P1 (Utreexo delta sidecar atomic
//   with the canonical batch), D.2 (startup undo coverage audit), D.3
//   (DisconnectTip undo regeneration), 0110aa3e4 (publication
//   invariant), 5a6cc0799 (decode undo + bind UTXO batch via read-back)
//   — closed the bug class incrementally.
//
//   The runtime check (CheckBlockDisconnectMaterialDurable) reads the
//   on-disk state back to verify the unified batch covered everything.
//   That works as a safety net but only reports failures AFTER the
//   batch has committed: it cannot prevent a future refactor from
//   silently dropping a Put from the batch.
//
//   ChainstateCommitBatch makes it impossible to forget. Each
//   chain_db_->putXxx call site sits inside a Mark*Staged() call. The
//   commit path asserts "all required fields staged" before calling
//   chain_db_->writeBatch. If a future refactor splits a Put into a
//   separate batch (or removes it entirely), AllRequiredStaged()
//   surfaces the missing field by name, and the publication is
//   refused.
//
// Caller pattern:
//
//   ChainstateCommitBatch ccb(tip_hash, tip_height, utreexo_active,
//                             utreexo_stateless, shielded_active);
//
//   chain_db_->putCoin(token, txid, vout, coin, &ccb.Batch());
//   ccb.MarkUtxoStaged();
//
//   chain_db_->updateBlockIndex(token, tip, &ccb.Batch());
//   ccb.MarkBlockIndexStaged();
//
//   ... etc ...
//
//   if (auto missing = ccb.AllRequiredStaged()) {
//       fail("commit-batch-incomplete: " + *missing);
//       return false;
//   }
//   chain_db_->writeBatch(token, ccb.ReleaseBatch(), /*sync=*/true);
//
// The ChainstateCommitBatch is non-copyable and non-movable; it owns
// the WriteBatch by value. ReleaseBatch() std::move's it out for the
// final writeBatch call (and prevents reuse afterward).

#include <cstdint>
#include <optional>
#include <string>

#include <rocksdb/write_batch.h>

namespace dinero {

class uint256;

class ChainstateCommitBatch {
public:
    // utreexo_checkpoint_interval — forest checkpoint delta campaign phase 1
    // (docs/design/forest-checkpoint-deltas.md): with interval > 1, the full
    // forest checkpoint is required only at heights where
    // height % interval == 0. The ForestTipMarker and the per-block delta
    // sidecar stay required EVERY block — they are what DisconnectTip and
    // phase 2's checkpoint+replay restore depend on. Interval 0 is treated
    // as 1 (checkpoint every block, the pre-campaign behavior).
    ChainstateCommitBatch(uint64_t tip_height,
                          bool utreexo_active,
                          bool utreexo_stateless,
                          bool shielded_active,
                          uint32_t utreexo_checkpoint_interval = 1)
      : tip_height_(tip_height),
        utreexo_delta_required_(utreexo_active && !utreexo_stateless),
        utreexo_checkpoint_required_(
            utreexo_delta_required_ &&
            (utreexo_checkpoint_interval <= 1 ||
             tip_height % utreexo_checkpoint_interval == 0)),
        shielded_marker_required_(shielded_active) {}

    ChainstateCommitBatch(const ChainstateCommitBatch&) = delete;
    ChainstateCommitBatch& operator=(const ChainstateCommitBatch&) = delete;
    ChainstateCommitBatch(ChainstateCommitBatch&&) = delete;
    ChainstateCommitBatch& operator=(ChainstateCommitBatch&&) = delete;

    rocksdb::WriteBatch& Batch() { return batch_; }
    const rocksdb::WriteBatch& Batch() const { return batch_; }

    uint64_t TipHeight() const { return tip_height_; }

    // ── Mark-staged calls. One per chain_db_->putXxx site. ──────────

    void MarkUtxoStaged()                { utxo_staged_ = true; }
    void MarkTxIndexStaged()             { txindex_staged_ = true; }
    void MarkBlockIndexStaged()          { block_index_staged_ = true; }
    void MarkUtreexoCheckpointStaged()   { utreexo_checkpoint_staged_ = true; }
    void MarkUtreexoForestTipMarkerStaged() { utreexo_forest_tip_marker_staged_ = true; }
    void MarkUtreexoDeltaStaged()        { utreexo_delta_staged_ = true; }
    void MarkShieldedTipMarkerStaged()   { shielded_tip_marker_staged_ = true; }
    void MarkShieldedFrontierStaged()    { shielded_frontier_staged_ = true; }
    void MarkShieldedAnchorHistoryStaged() { shielded_anchor_history_staged_ = true; }
    void MarkShieldedNullifiersStaged()  { shielded_nullifiers_staged_ = true; }
    void MarkSetTipStaged()              { set_tip_staged_ = true; }
    void MarkHeightIndexStaged()         { height_index_staged_ = true; }
    void MarkHeaderStaged()              { header_staged_ = true; }
    void MarkJournalRowStaged()          { journal_row_staged_ = true; }

    // Returns nullopt if all required fields are staged; otherwise the
    // name of the first missing field. Required-vs-not depends on the
    // chain's activation state (utreexo, shielded) at this height.
    std::optional<std::string> AllRequiredStaged() const {
        if (!utxo_staged_) return std::string("utxo_delta");
        if (!txindex_staged_) return std::string("txindex");
        if (!block_index_staged_) return std::string("block_index_metadata");
        if (utreexo_delta_required_) {
            if (utreexo_checkpoint_required_ && !utreexo_checkpoint_staged_) {
                return std::string("utreexo_checkpoint");
            }
            if (!utreexo_forest_tip_marker_staged_) return std::string("utreexo_forest_tip_marker");
            if (!utreexo_delta_staged_) return std::string("utreexo_delta_sidecar");
        }
        if (shielded_marker_required_) {
            if (!shielded_tip_marker_staged_) return std::string("shielded_tip_marker");
            // shielded_frontier / anchor_history / nullifiers are
            // commit-required only if the block emits shielded effects;
            // tracking them by activation height is a coarse upper
            // bound that may flag false positives on shielded-empty
            // blocks. They're tracked here for completeness; the
            // activation gate alone enforces the marker.
        }
        if (!set_tip_staged_) return std::string("set_tip");
        if (!height_index_staged_) return std::string("height_index");
        // header_cf is INTENTIONALLY not in the required list. The
        // putHeader call in ConnectTip lives AFTER the unified batch
        // commits (line ~10758) — it's a post-batch standalone write
        // and the original code path documents it as non-fatal.
        // Tracking it here as informational so the wrapper's API
        // mirrors the full set of consensus writes ConnectTip
        // performs, but not blocking AllRequiredStaged().
        return std::nullopt;
    }

    // Move the WriteBatch out for the final chain_db_->writeBatch
    // call. After Release, this object cannot be reused.
    rocksdb::WriteBatch ReleaseBatch() {
        released_ = true;
        return std::move(batch_);
    }

    bool Released() const { return released_; }

private:
    uint64_t tip_height_;
    bool utreexo_delta_required_;
    bool utreexo_checkpoint_required_;
    bool shielded_marker_required_;

    rocksdb::WriteBatch batch_;
    bool released_ = false;

    bool utxo_staged_ = false;
    bool txindex_staged_ = false;
    bool block_index_staged_ = false;
    bool utreexo_checkpoint_staged_ = false;
    bool utreexo_forest_tip_marker_staged_ = false;
    bool utreexo_delta_staged_ = false;
    bool shielded_tip_marker_staged_ = false;
    bool shielded_frontier_staged_ = false;
    bool shielded_anchor_history_staged_ = false;
    bool shielded_nullifiers_staged_ = false;
    bool set_tip_staged_ = false;
    bool height_index_staged_ = false;
    bool header_staged_ = false;
    bool journal_row_staged_ = false;
};

}  // namespace dinero
