#include "network/bridge_node.h"
#include "consensus/interfaces/iconsensus_utxo_set.h"
#include "storage/archival_block_reader.h"
#include "storage/chain_db.h"
#include "storage/forest_restore.h"
#include "common/logger.h"
#include "crypto/sha256.h"
#include "consensus/outpoint.h"  // For OutPoint type
#include "consensus/undo.h"      // For UndoRecord (spent UTXO fallback)
#include "consensus/utreexo_maturity_leaf_activation.h"
// Wallet header removed - bridge_node uses only IUTXOProvider interface
#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <set>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace dinero {
namespace network {

namespace {

std::unordered_set<OutPoint> CollectEphemeralOutputs(const Block& block) {
    std::unordered_map<OutPoint, size_t> intra_block_outputs;
    std::unordered_set<OutPoint> ephemeral_outputs;

    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
        const TxId txid = block.vtx[tx_idx].GetTxid();
        for (uint32_t vout = 0; vout < block.vtx[tx_idx].vout.size(); ++vout) {
            intra_block_outputs[OutPoint(txid, vout)] = tx_idx;
        }
    }

    for (size_t tx_idx = 1; tx_idx < block.vtx.size(); ++tx_idx) {
        for (const auto& input : block.vtx[tx_idx].vin) {
            const OutPoint prevout(input.prevout.txid, input.prevout.vout);
            auto output_it = intra_block_outputs.find(prevout);
            if (output_it != intra_block_outputs.end() && output_it->second < tx_idx) {
                ephemeral_outputs.insert(prevout);
            }
        }
    }

    return ephemeral_outputs;
}

std::unordered_map<OutPoint, consensus::SpentOutputData> CollectIntraBlockSpentOutputs(
    const Block& block,
    uint32_t block_height
) {
    std::unordered_map<OutPoint, consensus::SpentOutputData> outputs;

    for (const auto& tx : block.vtx) {
        const TxId txid = tx.GetTxid();
        for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
            consensus::SpentOutputData spent_output;
            spent_output.value = tx.vout[vout].value.GetUna();
            spent_output.scriptPubKey = tx.vout[vout].scriptPubKey;
            spent_output.is_confidential = tx.vout[vout].is_confidential;
            spent_output.commitment = tx.vout[vout].commitment;
            spent_output.created_height = block_height;
            spent_output.is_coinbase = tx.IsCoinbase();
            outputs.emplace(OutPoint(txid, vout), std::move(spent_output));
        }
    }

    return outputs;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Constructor
// ═════════════════════════════════════════════════════════════════════════════

BridgeNode::BridgeNode(
    std::shared_ptr<consensus::IUTXOProvider> utxo_provider,
    consensus::UtreexoForest* utreexo_forest,
    consensus::ProofCache* proof_cache,
    ChainDB* chain_db,
    BlockStorage* block_storage,
    consensus::IConsensusUTXOSet* owner
)
    : utxo_provider_(std::move(utxo_provider))
    , utreexo_forest_(utreexo_forest)
    , proof_cache_(proof_cache)
    , chain_db_(chain_db)
    , block_storage_(block_storage)
    , owner_(owner)
{
    if (!utxo_provider_) {
        throw std::invalid_argument("BridgeNode: utxo_provider cannot be null");
    }
    if (!utreexo_forest_) {
        throw std::invalid_argument("BridgeNode: utreexo_forest cannot be null");
    }

    // Keep worker count small by default; proof generation can still scale via
    // cache/coalescing, and this avoids oversubscription in test environments.
    const unsigned int hc = std::thread::hardware_concurrency();
    const size_t workers = std::max<size_t>(2, std::min<size_t>(8, hc == 0 ? 4 : hc / 2));
    StartProofWorkers(workers);
}

BridgeNode::~BridgeNode() {
    StopProofWorkers();
}

void BridgeNode::readForestShared(
    const std::function<void(const consensus::UtreexoForest&)>& fn) const {
    if (owner_) {
        auto forest_lock = owner_->LockForestShared();
        fn(*utreexo_forest_);
    } else {
        fn(*utreexo_forest_);
    }
}

consensus::UtreexoHash BridgeNode::GetCurrentForestCommitment() const {
    consensus::UtreexoHash commitment{};
    readForestShared([&](const consensus::UtreexoForest& f) {
        commitment = f.getCommitment();
    });
    return commitment;
}

// ═════════════════════════════════════════════════════════════════════════════
// Proof Generation
// ═════════════════════════════════════════════════════════════════════════════

consensus::BlockUtreexoData BridgeNode::GenerateProofForBlock(
    const Block& block,
    uint32_t block_height
) {
    // Phase 9.3: Check cache first (under lock)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto cache_it = block_proof_cache_.find(block.GetHash());
        if (cache_it != block_proof_cache_.end()) {
            if (!isCacheEntryExpired(cache_it->second) &&
                IsCacheEntryChainFresh(block.GetHash(), cache_it->second)) {
                cache_hits_++;
                cache_it->second.access_count++;
                touchCacheLRU(block.GetHash());
                return cache_it->second.proof_data;
            } else {
                eraseBlockCacheEntryLocked(block.GetHash());
                cache_evictions_++;
            }
        }
        cache_misses_++;
    }
    // Lock released — proof generation below is CPU-bound, no cache access

    consensus::BlockUtreexoData proof_data;
    std::optional<consensus::UtreexoForest> historical_forest;
    std::optional<consensus::UtreexoForest> live_forest_clone;
    consensus::UtreexoForest* proof_forest = utreexo_forest_;

    // 1. Capture accumulator root before applying block.
    // Start with previous-header metadata when available so we can detect
    // checkpoint drift, but always overwrite it with the canonical proof forest
    // commitment once the serving forest is selected below.
    if (chain_db_ && !block.header.prev_block_hash.IsNull()) {
        auto prev_result = storage::ReadArchivalBlock(*chain_db_, block_storage_, block.header.prev_block_hash);
        if (prev_result.ok()) {
            const Block& prev_block = prev_result.value();
            proof_data.accumulator_root_before.assign(
                prev_block.header.utreexo_root.data,
                prev_block.header.utreexo_root.data + 32
            );
        }
    }
    if (proof_data.accumulator_root_before.empty()) {
        if (block.header.prev_block_hash.IsNull()) {
            // Genesis has an empty accumulator before state.
            proof_data.accumulator_root_before.assign(32, 0x00);
        } else {
            // Fallback when historical lookup is unavailable.
            readForestShared([&](const consensus::UtreexoForest& f) {
                proof_data.accumulator_root_before = f.getCommitment();
            });
        }
    }

