#pragma once

// Offline `--rebuild-undo-range` orchestrator (commit #4 of the post-Apr 30
// undo-rebuild series). Owns the temp ChainDB lifecycle, preflight
// classification, BlockReindexer invocation, and manifest emission. It NEVER
// mutates LIVE state outside the verified undo writes that flow through the
// reindexer's own LIVE write path (see commit #2's processBlock Step 5b).
//
// Lifecycle:
//   1. Phase 0 (preflight, read-only on LIVE): walk every height in
//      [window_start, window_end] and classify the LIVE row as one of
//        - already_ok       — undo bytes durable + readable on LIVE
//        - hole             — header metadata exists but BLOCK_HAVE_UNDO
//                             missing or undo_size==0
//        - missing_metadata — no header metadata for the canonical height
//                             (consensus-broken; refuse to proceed)
//        - blocked          — block body file unreadable in LIVE blocks/
//                             (cannot be re-applied; refuse to proceed)
//   2. Phase 1: validate the caller-provided AnchorState shape (height
//      must be < window_start; hash + forest blob must be self-consistent
//      via BlockReindexer's own seedFromAnchor refusal rules).
//   3. Phase 2: if not dry_run, create a throwaway temp ChainDB at
//      `<datadir>/chainstate.rebuild-undo.tmp/` and a parallel
//      BlockStorage that READS from the LIVE blocks/ directory. The
//      temp DB must already be populated up to the anchor — caller's
//      responsibility (commit #5 wires this through `--reindex` if
//      necessary; commit #4 only validates the precondition that the
//      temp DB has a tip at anchor.height).
//   4. Phase 3: invoke BlockReindexer in WINDOWED_UNDO_ONLY mode with
//      anchor + window + LIVE handles. Each in-window block goes
//      through the verifier (commit #3); only verified bytes become
//      durable on LIVE.
//   5. Phase 4: emit `<datadir>/rebuild_undo_manifest.json` (or the
//      caller-overridden path) with per-height status.
//
// Hard guardrails (sanity-checked at the API boundary):
//   * Refuses to start if any height in the range is `blocked` or
//     `missing_metadata`.
//   * Refuses if window_start == 0, window_end < window_start, or
//     anchor.height >= window_start.
//   * `dry_run = true` runs Phase 0 only (no temp DB, no reindexer
//     invocation, no LIVE writes); manifest still emitted.

#include "common/status.h"
#include "consensus/reindexer.h"
#include "primitives/uint256.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dinero {

class ChainDB;
class BlockStorage;

namespace daemon {

/**
 * Per-height classification surfaced in the manifest. The first three
 * are preflight outputs (set before reindexer runs); `rebuilt`,
 * `verify_failed`, and `skipped` are post-run outcomes.
 */
enum class UndoRebuildStatus {
    AlreadyOk,        // preflight: BLOCK_HAVE_UNDO + readable bytes
    Hole,             // preflight: undo missing — candidate for rebuild
    MissingMetadata,  // preflight: header row absent (refuses run)
    Blocked,          // preflight: block body unreadable (refuses run)
    Rebuilt,          // post-run: verifier passed, LIVE bytes committed
    VerifyFailed,     // post-run: verifier rejected, LIVE untouched
    Skipped,          // post-run: in window but never reached (e.g. run aborted earlier; also dry_run leaves all `Hole` entries here on emit)
};

/**
 * Per-height manifest entry. `reason` is populated for VerifyFailed,
 * MissingMetadata, and Blocked rows so an operator can read the manifest
 * and know exactly why a height was not rebuilt.
 */
struct UndoRebuildManifestEntry {
    uint32_t height = 0;
    uint256 block_hash;
    UndoRebuildStatus status = UndoRebuildStatus::Hole;
    std::string reason;  // Empty unless status carries diagnostic info
};

struct UndoRebuildManifest {
    uint32_t window_start = 0;
    uint32_t window_end = 0;
    uint32_t anchor_height = 0;
    uint256 anchor_hash;
    bool dry_run = false;
    int64_t emitted_at_unix = 0;
    uint64_t already_ok_count = 0;
    uint64_t holes_count = 0;
    uint64_t rebuilt_count = 0;
    uint64_t verify_failed_count = 0;
    uint64_t skipped_count = 0;
    uint64_t missing_metadata_count = 0;
    uint64_t blocked_count = 0;
    std::string final_status;  // "ok", "preflight_refused", "reindex_failed", "dry_run_complete"
    std::vector<UndoRebuildManifestEntry> entries;

    /**
     * Serialize manifest to a JSON string. Stable schema, sorted by
     * height. Designed to be diffable across runs.
     */
    std::string ToJson() const;

    /**
     * Parse manifest from a JSON string. Returns nullopt on malformed
     * input.
     */
    static std::optional<UndoRebuildManifest> FromJson(const std::string& json);
};

struct UndoRebuildOptions {
    std::filesystem::path datadir;
    uint32_t window_start = 0;
    uint32_t window_end = 0;
    consensus::BlockReindexer::AnchorState anchor;

    // LIVE read/write targets. The orchestrator NEVER mutates anything
    // through these except via the reindexer's verified Step 5b LIVE
    // writeUndo + putHeaderMetadata path.
    ChainDB* live_chain_db = nullptr;
    BlockStorage* live_block_storage = nullptr;

    // When true, run Phase 0 (preflight) only, then exit. Manifest is
    // still written. No temp DB is created, no reindexer is invoked,
    // and LIVE state is never touched.
    bool dry_run = false;

    // Override the manifest output path. When empty, defaults to
    // `<datadir>/rebuild_undo_manifest.json`.
    std::filesystem::path manifest_path_override;
};

/**
 * Orchestrate an offline undo-rebuild over the configured window.
 *
 * Returns the populated manifest on completion (whether successful or
 * preflight-refused). Returns Status::InvalidArgument for clearly
 * malformed `opts` (e.g. null LIVE handles, window_end < window_start,
 * anchor.height >= window_start). Returns Status::Invalid if preflight
 * found `blocked` or `missing_metadata` rows — the manifest is still
 * written so the operator can inspect.
 *
 * On Status::Ok: the manifest contains per-height outcomes. Operators
 * inspect `final_status` to discriminate dry-run-complete vs full-run
 * vs partial-run cases.
 */
StatusOr<UndoRebuildManifest> RunOfflineUndoRebuild(const UndoRebuildOptions& opts);

/**
 * Pure-string helper for the manifest's status enum so the JSON
 * stays stable across renames.
 */
std::string UndoRebuildStatusToString(UndoRebuildStatus status);
std::optional<UndoRebuildStatus> ParseUndoRebuildStatus(const std::string& s);

} // namespace daemon
} // namespace dinero
