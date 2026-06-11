#pragma once

#include "consensus/block_validation.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/utxo_set_digest.h"
#include "primitives/block.h"
#include "primitives/uint256.h"

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

namespace dinero::assumeutxo {

// Isolated genesis->base replay for AssumeUTXO background validation
// (docs/design/assumeutxo-fatal-state-machine.md — Completion Criteria).
// Owns a fresh ConsensusUTXOSet + BlockValidator; never touches the live
// chainstate. Single-threaded use by BackgroundValidationWorker.
//
// GENESIS HANDLING: genesis (height 0) is never validated — production
// ConnectTip's early-init path installs genesis as tip without ConnectBlock.
// But genesis is NOT UTXO-neutral in the canonical set: genesis_init.cpp
// persists EVERY genesis coinbase output as a ChainDB coin (height 0,
// coinbase=true, INCLUDING OP_RETURN outputs that normal ConnectBlock
// skips), and the startup BulkLoad (PersistentUTXOAdapter::LoadInitialState)
// carries them into the live consensus set that ExportSnapshot exports.
// The replayed set must be seeded identically or every honest snapshot
// mismatches its own commitment (found by the Task 8 e2e: regtest genesis
// carries a 100-DIN OP_RETURN record at height 0). Callers seed via
// SeedGenesis(genesis_block), then start ConnectAndAdvance at height 1.
// Genesis outputs are NOT inserted into the utreexo forest — production's
// height-0 checkpoint is an empty forest and the live forest is rebuilt
// from block replay only, so it excludes genesis as well.
// The first ConnectAndAdvance accepts any starting height; subsequent calls
// must be strictly ascending.
//
// SHIELDED STATE: the engine owns genesis-fresh shielded pool state
// (CommitmentTree, NullifierSet, AnchorHistory — same trio production
// ConnectTip wires via BlockValidator::setShieldedState) and replays it
// from genesis alongside the transparent set. Without it, a stateful
// BlockValidator hard-rejects EVERY shielded transaction ("Shielded state
// unavailable"), turning honest post-activation history (mainnet shielded
// activation: height 8650; registry snapshot bases: 13000/33048) into a
// false fatal. The records digest commits only the transparent set
// (shielded commitment scope: see plan Task 10 accounting).
class AssumeUtxoReplayEngine {
public:
    AssumeUtxoReplayEngine();
    ~AssumeUtxoReplayEngine();

    // Seed the genesis block's coinbase outputs into the replay set exactly
    // as genesis_init.cpp persists them to ChainDB (height 0, coinbase=true,
    // OP_RETURN outputs INCLUDED, no utreexo leaves — see class comment).
    // Call once per engine, before any ConnectAndAdvance. Returns false with
    // `error` set only on a duplicate-coin insert (engine misuse).
    bool SeedGenesis(const Block& genesis_block, std::string& error);

    // Validate + apply one block through the normal connection path
    // (full BlockValidator::ConnectBlock — verify_root enforced).
    // Heights must be fed strictly ascending (start at 1; genesis is
    // seeded via SeedGenesis, see class comment). Returns false with `error` set on
    // validation failure (the spec's "hard validation failure proving the
    // snapshot cannot be trusted" when the block came from the canonical
    // chain below the base).
    bool ConnectAndAdvance(const Block& block, uint32_t height,
                           const uint256& block_hash, std::string& error);

    uint32_t Height() const { return last_height_; }
    uint64_t UtxoCount() const;
    // Canonical content commitment of the replayed set (Task 1 digest).
    std::string RecordsDigestHex() const;
    // Utreexo root of the replayed forest, hex (compare vs v3 snapshot root).
    std::string UtreexoRootHex() const;

    // =========================================================================
    // Promotion inputs (Task 1)
    // =========================================================================

    // Per-block undo captured for the last `window` connected heights.
    // Ring semantics: older entries are dropped as new heights connect.
    // 0 = off (default). Size for kStartupUndoAuditWindow (1024) so that
    // PromoteValidatedHistory can persist exactly the audited tail.
    // Below-base disconnects are fatal-guarded, so deeper undo is never read.
    struct CapturedUndo {
        uint32_t height = 0;
        uint256 block_hash;
        consensus::BlockUndo undo;
    };
    void SetUndoTailWindow(uint32_t window);
    const std::deque<CapturedUndo>& UndoTail() const { return undo_tail_; }

    // Proven UTXO map (by const-ref; the reference is valid for the engine's
    // lifetime — it aliases the live set, so CONTENTS change on each
    // ConnectAndAdvance; read it only at replay completion).
    // OutPoint is in dinero:: (not dinero::consensus::); UTXOEntry is in
    // dinero::consensus:: — both are in scope from within dinero::assumeutxo.
    const std::unordered_map<OutPoint, consensus::UTXOEntry>& ProvenUtxos() const;
    // Forest and shielded state at the tip of the replay (const pointers;
    // remain valid for the lifetime of the engine).
    const consensus::UtreexoForest* Forest() const;
    const consensus::shielded::CommitmentTree* ShieldedTree() const;
    const consensus::shielded::NullifierSet* ShieldedNullifiers() const;
    const consensus::shielded::AnchorHistory* ShieldedAnchors() const;

private:
    std::unique_ptr<consensus::ConsensusUTXOSet> set_;
    std::unique_ptr<consensus::BlockValidator> validator_;
    // Genesis-fresh shielded pool state (see class comment). ConnectBlock
    // mutates these through the raw pointers handed to setShieldedState,
    // so they must outlive every ConnectAndAdvance call.
    std::unique_ptr<consensus::shielded::CommitmentTree> shielded_tree_;
    std::unique_ptr<consensus::shielded::NullifierSet> shielded_nullifiers_;
    std::unique_ptr<consensus::shielded::AnchorHistory> shielded_anchor_history_;
    uint32_t last_height_ = 0;
    bool any_connected_ = false;
    // Undo tail ring (populated when undo_tail_window_ > 0).
    uint32_t undo_tail_window_ = 0;
    std::deque<CapturedUndo> undo_tail_;
};

}  // namespace dinero::assumeutxo
