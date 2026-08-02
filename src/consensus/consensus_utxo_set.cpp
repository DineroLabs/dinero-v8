// ============================================================================
// CONSENSUS LAYER - PURE IN-MEMORY UTXO SET IMPLEMENTATION
// ============================================================================
//
// Phase 2: Pure Consensus Architecture
//
// This file implements ConsensusUTXOSet - pure in-memory UTXO state.
// NO database, NO mutex, NO filesystem operations.
//
// ============================================================================

#include "consensus/consensus_utxo_set.h"
#include "consensus/utreexo_canonical_roots_activation.h"  // Apr 13 2026 Stage 3 fork
#include "primitives/block.h"
#include "primitives/transaction.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace dinero {
namespace consensus {

namespace {

uint64_t GetUtreexoLeafAmount(const UTXOEntry& utxo) {
    return utxo.is_confidential ? 0 : utxo.value.GetUna();
}

uint64_t GetUtreexoLeafAmount(const TxOutput& output) {
    return output.is_confidential ? 0 : output.value.GetUna();
}

}  // namespace

ConsensusUTXOSet::ConsensusUTXOSet() = default;

// =============================================================================
// Core UTXO Operations
// =============================================================================

bool ConsensusUTXOSet::AddCoin(const OutPoint& outpoint, const UTXOEntry& coin) {
    // Check if already exists
    if (utxos_.find(outpoint) != utxos_.end()) {
        return false;
    }

    utxos_[outpoint] = coin;
    return true;
}

std::unique_ptr<UTXOEntry> ConsensusUTXOSet::SpendCoin(const OutPoint& outpoint) {
    auto it = utxos_.find(outpoint);
    if (it == utxos_.end()) {
        return nullptr;
    }

    auto coin = std::make_unique<UTXOEntry>(it->second);
    utxos_.erase(it);
    return coin;
}

const UTXOEntry* ConsensusUTXOSet::GetCoin(const OutPoint& outpoint) const {
    auto it = utxos_.find(outpoint);
    if (it == utxos_.end()) {
        return nullptr;
    }
    return &it->second;
}

bool ConsensusUTXOSet::HaveCoin(const OutPoint& outpoint) const {
    return utxos_.find(outpoint) != utxos_.end();
}

bool ConsensusUTXOSet::DeleteCoin(const OutPoint& outpoint) {
    // Idempotent: return true even if not found
    utxos_.erase(outpoint);
    return true;
}

// =============================================================================
// Block Operations
// =============================================================================

bool ConsensusUTXOSet::ApplyBlock(const Block& block, uint32_t height,
                                  const uint256& block_hash, BlockUndo& undo,
                                  UtreexoHash& computed_utreexo_root,
                                  std::string& error) {
    // ALL-OR-NOTHING (issue #490).
    //
    // ProcessTransaction mutates both the UTXO map and the Utreexo forest as it
    // goes. A mid-block failure previously returned false having already
    // applied part of the block: some coins spent, some leaves removed, some
    // outputs created, at a state corresponding to no block boundary.
    //
    // Rollback uses an EXACT in-memory copy, deliberately NOT
    // Snapshot()/Restore(). That path serializes the forest, and its own
    // deserializer can refuse the payload it just produced -- falling back to a
    // rebuild that lands on a different leaf count and root:
    //
    //   [Utreexo Deserialize] Serialized roots do not match node/deletion state
    //   WARNING [Restore]: Forest numLeaves (109) != snapshot numLeaves (124)
    //
    // Rolling back through a lossy primitive would replace one partial state
    // with a different corrupted state. UtreexoForest holds only value members
    // (roots_, numLeaves_, nodes_, leaf_positions_, deleted_positions_,
    // canonical_empty_roots_), so its implicit copy is exact and needs no
    // serialization round-trip.
    auto saved_utxos = utxos_;
    UtreexoForest saved_forest = forest_;
    const uint32_t saved_height = height_;
    const uint256 saved_best_block = best_block_;
    const auto rollback = [&]() {
        utxos_ = std::move(saved_utxos);
        forest_ = std::move(saved_forest);
        height_ = saved_height;
        best_block_ = saved_best_block;
    };

    // Initialize undo data
    undo = BlockUndo(height, block_hash);
    UtreexoDelta utreexo_delta;
    utreexo_delta.numLeavesBefore = forest_.getNumLeaves();

    // Process all transactions in order
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
        const Transaction& tx = block.vtx[tx_idx];
        bool is_coinbase = (tx_idx == 0);

        if (!ProcessTransaction(tx, height, is_coinbase, undo, utreexo_delta, error)) {
            rollback();
            undo = BlockUndo(height, block_hash);  // discard the partial delta
            return false;
        }
    }

