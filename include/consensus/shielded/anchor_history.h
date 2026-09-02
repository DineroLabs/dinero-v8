#pragma once
/**
 * Shielded pool anchor depth window.
 *
 * Sapling-shape spend proofs reference an "anchor" — the commitment
 * tree root the prover saw at proof-construction time. Without a
 * window, a spend that was provably valid one block ago can race
 * with a concurrent shielded transfer and become invalid (the tree
 * root advanced when someone else's output appended a leaf).
 *
 * The fix: validators accept any anchor that was the canonical root
 * within the last `kDepth` blocks. Sapling uses 100; we follow.
 *
 * Storage: in-memory FIFO. Reorg-safe via RollbackAbove(height) —
 * the caller (chainstate connect/disconnect) is responsible for
 * recording roots on block-connect and rolling back on disconnect.
 *
 * Boundary rule: this class tracks ROOTS only, not commitments.
 * Combined with the consensus invariants in shielded_validation.h
 * (no shielded leaf enters Utreexo), the recent-roots window is the
 * only structure that lets a spend's stale anchor stay valid.
 */

#include "consensus/shielded/commitment_tree.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace dinero::consensus::shielded {

class AnchorHistory {
public:
    /// Sapling depth — number of recent roots a spend may reference.
    static constexpr size_t kDepth = 100;

    AnchorHistory() = default;

    /**
     * Record the canonical commitment-tree root after block `height`.
     * Caller MUST invoke this exactly once per connected block, AFTER
     * all of that block's shielded outputs have been appended to the
     * tree. If this is called more than once for the same height, the
     * later call overwrites the earlier (idempotent).
     *
     * Drops the oldest entry when size exceeds `kDepth`.
     */
    void RecordRoot(uint32_t height, const Hash& root);

    /**
     * True iff `candidate` equals any recorded recent root. Linear in
     * kDepth (max 100 entries) — cheap enough that the validation hot
     * path doesn't need a hash-set.
     */
    bool Contains(const Hash& candidate) const;

    /**
     * Reorg support: drop every entry recorded at height > `height`, and
     * RESTORE the entries those blocks displaced when they were connected.
     *
     * Restoration matters because RecordRoot's eviction is lossy. Without it,
     * a node that disconnected D blocks runs with `kDepth - D` anchors while a
     * never-reorged peer at the same tip has `kDepth`, so the reorged node
     * REJECTS as AnchorInvalid a block the peer accepts (audit finding #4).
     * Mainnet has been past kDepth since long before the current tip, so the
     * window is permanently full and this is reachable today.
     *
     * Restoration only ever returns the window to what a never-reorged peer at
     * the same tip holds — it never widens it beyond kDepth.
     *
     * Idempotent and safe on an empty history.
     */
    void RollbackAbove(uint32_t height);

    /** Number of recorded roots (≤ kDepth). */
    size_t Size() const { return roots_.size(); }

    /** Test/debug: clear all entries. */
    void Clear() { roots_.clear(); evicted_.clear(); }

    /** Entries currently retained for rollback restoration (diagnostics). */
    size_t EvictedRetained() const { return evicted_.size(); }

    // ── Persistence ─────────────────────────────────────────────────
    // Phase 3 wave 2: write the window to disk on shutdown / read on
    // startup so the depth tolerance survives daemon restart. Without
    // this, the first ~kDepth blocks after restart can only accept
    // exact-tip-root anchors — a silent regression vs steady-state.
    //
    // Format (little-endian):
    //   [4 bytes] magic   = 0xA0C30001
    //   [2 bytes] version = 1
    //   [2 bytes] count   = N (≤ kDepth)
    //   for each of N entries (in oldest-first order):
    //     [4 bytes]  height
    //     [32 bytes] root
    // No checksum: corruption is caught by length / version mismatch
    // on read. Anchor history is non-canonical operator state — losing
    // it just degrades the window briefly until refilled.

    enum class IoResult : uint8_t {
        Ok           = 0,
        IoError      = 1,
        FormatError  = 2,
        Truncated    = 3,
        VersionMismatch = 4,
    };

    /// Atomically write to `path`. Writes to `path + ".tmp"` then renames.
    IoResult Save(const std::string& path) const;

    /// Replace contents from `path`. Idempotent on missing-file (returns
    /// IoError; caller may treat as "start empty"). Validates format and
    /// version; corrupt files leave the in-memory state empty.
    IoResult Load(const std::string& path);

    /// In-memory equivalent of Save(): produces the same byte layout
    /// without going through the filesystem. Used by the shielded reorg
    /// invertibility property test (and any other caller that needs a
    /// content-hash of the anchor window).
    std::vector<uint8_t> SerializeBytes() const;

    /// In-memory equivalent of Load(): consumes a payload in the same
    /// byte layout SerializeBytes/Save produce. Used by phase 3b step 2
    /// to load anchor history from a ChainDB-backed blob without going
    /// through the filesystem. Idempotent on empty input (returns
    /// IoError; caller may treat as "start empty"). Validates format
    /// and version; corrupt input leaves the in-memory state empty.
    IoResult DeserializeBytes(const std::vector<uint8_t>& bytes);

    /**
     * Durable v2 envelope used by ChainDB / flat-file persistence.  Unlike
     * SerializeBytes(), this includes the bounded eviction journal required to
     * restore a full anchor window after restart + disconnect.  Keeping this
     * separate preserves the historical v1 bytes consumed by DSR2 consensus
     * state fingerprints and epoch snapshots.
     *
     * DeserializePersistenceBytes accepts both legacy v1 SerializeBytes()
     * payloads and the v2 envelope, providing an in-place migration on the
     * next successful persist.
     */
    std::vector<uint8_t> SerializePersistenceBytes() const;
    IoResult DeserializePersistenceBytes(const std::vector<uint8_t>& bytes);

private:
    // (height, root) pairs in insertion order. New entries appended at
    // the back; oldest evicted from the front when size > kDepth.
    /// The validity window: the most recent kDepth roots, ascending by
    /// height. This is what Contains() checks and what Save()/SerializeBytes()
    /// persist — the on-disk format and the DSR2 state hash are unchanged by
    /// the retention below.
    std::deque<std::pair<uint32_t, Hash>> roots_;

    /// Entries evicted from the front of `roots_` by RecordRoot, kept so
    /// RollbackAbove can put them back. Ascending by height; the back is the
    /// most recently evicted (i.e. the one immediately below roots_.front()).
    ///
    /// Persisted by the v2 persistence envelope, but intentionally excluded
    /// from SerializeBytes() so the historical DSR2 consensus fingerprint is
    /// byte-identical. Legacy v1 payloads load with an empty journal and migrate
    /// to v2 on the next persist.
    static constexpr size_t kEvictionRetention = kDepth;
    std::deque<std::pair<uint32_t, Hash>> evicted_;
};

}  // namespace dinero::consensus::shielded
