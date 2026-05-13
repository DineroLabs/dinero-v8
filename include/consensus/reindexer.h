#pragma once

#include "common/status.h"
#include "consensus/chainwork.h"  // For arith_uint256
#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/utreexo_accumulator.h"  // UtreexoForest — reindex rebuilds the forest inline
#include "primitives/uint256.h"  // uint256
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

namespace dinero {

// Forward declarations
class ChainDB;
class BlockStorage;
struct Block;  // defined as struct in primitives/block.h — mismatch on MSVC mangles 'class'/'struct' differently
struct FilePosition;
struct UndoRecord;

namespace consensus {
struct UtreexoDelta;

/**
 * F.11.11: Block Reindexer
 *
 * Rebuilds blockchain database from blk*.dat files on disk.
 * Used for corruption recovery and database migration.
 *
 * Two reindex modes:
 *   1. Full reindex (-reindex): Rebuild block index + UTXO set from genesis
 *   2. Chainstate-only (-reindex-chainstate): Rebuild UTXO set, keep block index
 *
 * Process:
 *   1. Scan all blk*.dat files in blocks/ directory
 *   2. Parse each block sequentially
 *   3. Rebuild block index (hash → FilePosition mapping)
 *   4. Rebuild UTXO set from transaction inputs/outputs
 *   5. Validate all blocks (or use assumevalid if enabled)
 *   6. Report progress to user (blocks processed / total)
 *
 * Safety:
 *   - Atomic: Creates new database, swaps on success
 *   - Validates all blocks during rebuild (unless assumevalid)
 *   - Verifies chainwork and PoW
 *   - Fails loudly on corruption (better than silent corruption)
 */
class BlockReindexer {
public:
    /**
     * Reindex modes
     */
    enum class Mode {
        FULL,              // Rebuild block index + UTXO set (-reindex)
        CHAINSTATE_ONLY,   // Rebuild UTXO set only (-reindex-chainstate)
        // Offline undo reconstruction: replay anchor → window-end into a
        // throwaway temp ChainDB (chain_db_ / block_storage_), then for
        // blocks in [window.start_height, window.end_height] additionally
        // write rebuilt undo bytes to a separate LIVE rev*.dat and stage
        // a metadata-only Put on the LIVE ChainDB. Never mutates LIVE
        // setTip / heightIndex / forest checkpoint / sidecar / UTXO state.
        // See `Config::undo_rebuild_window`.
        WINDOWED_UNDO_ONLY
    };

    /**
     * Progress callback function
     * Called periodically with (blocks_processed, total_blocks)
     */
    using ProgressCallback = std::function<void(uint64_t, uint64_t)>;

    /**
     * Anchor state for windowed reindex.
     *
     * Carries the in-memory snapshot of consensus state AT a given block height.
     * Lets the reindexer skip the genesis-seed step and start its block-processing
     * loop at `height + 1` with the forest, shielded tree, chainwork, and tip
     * already populated. Used by:
     *   - the offline `--rebuild-undo-range` orchestrator (commit set after Apr 30
     *     chainstate-publication hardening), which seeds a temp ChainDB up to a
     *     pre-window anchor and then runs the reindexer in WINDOWED_UNDO_ONLY
     *     mode so it builds undo bytes for the historical hole window without
     *     replaying from genesis again
     *
     * Invariants the caller MUST guarantee:
     *   - `forest_serialized` is a `UtreexoForest::serialize()` blob whose
     *     `getCommitment()` matches the block-at-height's `header.utreexo_root`
     *     (or empty if `IsUtreexoActive(height)` is false)
     *   - `shielded_frontier_serialized` is `CommitmentTree::SerializeFrontier()`
     *     output for the post-block-at-height shielded tree (or empty if the
     *     anchor predates `Params().shielded_activation_height`)
     *   - `chainwork` matches the cumulative work up through and including
     *     `height`
     *   - `hash` matches the block hash at `height`
     *
     * The reindexer does NOT validate these against ChainDB during
     * `seedFromAnchor`; it trusts the orchestrator's preflight. Mismatches
     * surface immediately at the next `processBlock`'s
     * `applyBlockToForest` root-verification step.
     */
    struct AnchorState {
        uint32_t height = 0;
        uint256 hash;
        arith_uint256 chainwork;
        std::vector<uint8_t> forest_serialized;
        std::vector<uint8_t> shielded_frontier_serialized;
    };

