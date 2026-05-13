#include "network/stateless_node.h"
#include "network/types.h"
#include "daemon/peer_connection.h"
#include "common/logger.h"
#include "consensus/outpoint.h"
#include "consensus/utreexo_stump.h"
#include "crypto/sha256.h"
#include "primitives/uint256.h"
#include <stdexcept>
#include <algorithm>
#include <set>
#include <sstream>
#include <cstring>
#include <unordered_set>

namespace dinero {
namespace network {

namespace {

std::unordered_set<OutPoint> CollectEphemeralOutputs(const Block& block) {
    std::unordered_set<OutPoint> intra_block_outputs;
    std::unordered_set<OutPoint> ephemeral_outputs;

    for (const auto& tx : block.vtx) {
        const TxId txid = tx.GetTxid();
        for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
            intra_block_outputs.insert(OutPoint(txid, vout));
        }
    }

    for (size_t tx_idx = 1; tx_idx < block.vtx.size(); ++tx_idx) {
        for (const auto& input : block.vtx[tx_idx].vin) {
            const OutPoint prevout(input.prevout.txid, input.prevout.vout);
            if (intra_block_outputs.count(prevout) != 0) {
                ephemeral_outputs.insert(prevout);
            }
        }
    }

    return ephemeral_outputs;
}

std::vector<consensus::UtreexoHash> ComputeCanonicalAdditionHashes(const Block& block) {
    const auto ephemeral_outputs = CollectEphemeralOutputs(block);
    std::vector<consensus::UtreexoHash> additions;

    for (const auto& tx : block.vtx) {
        const TxId txid = tx.GetTxid();
        for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
            if (ephemeral_outputs.count(OutPoint(txid, vout)) != 0) {
                continue;
            }

            const auto& output = tx.vout[vout];
            additions.push_back(consensus::HashUTXO(
                txid.AsUint256(),
                vout,
                output.value.GetUna(),
                output.scriptPubKey
            ));
        }
    }