    // Historical serving must generate proofs against the pre-block forest
    // state, not the current tip forest. Forest checkpoint delta campaign
    // phase 3: full checkpoints exist only every N blocks now, so restore
    // height-1 as nearest-checkpoint + UD-sidecar replay (header-root
    // verified per replayed block) when the live forest no longer matches
    // the requested block's pre-state commitment.
    if (chain_db_ && block_height > 0) {
        consensus::UtreexoHash live_commitment{};
        readForestShared([&](const consensus::UtreexoForest& f) {
            live_commitment = f.getCommitment();
        });
        if (live_commitment != proof_data.accumulator_root_before) {
            historical_forest.emplace();
            std::string restore_error;
            const auto restore_status = storage::RestoreHistoricalForest(
                *chain_db_, block_height - 1, *historical_forest, restore_error);
            if (restore_status != Status::Ok) {
                if (block_height == 1 && restore_status == Status::NotFound) {
                    // Genesis pre-state: empty accumulator.
                    historical_forest = consensus::UtreexoForest();
                } else {
                    std::ostringstream oss;
                    oss << "BridgeNode::GenerateProofForBlock - Failed to restore forest at height "
                        << (block_height - 1) << ": " << restore_error;
                    throw std::runtime_error(oss.str());
                }
            }

            if (block_height > 1 &&
                historical_forest->getCommitment() != proof_data.accumulator_root_before) {
                std::ostringstream oss;
                oss << "BridgeNode::GenerateProofForBlock - Restored forest root mismatch at height "
                    << (block_height - 1);
                throw std::runtime_error(oss.str());
            }
            if (block_height == 1) {
                const auto expected_root_before = historical_forest->getCommitment();
                if (!proof_data.accumulator_root_before.empty() &&
                    proof_data.accumulator_root_before != expected_root_before) {
                    std::ostringstream oss;
                    oss << "BridgeNode::GenerateProofForBlock - prev-header root drift at height "
                        << block_height << " (using canonical genesis pre-state)";
                    g_logger.warning(oss.str());
                }
            }

            proof_forest = &*historical_forest;
        }
    }

    // Issue #578: when the LIVE forest is the serving forest, proof
    // generation below performs long structural reads (findLeafPosition /
    // prove / verifyBatchProofStateless / getRoots) on P2P worker threads,
    // racing guarded forest writes on the activation side. Snapshot the live
    // forest ONCE under the owner's shared lock and serve from the clone —
    // the same shape historical serving already uses. The historical path is
    // an owned local restore and needs no lock.
    const bool serving_live_state = (proof_forest == utreexo_forest_);
    if (serving_live_state) {
        live_forest_clone.emplace();
        readForestShared([&](const consensus::UtreexoForest& f) {
            *live_forest_clone = f;
        });
        proof_forest = &*live_forest_clone;
    }

    if (!block.header.prev_block_hash.IsNull()) {
        const auto canonical_root_before = proof_forest->getCommitment();
        if (!proof_data.accumulator_root_before.empty() &&
            proof_data.accumulator_root_before != canonical_root_before) {
            std::ostringstream oss;
            oss << "BridgeNode::GenerateProofForBlock - prev-header root drift at height "
                << block_height << " (using canonical checkpoint root)";
            g_logger.warning(oss.str());
        }
        proof_data.accumulator_root_before = canonical_root_before;
    }

    // 2. Load undo record lazily — needed when UTXOs have already been spent
    //    (the UTXO provider only has the *current* UTXO set, but for historical
    //    blocks the spent outputs no longer exist there).
    std::optional<UndoRecord> undo_record;
    bool undo_loaded = false;
    std::vector<consensus::UtreexoHash> spend_targets;
    std::vector<uint64_t> spend_positions;
    const auto ephemeral_outputs = CollectEphemeralOutputs(block);
    const auto intra_block_spent_outputs = CollectIntraBlockSpentOutputs(block, block_height);