    // Store Utreexo delta for undo
    undo.utreexo_delta = std::move(utreexo_delta);

    // Compute final Utreexo root
    computed_utreexo_root = forest_.getCommitment();

    // Update state
    height_ = height;
    best_block_ = block_hash;

    return true;
}

bool ConsensusUTXOSet::ProcessTransaction(const Transaction& tx, uint32_t height,
                                          bool is_coinbase, BlockUndo& undo,
                                          UtreexoDelta& utreexo_delta,
                                          std::string& error) {
    // Compute txid for output creation
    TxId txid = tx.GetTxid();

    // 1. Spend inputs (skip for coinbase)
    if (!is_coinbase) {
        for (const TxInput& input : tx.vin) {
            // Convert TxOutPoint to OutPoint
            OutPoint outpoint(input.prevout.txid, input.prevout.vout);

            // Get and spend the UTXO
            auto spent_coin = SpendCoin(outpoint);
            if (!spent_coin) {
                error = "Missing input: " + input.prevout.txid.AsUint256().GetHex() +
                       ":" + std::to_string(input.prevout.vout);
                return false;
            }

            // Record in undo data
            undo.AddSpentCoin(input.prevout.txid.AsUint256(), input.prevout.vout, *spent_coin);

            // Compute leaf hash for Utreexo
            UtreexoHash leaf_hash = HashUTXOForCreationHeight(
                input.prevout.txid.AsUint256(),
                input.prevout.vout,
                GetUtreexoLeafAmount(*spent_coin),
                spent_coin->scriptPubKey,
                spent_coin->height,
                spent_coin->isCoinbase
            );

            // Remove from Utreexo, then record the deletion.
            //
            // ORDER MATTERS (issue #490). This previously recorded the deletion
            // BEFORE attempting removal and discarded remove()'s result, so a
            // failed removal still left "position P was deleted" in the undo
            // delta. UndoBlock later replayed that delta into
            // restoreDeletedLeaf(P), which failed with "Position P was not
            // deleted" because P had never been deleted at all. ConsensusFuzzer
            // reproduced it on 5 of 40 fixed seeds.
            //
            // Every step now fails closed, and the delta is written only after
            // the accumulator has actually changed. The delta must describe
            // what happened, never what was intended.
            auto position = forest_.findLeafPosition(leaf_hash);
            if (!position) {
                error = "utreexo-leaf-missing: " + outpoint.ToString();
                return false;
            }
            auto proof = forest_.prove(*position);
            if (!proof) {
                error = "utreexo-proof-unavailable: " + outpoint.ToString() +
                        " at position " + std::to_string(*position);
                return false;
            }
            if (!forest_.remove(leaf_hash, *proof)) {
                error = "utreexo-remove-failed: " + outpoint.ToString() +
                        " at position " + std::to_string(*position);
                return false;
            }
            utreexo_delta.recordDelete(*position, leaf_hash);
        }
    }

    // 2. Create outputs
    for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
        const TxOutput& output = tx.vout[vout];

        // Skip OP_RETURN outputs (they're not spendable)
        if (!output.scriptPubKey.empty() && output.scriptPubKey[0] == 0x6a) {
            continue;
        }

        // Create UTXO entry
        OutPoint outpoint(txid, vout);
        UTXOEntry entry(
            output.value,
            output.scriptPubKey,
            height,
            is_coinbase,
            output.is_confidential,
            output.commitment
        );

        // Add to UTXO set
        if (!AddCoin(outpoint, entry)) {
            error = "Duplicate output: " + txid.AsUint256().GetHex() + ":" + std::to_string(vout);
            return false;
        }

        // Add to Utreexo
        UtreexoHash leaf_hash = HashUTXOForCreationHeight(
            txid.AsUint256(),
            vout,
            GetUtreexoLeafAmount(output),
            output.scriptPubKey,
            height,
            is_coinbase
        );
        {
            std::ostringstream lh;
            for (size_t b = 0; b < std::min(leaf_hash.size(), size_t(8)); b++)
                lh << std::hex << std::setfill('0') << std::setw(2) << (int)leaf_hash[b];
            std::cout << "   [CUTXO] Adding output " << vout << ": "
                      << "value=" << GetUtreexoLeafAmount(output)
                      << ", spk_size=" << output.scriptPubKey.size()
                      << ", is_ct=" << output.is_confidential
                      << ", txid=" << txid.AsUint256().GetHex().substr(0, 16)
                      << ", leaf=" << lh.str()
                      << std::endl;
        }
        uint64_t position = forest_.add(leaf_hash);
        if (position == UINT64_MAX) {
            error = "Utreexo add failed: duplicate leaf hash or forest capacity reached";
            return false;
        }
        utreexo_delta.recordAdd(leaf_hash, position);
    }

    return true;
}