    return additions;
}

bool ApplyAccumulatorDelta(
    consensus::UtreexoForest& forest,
    const std::vector<consensus::UtreexoHash>& spend_targets,
    const std::vector<consensus::UtreexoHash>& additions,
    const char* context
) {
    try {
        for (size_t i = 0; i < spend_targets.size(); ++i) {
            const auto& target = spend_targets[i];

            auto position_opt = forest.findLeafPosition(target);
            if (!position_opt.has_value()) {
                g_logger.error(std::string(context) +
                              ": leaf not found in forest at index " +
                              std::to_string(i) + "/" +
                              std::to_string(spend_targets.size()));
                return false;
            }

            auto proof_opt = forest.prove(position_opt.value());
            if (!proof_opt.has_value()) {
                g_logger.error(std::string(context) +
                              ": prove() failed for position " +
                              std::to_string(position_opt.value()) +
                              " at index " + std::to_string(i));
                return false;
            }

            if (!forest.remove(target, proof_opt.value())) {
                g_logger.error(std::string(context) +
                              ": remove() failed for position " +
                              std::to_string(position_opt.value()) +
                              " at index " + std::to_string(i));
                return false;
            }
        }

        for (size_t i = 0; i < additions.size(); ++i) {
            if (forest.add(additions[i]) == UINT64_MAX) {
                g_logger.error(std::string(context) +
                              ": add() failed at addition index " +
                              std::to_string(i));
                return false;
            }
        }

        return true;
    } catch (const std::exception& e) {
        g_logger.error(std::string(context) + ": exception: " + std::string(e.what()));
        return false;
    }
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Constructor
// ═════════════════════════════════════════════════════════════════════════════

StatelessNode::StatelessNode(consensus::UtreexoForest* utreexo_forest)
    : utreexo_forest_(utreexo_forest)
    , current_sync_height_(0)
    , sync_state_(StatelessSyncState::IDLE)
    , next_bridge_index_(0)
{
    if (!utreexo_forest_) {
        throw std::invalid_argument("StatelessNode: utreexo_forest cannot be null");
    }

    // Seed stump from forest (extracts roots only — ~2KB)
    local_stump_ = consensus::UtreexoStump::fromForest(*utreexo_forest_);
    local_commitment_ = local_stump_.getCommitment();
    local_num_leaves_ = local_stump_.getNumLeaves();

    // Initialize sync stats
    sync_stats_ = {
        0,  // current_height
        0,  // target_height
        0,  // proofs_requested
        0,  // proofs_received
        0,  // proofs_validated
        0,  // proofs_failed
        0,  // blocks_applied
        0,  // root_continuity_errors
        StatelessSyncState::IDLE
    };
}

// ═════════════════════════════════════════════════════════════════════════════
// Peer Management
// ═════════════════════════════════════════════════════════════════════════════

void StatelessNode::AddBridgePeer(const BridgePeer& peer) {
    bridge_peers_[peer.peer_id] = peer;
}

void StatelessNode::RemoveBridgePeer(uint64_t peer_id) {
    bridge_peers_.erase(peer_id);
}

std::vector<BridgePeer> StatelessNode::SelectBridgeNodes(size_t count) const {
    std::vector<BridgePeer> candidates;

    // 1. Filter for bridge capability (NODE_UTREEXO_BRIDGE)
    for (const auto& [peer_id, peer] : bridge_peers_) {
        if (peer.service_flags & ServiceFlags::NODE_UTREEXO_BRIDGE) {
            candidates.push_back(peer);
        }
    }

    // 2. Sort by quality score (success rate + latency)
    std::sort(candidates.begin(), candidates.end(), [](const BridgePeer& a, const BridgePeer& b) {
        return a.GetQualityScore() > b.GetQualityScore();
    });

    // 3. Apply subnet diversity (prefer peers from different subnets)
    std::vector<BridgePeer> selected;
    std::set<std::string> used_subnets;

    // First pass: select best peers from unique subnets
    for (const auto& peer : candidates) {
        if (selected.size() >= count) break;

        if (used_subnets.find(peer.subnet) == used_subnets.end()) {
            selected.push_back(peer);
            used_subnets.insert(peer.subnet);
        }
    }

    // Second pass: fill remaining slots from any subnet
    for (const auto& peer : candidates) {
        if (selected.size() >= count) break;

        // Check if already selected
        bool already_selected = false;
        for (const auto& sel_peer : selected) {
            if (sel_peer.peer_id == peer.peer_id) {
                already_selected = true;
                break;
            }
        }

        if (!already_selected) {
            selected.push_back(peer);
        }
    }

    return selected;
}

std::vector<BridgePeer> StatelessNode::GetBridgePeers() const {
    std::vector<BridgePeer> peers;
    for (const auto& [peer_id, peer] : bridge_peers_) {
        peers.push_back(peer);
    }
    return peers;
}

void StatelessNode::ClearBridgePeers() {
    bridge_peers_.clear();
}

// ═════════════════════════════════════════════════════════════════════════════
// Proof Request & Validation
// ═════════════════════════════════════════════════════════════════════════════

size_t StatelessNode::RequestProofsForBlocks(
    const std::vector<uint256>& block_hashes,
    std::function<void(uint64_t peer_id, const GetUtreexoProofMessage&)> bridge_peer_callback
) {
    // Validate batch size (max 16 from design)
    if (block_hashes.empty() || block_hashes.size() > 16) {
        return 0;
    }

    // Select bridge nodes
    auto bridge_nodes = SelectBridgeNodes(8);
    if (bridge_nodes.empty()) {
        return 0;  // No bridge nodes available
    }

    // Distribute requests across bridge nodes (round-robin)
    size_t requests_sent = 0;

    for (const auto& block_hash : block_hashes) {
        // Select next bridge node
        BridgePeer& bridge = bridge_nodes[next_bridge_index_ % bridge_nodes.size()];

        // Create request message
        GetUtreexoProofMessage request;
        request.block_hashes.push_back(block_hash);

        // Send request via callback
        bridge_peer_callback(bridge.peer_id, request);

        // Update statistics
        bridge_peers_[bridge.peer_id].proofs_requested++;
        sync_stats_.proofs_requested++;
        requests_sent++;

        // Advance round-robin index
        next_bridge_index_++;
    }

    return requests_sent;
}

bool StatelessNode::ValidateUtreexoProof(
    const Block& block,
    const UtreexoProofMessage& proof_msg,
    uint64_t peer_id
) {
    // Helper: UtreexoHash (vector<uint8_t>) to hex string (first 16 chars)
    auto hashHex = [](const consensus::UtreexoHash& h) -> std::string {
        if (h.empty()) return "(empty)";
        std::string hex;
        hex.reserve(h.size() * 2);
        for (uint8_t b : h) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", b);
            hex += buf;
        }
        return hex.substr(0, 16) + "...";
    };

    // Update statistics
    sync_stats_.proofs_received++;

    if (bridge_peers_.find(peer_id) != bridge_peers_.end()) {
        bridge_peers_[peer_id].proofs_received++;
    }

    // Resync stump from forest if external mutation occurred
    {
        const consensus::UtreexoHash forest_commitment = utreexo_forest_->getCommitment();
        const uint64_t forest_num_leaves = utreexo_forest_->getNumLeaves();
        if (local_commitment_ != forest_commitment || local_num_leaves_ != forest_num_leaves) {
            g_logger.warning("[StatelessNode] Stump diverged from forest at height " +
                             std::to_string(proof_msg.block_height) +
                             " — rebuilding stump from forest");
            local_stump_ = consensus::UtreexoStump::fromForest(*utreexo_forest_);
            local_commitment_ = local_stump_.getCommitment();
            local_num_leaves_ = local_stump_.getNumLeaves();
        }
    }

    // 1. Block hash matches proof message
    uint256 block_hash = block.GetHash();
    if (block_hash != proof_msg.block_hash) {
        g_logger.error("[StatelessNode] FAIL step 1: block hash mismatch: computed=" +
                      block_hash.GetHex().substr(0, 16) + " proof=" +
                      proof_msg.block_hash.GetHex().substr(0, 16));
        sync_stats_.proofs_failed++;
        if (bridge_peers_.find(peer_id) != bridge_peers_.end()) {
            bridge_peers_[peer_id].proofs_failed++;
        }
        return false;
    }

    // 2. Root continuity check
    consensus::UtreexoHash current_root = local_commitment_;
    if (current_root != proof_msg.accumulator_root_before) {
        g_logger.error("[StatelessNode] FAIL step 2: root_before mismatch at height " +
                      std::to_string(proof_msg.block_height) +
                      ": local=" + hashHex(current_root) +
                      " proof=" + hashHex(proof_msg.accumulator_root_before) +
                      " (forest_size=" + std::to_string(current_root.size()) +
                      " proof_size=" + std::to_string(proof_msg.accumulator_root_before.size()) + ")");
        sync_stats_.root_continuity_errors++;
        sync_stats_.proofs_failed++;
        if (bridge_peers_.find(peer_id) != bridge_peers_.end()) {
            bridge_peers_[peer_id].proofs_failed++;
        }
        return false;
    }

    // 3. Extract targets from block inputs
    std::vector<consensus::UtreexoHash> targets = ExtractTargetsFromBlock(
        block,
        proof_msg.proof_data.spent_outputs
    );

    // 4. Verify Utreexo batch proof
    // Check that targets match proof_data targets
    if (targets.size() != proof_msg.proof_data.spend_proof.targets.size()) {
        g_logger.error("[StatelessNode] FAIL step 4a: target count mismatch at height " +
                      std::to_string(proof_msg.block_height) +
                      ": extracted=" + std::to_string(targets.size()) +
                      " proof=" + std::to_string(proof_msg.proof_data.spend_proof.targets.size()));
        sync_stats_.proofs_failed++;
        if (bridge_peers_.find(peer_id) != bridge_peers_.end()) {
            bridge_peers_[peer_id].proofs_failed++;
        }
        return false;
    }

    // Verify each target matches
    for (size_t i = 0; i < targets.size(); i++) {
        if (targets[i] != proof_msg.proof_data.spend_proof.targets[i]) {
            g_logger.error("[StatelessNode] FAIL step 4b: target hash mismatch at index " +
                          std::to_string(i) + " height " +
                          std::to_string(proof_msg.block_height) +
                          ": extracted=" + hashHex(targets[i]) +
                          " proof=" + hashHex(proof_msg.proof_data.spend_proof.targets[i]));
            sync_stats_.proofs_failed++;
            if (bridge_peers_.find(peer_id) != bridge_peers_.end()) {
                bridge_peers_[peer_id].proofs_failed++;
            }
            return false;
        }
    }

    // 4c. Verify batch proof cryptographically via stump (pure root math)
    const auto& spend_proof = proof_msg.proof_data.spend_proof;
    if (!spend_proof.targets.empty()) {
        bool proof_valid = local_stump_.verifyBatchProof(
            spend_proof.targets,
            spend_proof.positions,
            spend_proof.proof_hashes
        );
        if (!proof_valid) {
            g_logger.error("[StatelessNode] FAIL step 4c: batch proof crypto verification failed at height " +
                          std::to_string(proof_msg.block_height) +
                          " (targets=" + std::to_string(spend_proof.targets.size()) +
                          " positions=" + std::to_string(spend_proof.positions.size()) +
                          " proof_hashes=" + std::to_string(spend_proof.proof_hashes.size()) +
                          " stump_leaves=" + std::to_string(local_stump_.getNumLeaves()) + ")");
            sync_stats_.proofs_failed++;
            if (bridge_peers_.find(peer_id) != bridge_peers_.end()) {
                bridge_peers_[peer_id].proofs_failed++;
            }
            return false;
        }
    }

    // 5. Apply deletions + additions to the shared forest transactionally.
    //    ConnectBlock() in STATELESS mode assumes the canonical forest has
    //    already been advanced here, so batch validation must mutate the
    //    forest on success and leave it untouched on failure.
    auto additions = ComputeCanonicalAdditionHashes(block);
    consensus::UtreexoForest working_forest = *utreexo_forest_;
    if (!ApplyAccumulatorDelta(
            working_forest,
            spend_proof.targets,
            additions,
            "[StatelessNode] ApplyBlockToAccumulator")) {
        g_logger.error("[StatelessNode] FAIL step 5: forest apply failed at height " +
                      std::to_string(proof_msg.block_height) +
                      " (dels=" + std::to_string(spend_proof.targets.size()) +
                      " adds=" + std::to_string(additions.size()) + ")");
        sync_stats_.proofs_failed++;
        if (bridge_peers_.find(peer_id) != bridge_peers_.end()) {
            bridge_peers_[peer_id].proofs_failed++;
        }
        return false;
    }

    // 6. Verify root_after matches
    consensus::UtreexoHash root_after = working_forest.getCommitment();
    if (root_after != proof_msg.accumulator_root_after) {
        g_logger.error("[StatelessNode] FAIL step 6: root_after mismatch at height " +
                      std::to_string(proof_msg.block_height) +
                      ": forest=" + hashHex(root_after) +
                      " proof=" + hashHex(proof_msg.accumulator_root_after));
        sync_stats_.root_continuity_errors++;
        sync_stats_.proofs_failed++;
        if (bridge_peers_.find(peer_id) != bridge_peers_.end()) {
            bridge_peers_[peer_id].proofs_failed++;
        }
        return false;
    }

    // Commit forest + stump atomically only after the full transition matches.
    *utreexo_forest_ = std::move(working_forest);
    local_stump_ = consensus::UtreexoStump::fromForest(*utreexo_forest_);

    // Proof validated successfully
    g_logger.info("[StatelessNode] Proof OK at height " +
                 std::to_string(proof_msg.block_height) +
                 " (dels=" + std::to_string(proof_msg.proof_data.spend_proof.targets.size()) +
                 " adds=" + std::to_string(block.vtx.size()) +
                 " root_after=" + hashHex(root_after) + ")");

    // Keep cached state aligned with the committed forest/stump.
    local_commitment_ = local_stump_.getCommitment();
    local_num_leaves_ = local_stump_.getNumLeaves();

    sync_stats_.proofs_validated++;
    sync_stats_.blocks_applied++;

    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Phase 3: Transition Proof Validation (stump-only, no forest mutation)
// ═════════════════════════════════════════════════════════════════════════════

bool StatelessNode::ValidateWithTransitionProof(
    const Block& block,
    const UtreexoProofMessage& proof_msg,
    const consensus::UtreexoTransitionProof& tp,
    uint64_t peer_id
) {
    auto hashHex = [](const consensus::UtreexoHash& h) -> std::string {
        if (h.empty()) return "(empty)";
        std::string hex;
        hex.reserve(h.size() * 2);
        for (uint8_t b : h) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", b);
            hex += buf;
        }
        return hex.substr(0, 16) + "...";
    };

    sync_stats_.proofs_received++;

    if (bridge_peers_.find(peer_id) != bridge_peers_.end()) {
        bridge_peers_[peer_id].proofs_received++;
    }

    // Resync stump from forest if external mutation occurred
    {
        const consensus::UtreexoHash forest_commitment = utreexo_forest_->getCommitment();
        const uint64_t forest_num_leaves = utreexo_forest_->getNumLeaves();
        if (local_commitment_ != forest_commitment || local_num_leaves_ != forest_num_leaves) {
            g_logger.warning("[StatelessNode-TP] Stump diverged from forest at height " +
                             std::to_string(proof_msg.block_height) +
                             " — rebuilding stump from forest");
            local_stump_ = consensus::UtreexoStump::fromForest(*utreexo_forest_);
            local_commitment_ = local_stump_.getCommitment();
            local_num_leaves_ = local_stump_.getNumLeaves();
        }
    }

    // 1. Block hash must match proof message
    uint256 block_hash = block.GetHash();
    if (block_hash != proof_msg.block_hash) {
        g_logger.error("[StatelessNode-TP] FAIL step 1: block hash mismatch");
        sync_stats_.proofs_failed++;
        return false;
    }

    // 2. Commitment continuity: tp.roots_before must hash to our local commitment
    auto stump_before = consensus::UtreexoStump::fromRoots(
        tp.roots_before, tp.num_leaves_before);
    consensus::UtreexoHash tp_commitment = stump_before.getCommitment();
    if (tp_commitment != local_commitment_) {
        g_logger.error("[StatelessNode-TP] FAIL step 2: commitment continuity at height " +
                      std::to_string(proof_msg.block_height) +
                      ": local=" + hashHex(local_commitment_) +
                      " tp=" + hashHex(tp_commitment));
        sync_stats_.root_continuity_errors++;
        sync_stats_.proofs_failed++;
        return false;
    }
    if (tp.num_leaves_before != local_num_leaves_) {
        g_logger.error("[StatelessNode-TP] FAIL step 2b: num_leaves mismatch at height " +
                      std::to_string(proof_msg.block_height) +
                      ": local=" + std::to_string(local_num_leaves_) +
                      " tp=" + std::to_string(tp.num_leaves_before));
        sync_stats_.proofs_failed++;
        return false;
    }

    // 3. Core stateless verification (batch proof + additions + intermediate roots)
    if (!tp.verify()) {
        g_logger.error("[StatelessNode-TP] FAIL step 3: tp.verify() failed at height " +
                      std::to_string(proof_msg.block_height));
        sync_stats_.proofs_failed++;
        if (bridge_peers_.find(peer_id) != bridge_peers_.end()) {
            bridge_peers_[peer_id].proofs_failed++;
        }
        return false;
    }

    // 4. Post-state commitment must match block header commitment
    if (tp.commitment_after != proof_msg.accumulator_root_after) {
        g_logger.error("[StatelessNode-TP] FAIL step 4: root_after mismatch at height " +
                      std::to_string(proof_msg.block_height) +
                      ": tp=" + hashHex(tp.commitment_after) +
                      " header=" + hashHex(proof_msg.accumulator_root_after));
        sync_stats_.proofs_failed++;
        return false;
    }

    // 5. Advance stump from transition proof post-state.
    //    Reconstruct intermediate stump from roots_after_deletions, apply additions.
    //    Pure math — no forest mutation, no findLeafPosition, cannot diverge.
    {
        std::vector<std::optional<consensus::UtreexoHash>> indexed_roots;
        uint64_t n = tp.num_leaves_before;
        size_t idx = 0;
        for (uint8_t h = 0; h < 64 && n > 0; ++h) {
            if (n & 1) {
                if (idx < tp.roots_after_deletions.size()) {
                    if (indexed_roots.size() <= h) indexed_roots.resize(h + 1, std::nullopt);
                    indexed_roots[h] = tp.roots_after_deletions[idx++];
                }
            }
            n >>= 1;
        }
        auto stump_mid = consensus::UtreexoStump::fromRoots(indexed_roots, tp.num_leaves_before);
        for (const auto& leaf : tp.addition_hashes) {
            stump_mid.addSingle(leaf);
        }
        local_stump_ = std::move(stump_mid);
    }

    // Batch validation already advances the shared canonical forest before
    // ConnectTip. Do the same for the safe TP-only subset we can reproduce
    // without positional data: deletion-free blocks. This keeps the shared
    // forest aligned with the verified stump so the next batch proof sees the
    // correct pre-state instead of snapping back to the stale forest.
    if (tp.deletion_targets.empty()) {
        consensus::UtreexoForest working_forest = *utreexo_forest_;
        for (const auto& leaf : tp.addition_hashes) {
            if (working_forest.add(leaf) == UINT64_MAX) {
                g_logger.error("[StatelessNode-TP] FAIL step 5b: forest add failed at height " +
                              std::to_string(proof_msg.block_height));
                sync_stats_.proofs_failed++;
                return false;
            }
        }

        const auto forest_after = working_forest.getCommitment();
        if (forest_after != tp.commitment_after) {
            g_logger.error("[StatelessNode-TP] FAIL step 5c: forest root_after mismatch at height " +
                          std::to_string(proof_msg.block_height) +
                          ": forest=" + hashHex(forest_after) +
                          " tp=" + hashHex(tp.commitment_after));
            sync_stats_.proofs_failed++;
            return false;
        }

        *utreexo_forest_ = std::move(working_forest);
        local_stump_ = consensus::UtreexoStump::fromForest(*utreexo_forest_);
    }

    local_commitment_ = tp.commitment_after;
    local_num_leaves_ = tp.num_leaves_after;

    g_logger.info("[StatelessNode-TP] Proof OK at height " +
                 std::to_string(proof_msg.block_height) +
                 " (dels=" + std::to_string(tp.deletion_targets.size()) +
                 " adds=" + std::to_string(tp.addition_hashes.size()) +
                 " root_after=" + hashHex(tp.commitment_after) + ")");
    sync_stats_.proofs_validated++;
    sync_stats_.blocks_applied++;

    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Transaction Validation (Phase #4)
// ═════════════════════════════════════════════════════════════════════════════

bool StatelessNode::ValidateUtreexoTx(
    const Transaction& tx,
    const std::vector<std::pair<consensus::UtreexoProof, consensus::SpentOutputData>>& input_proofs,
    const consensus::UtreexoHash& accumulator_root
) {
    // Helper: hex encoding for UtreexoHash
    auto toHex = [](const consensus::UtreexoHash& h) -> std::string {
        static const char hex[] = "0123456789abcdef";
        std::string s;
        s.reserve(h.size() * 2);
        for (auto b : h) { s += hex[b >> 4]; s += hex[b & 0xF]; }
        return s.substr(0, 16) + "...";
    };

    // 1. Freshness: proof root must match current stump commitment.
    const auto stump_commitment = local_stump_.getCommitment();
    if (accumulator_root != stump_commitment) {
        g_logger.warning("[StatelessNode-TX] Root mismatch: proof=" +
                        toHex(accumulator_root) + " local=" + toHex(stump_commitment));
        return false;
    }

    // 2. Count non-coinbase inputs
    size_t expected_proofs = 0;
    for (const auto& input : tx.vin) {
        if (!input.prevout.txid.IsNull()) expected_proofs++;
    }
    if (input_proofs.size() != expected_proofs) {
        g_logger.error("[StatelessNode-TX] Proof count mismatch: expected=" +
                      std::to_string(expected_proofs) + " got=" +
                      std::to_string(input_proofs.size()));
        return false;
    }

    // 3. Verify each input's inclusion proof
    auto forest_roots = local_stump_.getRoots();
    size_t proof_idx = 0;
    for (const auto& input : tx.vin) {
        if (input.prevout.txid.IsNull()) continue;  // Skip coinbase

        const auto& [proof, spent_output] = input_proofs[proof_idx];

        // Compute leaf hash
        consensus::UtreexoHash leaf_hash = ComputeLeafHash(
            input.prevout.txid.AsUint256(),
            input.prevout.vout,
            spent_output.value,
            spent_output.scriptPubKey
        );

        // Verify inclusion proof
        if (!proof.verify(leaf_hash, forest_roots)) {
            g_logger.error("[StatelessNode-TX] Proof verification failed for input " +
                          std::to_string(proof_idx) + " of tx " +
                          tx.GetTxid().AsUint256().GetHex().substr(0, 16));
            return false;
        }

        proof_idx++;
    }

    g_logger.info("[StatelessNode-TX] TX validated: " +
                 tx.GetTxid().AsUint256().GetHex().substr(0, 16) + "... (" +
                 std::to_string(expected_proofs) + " proofs verified)");
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// State Management
// ═════════════════════════════════════════════════════════════════════════════

consensus::UtreexoHash StatelessNode::GetCurrentAccumulatorRoot() const {
    return local_commitment_;
}

uint32_t StatelessNode::GetSyncHeight() const {
    return current_sync_height_;
}

void StatelessNode::SetSyncHeight(uint32_t height) {
    current_sync_height_ = height;
    sync_stats_.current_height = height;
}

void StatelessNode::SyncToForestState(std::optional<uint32_t> height) {
    if (!utreexo_forest_) {
        return;
    }

    local_stump_ = consensus::UtreexoStump::fromForest(*utreexo_forest_);
    local_commitment_ = local_stump_.getCommitment();
    local_num_leaves_ = local_stump_.getNumLeaves();
    if (height.has_value()) {
        current_sync_height_ = *height;
        sync_stats_.current_height = *height;
    }
}

StatelessSyncState StatelessNode::GetSyncState() const {
    return sync_state_;
}

void StatelessNode::SetSyncState(StatelessSyncState state) {
    sync_state_ = state;
    sync_stats_.state = state;
}

StatelessSyncStats StatelessNode::GetSyncStats() const {
    return sync_stats_;
}

void StatelessNode::ResetSyncStats() {
    sync_stats_ = {
        current_sync_height_,  // current_height
        0,  // target_height
        0,  // proofs_requested
        0,  // proofs_received
        0,  // proofs_validated
        0,  // proofs_failed
        0,  // blocks_applied
        0,  // root_continuity_errors
        sync_state_
    };
}

// ═════════════════════════════════════════════════════════════════════════════
// Internal Helpers
// ═════════════════════════════════════════════════════════════════════════════

std::vector<consensus::UtreexoHash> StatelessNode::ExtractTargetsFromBlock(
    const Block& block,
    const std::vector<consensus::SpentOutputData>& spent_outputs
) const {
    std::vector<consensus::UtreexoHash> targets;
    const auto ephemeral_outputs = CollectEphemeralOutputs(block);

    // Count total inputs (skip coinbase)
    size_t input_count = 0;
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); tx_idx++) {
        const auto& tx = block.vtx[tx_idx];

        // Skip coinbase transaction
        if (tx_idx == 0 && tx.IsCoinbase()) {
            continue;
        }

        input_count += tx.vin.size();
    }

    // Validate spent_outputs size matches input count
    if (spent_outputs.size() != input_count) {
        return targets;  // Empty - mismatch
    }

    // Extract targets for each input
    size_t spent_idx = 0;
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); tx_idx++) {
        const auto& tx = block.vtx[tx_idx];

        // Skip coinbase transaction
        if (tx_idx == 0 && tx.IsCoinbase()) {
            continue;
        }

        for (const auto& input : tx.vin) {
            const auto& spent_output = spent_outputs[spent_idx];
            const OutPoint prevout(input.prevout.txid, input.prevout.vout);

            // Intra-block spends are ephemeral: they never enter the
            // accumulator, so they have spent_output metadata but no proof
            // target in the batch proof.
            if (ephemeral_outputs.count(prevout) == 0) {
                consensus::UtreexoHash leaf_hash = ComputeLeafHash(
                    input.prevout.txid.AsUint256(),
                    input.prevout.vout,
                    spent_output.value,
                    spent_output.scriptPubKey
                );
                targets.push_back(leaf_hash);
            }
            spent_idx++;
        }
    }