    // 2a. Process all transactions (skip coinbase)
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); tx_idx++) {
        const auto& tx = block.vtx[tx_idx];

        // Skip coinbase transaction (no inputs to prove)
        if (tx_idx == 0 && tx.IsCoinbase()) {
            continue;
        }

        // 3. For each input, generate proof
        for (const auto& input : tx.vin) {
            const OutPoint outpoint(input.prevout.txid, input.prevout.vout);
            uint64_t utxo_value = 0;
            std::vector<uint8_t> utxo_script;
            bool utxo_is_confidential = false;
            std::vector<uint8_t> utxo_commitment;
            uint32_t utxo_created_height = 0;
            bool utxo_is_coinbase = false;
            const auto intra_block_it = intra_block_spent_outputs.find(outpoint);
            if (ephemeral_outputs.count(outpoint) != 0 &&
                intra_block_it != intra_block_spent_outputs.end()) {
                utxo_value = intra_block_it->second.value;
                utxo_script = intra_block_it->second.scriptPubKey;
                utxo_is_confidential = intra_block_it->second.is_confidential;
                utxo_commitment = intra_block_it->second.commitment;
                utxo_created_height = intra_block_it->second.created_height;
                utxo_is_coinbase = intra_block_it->second.is_coinbase;
            } else {
                // Look up UTXO from current chainstate
                std::optional<consensus::UTXOEntry> utxo_opt;
                if (serving_live_state) {
                    utxo_opt = utxo_provider_->GetUTXO(outpoint);
                }

                if (utxo_opt.has_value()) {
                    utxo_value = utxo_opt.value().value.GetUna();
                    utxo_script = utxo_opt.value().scriptPubKey;
                    utxo_is_confidential = utxo_opt.value().is_confidential;
                    utxo_commitment = utxo_opt.value().commitment;
                    utxo_created_height = utxo_opt.value().height;
                    utxo_is_coinbase = utxo_opt.value().isCoinbase;
                } else {
                    // UTXO already consumed — fall back to undo record
                    if (!undo_loaded && chain_db_) {
                        auto undo_result = storage::ReadArchivalUndo(
                            *chain_db_,
                            block_storage_,
                            block.GetHash());
                        if (undo_result.ok()) {
                            undo_record = std::move(undo_result.value());
                        }
                        undo_loaded = true;
                    }

                    bool found_in_undo = false;
                    if (undo_record.has_value()) {
                        for (const auto& sc : undo_record->spent) {
                            if (sc.prev_txid == input.prevout.txid.AsUint256() &&
                                sc.prev_vout == input.prevout.vout) {
                                utxo_value = sc.value;
                                utxo_script = sc.scriptPubKey;
                                utxo_is_confidential = sc.is_confidential;
                                utxo_commitment = sc.commitment;
                                utxo_created_height = sc.height;
                                utxo_is_coinbase = sc.is_coinbase;
                                found_in_undo = true;
                                break;
                            }
                        }
                    }

                    if (!found_in_undo) {
                        std::ostringstream oss;
                        oss << "BridgeNode::GenerateProofForBlock - UTXO not found in chainstate or undo: "
                            << input.prevout.txid.AsUint256().GetHex() << ":" << input.prevout.vout;
                        throw std::runtime_error(oss.str());
                    }
                }
            }

            // Store spent output metadata (for stateless validation)
            consensus::SpentOutputData spent_output;
            spent_output.value = utxo_value;
            spent_output.scriptPubKey = utxo_script;
            spent_output.is_confidential = utxo_is_confidential;
            spent_output.commitment = utxo_commitment;
            spent_output.created_height = utxo_created_height;
            spent_output.is_coinbase = utxo_is_coinbase;
            proof_data.spent_outputs.push_back(spent_output);

            // Intra-block spends are ephemeral: they never exist in the
            // pre-block forest, so they carry spent_output metadata but no
            // accumulator target/proof entry.
            if (ephemeral_outputs.count(outpoint) != 0) {
                continue;
            }

            // Compute leaf hash
            consensus::UtreexoHash leaf_hash = ComputeLeafHash(
                input.prevout.txid.AsUint256(),
                input.prevout.vout,
                utxo_value,
                utxo_script,
                utxo_created_height,
                utxo_is_coinbase
            );

            // Historical block proofs must use the canonical per-target sequential
            // format generated by UtreexoForest::generateBlockProof(). Keep a
            // strict existence check here so we fail closed instead of emitting
            // placeholder positions for missing leaves.
            auto position_opt = proof_forest->findLeafPosition(leaf_hash);
            if (!position_opt.has_value()) {
                std::ostringstream oss;
                oss << "BridgeNode::GenerateProofForBlock - Leaf not found in forest: "
                    << crypto::bytes_to_hex(leaf_hash);
                throw std::runtime_error(oss.str());
            }
            spend_targets.push_back(leaf_hash);
            spend_positions.push_back(position_opt.value());
        }
    }

    // 4. Generate the canonical block proof format explicitly from the
    // previously validated leaf positions. This avoids a second permissive
    // lookup inside generateBlockProof() and fails closed if any target cannot
    // produce a full proof.
    proof_data.spend_proof.targets = spend_targets;
    proof_data.spend_proof.positions = spend_positions;
    proof_data.spend_proof.numLeaves = proof_forest->getNumLeaves();
    proof_data.spend_proof.format_version = consensus::GetUtreexoProofFormatVersion(block_height);

    for (size_t i = 0; i < spend_positions.size(); ++i) {
        auto proof_opt = proof_forest->prove(spend_positions[i]);
        if (!proof_opt.has_value()) {
            std::ostringstream oss;
            oss << "BridgeNode::GenerateProofForBlock - Proof generation failed for position "
                << spend_positions[i] << " at target index " << i;
            throw std::runtime_error(oss.str());
        }

        const auto& proof = proof_opt.value();
        proof_data.spend_proof.proof_hashes.insert(
            proof_data.spend_proof.proof_hashes.end(),
            proof.siblings.begin(),
            proof.siblings.end()
        );
    }

    if (!proof_forest->verifyBatchProofStateless(
            proof_data.spend_proof.targets,
            proof_data.spend_proof.positions,
            proof_data.spend_proof.proof_hashes,
            proof_data.spend_proof.numLeaves,
            proof_forest->getRoots())) {
        std::ostringstream oss;
        oss << "BridgeNode::GenerateProofForBlock - Self-verification failed at height "
            << block_height << " targets=" << proof_data.spend_proof.targets.size()
            << " positions=" << proof_data.spend_proof.positions.size()
            << " proof_hashes=" << proof_data.spend_proof.proof_hashes.size()
            << " numLeaves=" << proof_data.spend_proof.numLeaves;
        throw std::runtime_error(oss.str());
    }

    // Phase 9.3: Store in cache (under lock)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        CachedProofEntry entry;
        entry.proof_data = proof_data;
        entry.cached_at = std::chrono::steady_clock::now();
        entry.access_count = 0;
        entry.block_height = block_height;
        entry.root_after.assign(
            block.header.utreexo_root.data,
            block.header.utreexo_root.data + 32
        );

        if (block_proof_cache_.size() >= cache_max_size_) {
            evictOldestCacheEntry();
        }

        uint256 block_hash = block.GetHash();
        // Replace existing entry atomically if present (keeps indices consistent).
        eraseBlockCacheEntryLocked(block_hash);
        block_proof_cache_[block_hash] = std::move(entry);
        cache_lru_list_.push_front(block_hash);
        cache_lru_lookup_[block_hash] = cache_lru_list_.begin();
        addBlockHeightIndexLocked(block_height, block_hash);
    }

    return proof_data;
}

std::optional<std::pair<consensus::UtreexoProof, consensus::SpentOutputData>>
BridgeNode::GenerateProofForUTXO(const uint256& txid, uint32_t vout) {
    // Look up UTXO
    OutPoint outpoint(TxId(txid), vout);
    auto utxo_opt = utxo_provider_->GetUTXO(outpoint);
    if (!utxo_opt.has_value()) {
        return std::nullopt;
    }

    const auto& utxo = utxo_opt.value();

    // Compute leaf hash
    consensus::UtreexoHash leaf_hash = ComputeLeafHash(
        txid,
        vout,
        utxo.value.GetUna(),
        utxo.scriptPubKey,
        utxo.height,
        utxo.isCoinbase
    );

    // Find position + generate proof under ONE shared-lock span (#578): the
    // pair must observe a mutually consistent forest, and this path runs on
    // P2P threads concurrent with guarded forest writes.
    std::optional<uint64_t> position_opt;
    std::optional<consensus::UtreexoProof> proof_opt;
    readForestShared([&](const consensus::UtreexoForest& f) {
        position_opt = f.findLeafPosition(leaf_hash);
        if (position_opt.has_value()) {
            proof_opt = f.prove(position_opt.value());
        }
    });
    if (!position_opt.has_value() || !proof_opt.has_value()) {
        return std::nullopt;
    }

    // Create spent output metadata
    consensus::SpentOutputData spent_output;
    spent_output.value = utxo.value.GetUna();
    spent_output.scriptPubKey = utxo.scriptPubKey;
    spent_output.is_confidential = utxo.is_confidential;
    spent_output.commitment = utxo.commitment;
    spent_output.created_height = utxo.height;
    spent_output.is_coinbase = utxo.isCoinbase;

    return std::make_pair(proof_opt.value(), spent_output);
}

std::optional<std::vector<std::pair<consensus::UtreexoProof, consensus::SpentOutputData>>>
BridgeNode::GenerateProofsForTransaction(const Transaction& tx) {
    const uint256 txid = tx.GetTxid().AsUint256();
    const auto current_root = GetCurrentForestCommitment();

    // Check tx-proof cache first (valid only for current root).
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto cache_it = tx_proof_cache_.find(txid);
        if (cache_it != tx_proof_cache_.end()) {
            if (!isTxCacheEntryExpired(cache_it->second) &&
                cache_it->second.root_at_generation == current_root) {
                cache_hits_++;
                cache_it->second.access_count++;
                touchTxCacheLRU(txid);
                return cache_it->second.proofs;
            }

            eraseTxCacheEntryLocked(txid);
            cache_evictions_++;
        }
        cache_misses_++;
    }

    std::vector<std::pair<consensus::UtreexoProof, consensus::SpentOutputData>> results;

    for (size_t i = 0; i < tx.vin.size(); ++i) {
        const auto& input = tx.vin[i];

        // Skip coinbase inputs
        if (input.prevout.txid.IsNull()) continue;

        auto proof_opt = GenerateProofForUTXO(
            input.prevout.txid.AsUint256(),
            input.prevout.vout
        );

        if (!proof_opt.has_value()) {
            return std::nullopt;  // If any input fails, fail whole tx
        }

        results.push_back(std::move(proof_opt.value()));
    }

    // Cache successful proof generation for repeated utxotx requests at same tip/root.
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        CachedTxProofEntry entry;
        entry.proofs = results;
        entry.root_at_generation = current_root;
        entry.cached_at = std::chrono::steady_clock::now();
        entry.access_count = 0;

        if (tx_proof_cache_.size() >= tx_cache_max_size_) {
            evictOldestTxCacheEntry();
        }
        eraseTxCacheEntryLocked(txid);
        tx_proof_cache_[txid] = std::move(entry);
        tx_cache_lru_list_.push_front(txid);
        tx_cache_lru_lookup_[txid] = tx_cache_lru_list_.begin();
    }

    return results;
}

