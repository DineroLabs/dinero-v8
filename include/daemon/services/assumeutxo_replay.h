#pragma once

#include "consensus/block_validation.h"
#include "consensus/consensus_utxo_set.h"
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
    uint32_t last_height_ = 0;
    bool any_connected_ = false;
};

}  // namespace dinero::assumeutxo