bool ConsensusUTXOSet::UndoBlock(const Block& block, uint32_t height,
                                 const BlockUndo& undo, std::string& error) {
    // Verify height matches
    if (undo.height != height) {
        error = "Undo height mismatch";
        return false;
    }

    // ALL-OR-NOTHING (issue #490), same reasoning and same exact-copy rollback
    // as ApplyBlock.
    //
    // This walks the UTXO map first and the Utreexo forest afterwards, so a
    // forest failure previously returned with the UTXO map already rewound --
    // the two halves of the same state disagreeing about which block they are
    // at. The copy also covers the reverse case: a UTXO-map failure occurring
    // after the forest has already been mutated.
    auto saved_utxos = utxos_;
    UtreexoForest saved_forest = forest_;
    const uint32_t saved_height = height_;
    const uint256 saved_best_block = best_block_;
    const auto rollback = [&]() {
        utxos_ = std::move(saved_utxos);
        forest_ = std::move(saved_forest);
        height_ = saved_height;
        best_block_ = saved_best_block;
    };

    // Process transactions in reverse order
    size_t spent_index = undo.spent_coins.size();

    for (size_t tx_idx = block.vtx.size(); tx_idx > 0; --tx_idx) {
        const Transaction& tx = block.vtx[tx_idx - 1];
        bool is_coinbase = (tx_idx == 1);

        // Get txid
        TxId txid = tx.GetTxid();

        // 1. Remove created outputs (in reverse order)
        for (uint32_t vout = tx.vout.size(); vout > 0; --vout) {
            const TxOutput& output = tx.vout[vout - 1];

            // Skip OP_RETURN outputs
            if (!output.scriptPubKey.empty() && output.scriptPubKey[0] == 0x6a) {
                continue;
            }

            OutPoint outpoint(txid, vout - 1);
            DeleteCoin(outpoint);
        }

        // 2. Restore spent inputs (skip for coinbase)
        if (!is_coinbase) {
            // Walk backward through inputs
            for (size_t i = tx.vin.size(); i > 0; --i) {
                if (spent_index == 0) {
                    error = "Undo data exhausted";
                    rollback();
                    return false;
                }
                --spent_index;

                const UndoEntry& undo_entry = undo.spent_coins[spent_index];
                OutPoint outpoint(TxId(undo_entry.txid), undo_entry.vout);
                // AddCoin returns false when the outpoint already exists.
                // Ignoring it silently accepted an undo that did not actually
                // restore the coin, leaving the UTXO map disagreeing with the
                // undo record it was built from.
                if (!AddCoin(outpoint, undo_entry.coin)) {
                    error = "undo-restore-coin-failed: " + outpoint.ToString();
                    rollback();
                    return false;
                }
            }
        }
    }

    // Restore Utreexo state using delta
    if (undo.utreexo_delta) {
        const UtreexoDelta& delta = *undo.utreexo_delta;

        // Remove added leaves (reverse order)
        if (!delta.addedLeaves.empty()) {
            if (!forest_.removeLastNLeaves(delta.addedLeaves.size())) {
                error = "utreexo-delta-undo-remove-failed";
                rollback();
                return false;
            }
        }

        // Restore deleted leaves (reverse order)
        for (auto it = delta.deletedLeaves.rbegin(); it != delta.deletedLeaves.rend(); ++it) {
            if (!forest_.restoreDeletedLeaf(it->position, it->leafHash)) {
                error = "utreexo-delta-undo-restore-failed at position " +
                        std::to_string(it->position);
                rollback();
                return false;
            }
        }
    }

    // Update state (go back one block)
    if (height > 0) {
        height_ = height - 1;
    }
    // Note: best_block_ will be set by caller after successful undo

    return true;
}