// ═════════════════════════════════════════════════════════════════════════════
// Request Handling
// ═════════════════════════════════════════════════════════════════════════════

BridgeNode::ProofRequestResult BridgeNode::HandleProofRequest(
    const GetUtreexoProofMessage& request,
    std::function<std::optional<Block>(const uint256&)> block_provider
) {
    ProofRequestResult result;

    // Validate request
    if (!request.isValid()) {
        return result;  // Empty response for invalid request
    }

    // Process each requested block
    for (const auto& block_hash : request.block_hashes) {
        bool served_from_cache = false;

        // Check internal cache first (under lock)
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto cache_it = block_proof_cache_.find(block_hash);
            if (cache_it != block_proof_cache_.end()) {
                if (isCacheEntryExpired(cache_it->second)) {
                    eraseBlockCacheEntryLocked(block_hash);
                    cache_evictions_++;
                } else if (!IsCacheEntryChainFresh(block_hash, cache_it->second)) {
                    // Reorg freshness: never serve cached proofs that fail canonical/root checks.
                    eraseBlockCacheEntryLocked(block_hash);
                    cache_evictions_++;
                } else {
                    cache_hits_++;
                    cache_it->second.access_count++;
                    touchCacheLRU(block_hash);

                    UtreexoProofMessage response;
                    response.block_hash = block_hash;
                    response.block_height = cache_it->second.block_height;
                    response.accumulator_root_before = cache_it->second.proof_data.accumulator_root_before;
                    response.accumulator_root_after = cache_it->second.root_after;
                    response.proof_data = cache_it->second.proof_data;
                    result.proofs.push_back(response);
                    served_from_cache = true;
                }
            }

            if (!served_from_cache) {
                cache_misses_++;
            }
        }

        if (served_from_cache) {
            continue;
        }

        // Cache miss — look up block and generate on-demand
        auto block_opt = block_provider(block_hash);
        if (!block_opt.has_value()) {
            continue;  // Skip blocks we don't have (not backpressure — don't NACK)
        }
        const auto& block = block_opt.value();

        // Look up block height from ChainDB
        uint32_t height = 0;
        if (chain_db_) {
            auto height_result = chain_db_->getBlockHeight(block_hash);
            if (!height_result.ok()) {
                // Require indexed height when ChainDB is available.
                continue;
            }
            height = static_cast<uint32_t>(height_result.value());
            if (!IsCanonicalHashAtHeight(block_hash, height)) {
                // Freshness gate: proof serving is best-chain only.
                continue;
            }
        }

        ProofRejectReason reject_reason = ProofRejectReason::None;
        auto response_opt = GenerateProofForHashViaEngine(block_hash, block, height, &reject_reason);
        if (!response_opt.has_value()) {
            // Only NACK for queue-full — the peer should back off and retry.
            // Other failures (stale, shutdown, worker error) are not retryable.
            if (reject_reason == ProofRejectReason::QueueFull) {
                result.backpressure_rejected.push_back(block_hash);
            }
            continue;
        }
        result.proofs.push_back(std::move(response_opt.value()));
    }

    return result;
}

UtreexoHeadersMessage BridgeNode::HandleHeadersRequest(
    const GetUtreexoHeadersMessage& request,
    std::function<std::optional<BlockHeader>(const uint256&)> header_provider,
    std::function<std::optional<BlockHeader>(uint32_t)> header_by_height_provider
) {
    UtreexoHeadersMessage response;

    // Validate request
    if (!request.isValid()) {
        return response;  // Empty response
    }

    // Find common ancestor using ChainDB height index
    auto find_height_by_hash = [&](const uint256& hash) -> std::optional<uint32_t> {
        if (chain_db_) {
            auto height_result = chain_db_->getBlockHeight(hash);
            if (height_result.ok()) {
                return static_cast<uint32_t>(height_result.value());
            }
        }
        return std::nullopt;
    };

    uint32_t start_height = FindCommonAncestor(request.locator_hashes, find_height_by_hash);

    // Fetch up to MAX_HEADERS_COUNT headers forward
    static constexpr size_t MAX_HEADERS = 2000;
    uint32_t current_height = start_height + 1;

    for (size_t i = 0; i < MAX_HEADERS; i++) {
        auto header_opt = header_by_height_provider(current_height);
        if (!header_opt.has_value()) {
            break;  // Reached chain tip
        }

        BlockHeader header = header_opt.value();
        // BlockHeader already contains utreexo_root field (offset 0x44)

        response.headers.push_back(header);

        // Check stop hash
        uint256 header_hash = header.GetHash();
        if (!request.hash_stop.IsNull() && header_hash == request.hash_stop) {
            break;
        }

        current_height++;
    }

    return response;
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 9.3: Caching & Statistics
// ═══════════════════════════════════════════════════════════════════════════

// (Implementations moved to end of file after internal helpers)

// ═════════════════════════════════════════════════════════════════════════════
// Internal Helpers
// ═════════════════════════════════════════════════════════════════════════════

consensus::UtreexoHash BridgeNode::ComputeLeafHash(
    const uint256& txid,
    uint32_t vout,
    uint64_t value,
    const std::vector<uint8_t>& script_pub_key,
    uint32_t created_height,
    bool is_coinbase
) const {
    return consensus::HashUTXOForCreationHeight(
        txid,
        vout,
        value,
        script_pub_key,
        created_height,
        is_coinbase
    );
}

void BridgeNode::DeduplicateProofHashes(std::vector<consensus::UtreexoHash>& proof_hashes) const {
    // Use set to track unique hashes while preserving order
    std::set<consensus::UtreexoHash> seen;
    std::vector<consensus::UtreexoHash> deduplicated;

    for (const auto& hash : proof_hashes) {
        if (seen.insert(hash).second) {
            // Hash was not seen before
            deduplicated.push_back(hash);
        }
    }

    proof_hashes = std::move(deduplicated);
}

uint32_t BridgeNode::FindCommonAncestor(
    const std::vector<uint256>& locator_hashes,
    std::function<std::optional<uint32_t>(const uint256&)> header_provider
) const {
    // Return the height of the first locator hash that is on our ACTIVE chain.
    //
    // #583: a locator hash may be INDEXED yet belong to a DEAD (orphaned) branch
    // — after a reorg the bridge holds both competing branches in its index, so
    // header_provider (chain_db_->getBlockHeight) resolves a height for either.
    // Accepting a dead-branch hash on its height alone makes us serve forward
    // from the dead branch's tip (start_height + 1), PAST the fork block the
    // requester actually needs — so a peer stranded on a dead branch after a
    // reorg receives an empty / fork-skipping reply and silently freezes,
    // believing it is synced. Verify canonicality (the hash IS the active-chain
    // block at that height) before accepting, so we keep walking the locator's
    // deeper, exponentially-spaced entries down to the last common ancestor with
    // the active chain — from which serving covers the fork block forward.
    for (const auto& hash : locator_hashes) {
        auto height_opt = header_provider(hash);
        if (height_opt.has_value() &&
            IsCanonicalHashAtHeight(hash, height_opt.value())) {
            return height_opt.value();
        }
    }

    // No common ancestor found, return genesis (height 0)
    return 0;
}

bool BridgeNode::IsCanonicalHashAtHeight(const uint256& block_hash, uint32_t height) const {
    if (!chain_db_) {
        return true;
    }

    auto hash_result = chain_db_->getBlockHashByHeight(static_cast<int>(height));
    if (!hash_result.ok()) {
        return false;
    }

    return hash_result.value() == block_hash;
}

BridgeNode::ProofPriority BridgeNode::ClassifyPriority(uint32_t height) const {
    if (!chain_db_) {
        return ProofPriority::Recent;
    }

    auto tip_result = chain_db_->getTip();
    if (!tip_result.ok()) {
        return ProofPriority::Recent;
    }

    const uint32_t tip_height = tip_result.value().height;
    if (height >= tip_height || tip_height - height <= 2) {
        return ProofPriority::TipCritical;
    }
    if (tip_height - height <= 144) {
        return ProofPriority::Recent;
    }
    return ProofPriority::Historical;
}

std::string BridgeNode::MakeCoalesceKey(
    const uint256& block_hash,
    const consensus::UtreexoHash& root_after
) const {
    static constexpr char HEX[] = "0123456789abcdef";

    std::string key;
    key.reserve(64 + 1 + 64);
    key.append(block_hash.GetHex());
    key.push_back(':');
    for (uint8_t byte : root_after) {
        key.push_back(HEX[(byte >> 4) & 0x0f]);
        key.push_back(HEX[byte & 0x0f]);
    }
    return key;
}

void BridgeNode::StartProofWorkers(size_t worker_count) {
    if (worker_count == 0) {
        worker_count = 1;
    }

    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (!proof_workers_.empty()) {
        return;
    }

    proof_workers_shutdown_ = false;
    proof_worker_count_ = worker_count;
    proof_workers_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        proof_workers_.emplace_back([this]() { ProofWorkerLoop(); });
    }
}