    return targets;
}



consensus::UtreexoHash StatelessNode::ComputeLeafHash(
    const uint256& txid,
    uint32_t vout,
    uint64_t value,
    const std::vector<uint8_t>& script_pub_key
) {
    // Delegate to canonical HashUTXO which includes the domain tag
    // "DINERO-UTXO-LEAF-v1" — must match miner/block_assembler for root consistency
    return consensus::HashUTXO(txid, vout, value, script_pub_key);
}

// ═════════════════════════════════════════════════════════════════════════════
// CSN Reorg Support (Checkpoint + Replay)
// ═════════════════════════════════════════════════════════════════════════════

void StatelessNode::RewindToCheckpoint(uint32_t height, const consensus::UtreexoForest& restored_forest) {
    g_logger.info("[StatelessNode] RewindToCheckpoint: height=" + std::to_string(height));

    // Deep copy the restored forest into our forest pointer
    *utreexo_forest_ = restored_forest;

    // Rebuild stump from restored forest
    local_stump_ = consensus::UtreexoStump::fromForest(*utreexo_forest_);
    local_commitment_ = local_stump_.getCommitment();
    local_num_leaves_ = local_stump_.getNumLeaves();
    current_sync_height_ = height;

    g_logger.info("[StatelessNode] RewindToCheckpoint complete: leaves=" +
                 std::to_string(local_num_leaves_) +
                 " commitment=" + [&]() {
                     std::string hex;
                     for (size_t i = 0; i < std::min(local_commitment_.size(), size_t(8)); i++) {
                         char buf[3];
                         snprintf(buf, sizeof(buf), "%02x", local_commitment_[i]);
                         hex += buf;
                     }
                     return hex + "...";
                 }());
}

