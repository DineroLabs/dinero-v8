#pragma once

#include "consensus/block_validation.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/utxo_set_digest.h"
#include "primitives/block.h"
#include "primitives/uint256.h"

#include <memory>
#include <string>

namespace dinero::assumeutxo {

// Isolated genesis->base replay for AssumeUTXO background validation
// (docs/design/assumeutxo-fatal-state-machine.md — Completion Criteria).
// Owns a fresh ConsensusUTXOSet + BlockValidator; never touches the live
// chainstate. Single-threaded use by BackgroundValidationWorker.
//
// GENESIS HANDLING: genesis (height 0) is UTXO-neutral in Dinero — its
// outputs are OP_RETURN-only and GetBlockSubsidy(0) == 0, so it contributes
// no UTXOs and no utreexo leaves. Production mirrors this: ConnectTip's
// early-init path sets genesis as tip directly without ConnectBlock, and the
// deterministic consensus fuzzer likewise starts applying at height 1 over
// an empty set. Therefore genesis is treated as PRE-APPLIED here: callers
// (the worker, tests) start replay at height 1 against the fresh empty set.
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

    // Validate + apply one block through the normal connection path
    // (full BlockValidator::ConnectBlock — verify_root enforced).
    // Heights must be fed strictly ascending (start at 1; genesis is
    // pre-applied, see class comment). Returns false with `error` set on
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
};

}  // namespace dinero::assumeutxo