void BridgeNode::StopProofWorkers() {
    std::vector<std::thread> workers;
    std::vector<std::shared_ptr<InflightProofState>> notify_states;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        proof_workers_shutdown_ = true;

        while (!tip_queue_.empty()) {
            auto task = std::move(tip_queue_.front());
            tip_queue_.pop_front();
            FailQueuedTaskLocked(std::move(task), "proof worker shutdown");
        }
        while (!recent_queue_.empty()) {
            auto task = std::move(recent_queue_.front());
            recent_queue_.pop_front();
            FailQueuedTaskLocked(std::move(task), "proof worker shutdown");
        }
        while (!historical_queue_.empty()) {
            auto task = std::move(historical_queue_.front());
            historical_queue_.pop_front();
            FailQueuedTaskLocked(std::move(task), "proof worker shutdown");
        }

        workers.swap(proof_workers_);
        proof_worker_count_ = 0;
    }

    proof_queue_cv_.notify_all();
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        active_generations_ = 0;
        for (auto& [key, state] : inflight_proofs_) {
            (void)key;
            if (!state) {
                continue;
            }
            {
                std::lock_guard<std::mutex> state_lock(state->mutex);
                if (!state->done) {
                    state->done = true;
                    state->success = false;
                    state->error = "proof worker shutdown";
                    proof_tasks_failed_++;
                }
            }
            notify_states.push_back(state);
        }
        inflight_proofs_.clear();
    }

    for (const auto& state : notify_states) {
        if (state) {
            state->cv.notify_all();
        }
    }
}

size_t BridgeNode::GetQueuedTaskCountLocked() const {
    return tip_queue_.size() + recent_queue_.size() + historical_queue_.size();
}

bool BridgeNode::PopNextProofTaskLocked(QueuedProofTask& out_task) {
    if (!tip_queue_.empty()) {
        out_task = std::move(tip_queue_.front());
        tip_queue_.pop_front();
        return true;
    }
    if (!recent_queue_.empty()) {
        out_task = std::move(recent_queue_.front());
        recent_queue_.pop_front();
        return true;
    }
    if (!historical_queue_.empty()) {
        out_task = std::move(historical_queue_.front());
        historical_queue_.pop_front();
        return true;
    }
    return false;
}

void BridgeNode::RecordLatencySampleLocked(std::deque<uint64_t>& samples, uint64_t value) {
    samples.push_back(value);
    if (samples.size() > LATENCY_SAMPLE_WINDOW) {
        samples.pop_front();
    }
}

double BridgeNode::ComputePercentileLocked(
    const std::deque<uint64_t>& samples,
    double pct
) const {
    if (samples.empty()) {
        return 0.0;
    }

    std::vector<uint64_t> sorted(samples.begin(), samples.end());
    std::sort(sorted.begin(), sorted.end());

    const double clamped = std::clamp(pct, 0.0, 1.0);
    const size_t idx = static_cast<size_t>(
        std::ceil((sorted.size() - 1) * clamped)
    );
    return static_cast<double>(sorted[idx]);
}

void BridgeNode::FailQueuedTaskLocked(QueuedProofTask&& task, const std::string& error) {
    std::shared_ptr<InflightProofState> state = task.state;
    if (!state) {
        auto it = inflight_proofs_.find(task.key);
        if (it != inflight_proofs_.end()) {
            state = it->second;
        }
    }
    if (!state) {
        inflight_proofs_.erase(task.key);
        return;
    }

    {
        std::lock_guard<std::mutex> state_lock(state->mutex);
        if (!state->done) {
            state->done = true;
            state->success = false;
            state->error = error;
            state->block_height = task.block_height;
            proof_tasks_failed_++;
        }
    }

    if (state->waiters == 0) {
        inflight_proofs_.erase(task.key);
    }
    state->cv.notify_all();
}