    /**
     * Window of historical block heights for which the offline reindexer
     * should reconstruct and persist undo bytes back to a LIVE ChainDB +
     * LIVE BlockStorage, without ever publishing a tip or mutating any
     * other LIVE chain metadata.
     *
     * Used exclusively in `Mode::WINDOWED_UNDO_ONLY`. See the offline
     * `--rebuild-undo-range` orchestrator (commit #4 of the post-Apr 30
     * undo-rebuild series) for the full lifecycle.
     *
     * Hard guardrails enforced by the reindexer (in commit #2):
     *   - Never publishes a tip on the LIVE DB
     *   - Never writes putHeader / putHeightIndex / putBlock / setTip on LIVE
     *   - Never mutates LIVE UTXO entries / Utreexo checkpoints / shielded
     *     frontier file / shielded nullifier DB
     *   - Never uses ChainstateCommitBatch (which by design refuses commit
     *     unless tip publication fields are staged)
     *   - LIVE writes are limited to: writeUndo on `live_block_storage`
     *     (rev*.dat append + fsync) and a single putHeaderMetadata on
     *     `live_chain_db` with only `BLOCK_HAVE_UNDO + undo_file/pos/size`
     *     updated (all other metadata fields preserved from the existing
     *     LIVE row), committed via a raw rocksdb::WriteBatch with
     *     sync=true.
     *
     * The temp ChainDB (the reindexer's primary `chain_db_`) is the only
     * thing that gets fully-mutated writes — UTXO mutations, forest
     * checkpoints, sidecars, tip pointers — because the next block's
     * prevout lookups depend on those temp writes being in place. The
     * orchestrator deletes the temp DB after the run.
     */
    struct UndoRebuildWindow {
        uint32_t start_height = 0;
        uint32_t end_height = 0;
        // LIVE write targets. The reindexer treats both as required when
        // window mode is active. The orchestrator opens these against the
        // production data directory; the reindexer's primary `chain_db_`
        // and `block_storage_` are pointed at the throwaway temp DB.
        ChainDB* live_chain_db = nullptr;
        BlockStorage* live_block_storage = nullptr;

        // When true (default), every in-window block runs the
        // DisconnectBlock-roundtrip verification harness BEFORE its
        // rebuilt undo bytes become durable on the LIVE rev*.dat /
        // LIVE ChainDB metadata. Verification failure for a height
        // means: do NOT touch LIVE state for that height; log the
        // reason; keep advancing through the rest of the window.
        // The structural invariant the harness pins:
        //   rebuilt undo bytes are accepted only if a clone of the
        //   post-apply temp state, fed those bytes through a
        //   DisconnectBlock-equivalent reverse pass, produces a
        //   recovered state that matches the pre-apply temp state
        //   field-by-field (forest commitment, shielded frontier,
        //   spent-coin contents, created-output cardinality,
        //   serialization round-trip stability).
        // Setting this to false disables the harness — DO NOT do
        // that in production; it exists only for offline diagnostics
        // and the `corrupt-undo-must-fail-verify` regression tests.
        bool verify_disconnect_roundtrip = true;

