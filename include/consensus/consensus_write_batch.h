#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// ConsensusWriteBatch — phase 3a scaffold (skeleton only)
//
// Implements the transaction boundary defined in
// docs/specs/atomic_consensus_persistence_phase3.md. Read that spec
// before touching this file; every method is a corollary of one of
// its rules.
//
// This is the SKELETON commit. The header documents the full phase
// 3a mutation surface from §3.1, but only the no-op staging + commit
// shell is implemented; subsequent commits wire each container in
// (D2 standing rule: shielded persistence is NOT touched in 3a).
//
// Activation contract:
//   - Hidden config flag `consensus.atomic_persist` (default false).
//   - Flag off  → callers DO NOT construct this type; existing
//                  per-write paths run unchanged.
//   - Flag on   → ChainstateService::ConnectTip constructs one
//                  batch per connect, stages UTXO map mutations,
//                  commits at the end.
//
// SCOPE LIMITS as shipped (phase 3a scaffold):
//   - Only ConnectTip is wired through the batch. DisconnectTip
//     stays on the legacy path entirely. §1's law binds Connect
//     AND Disconnect equally; the DisconnectTip routing is open
//     work, deliberately deferred to phase 3b alongside the
//     working-copy pattern.
//   - Only the UTXO map is staged. Forest, position index, block
//     undo, block index entry, tip, height index — all on legacy
//     persist paths under the flag.
//   - Forest commit at block_validation.cpp still happens INSIDE
//     ConnectBlockInternal, BEFORE the batch's Commit() runs.
//     Ordering is reversed from §1.2's "canonical pointers move
//     last" rule; phase 3b fixes the ordering.
//   - Commit() logs AddCoin/SpendCoin failures but returns
//     Status::Ok. Scaffold behavior; phase 3b promotes this
//     branch to fail-loud once the working-copy pattern is in.
//
// Operator-facing rule: `consensus.atomic_persist=1` is dev /
// regtest only. Do not enable on the live fleet. The flag is a
// scaffold gate, not a soak switch.
//
// Destructor contract (D4):
//   - Debug/regtest builds: hard abort if the batch was destructed
//                            without a successful Commit() or
//                            explicit Abort().
//   - Release builds:        enter consensus safe mode with reason
//                            "consensus_write_batch_dropped" and
//                            require operator safemode.exit before
//                            block connect / template generation
//                            resume.
//
// The skeleton implements both paths — the abort path is wired now
// because it is a pure compile-time branch and we want CI to assert
// it from the first commit.
// ─────────────────────────────────────────────────────────────────────────────

#include "common/status.h"
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include "primitives/uint256.h"

#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

namespace dinero {

class ChainstateService;

namespace consensus {

class ConsensusWriteBatch {
public:
    // Construction MUST go through a ChainstateService that has
    // already taken its ChainWriteToken for this block. The batch
    // does not acquire the token itself; the caller's existing
    // write-token discipline is preserved unchanged.
    explicit ConsensusWriteBatch(ChainstateService& chainstate,
                                 uint32_t block_height,
                                 const uint256& block_hash);
    ~ConsensusWriteBatch();

    ConsensusWriteBatch(const ConsensusWriteBatch&) = delete;
    ConsensusWriteBatch& operator=(const ConsensusWriteBatch&) = delete;
    ConsensusWriteBatch(ConsensusWriteBatch&&) = delete;
    ConsensusWriteBatch& operator=(ConsensusWriteBatch&&) = delete;

    // ── Phase 3a staging surface (§4 phase-3a in the spec) ──────────
    //
    // Each Stage* call mutates an internal working copy only. The
    // live containers are not touched until Commit() succeeds. None
    // of these methods are implemented in the skeleton commit; they
    // are declared so the call-site wiring lands one container at a
    // time in subsequent commits without re-touching the header.

    void StageUTXOAddition(const OutPoint& outpoint, const UTXOEntry& entry);
    void StageUTXOSpend(const OutPoint& outpoint);