std::optional<UtreexoProofMessage> BridgeNode::GenerateProofForHashViaEngine(
    const uint256& block_hash,
    const Block& block,
    uint32_t height,
    ProofRejectReason* reject_reason
) {
    consensus::UtreexoHash root_after;
    root_after.assign(
        block.header.utreexo_root.data,
        block.header.utreexo_root.data + 32
    );
    const std::string key = MakeCoalesceKey(block_hash, root_after);
    const ProofPriority priority = ClassifyPriority(height);
    const auto now = std::chrono::steady_clock::now();

    std::shared_ptr<InflightProofState> state;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        proof_requests_total_++;

        auto inflight_it = inflight_proofs_.find(key);
        if (inflight_it != inflight_proofs_.end()) {
            state = inflight_it->second;
            state->waiters++;
            proof_requests_coalesced_++;
        } else {
            if (proof_workers_shutdown_) {
                proof_requests_rejected_++;
                switch (priority) {
                    case ProofPriority::TipCritical: tip_priority_rejected_++; break;
                    case ProofPriority::Recent: recent_priority_rejected_++; break;
                    case ProofPriority::Historical: historical_priority_rejected_++; break;
                }
                if (reject_reason) *reject_reason = ProofRejectReason::Shutdown;
                return std::nullopt;
            }

            // Backpressure with priority-aware preemption.
            size_t queued = GetQueuedTaskCountLocked();
            if (queued >= proof_queue_capacity_) {
                bool preempted = false;
                if ((priority == ProofPriority::TipCritical || priority == ProofPriority::Recent) &&
                    !historical_queue_.empty()) {
                    auto dropped = std::move(historical_queue_.back());
                    historical_queue_.pop_back();
                    FailQueuedTaskLocked(std::move(dropped), "preempted by higher priority request");
                    preempted = true;
                }
                if (!preempted && priority == ProofPriority::TipCritical && !recent_queue_.empty()) {
                    auto dropped = std::move(recent_queue_.back());
                    recent_queue_.pop_back();
                    FailQueuedTaskLocked(std::move(dropped), "preempted by tip-critical request");
                    preempted = true;
                }
                queued = GetQueuedTaskCountLocked();
                if (!preempted || queued >= proof_queue_capacity_) {
                    proof_requests_rejected_++;
                    switch (priority) {
                        case ProofPriority::TipCritical: tip_priority_rejected_++; break;
                        case ProofPriority::Recent: recent_priority_rejected_++; break;
                        case ProofPriority::Historical: historical_priority_rejected_++; break;
                    }
                    if (reject_reason) *reject_reason = ProofRejectReason::QueueFull;
                    return std::nullopt;
                }
            }

            state = std::make_shared<InflightProofState>();
            state->waiters = 1;
            state->block_height = height;
            inflight_proofs_[key] = state;

            QueuedProofTask task;
            task.key = key;
            task.block_hash = block_hash;
            task.block = block;
            task.block_height = height;
            task.priority = priority;
            task.state = state;
            task.enqueued_at = now;

            switch (priority) {
                case ProofPriority::TipCritical:
                    tip_queue_.push_back(std::move(task));
                    tip_priority_accepted_++;
                    break;
                case ProofPriority::Recent:
                    recent_queue_.push_back(std::move(task));
                    recent_priority_accepted_++;
                    break;
                case ProofPriority::Historical:
                    historical_queue_.push_back(std::move(task));
                    historical_priority_accepted_++;
                    break;
            }
            proof_queue_cv_.notify_one();
        }
    }

    if (!state) {
        return std::nullopt;
    }

    {
        std::unique_lock<std::mutex> state_lock(state->mutex);
        state->cv.wait(state_lock, [&state]() {
            return state->done;
        });
    }

    bool success = false;
    uint32_t response_height = 0;
    consensus::UtreexoHash response_root_after;
    consensus::BlockUtreexoData response_proof;
    {
        std::lock_guard<std::mutex> state_lock(state->mutex);
        success = state->success;
        response_height = state->block_height;
        response_root_after = state->root_after;
        response_proof = state->proof_data;
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (state->waiters > 0) {
            state->waiters--;
        }
        auto it = inflight_proofs_.find(key);
        if (it != inflight_proofs_.end() && it->second == state && state->waiters == 0 && state->done) {
            inflight_proofs_.erase(it);
        }
    }

    if (!success) {
        if (reject_reason) *reject_reason = ProofRejectReason::WorkerFailed;
        return std::nullopt;
    }

    // TIER-0 SAFETY: Re-check chain freshness after async generation.
    // A reorg may have occurred while the worker was computing the proof,
    // making the result valid for a now-orphaned block.
    {
        CachedProofEntry freshness_check;
        freshness_check.proof_data = response_proof;
        freshness_check.block_height = response_height;
        freshness_check.root_after = response_root_after;
        if (!IsCacheEntryChainFresh(block_hash, freshness_check)) {
            if (reject_reason) *reject_reason = ProofRejectReason::Stale;
            return std::nullopt;
        }
    }

    UtreexoProofMessage response;
    response.block_hash = block_hash;
    response.block_height = response_height;
    response.accumulator_root_before = response_proof.accumulator_root_before;
    response.accumulator_root_after = response_root_after;
    response.proof_data = std::move(response_proof);
    return response;
}

void BridgeNode::ProofWorkerLoop() {
    while (true) {
        QueuedProofTask task;
        {
            std::unique_lock<std::mutex> lock(cache_mutex_);
            proof_queue_cv_.wait(lock, [this]() {
                return proof_workers_shutdown_ || GetQueuedTaskCountLocked() > 0;
            });

            if (proof_workers_shutdown_ && GetQueuedTaskCountLocked() == 0) {
                return;
            }
            if (!PopNextProofTaskLocked(task)) {
                continue;
            }

            active_generations_++;
            const auto now = std::chrono::steady_clock::now();
            const uint64_t queue_wait_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - task.enqueued_at).count()
            );
            RecordLatencySampleLocked(proof_queue_wait_ms_, queue_wait_ms);
        }

        consensus::BlockUtreexoData proof_data;
        consensus::UtreexoHash root_after;
        bool success = false;
        std::string error;
        const auto generation_start = std::chrono::steady_clock::now();
        try {
            proof_data = GenerateProofForBlock(task.block, task.block_height);
            root_after.assign(
                task.block.header.utreexo_root.data,
                task.block.header.utreexo_root.data + 32
            );
            success = true;
        } catch (const std::exception& e) {
            error = e.what();
        } catch (...) {
            error = "unknown proof generation failure";
        }

        if (!success) {
            g_logger.warning("[BridgeNode] Proof generation failed for " +
                             task.block_hash.GetHex().substr(0, 16) +
                             "... at height " + std::to_string(task.block_height) +
                             ": " + error);
        }

        const uint64_t generation_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - generation_start
            ).count()
        );

        std::shared_ptr<InflightProofState> state = task.state;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (active_generations_ > 0) {
                active_generations_--;
            }
            RecordLatencySampleLocked(proof_generation_latency_ms_, generation_ms);
            if (success) {
                proof_tasks_completed_++;
            } else {
                proof_tasks_failed_++;
            }

            auto it = inflight_proofs_.find(task.key);
            if (it != inflight_proofs_.end()) {
                state = it->second;
            }
            if (!state) {
                continue;
            }

            {
                std::lock_guard<std::mutex> state_lock(state->mutex);
                state->done = true;
                state->success = success;
                state->error = error;
                state->block_height = task.block_height;
                if (success) {
                    state->proof_data = std::move(proof_data);
                    state->root_after = root_after;
                }
            }

            if (state->waiters == 0) {
                inflight_proofs_.erase(task.key);
            }
        }

        state->cv.notify_all();
    }
}