        // Optional whitelist of heights that should have their rebuilt
        // undo bytes propagated to LIVE rev*.dat + metadata.
        //
        // When EMPTY (default): every in-window height gets LIVE
        // writes — this is the legacy behavior that worked correctly
        // but rewrote `already_ok` blocks unnecessarily, doubling
        // their bytes in rev*.dat and churning their metadata
        // pointers (the CN canary surfaced this on 2026-04-30:
        // 333 holes rebuilt cleanly, but the run also re-wrote
        // 10450 `already_ok` heights that didn't need touching).
        //
        // When NON-EMPTY: every in-window block still walks the
        // temp DB and runs the DisconnectBlock-roundtrip verifier
        // (so any silent divergence between live and offline
        // semantics still surfaces), but LIVE writes only fire for
        // heights present in this set. The orchestrator populates
        // this from its preflight `Hole` classification so
        // `already_ok` heights are byte-equal pre and post run.
        //
        // The verifier still computes a full undo blob + reverse-
        // applies the forest delta on a clone for every block,
        // because the operator's intent in running the rebuilder
        // is partly to confirm chain-wide DisconnectBlock parity —
        // skipping verification on `already_ok` blocks would
        // hide drift bugs of the kind that bit reindex on Apr 30.
        std::unordered_set<uint32_t> hole_heights_to_rebuild;
    };

    /**
     * Reindex configuration
     */
    struct Config {
        Mode mode;
        bool use_assumevalid;
        uint64_t progress_interval;
        ProgressCallback progress_cb;
        std::filesystem::path shielded_frontier_output_path;
        std::filesystem::path shielded_nullifier_db_path;

        // When set, the reindexer skips genesis seeding and instead seeds its
        // in-memory state from this anchor before processing blocks. The
        // block body for `height` is assumed already present in (the temp)
        // ChainDB; the reindexer's processing loop will start at `height + 1`.
        // Required when `mode == Mode::WINDOWED_UNDO_ONLY`.
        std::optional<AnchorState> anchor_state;

        // When set, processBlock writes rebuilt undo bytes to LIVE rev*.dat
        // and stages a metadata-only Put on the LIVE ChainDB for every
        // height in [start_height, end_height]. Required when
        // `mode == Mode::WINDOWED_UNDO_ONLY`.
        std::optional<UndoRebuildWindow> undo_rebuild_window;

        // When true, `initializeShieldedArtifacts` opens the existing
        // shielded nullifier DB and frontier file at the configured paths
        // *without removing* them first. Used in WINDOWED_UNDO_ONLY mode
        // where the orchestrator pre-populates the temp DB's shielded
        // state via a prior reindex from genesis to anchor (or via
        // assumeUTXO snapshot load).
        bool preserve_shielded_state_on_init = false;

        // Optional anchor for canonical-chain selection.
        //
        // When set: the reindexer's SelectCanonicalChain skips the
        // chainwork-search across all parsed records and walks
        // backward from the supplied tip hash. Required when blk*.dat
        // contains stale orphan blocks from prior chain incarnations
        // (LA on 2026-04-30: ~52,000 magic-aligned regions for a
        // 10,784-block canonical chain). Without this, the chainwork
        // search may select a non-canonical tip OR fail to construct
        // a complete canonical chain.
        //
        // The orchestrator reads this from the LIVE ChainDB
        // (`getBlockHashByHeight(window.end_height)`) BEFORE invoking
        // the reindexer, so it represents the canonical tip the
        // operator intends to rebuild against. Empty / null means
        // "use legacy chainwork-search" — appropriate for clean
        // datadirs and for diagnostic runs without a known anchor.
        std::optional<uint256> known_canonical_tip_hash;

        Config()
            : mode(Mode::FULL)
            , use_assumevalid(true)
            , progress_interval(100)
            , progress_cb(nullptr)
        {}
    };

    /**
     * Reindex result statistics
     */
    struct Stats {
        uint64_t blocks_processed = 0;
        uint64_t files_scanned = 0;
        uint64_t utxos_created = 0;
        uint64_t utxos_spent = 0;
        uint64_t total_bytes = 0;
        uint64_t duration_ms = 0;
        bool success = false;
        std::string error;