    // Subsequent commits will add:
    //   StageForestDelta, StagePositionIndexDelta, StageBlockUndo,
    //   StageBlockIndexEntry, StageTipMove, StageHeightIndex.
    // Phase 3b adds the shielded surface; explicitly NOT in 3a.

    // ── Commit lifecycle (§3.1 step ordering) ───────────────────────
    //
    // Two-phase shape so callers can fold the journal row into an
    // outer rocksdb::WriteBatch and have it commit atomically with
    // their other writes:
    //
    //   1. CommitStagedLiveState() — replays staged in-memory ops
    //      (UTXO additions/spends) onto live containers. Does NOT
    //      write the journal row. Returns Status::Ok if the replay
    //      succeeded.
    //
    //   2a. AttachJournalRowToBatch(outer_wb)  → MarkCommittedAfterOuterBatch()
    //       — caller owns committing outer_wb. The journal row goes
    //       into outer_wb (atomic with the caller's other writes);
    //       MarkCommittedAfterOuterBatch flips the state machine to
    //       Committed so the destructor doesn't trip the D4 leak panic.
    //
    //   2b. Or call the legacy Commit() — runs CommitStagedLiveState
    //       internally and writes the journal row standalone via
    //       putUtreexoMeta. Used when no outer batch is available.
    //
    // Commit() is the single-call shorthand that combines (1) + (2b).
    // Most call sites use the split form when they have an outer
    // WriteBatch they're already committing.
    Status CommitStagedLiveState();
    void MarkCommittedAfterOuterBatch() noexcept;
    Status Commit();

    // Explicit abort. Drops every staged mutation; live state
    // unchanged. After Abort the destructor runs cleanly.
    void Abort() noexcept;

    // ── Activation helper ───────────────────────────────────────────
    //
    // Single source of truth for "is the atomic-persist flag on?".
    // Call sites use this to choose between the new batch path and
    // the legacy per-write path during phase 3a's opt-in window.
    static bool IsEnabled(const ChainstateService& chainstate);

    // ── Phase 3b step 3 part 3 — atomic journal-row attachment ──────
    //
    // ConnectTip already builds a single rocksdb::WriteBatch for
    // its UTXO + txindex column writes. This method adds the
    // consensus_journal:<height_be_hex>:<block_hash_hex> row to
    // THAT batch instead of letting Commit() write it via a
    // separate putUtreexoMeta call. The result: with the flag
    // on, the journal row commits ATOMICALLY with the UTXO
    // writes — no crash window between the two.
    //
    // Call this AFTER CommitStagedLiveState() and BEFORE the outer
    // WriteBatch is committed by the caller. After the outer batch
    // commits, the caller invokes MarkCommittedAfterOuterBatch() to
    // close the lifecycle.
    //
    // Returns true if the row was attached (flag on, ChainDB
    // available, valid WriteBatch pointer). Returns false otherwise.
    bool AttachJournalRowToBatch(void* rocksdb_write_batch);

private:
    enum class State : uint8_t {
        Open,
        Committed,
        Aborted,
    };

    // Phase 3a stage representation for the UTXO map. Add and Spend
    // are recorded in the SAME ordered list — order matters because
    // intra-block ephemeral outputs are added then spent within the
    // same block, and Commit() replays them in the order ConnectBlock
    // produced them. A separate per-side map would lose that order.
    struct StagedUTXOAddition {
        OutPoint outpoint;
        UTXOEntry entry;
    };
    struct StagedUTXOSpend {
        OutPoint outpoint;
    };
    using StagedUTXOOp = std::variant<StagedUTXOAddition, StagedUTXOSpend>;

    ChainstateService& chainstate_;
    uint32_t block_height_;
    uint256 block_hash_;
    State state_;
    bool journal_row_attached_ = false;
    std::vector<StagedUTXOOp> staged_utxo_ops_;
};

}  // namespace consensus
}  // namespace dinero