bool BridgeNode::IsCacheEntryChainFresh(
    const uint256& block_hash,
    const CachedProofEntry& entry
) const {
    if (!chain_db_) {
        return true;
    }

    auto indexed_height_result = chain_db_->getBlockHeight(block_hash);
    if (!indexed_height_result.ok()) {
        return false;
    }
    const uint32_t indexed_height = static_cast<uint32_t>(indexed_height_result.value());
    if (indexed_height != entry.block_height) {
        return false;
    }

    if (!IsCanonicalHashAtHeight(block_hash, entry.block_height)) {
        return false;
    }

    auto block_result = storage::ReadArchivalBlock(*chain_db_, block_storage_, block_hash);
    if (!block_result.ok()) {
        return false;
    }
    const Block& block = block_result.value();

    consensus::UtreexoHash expected_root_after;
    expected_root_after.assign(
        block.header.utreexo_root.data,
        block.header.utreexo_root.data + 32
    );
    if (entry.root_after.size() != 32 || entry.root_after != expected_root_after) {
        return false;
    }

    consensus::UtreexoHash expected_root_before;
    if (entry.block_height == 0) {
        if (!block.header.prev_block_hash.IsNull()) {
            return false;
        }
        expected_root_before.assign(32, 0x00);
    } else {
        if (block.header.prev_block_hash.IsNull()) {
            return false;
        }

        auto prev_hash_result = chain_db_->getBlockHashByHeight(static_cast<int>(entry.block_height - 1));
        if (!prev_hash_result.ok() || prev_hash_result.value() != block.header.prev_block_hash) {
            return false;
        }

        auto prev_block_result = storage::ReadArchivalBlock(*chain_db_, block_storage_, block.header.prev_block_hash);
        if (!prev_block_result.ok()) {
            return false;
        }
        const Block& prev_block = prev_block_result.value();
        expected_root_before.assign(
            prev_block.header.utreexo_root.data,
            prev_block.header.utreexo_root.data + 32
        );
    }

    return entry.proof_data.accumulator_root_before == expected_root_before;
}

// ═════════════════════════════════════════════════════════════════════════════
// Phase 9.3: Cache Management Implementation
// ═════════════════════════════════════════════════════════════════════════════

void BridgeNode::evictOldestCacheEntry() {
    if (cache_lru_list_.empty()) return;

    // Remove least recently used (back of list)
    const uint256 oldest_hash = cache_lru_list_.back();
    if (eraseBlockCacheEntryLocked(oldest_hash)) {
        cache_evictions_++;
    }
}

void BridgeNode::touchCacheLRU(const uint256& block_hash) {
    auto it = cache_lru_lookup_.find(block_hash);
    if (it == cache_lru_lookup_.end()) {
        auto fallback = std::find(cache_lru_list_.begin(), cache_lru_list_.end(), block_hash);
        if (fallback == cache_lru_list_.end()) {
            return;
        }
        it = cache_lru_lookup_.emplace(block_hash, fallback).first;
    }
    cache_lru_list_.splice(cache_lru_list_.begin(), cache_lru_list_, it->second);
    it->second = cache_lru_list_.begin();
}

bool BridgeNode::isCacheEntryExpired(const CachedProofEntry& entry) const {
    auto now = std::chrono::steady_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::minutes>(now - entry.cached_at);
    return age >= cache_ttl_;
}

void BridgeNode::ClearCache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_evictions_ += block_proof_cache_.size();
    cache_evictions_ += tx_proof_cache_.size();
    block_proof_cache_.clear();
    cache_lru_list_.clear();
    cache_lru_lookup_.clear();
    block_height_index_.clear();
    tx_proof_cache_.clear();
    tx_cache_lru_list_.clear();
    tx_cache_lru_lookup_.clear();
}

void BridgeNode::InvalidateTxProofCache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_evictions_ += tx_proof_cache_.size();
    tx_proof_cache_.clear();
    tx_cache_lru_list_.clear();
    tx_cache_lru_lookup_.clear();
}

size_t BridgeNode::PruneStaleCacheEntries() {
    const auto current_root = GetCurrentForestCommitment();
    std::lock_guard<std::mutex> lock(cache_mutex_);
    std::vector<uint256> stale_keys;
    stale_keys.reserve(block_proof_cache_.size());
    std::vector<uint256> stale_tx_keys;
    stale_tx_keys.reserve(tx_proof_cache_.size());

    for (const auto& [block_hash, entry] : block_proof_cache_) {
        if (isCacheEntryExpired(entry) || !IsCacheEntryChainFresh(block_hash, entry)) {
            stale_keys.push_back(block_hash);
        }
    }
    for (const auto& [txid, entry] : tx_proof_cache_) {
        if (isTxCacheEntryExpired(entry) || entry.root_at_generation != current_root) {
            stale_tx_keys.push_back(txid);
        }
    }

    for (const auto& block_hash : stale_keys) {
        if (eraseBlockCacheEntryLocked(block_hash)) {
            cache_evictions_++;
        }
    }
    for (const auto& txid : stale_tx_keys) {
        if (eraseTxCacheEntryLocked(txid)) {
            cache_evictions_++;
        }
    }

    const size_t evicted_total = stale_keys.size() + stale_tx_keys.size();
    return evicted_total;
}

size_t BridgeNode::EvictBlockProofsAtOrAboveHeight(uint32_t min_height) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    std::vector<uint32_t> heights_to_evict;
    heights_to_evict.reserve(block_height_index_.size());
    for (const auto& entry : block_height_index_) {
        if (entry.first >= min_height) {
            heights_to_evict.push_back(entry.first);
        }
    }

    size_t evicted = 0;
    for (uint32_t height : heights_to_evict) {
        auto height_it = block_height_index_.find(height);
        if (height_it == block_height_index_.end()) {
            continue;
        }

        std::vector<uint256> hashes(height_it->second.begin(), height_it->second.end());
        for (const auto& hash : hashes) {
            if (eraseBlockCacheEntryLocked(hash)) {
                ++evicted;
            }
        }
    }

    cache_evictions_ += evicted;
    return evicted;
}

void BridgeNode::SetCacheSize(size_t max_entries) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_max_size_ = max_entries;

    // Evict excess entries if new size is smaller
    while (block_proof_cache_.size() > cache_max_size_) {
        evictOldestCacheEntry();
    }
}

void BridgeNode::evictOldestTxCacheEntry() {
    if (tx_cache_lru_list_.empty()) return;

    const uint256 oldest_txid = tx_cache_lru_list_.back();
    if (eraseTxCacheEntryLocked(oldest_txid)) {
        cache_evictions_++;
    }
}

void BridgeNode::touchTxCacheLRU(const uint256& txid) {
    auto it = tx_cache_lru_lookup_.find(txid);
    if (it == tx_cache_lru_lookup_.end()) {
        auto fallback = std::find(tx_cache_lru_list_.begin(), tx_cache_lru_list_.end(), txid);
        if (fallback == tx_cache_lru_list_.end()) {
            return;
        }
        it = tx_cache_lru_lookup_.emplace(txid, fallback).first;
    }
    tx_cache_lru_list_.splice(tx_cache_lru_list_.begin(), tx_cache_lru_list_, it->second);
    it->second = tx_cache_lru_list_.begin();
}