        // Apr 29 2026 incident recovery: when applyBlockToForest detects
        // a header-vs-canonical-replay forest root mismatch mid-reindex,
        // reindex stops at the last good height (canonical_truncated_at_height)
        // and marks the failing block + descendants permanently invalid
        // instead of aborting the whole operation. canonical_truncation_reason
        // is the human-readable detail surfaced by daemon startup logs.
        // Both fields stay zero / empty on a clean reindex.
        int32_t canonical_truncated_at_height = -1;
        std::string canonical_truncation_reason;

        // Count of block-shaped on-disk regions that ReadDiskBlocks
        // skipped because their checksum mismatched or their body
        // failed to deserialize. These are typically orphaned/stale
        // blocks from prior chain incarnations (pre-v7 era, dropped
        // reorg forks, partial writes from interrupted ConnectTip
        // calls) that the live daemon never references via metadata
        // and that SelectCanonicalChain would have discarded anyway.
        // A skip is informational; non-zero `parse_skipped_blocks`
        // does NOT indicate a problem with the canonical chain. The
        // reindexer's old behavior was to abort on the first skip
        // candidate, which prevented offline tooling (--rebuild-undo-range)
        // from running on any datadir with stale orphans on disk —
        // surfaced on LA on 2026-04-30.
        uint64_t parse_skipped_blocks = 0;

        // WINDOWED_UNDO_ONLY accounting (zero in other modes).
        // verify_failures_in_window — count of in-window blocks whose
        //   DisconnectBlock-roundtrip verification failed; their LIVE
        //   undo bytes were NOT made durable. The orchestrator's
        //   manifest records per-height failure reasons.
        // live_undo_writes_committed — count of in-window blocks whose
        //   rebuilt undo bytes were verified and made durable on LIVE.
        // live_undo_write_success_heights — exact set of heights for
        //   which a LIVE writeUndo + LIVE putHeaderMetadata both
        //   committed durably. The orchestrator uses this as the
        //   authoritative ground truth for marking heights as
        //   Rebuilt. A Hole height present in preflight that does
        //   NOT appear here did not get rebuilt — the orchestrator
        //   surfaces those as Skipped and refuses to claim success.
        //   Solved the LA 2026-04-30 bug where the manifest reported
        //   `rebuilt=518` while the reindexer's counter showed 0
        //   actual LIVE writes — a dishonest manifest hiding zero
        //   real progress.
        uint64_t verify_failures_in_window = 0;
        uint64_t live_undo_writes_committed = 0;
        std::vector<uint32_t> verify_failure_heights;
        std::vector<uint32_t> live_undo_write_success_heights;

        Stats() = default;
    };

    /**
     * Constructor
     *
     * @param datadir       Data directory containing blocks/ folder
     * @param chain_db      ChainDB instance (for UTXO set + block index)
     * @param block_storage BlockStorage instance (for reading blk*.dat files)
     * @param config        Reindex configuration
     */
    BlockReindexer(
        const std::filesystem::path& datadir,
        ChainDB* chain_db,
        BlockStorage* block_storage,
        const Config& config = Config()
    );

    ~BlockReindexer();

    /**
     * Execute reindex operation
     *
     * Returns statistics on success, error on failure.
     * This is a blocking operation that may take hours for large chains.
     */
    StatusOr<Stats> execute();

    /**
     * Seed the reindexer's in-memory consensus state from an externally-prepared
     * anchor at a non-genesis height.
     *
     * Mirrors the in-memory effects of `seedGenesis` (forest_, shielded_tree_,
     * accumulated_chainwork_, final_tip_*) but does NOT touch ChainDB. The
     * caller (the windowed-undo orchestrator) is responsible for ensuring the
     * temp ChainDB already contains the anchor block, its header, height
     * index, tip pointer, and the UTXO state at that height — typically by
     * a prior full reindex run from genesis to anchor or by an assumeUTXO
     * snapshot load.
     *
     * Postconditions on Status::Ok:
     *   - `*forest_` reflects `anchor.forest_serialized` (if non-empty and
     *     Utreexo is active at `anchor.height`)
     *   - `shielded_tree_` frontier reflects `anchor.shielded_frontier_serialized`
     *     (if non-empty and shielded is active at `anchor.height`)
     *   - `accumulated_chainwork_ == anchor.chainwork`
     *   - `final_tip_hash_ == anchor.hash`
     *   - `final_tip_height_ == static_cast<int32_t>(anchor.height)`
     *
     * NullifierSet and AnchorHistory are NOT populated by this call — the
     * orchestrator is responsible for opening the existing on-disk
     * nullifier DB at the configured path before invoking the reindexer
     * (commit #2 will plumb a `Config::preserve_shielded_state_on_init`
     * flag that makes `initializeShieldedArtifacts` open without clearing).
     */
    Status seedFromAnchor(const AnchorState& anchor);