bool StatelessNode::ReplayBlock(const Block& block, const std::vector<consensus::UtreexoHash>& spend_targets) {
    try {
        // Transactional replay:
        // mutate a working copy first, verify root commitment, then commit.
        // This prevents partial forest mutation on any failure path.
        consensus::UtreexoForest working_forest = *utreexo_forest_;

        const auto additions = ComputeCanonicalAdditionHashes(block);
        if (!ApplyAccumulatorDelta(
                working_forest,
                spend_targets,
                additions,
                "[StatelessNode] ReplayBlock")) {
            return false;
        }

        // 3. Consensus-critical invariant: replayed root must match header commitment.
        // Replay is the forest mutator in CSN reorg mode, so this check must live here
        // (not only in callers) to prevent accidental bypass.
        consensus::UtreexoHash replayed_commitment = working_forest.getCommitment();
        uint256 replayed_root;
        if (replayed_commitment.size() == 32) {
            std::memcpy(replayed_root.begin(), replayed_commitment.data(), 32);
        } else {
            g_logger.error("[StatelessNode] ReplayBlock: invalid commitment size " +
                          std::to_string(replayed_commitment.size()) + " (expected 32)");
            return false;
        }

        if (replayed_root != block.header.utreexo_root) {
            g_logger.error("[StatelessNode] ReplayBlock: root mismatch after replay");
            g_logger.error("[StatelessNode]   replayed=" + replayed_root.GetHex().substr(0, 16) + "...");
            g_logger.error("[StatelessNode]   header=" + block.header.utreexo_root.GetHex().substr(0, 16) + "...");
            return false;
        }

        // 4. Commit atomically to live forest + local state.
        *utreexo_forest_ = std::move(working_forest);
        local_stump_ = consensus::UtreexoStump::fromForest(*utreexo_forest_);
        local_commitment_ = replayed_commitment;
        local_num_leaves_ = local_stump_.getNumLeaves();
        current_sync_height_++;

        return true;

    } catch (const std::exception& e) {
        g_logger.error("[StatelessNode] ReplayBlock: exception: " + std::string(e.what()));
        return false;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Phase 8.2: Sync Loop Coordination
// ═════════════════════════════════════════════════════════════════════════════

size_t StatelessNode::requestNextProofBatch() {
    if (pending_headers_.empty()) {
        return 0;  // No headers to request proofs for
    }

    if (!proof_requester_) {
        g_logger.error("StatelessNode: proof_requester_ callback not set");
        return 0;
    }

    // Determine batch size (max 16 from design)
    constexpr size_t MAX_BATCH_SIZE = 16;
    size_t batch_size = std::min(pending_headers_.size(), MAX_BATCH_SIZE);

    // Extract next batch of block hashes
    std::vector<uint256> block_hashes;
    for (size_t i = 0; i < batch_size; i++) {
        uint256 block_hash = pending_headers_[i].GetHash();
        block_hashes.push_back(block_hash);
        // Track pending request for unsolicited-proof filtering + timeout handling.
        TrackExternalProofRequest(block_hash);
    }

    // Request proofs via callback
    proof_requester_(block_hashes);

    g_logger.info("StatelessNode: Requested proofs for " + std::to_string(batch_size) + " blocks");

    return batch_size;
}

void StatelessNode::TrackExternalProofRequest(const uint256& block_hash) {
    pending_proofs_.insert(block_hash);

    // Phase 8.4: Track request for timeout detection and peer rotation.
    PendingProofRequest& request = proof_requests_[block_hash];
    request.block_hash = block_hash;
    request.request_time = std::chrono::steady_clock::now();
    request.current_peer_id = "";  // Filled when proof response arrives
}

size_t StatelessNode::checkTimeoutsAndRetry() {
    if (!proof_requester_) {
        return 0;  // Can't retry without proof requester
    }

    auto now = std::chrono::steady_clock::now();
    std::vector<uint256> blocks_to_retry;
    size_t timeouts_handled = 0;

    // Phase 8.4: Minimal peer rotation - timeout ≠ malicious
    // Check all pending proof requests for timeouts
    for (auto& [block_hash, request] : proof_requests_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - request.request_time);

        if (elapsed >= proof_timeout_) {
            timeouts_handled++;

            g_logger.warning("StatelessNode: Proof request timed out for block " +
                           block_hash.ToString() + " (waited " +
                           std::to_string(elapsed.count()) + "s)");

            // Disconnect timed-out peer (polite disconnect, not ban)
            if (!request.current_peer_id.empty() && peer_disconnect_callback_) {
                peer_disconnect_callback_(request.current_peer_id);
                g_logger.info("StatelessNode: Disconnected slow peer " + request.current_peer_id +
                             " for block " + block_hash.ToString());

                // Mark this peer as tried for this block (avoid immediate retry)
                tried_peers_[block_hash].insert(request.current_peer_id);
            }

            // Rotate to next peer (clear current_peer_id to force rotation)
            request.current_peer_id = "";
            request.request_time = now;
            blocks_to_retry.push_back(block_hash);
        }
    }

    // Re-request timed-out proofs (will rotate to different peers)
    if (!blocks_to_retry.empty()) {
        proof_requester_(blocks_to_retry);
        g_logger.info("StatelessNode: Retrying " + std::to_string(blocks_to_retry.size()) +
                     " timed-out proof requests with peer rotation");
    }

    return timeouts_handled;
}

void StatelessNode::advanceSyncTip(const uint256& block_hash, uint32_t height) {
    // Update sync height
    current_sync_height_ = height;
    sync_stats_.current_height = height;
    sync_stats_.blocks_applied++;

    // Remove from pending proofs
    pending_proofs_.erase(block_hash);

    // Phase 8.4: Clean up proof request tracking and tried peers
    proof_requests_.erase(block_hash);
    tried_peers_.erase(block_hash);

    // Remove header from pending queue
    auto it = std::find_if(pending_headers_.begin(), pending_headers_.end(),
                          [&](const BlockHeader& h) { return h.GetHash() == block_hash; });
    if (it != pending_headers_.end()) {
        pending_headers_.erase(it);
    }

    g_logger.info("StatelessNode: Advanced sync tip to height " + std::to_string(height) +
                  " (" + std::to_string(pending_headers_.size()) + " headers remaining)");

    // Check if we should request more proofs
    if (!pending_headers_.empty() && pending_proofs_.size() < 8) {
        requestNextProofBatch();
    }

    // Update sync state
    if (pending_headers_.empty() && pending_proofs_.empty()) {
        if (sync_stats_.current_height >= sync_stats_.target_height) {
            sync_state_ = StatelessSyncState::SYNCED;
            g_logger.info("StatelessNode: Sync complete at height " + std::to_string(height));
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Phase 8.3: Atomic Proof ↔ Block Correlation
// ═════════════════════════════════════════════════════════════════════════════

bool StatelessNode::applyPending(const uint256& block_hash) {
    // Check if this block/proof pair exists in pending
    auto it = pending_.find(block_hash);
    if (it == pending_.end()) {
        g_logger.error("StatelessNode::applyPending: Block hash not found in pending map");
        return false;
    }

    PendingBlockProof& pending = it->second;

    // Phase 8.3 Atomicity Check: Both block and proof must be present
    if (!pending.has_block || !pending.has_proof) {
        // Not ready yet - wait for the other piece
        return false;
    }

    g_logger.info("StatelessNode::applyPending: Both block and proof present for " +
                  block_hash.ToString() + " - applying atomically");

    // Convert peer_id string to uint64_t for ValidateUtreexoProof
    uint64_t peer_id_num = std::hash<std::string>{}(pending.peer_id);

    // Phase 8.3 Single Atomic Path: Validate proof (includes accumulator application)
    bool validation_result = ValidateUtreexoProof(pending.block, pending.proof, peer_id_num);

    if (!validation_result) {
        g_logger.error("StatelessNode::applyPending: Proof validation failed for block " +
                      block_hash.ToString() + " from peer " + pending.peer_id);

        // Phase 8.4: Ban peer for cryptographic validation failure
        if (peer_ban_callback_) {
            peer_ban_callback_(pending.peer_id, "Invalid Utreexo proof (cryptographic validation failed)");
            g_logger.info("StatelessNode::applyPending: Banned peer " + pending.peer_id +
                         " for invalid proof");
        }

        auto req_it = proof_requests_.find(block_hash);
        if (req_it != proof_requests_.end()) {
            if (!pending.peer_id.empty()) {
                tried_peers_[block_hash].insert(pending.peer_id);
            }
            req_it->second.current_peer_id.clear();
            req_it->second.request_time = std::chrono::steady_clock::now();

            if (proof_requester_) {
                proof_requester_({block_hash});
                g_logger.info("StatelessNode::applyPending: Re-requesting block " +
                              block_hash.ToString() + " after invalid proof rejection");
            }
        }

        // Clean up pending entry
        pending_.erase(it);
        return false;
    }

    // Success - advance sync tip
    advanceSyncTip(pending.proof.block_hash, pending.proof.block_height);

    g_logger.info("StatelessNode::applyPending: Successfully validated and applied block " +
                  block_hash.ToString() + " at height " +
                  std::to_string(pending.proof.block_height));

    // Clean up pending entry
    pending_.erase(it);

    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Phase 8.2: P2P Response Handlers (Real Sync Loop)
// ═════════════════════════════════════════════════════════════════════════════

bool StatelessNode::onProofResponse(
    PeerConnection* peer,
    const UtreexoProofMessage& proof_msg
) {
    std::string peer_id = peer->getPeerId();
    g_logger.info("StatelessNode: Processing utxoproof from peer " + peer_id +
                  " for block " + proof_msg.block_hash.ToString() +
                  " at height " + std::to_string(proof_msg.block_height));

    // Ignore unsolicited proofs to prevent proof-flood memory/CPU abuse.
    if (pending_proofs_.count(proof_msg.block_hash) == 0 &&
        proof_requests_.find(proof_msg.block_hash) == proof_requests_.end()) {
        uint32_t& count = unsolicited_proof_counts_[peer_id];
        count++;
        g_logger.warning("StatelessNode: Unsolicited utxoproof from peer " + peer_id +
                         " for block " + proof_msg.block_hash.ToString() +
                         " (count=" + std::to_string(count) + ")");
        if (count >= MAX_UNSOLICITED_PROOFS_PER_PEER && peer_ban_callback_) {
            peer_ban_callback_(peer_id, "Proof flood: unsolicited utxoproof messages");
        }
        return false;
    }

    // Drop stale proofs for already-advanced heights (common during reorg churn/replays).
    if (proof_msg.block_height <= current_sync_height_) {
        uint32_t& stale = stale_proof_counts_[peer_id];
        stale++;
        pending_proofs_.erase(proof_msg.block_hash);
        proof_requests_.erase(proof_msg.block_hash);
        g_logger.debug("StatelessNode: Ignoring stale utxoproof from peer " + peer_id +
                       " height=" + std::to_string(proof_msg.block_height) +
                       " current=" + std::to_string(current_sync_height_));
        return false;
    }

    unsolicited_proof_counts_.erase(peer_id);
    stale_proof_counts_.erase(peer_id);

    // Phase 8.3: Store proof in pending map
    PendingBlockProof& pending = pending_[proof_msg.block_hash];
    pending.block_hash = proof_msg.block_hash;
    pending.proof = proof_msg;
    pending.has_proof = true;
    pending.peer_id = peer_id;

    // Phase 8.4: Track peer that provided proof (for timeout/ban tracking)
    auto req_it = proof_requests_.find(proof_msg.block_hash);
    if (req_it != proof_requests_.end()) {
        req_it->second.current_peer_id = peer_id;
    }

    g_logger.info("StatelessNode: Stored proof for block " + proof_msg.block_hash.ToString() +
                  " (has_block=" + std::to_string(pending.has_block) + ")");

    // Phase 8.3: Fetch block if block provider is available
    if (!block_provider_) {
        g_logger.error("StatelessNode: block_provider_ callback not set (misconfiguration)");
        peer->disconnect();
        return false;
    }

    auto block_opt = block_provider_(proof_msg.block_hash);
    if (!block_opt.has_value()) {
        g_logger.error("StatelessNode: Failed to fetch block " + proof_msg.block_hash.ToString() +
                      " for proof validation");
        peer->disconnect();
        return false;
    }

    // Phase 8.3: Store block in pending map
    pending.block = block_opt.value();
    pending.has_block = true;

    g_logger.info("StatelessNode: Stored block for " + proof_msg.block_hash.ToString() +
                  " - both block and proof present, applying atomically");

    // Phase 8.3: Apply pending block/proof pair atomically
    bool result = applyPending(proof_msg.block_hash);
    if (!result) {
        // Phase 8.4: Ban already handled by applyPending() for validation failures
        // Just return false (no disconnect needed, peer already banned if applicable)
        return false;
    }

    return true;
}

bool StatelessNode::onProofResponse(
    const std::string& peer_id,
    const UtreexoProofMessage& proof_msg
) {
    g_logger.info("StatelessNode: Processing utxoproof/proofdata from peer " + peer_id +
                  " for block " + proof_msg.block_hash.ToString() +
                  " at height " + std::to_string(proof_msg.block_height));

    // Ignore unsolicited proofs to prevent proof-flood memory/CPU abuse.
    if (pending_proofs_.count(proof_msg.block_hash) == 0 &&
        proof_requests_.find(proof_msg.block_hash) == proof_requests_.end()) {
        uint32_t& count = unsolicited_proof_counts_[peer_id];
        count++;
        g_logger.warning("StatelessNode: Unsolicited proof response from peer " + peer_id +
                         " for block " + proof_msg.block_hash.ToString() +
                         " (count=" + std::to_string(count) + ")");
        if (count >= MAX_UNSOLICITED_PROOFS_PER_PEER && peer_ban_callback_) {
            peer_ban_callback_(peer_id, "Proof flood: unsolicited proof responses");
        }
        return false;
    }

    // Drop stale proofs for already-advanced heights (common during reorg churn/replays).
    if (proof_msg.block_height <= current_sync_height_) {
        uint32_t& stale = stale_proof_counts_[peer_id];
        stale++;
        pending_proofs_.erase(proof_msg.block_hash);
        proof_requests_.erase(proof_msg.block_hash);
        g_logger.debug("StatelessNode: Ignoring stale proof response from peer " + peer_id +
                       " height=" + std::to_string(proof_msg.block_height) +
                       " current=" + std::to_string(current_sync_height_));
        return false;
    }

    unsolicited_proof_counts_.erase(peer_id);
    stale_proof_counts_.erase(peer_id);

    // Phase 8.3: Store proof in pending map
    PendingBlockProof& pending = pending_[proof_msg.block_hash];
    pending.block_hash = proof_msg.block_hash;
    pending.proof = proof_msg;
    pending.has_proof = true;
    pending.peer_id = peer_id;

    // Phase 8.4: Track peer that provided proof (for timeout/ban tracking)
    auto req_it = proof_requests_.find(proof_msg.block_hash);
    if (req_it != proof_requests_.end()) {
        req_it->second.current_peer_id = peer_id;
    }

    g_logger.info("StatelessNode: Stored proof for block " + proof_msg.block_hash.ToString() +
                  " (has_block=" + std::to_string(pending.has_block) + ")");

    // Phase 8.3: Fetch block if block provider is available
    if (!block_provider_) {
        g_logger.error("StatelessNode: block_provider_ callback not set (misconfiguration)");
        return false;
    }

    auto block_opt = block_provider_(proof_msg.block_hash);
    if (!block_opt.has_value()) {
        g_logger.error("StatelessNode: Failed to fetch block " + proof_msg.block_hash.ToString() +
                      " for proof validation");
        return false;
    }

    // Phase 8.3: Store block in pending map
    pending.block = block_opt.value();
    pending.has_block = true;

    g_logger.info("StatelessNode: Stored block for " + proof_msg.block_hash.ToString() +
                  " - both block and proof present, applying atomically");

    // Phase 8.3: Apply pending block/proof pair atomically
    bool result = applyPending(proof_msg.block_hash);
    if (!result) {
        // Phase 8.4: Ban already handled by applyPending() for validation failures
        // Just return false (no disconnect needed here)
        return false;
    }

    return true;
}

bool StatelessNode::onHeadersResponse(
    PeerConnection* peer,
    const UtreexoHeadersMessage& headers_msg
) {
    std::string peer_id = peer->getPeerId();
    g_logger.info("StatelessNode: Processing utxohdrs from peer " + peer_id +
                  " with " + std::to_string(headers_msg.headers.size()) + " headers");

    // Phase 8.2: Validate empty headers
    if (headers_msg.headers.empty()) {
        g_logger.error("StatelessNode: Empty headers message from peer " + peer_id);

        // Phase 8.4: Ban peer for protocol violation (sending empty headers)
        if (peer_ban_callback_) {
            peer_ban_callback_(peer_id, "Protocol violation: empty headers message");
            g_logger.info("StatelessNode: Banned peer " + peer_id + " for empty headers");
        }

        return false;
    }

    // Validate header chain continuity
    for (size_t i = 1; i < headers_msg.headers.size(); i++) {
        const auto& prev_header = headers_msg.headers[i - 1];
        const auto& current_header = headers_msg.headers[i];

        // Check that current header's prev_block_hash matches previous header's hash
        uint256 prev_hash = prev_header.GetHash();
        if (current_header.prev_block_hash != prev_hash) {
            g_logger.error("StatelessNode: Header chain continuity break at index " +
                          std::to_string(i) + " from peer " + peer_id);

            // Phase 8.4: Ban peer for protocol violation (broken header chain)
            if (peer_ban_callback_) {
                peer_ban_callback_(peer_id, "Protocol violation: header chain continuity break");
                g_logger.info("StatelessNode: Banned peer " + peer_id + " for broken header chain");
            }

            return false;
        }
    }

    g_logger.info("StatelessNode: Header chain validated, continuity verified");

    // Phase 8.2: Store headers in pending queue
    for (const auto& header : headers_msg.headers) {
        pending_headers_.push_back(header);
    }

    // Update target height
    if (!headers_msg.headers.empty()) {
        // We don't have height in BlockHeader, so we estimate based on current_sync_height + count
        uint32_t estimated_height = current_sync_height_ + static_cast<uint32_t>(pending_headers_.size());
        sync_stats_.target_height = estimated_height;
    }

    // Phase 8.2: Schedule proof requests for new headers
    size_t requested = requestNextProofBatch();
    g_logger.info("StatelessNode: Scheduled " + std::to_string(requested) + " proof requests");

    // Update sync state
    if (sync_state_ == StatelessSyncState::IDLE) {
        sync_state_ = StatelessSyncState::PROOFS_SYNC;
    }

    return true;
}

} // namespace network
} // namespace dinero