bool BridgeNode::isTxCacheEntryExpired(const CachedTxProofEntry& entry) const {
    const auto now = std::chrono::steady_clock::now();
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - entry.cached_at);
    return age >= tx_cache_ttl_;
}

consensus::ProofCache::Stats BridgeNode::GetCacheStats() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    consensus::ProofCache::Stats stats;
    stats.hits = cache_hits_;
    stats.misses = cache_misses_;
    stats.evictions = cache_evictions_;
    const uint64_t total = stats.hits + stats.misses;
    stats.hit_rate = total ? static_cast<double>(stats.hits) / static_cast<double>(total) : 0.0;
    return stats;
}

BridgeNode::CacheSnapshot BridgeNode::GetCacheSnapshot() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    CacheSnapshot snapshot;
    snapshot.hits = cache_hits_;
    snapshot.misses = cache_misses_;
    snapshot.evictions = cache_evictions_;

    const uint64_t total = snapshot.hits + snapshot.misses;
    snapshot.hit_rate = total
        ? static_cast<double>(snapshot.hits) / static_cast<double>(total)
        : 0.0;

    snapshot.block_entries = block_proof_cache_.size();
    snapshot.block_capacity = cache_max_size_;
    snapshot.tx_entries = tx_proof_cache_.size();
    snapshot.tx_capacity = tx_cache_max_size_;
    snapshot.indexed_heights = block_height_index_.size();
    snapshot.block_ttl_seconds =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(cache_ttl_).count());
    snapshot.tx_ttl_seconds =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(tx_cache_ttl_).count());

    size_t indexed_blocks = 0;
    for (const auto& entry : block_height_index_) {
        indexed_blocks += entry.second.size();
    }
    snapshot.indexed_blocks = indexed_blocks;

    snapshot.proof_requests_total = proof_requests_total_;
    snapshot.proof_requests_rejected = proof_requests_rejected_;
    snapshot.proof_requests_coalesced = proof_requests_coalesced_;
    snapshot.proof_tasks_completed = proof_tasks_completed_;
    snapshot.proof_tasks_failed = proof_tasks_failed_;
    snapshot.proof_queue_depth = GetQueuedTaskCountLocked();
    snapshot.proof_queue_capacity = proof_queue_capacity_;
    snapshot.proof_workers = proof_worker_count_;
    snapshot.active_generations = active_generations_;
    snapshot.tip_priority_accepted = tip_priority_accepted_;
    snapshot.recent_priority_accepted = recent_priority_accepted_;
    snapshot.historical_priority_accepted = historical_priority_accepted_;
    snapshot.tip_priority_rejected = tip_priority_rejected_;
    snapshot.recent_priority_rejected = recent_priority_rejected_;
    snapshot.historical_priority_rejected = historical_priority_rejected_;
    snapshot.proof_generation_p50_ms = ComputePercentileLocked(proof_generation_latency_ms_, 0.50);
    snapshot.proof_generation_p95_ms = ComputePercentileLocked(proof_generation_latency_ms_, 0.95);
    snapshot.proof_generation_p99_ms = ComputePercentileLocked(proof_generation_latency_ms_, 0.99);
    snapshot.queue_wait_p50_ms = ComputePercentileLocked(proof_queue_wait_ms_, 0.50);
    snapshot.queue_wait_p95_ms = ComputePercentileLocked(proof_queue_wait_ms_, 0.95);
    snapshot.queue_wait_p99_ms = ComputePercentileLocked(proof_queue_wait_ms_, 0.99);

    return snapshot;
}

bool BridgeNode::PreCacheProofForBlock(const Block& block, uint32_t block_height) {
    try {
        // Generate and cache proof
        GenerateProofForBlock(block, block_height);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool BridgeNode::eraseBlockCacheEntryLocked(const uint256& block_hash) {
    auto cache_it = block_proof_cache_.find(block_hash);
    if (cache_it == block_proof_cache_.end()) {
        return false;
    }

    removeBlockHeightIndexLocked(cache_it->second.block_height, block_hash);
    block_proof_cache_.erase(cache_it);

    auto lru_it = cache_lru_lookup_.find(block_hash);
    if (lru_it != cache_lru_lookup_.end()) {
        cache_lru_list_.erase(lru_it->second);
        cache_lru_lookup_.erase(lru_it);
    } else {
        auto fallback = std::find(cache_lru_list_.begin(), cache_lru_list_.end(), block_hash);
        if (fallback != cache_lru_list_.end()) {
            cache_lru_list_.erase(fallback);
        }
    }

    return true;
}

bool BridgeNode::eraseTxCacheEntryLocked(const uint256& txid) {
    auto cache_it = tx_proof_cache_.find(txid);
    if (cache_it == tx_proof_cache_.end()) {
        return false;
    }
    tx_proof_cache_.erase(cache_it);

    auto lru_it = tx_cache_lru_lookup_.find(txid);
    if (lru_it != tx_cache_lru_lookup_.end()) {
        tx_cache_lru_list_.erase(lru_it->second);
        tx_cache_lru_lookup_.erase(lru_it);
    } else {
        auto fallback = std::find(tx_cache_lru_list_.begin(), tx_cache_lru_list_.end(), txid);
        if (fallback != tx_cache_lru_list_.end()) {
            tx_cache_lru_list_.erase(fallback);
        }
    }
    return true;
}

void BridgeNode::addBlockHeightIndexLocked(uint32_t height, const uint256& block_hash) {
    block_height_index_[height].insert(block_hash);
}

void BridgeNode::removeBlockHeightIndexLocked(uint32_t height, const uint256& block_hash) {
    auto height_it = block_height_index_.find(height);
    if (height_it == block_height_index_.end()) {
        return;
    }

    height_it->second.erase(block_hash);
    if (height_it->second.empty()) {
        block_height_index_.erase(height_it);
    }
}

void BridgeNode::SetCachedRootAfter(const uint256& block_hash, uint32_t block_height,
                                     const consensus::UtreexoHash& root_after) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = block_proof_cache_.find(block_hash);
    if (it != block_proof_cache_.end()) {
        if (it->second.block_height != block_height) {
            removeBlockHeightIndexLocked(it->second.block_height, block_hash);
            addBlockHeightIndexLocked(block_height, block_hash);
        }
        it->second.block_height = block_height;
        it->second.root_after = root_after;
    }
}

void BridgeNode::SetCachedTransitionProof(const uint256& block_hash,
                                           const consensus::UtreexoTransitionProof& tp) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = block_proof_cache_.find(block_hash);
    if (it != block_proof_cache_.end()) {
        it->second.transition_proof = tp;
    }
}

std::optional<consensus::UtreexoTransitionProof> BridgeNode::GetTransitionProof(
    const uint256& block_hash) const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = block_proof_cache_.find(block_hash);
    if (it != block_proof_cache_.end() && it->second.transition_proof.has_value()) {
        return it->second.transition_proof;
    }
    return std::nullopt;
}

} // namespace network
} // namespace dinero