    // ─────────────────────────────────────────────────────────────────────
    // Verification helper for WINDOWED_UNDO_ONLY mode (commit #3)
    // ─────────────────────────────────────────────────────────────────────
    /**
     * Pre-apply consensus-state snapshot used by the windowed-undo
     * verification harness. Captured at the top of `processBlock` BEFORE
     * the forest, shielded tree, and shielded nullifier set are mutated.
     * The harness uses these fields to verify that a candidate undo blob,
     * when fed through a DisconnectBlock-equivalent reverse pass against
     * a clone of the post-apply state, recovers exactly this captured
     * pre-apply state.
     */
    struct PreApplyStateForVerification {
        UtreexoHash forest_commitment;
        uint64_t forest_num_leaves = 0;
        std::vector<uint8_t> shielded_frontier_serialized;
        bool utreexo_active_at_height = false;
        bool shielded_active_at_height = false;
    };

    // ─────────────────────────────────────────────────────────────────────
    // Test-only accessors
    // ─────────────────────────────────────────────────────────────────────
    // These exist solely so `tests/consensus/test_reindexer_anchor_seed.cpp`
    // can verify that `seedFromAnchor` populated the reindexer's internal
    // state correctly. Production code MUST NOT call them.

    struct InternalStateSnapshotForTesting {
        uint64_t forest_num_leaves = 0;
        UtreexoHash forest_commitment;
        std::vector<uint8_t> shielded_frontier_serialized;
        arith_uint256 accumulated_chainwork;
        uint256 final_tip_hash;
        int32_t final_tip_height = -1;
    };

    InternalStateSnapshotForTesting snapshotInternalStateForTesting() const;

    // Test-only thin wrapper around `verifyRebuiltUndoRoundTrip` so the
    // tests/consensus/test_reindexer_anchor_seed.cpp suite can exercise
    // the harness directly. Production code MUST NOT call this — it
    // exists only so the tests can construct synthetic pre/post state
    // via `seedFromAnchor` and probe the verifier's behavior on
    // hand-crafted undo blobs.
    Status verifyRebuiltUndoRoundTripForTesting(
        const Block& block,
        uint64_t height,
        const std::vector<uint8_t>& candidate_undo_bytes,
        const ::dinero::UndoRecord& built_undo,
        const PreApplyStateForVerification& pre_state,
        const UtreexoDelta& utreexo_delta,
        std::string& error_out
    );

private:
    // Configuration
    std::filesystem::path datadir_;
    ChainDB* chain_db_;
    BlockStorage* block_storage_;
    Config config_;

    // State
    Stats stats_;
    arith_uint256 accumulated_chainwork_;  // Cumulative proof-of-work

    // Utreexo forest rebuilt inline during reindex. Mirrors the consensus forest
    // that ConnectTip mutates during normal block connection, so the reindex's
    // output is a fully-correct chainstate at tip — no "forest will be rebuilt
    // by later block activity" handwaving. See reindexer.cpp::applyBlockToForest.
    std::unique_ptr<UtreexoForest> forest_;
    // v7 shielded state rebuilt inline during reindex. Validation core stays
    // shared with live block connection; reindex owns only the persistence/
    // accounting glue needed to feed that shared path.
    shielded::CommitmentTree shielded_tree_;
    shielded::NullifierSet shielded_nullifiers_;
    shielded::AnchorHistory shielded_anchor_history_;  // Phase 3 wave 1
    std::filesystem::path shielded_frontier_output_path_;
    std::filesystem::path shielded_nullifier_db_path_;
    uint256 final_tip_hash_;
    int32_t final_tip_height_{-1};