// =============================================================================
// Snapshot Operations
// =============================================================================

UTXOSnapshot ConsensusUTXOSet::Snapshot() const {
    UTXOSnapshot snapshot;
    snapshot.height = height_;
    snapshot.block_hash = best_block_;
    snapshot.utreexo_root = forest_.getCommitment();
    snapshot.utxos = utxos_;  // Deep copy
    snapshot.utreexo_num_leaves = forest_.getNumLeaves();
    snapshot.utreexo_forest_state = forest_.serialize();
    return snapshot;
}

void ConsensusUTXOSet::Restore(const UTXOSnapshot& snapshot) {
    height_ = snapshot.height;
    best_block_ = snapshot.block_hash;
    utxos_ = snapshot.utxos;  // Deep copy

    bool rebuild_from_utxos = false;
    if (!snapshot.utreexo_forest_state.empty()) {
        forest_ = UtreexoForest::deserialize(snapshot.utreexo_forest_state);
        // deserialize fails all-or-nothing (empty forest) — never a partial
        // "rooted husk". If the blob claimed leaves but restored empty,
        // rebuild from the UTXO map instead of proceeding with a dead forest.
        if (forest_.getNumLeaves() == 0 && snapshot.utreexo_num_leaves != 0) {
            std::cerr << "WARNING [Restore]: forest deserialize refused payload ("
                      << snapshot.utreexo_forest_state.size()
                      << " bytes, expected " << snapshot.utreexo_num_leaves
                      << " leaves) — rebuilding forest from snapshot UTXOs"
                      << std::endl;
            rebuild_from_utxos = true;
        }
    } else if (snapshot.utreexo_num_leaves == 0 && snapshot.utxos.empty()) {
        forest_ = UtreexoForest();
    } else {
        rebuild_from_utxos = true;
    }

    if (rebuild_from_utxos) {
        // Fallback for snapshots that predate serialized forest state, or
        // whose forest payload was refused by deserialize. Slower but
        // preserves full-state restore semantics.
        forest_ = UtreexoForest();
        std::vector<OutPoint> sorted_outpoints;
        sorted_outpoints.reserve(utxos_.size());
        for (const auto& [outpoint, entry] : utxos_) {
            sorted_outpoints.push_back(outpoint);
        }
        std::sort(sorted_outpoints.begin(), sorted_outpoints.end());

        for (const OutPoint& outpoint : sorted_outpoints) {
            const UTXOEntry& entry = utxos_.at(outpoint);
            UtreexoHash leaf_hash = HashUTXOForCreationHeight(
                outpoint.txid.AsUint256(),
                outpoint.vout,
                GetUtreexoLeafAmount(entry),
                entry.scriptPubKey,
                entry.height,
                entry.isCoinbase
            );
            if (forest_.add(leaf_hash) == UINT64_MAX) {
                std::cerr << "ERROR [Restore]: Failed to rebuild Utreexo forest from snapshot UTXOs" << std::endl;
                forest_ = UtreexoForest();
                break;
            }
        }
    }

    if (forest_.getNumLeaves() != snapshot.utreexo_num_leaves) {
        std::cerr << "WARNING [Restore]: Forest numLeaves (" << forest_.getNumLeaves()
                  << ") != snapshot numLeaves (" << snapshot.utreexo_num_leaves << ")" << std::endl;
    }
    if (forest_.getCommitment() != snapshot.utreexo_root) {
        std::cerr << "WARNING [Restore]: Forest root mismatch after restore" << std::endl;
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Apr 13 2026 Stage 3 — symmetric canonical-roots fork activation on Restore.
    //
    // The bug we're fixing: ConnectBlockInternal's activation hook flipped
    // `canonical_empty_roots_` and called rebuildRoots() at the activation
    // block, but UtreexoForest::serialize/deserialize doesn't carry the flag,
    // and there was no symmetric un-flip on disconnect/restore. Result: any
    // restore() across the activation boundary left the forest in an
    // inconsistent state (deserialized flag=false, but roots_ shape may be
    // post-fork canonical or pre-fork ghost form depending on when the
    // snapshot was taken). That made block 2870 un-disconnectable on
    // mainnet on Apr 13 2026, causing the chain split that required
    // rebuilding MO + Mac.
    //
    // Fix: derive the correct flag from the height being restored TO, set
    // it on the freshly-deserialized forest, and call rebuildRoots() if the
    // flag transitioned. After this, the forest is internally consistent
    // and any subsequent operation (mining, connecting, disconnecting) sees
    // the right semantics for the new tip height.
    // ═════════════════════════════════════════════════════════════════════════
    {
        const bool expected_flag =
            consensus::IsUtreexoCanonicalRootsActive(static_cast<uint32_t>(snapshot.height));
        if (forest_.isCanonicalEmptyRoots() != expected_flag) {
            forest_.setCanonicalEmptyRoots(expected_flag);
            forest_.rebuildRoots();  // re-canonicalize roots_ to match the new flag
        }
    }
}

// =============================================================================
// State Accessors
// =============================================================================

UtreexoHash ConsensusUTXOSet::GetUtreexoRoot() const {
    return forest_.getCommitment();
}

size_t ConsensusUTXOSet::GetMemoryUsage() const {
    size_t usage = sizeof(ConsensusUTXOSet);

    for (const auto& [outpoint, entry] : utxos_) {
        usage += sizeof(OutPoint);
        usage += sizeof(UTXOEntry);
        usage += entry.scriptPubKey.size();
        usage += entry.commitment.size();
    }

    // Approximate forest memory (nodes + roots + maps)
    usage += forest_.getNumLeaves() * 40;  // ~40 bytes per leaf average

    return usage;
}

void ConsensusUTXOSet::Clear() {
    utxos_.clear();
    forest_ = UtreexoForest();
    height_ = 0;
    best_block_ = uint256();
}

bool ConsensusUTXOSet::BulkLoad(const std::unordered_map<OutPoint, UTXOEntry>& utxos,
                                uint32_t height, const uint256& best_block) {
    Clear();
    utxos_ = utxos;
    height_ = height;
    best_block_ = best_block;

    // Rebuild Utreexo forest from UTXOs
    // Process in deterministic order (sorted by outpoint)
    std::vector<OutPoint> sorted_outpoints;
    sorted_outpoints.reserve(utxos.size());
    for (const auto& [outpoint, entry] : utxos) {
        sorted_outpoints.push_back(outpoint);
    }
    std::sort(sorted_outpoints.begin(), sorted_outpoints.end());

    for (const OutPoint& outpoint : sorted_outpoints) {
        const UTXOEntry& entry = utxos.at(outpoint);
        UtreexoHash leaf_hash = HashUTXOForCreationHeight(
            outpoint.txid.AsUint256(),
            outpoint.vout,
            GetUtreexoLeafAmount(entry),
            entry.scriptPubKey,
            entry.height,
            entry.isCoinbase
        );
        if (forest_.add(leaf_hash) == UINT64_MAX) {
            std::cerr << "ERROR: BulkLoad failed (duplicate Utreexo leaf hash or forest full)" << std::endl;
            return false;
        }
    }

    return true;
}

} // namespace consensus
} // namespace dinero