    // Helper functions
    StatusOr<std::vector<std::filesystem::path>> scanBlockFiles();
    Status seedGenesis(arith_uint256& genesis_work_out, const FilePosition* genesis_pos = nullptr);
    Status processBlockFile(const std::filesystem::path& file_path, uint32_t file_number);
    Status processBlock(const Block& block, const FilePosition& pos, uint64_t height);
    Status rebuildUTXOSet();
    void reportProgress(uint64_t blocks_processed, uint64_t total_blocks);
    Status initializeShieldedArtifacts();
    Status persistShieldedArtifacts();

    // Apply one block to the rebuilt forest, mirroring BlockValidator's delta
    // computation: remove leaves for non-ephemeral spent inputs, add leaves for
    // non-ephemeral outputs, verify the resulting forest root against
    // block.header.utreexo_root. On mismatch or internal error, returns non-Ok
    // and leaves the forest unchanged (work is done on a local snapshot and
    // committed only after root verification passes).
    Status applyBlockToForest(const Block& block, uint32_t height,
                              UtreexoDelta& delta_out, std::string& error);

    /**
     * WINDOWED_UNDO_ONLY verification harness (commit #3).
     *
     * Decode `candidate_undo_bytes` back into an `UndoRecord` and prove
     * the bytes are sufficient to round-trip a `DisconnectBlock`-equivalent
     * reverse pass — i.e., the candidate undo, when fed to the disconnect
     * path, would recover exactly the pre-apply state captured in
     * `pre_state`. Concretely the harness asserts:
     *
     *   1. UndoRecord serialization is byte-stable (Deserialize → Serialize
     *      reproduces the input exactly). Catches encoder drift bugs in the
     *      bug class that bit reindex on Apr 30 (tx_sighash zero-default).
     *   2. `decoded.spent.size()` equals the count of non-coinbase inputs
     *      in the block.
     *   3. `decoded.created.size()` equals the total output count in the
     *      block, and each created entry's (txid, vout) maps to a real
     *      output.
     *   4. Each `decoded.spent[i]` field-by-field matches the in-memory
     *      `built_undo.spent[i]` we constructed from the temp DB pre-apply
     *      (value, scriptPubKey, is_coinbase, height, is_confidential,
     *      commitment).
     *   5. `decoded.pre_block_shielded_frontier` is present iff shielded
     *      was active at this height, and matches `pre_state.shielded_frontier_serialized`
     *      byte-for-byte.
     *   6. Forest reverse-apply: a clone of the post-apply forest, with
     *      `utreexo_delta`'s additions removed (in reverse order) and
     *      deletions restored, has a `getCommitment()` equal to
     *      `pre_state.forest_commitment`.
     *
     * If every check passes: return Status::Ok and the live writes proceed.
     * If any check fails: return non-Ok, set `error_out` to a per-block
     * reason string, and the caller skips live writes for this height.
     * The reindexer continues to advance through the rest of the window —
     * one bad block does not abort the run.
     *
     * The harness is pure-compute on a CLONE of the forest; the temp DB
     * is never mutated by verification. The shielded tree and nullifier
     * set are never mutated either (we compare frontier bytes captured
     * pre-apply, not the current shielded_tree_).
     */
    Status verifyRebuiltUndoRoundTrip(
        const Block& block,
        uint64_t height,
        const std::vector<uint8_t>& candidate_undo_bytes,
        const ::dinero::UndoRecord& built_undo,
        const PreApplyStateForVerification& pre_state,
        const UtreexoDelta& utreexo_delta,
        std::string& error_out
    );
};

} // namespace consensus
} // namespace dinero
