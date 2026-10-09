/**
 * Phase 34.2: Utreexo RPC Methods
 *
 * Implements RPC methods for querying the Utreexo accumulator:
 * - getutreexoroots: Get current Merkle forest roots
 * - getutreexocommitment: Get single 32-byte commitment hash
 * - getutxoproof: Generate cryptographic proof for a UTXO
 * - getutreexostats: Get accumulator statistics
 * - getutreexocachestats: Get bridge proof-cache statistics
 * - getutreexogossipstats: Get proof gossip health metrics
 * - rebuildutreexo: Rebuild accumulator from UTXO set
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "rpc/utreexo_proof_coin.h"
#include "rpc/methods_utreexo.h"
#include "daemon/config.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/p2p_service.h"
#include "network/bridge_node.h"
#include "network/types.h"
#include "consensus/proof_gossip.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_stump.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "storage/chain_db.h"  // Phase 11a.2: UTXO lookup for leaf_hash
#include "common/logger.h"
#include <algorithm>
#include <array>
#include <sstream>
#include <iomanip>

namespace din {
using ExecutionContext = ::ExecutionContext;  // Global ExecutionContext from rpc/rpc_registry.h

// Helper: Convert UtreexoHash to hex string
std::string hashToHex(const dinero::consensus::UtreexoHash& hash) {
    std::ostringstream oss;
    for (uint8_t byte : hash) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

void AppendUtreexoRootsSnapshot(Json& result,
                                const dinero::consensus::UtreexoForest& forest) {
    const auto roots = forest.getRoots();
    const uint64_t num_leaves = forest.getNumLeaves();

    Json roots_array = din::arr();
    for (const auto& root : roots) {
        roots_array.append(hashToHex(root));
    }

    result["stump_num_leaves"] = static_cast<Json::Int64>(num_leaves);
    result["stump_roots"] = roots_array;
    result["num_leaves"] = static_cast<Json::Int64>(num_leaves);
    result["num_roots"] = static_cast<Json::Int64>(roots.size());
    result["roots"] = roots_array;
}

void MaybeScheduleProofCoverageRecovery(dinero::ChainstateService& chainstate,
                                        dinero::ChainDB& chain_db,
                                        const dinero::uint256& txid,
                                        uint32_t vout,
                                        const std::string& source_tag) {
    auto coin_result = chain_db.getCoin(txid, vout);
    if (coin_result.status() != dinero::Status::Ok) {
        return;
    }

    const std::string outpoint =
        txid.GetHex() + ":" + std::to_string(vout);
    dinero::g_logger.error(source_tag +
                           " live forest missing canonical UTXO " + outpoint +
                           " — scheduling chainstate recovery");
    chainstate.RequestChainstateRecovery(
        "live UTXO missing from Utreexo forest for " + outpoint,
        source_tag);
}

/**
 * blockchain.getutreexoroots - Get Utreexo forest roots
 *
 * Returns the current Merkle forest roots that cryptographically commit
 * to the entire UTXO set.
 *
 * Params: none
 *
 * Returns:
 *   {
 *     "num_leaves": <n>,         // Total UTXOs
 *     "num_roots": <n>,          // Number of forest roots
 *     "roots": ["<hash>", ...]   // 32-byte root hashes (hex)
 *   }
 */
Json rpc_getutreexoroots(const ExecutionContext& ctx, const Json& params) {
    Json result;

    try {
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "ChainDB not available";
            return result;
        }

        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Chainstate service not available";
            return result;
        }

        auto* uset = chainstate->GetConsensusUTXOSet();
        if (!uset) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Utreexo forest not available";
            return result;
        }

        // Guarded reads: shared lock + copy out (audit: forest read-during-free UAF)
        std::vector<dinero::consensus::UtreexoHash> roots = uset->SnapshotForestRoots();
        uint64_t num_leaves = uset->SnapshotForestLeafCount();

        result["num_leaves"] = static_cast<Json::Int64>(num_leaves);
        result["num_roots"] = static_cast<Json::Int64>(roots.size());

        din::Json rootsArray(::Json::arrayValue);
        for (const auto& root : roots) {
            rootsArray.append(hashToHex(root));
        }
        result["roots"] = rootsArray;

        dinero::g_logger.info("[getutreexoroots] " + std::to_string(num_leaves) +
                             " leaves, " + std::to_string(roots.size()) + " roots");

    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
        dinero::g_logger.error("[getutreexoroots] Exception: " + std::string(e.what()));
    }

    return result;
}

Json BuildUtreexoProofContext(const ExecutionContext& ctx) {
    Json result;
    result["available"] = false;
    const auto service = ctx.daemon
        ? std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate)
        : nullptr;
    if (!service) {
        result["error"] = "Chainstate service not available";
        return result;
    }
    const auto snapshot = service->getUtreexoRpcSnapshot();
    if (!snapshot.ok()) {
        result["error"] = "Selected accumulator snapshot unavailable";
        return result;
    }
    result["tip_height"] = static_cast<Json::UInt64>(snapshot->height);
    result["tip_hash"] = snapshot->block_hash.GetHex();
    result["utreexo_root"] = hashToHex(snapshot->commitment);
    result["available"] = true;
    return result;
}

/**
 * blockchain.getutreexocommitment - Get single Utreexo commitment hash
 *
 * Returns a single 32-byte hash that commits to all forest roots.
 * This would be stored in block headers for consensus.
 *
 * Params: none
 *
 * Returns:
 *   {
 *     "commitment": "<hash>",            // 32-byte commitment hash (hex)
 *     "num_leaves": <n>,                 // Total UTXOs committed
 *     "num_roots": <n>,                  // Number of forest roots
 *     "verified_height": <n>,            // Active-chain height
 *     "validation_role": "<role>",       // compact/full/proof bridge
 *     "bridge_peer_count": <n>,          // Connected proof-capable peers
 *     "compact_state_bytes": <n>,        // Exact stump serialization size
 *     "forest_memory_bytes_estimate": <n>// Approximate full-forest memory
 *   }
 */
Json rpc_getutreexocommitment(const ExecutionContext& ctx, const Json& params) {
    Json result;

    try {
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "ChainDB not available";
            return result;
        }

        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Chainstate service not available";
            return result;
        }

        const auto snapshot=chainstate->getUtreexoRpcSnapshot();
        if (!snapshot.ok()) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Selected accumulator snapshot unavailable";
            return result;
        }
        const auto& commitment=snapshot->commitment;
        const auto num_leaves=snapshot->num_leaves;
        const auto num_roots=snapshot->num_roots;
        const auto compact_state_bytes=snapshot->compact_state_bytes;
        const auto& config = GetConfig();
        const bool compact_profile = snapshot->compact;
        const bool bridge_active =
            config.utreexo_bridge && chainstate->GetBridgeNode() != nullptr;
        std::size_t bridge_peer_count = 0;
        if (ctx.daemon->p2p) {
            const auto peers = ctx.daemon->p2p->GetConnectedPeers();
            bridge_peer_count = static_cast<std::size_t>(std::count_if(
                peers.begin(), peers.end(), [](const auto& peer) {
                    return (peer.service_flags & dinero::ServiceFlags::NODE_UTREEXO_BRIDGE) != 0;
                }));
        }

        result["commitment"] = hashToHex(commitment);
        result["num_leaves"] = static_cast<Json::Int64>(num_leaves);
        result["num_roots"] = static_cast<Json::Int64>(num_roots);
        result["verified_height"] = static_cast<Json::Int64>(snapshot->height);
        result["verified_block_hash"] = snapshot->block_hash.GetHex();
        result["sync_profile"] = config.sync_profile;
        result["validation_role"] = compact_profile
            ? "compact_validator"
            : (bridge_active ? "proof_bridge" : "full_validator");
        result["bridge_enabled"] = config.utreexo_bridge;
        result["bridge_active"] = bridge_active;
        result["bridge_peer_count"] = static_cast<Json::Int64>(bridge_peer_count);
        result["compact_state_bytes"] = static_cast<Json::Int64>(compact_state_bytes);
        // This is the same documented approximation used by
        // ConsensusUTXOSet::GetMemoryUsage for the forest component alone.
        result["forest_memory_bytes_estimate"] =
            static_cast<Json::Int64>(compact_profile ? 0 : num_leaves * 40ULL);
        result["retains_full_state"] = !compact_profile;
        // A defensible disk-savings number requires measuring a comparable
        // conventional chainstate on this host. Do not manufacture one from a
        // per-leaf constant and present it as telemetry.
        result["disk_savings_available"] = false;

    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
        dinero::g_logger.error("[getutreexocommitment] Exception: " + std::string(e.what()));
    }

    return result;
}

/**
 * blockchain.getutxoproof - Generate cryptographic proof for a UTXO
 *
 * Creates a compact Merkle proof (O(log n) size) that the UTXO exists
 * in the accumulator. Returns all data needed for CSN (Compact State Node)
 * verification.
 *
 * Params:
 *   [0] txid (string, required): Transaction ID
 *   [1] vout (int, required): Output index
 *
 * Returns:
 *   {
 *     "txid": "<txid>",
 *     "vout": <n>,
 *     "leaf_hash": "<hash>",           // SHA256(txid||vout||amount||script) - CSN verifier needs this
 *     "script_pubkey": "<hex>",        // Script bound into leaf_hash
 *     "position": <n>,                 // Leaf position in forest
 *     "num_leaves": <n>,               // Total leaves in forest
 *     "siblings": ["<hash>", ...],     // Merkle path hashes (from leaf to root)
 *     "proof_size": <n>                // Number of siblings
 *   }
 */
Json rpc_getutxoproof(const ExecutionContext& ctx, const Json& params) {
    Json result;

    try {
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "ChainDB not available";
            return result;
        }

        // Phase 11a.1: Proof generation implementation
        // 1. Parse parameters: txid, vout
        if (params.size() < 2) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Missing required parameters: txid, vout";
            return result;
        }

        if (!params[0].isString() || !params[1].isUInt()) {
            result["error"]["code"] = -32602;
            result["error"]["message"] = "Expected a 32-byte hexadecimal txid and unsigned vout";
            return result;
        }
        const std::string txid_hex = params[0].asString();
        const uint32_t vout = params[1].asUInt();
        dinero::uint256 txid_u256;
        if (txid_hex.size() != 64 ||
            !std::all_of(txid_hex.begin(), txid_hex.end(), [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F');
            }) || !dinero::uint256::FromHex(txid_hex, txid_u256)) {
            result["error"]["code"] = -32602;
            result["error"]["message"] = "Expected a 32-byte hexadecimal txid";
            return result;
        }
        // 2. Get chainstate service and canonical storage
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Chainstate service not available";
            return result;
        }

        const std::array<dinero::OutPoint, 1> points{
            dinero::OutPoint(dinero::TxId(txid_u256), vout)};
        auto captured = chainstate->CaptureUtreexoProofInputs(points);
        if (!captured.ok()) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Selected proof inputs unavailable";
            return result;
        }
        const auto& input = captured->inputs.at(0);
        if (input.status != dinero::Status::Ok || !input.coin) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Canonical UTXO data unavailable";
            return result;
        }
        const dinero::Coin& coin = *input.coin;

        // Convert scriptPubKey from hex string to bytes
        std::vector<uint8_t> spk_bytes;
        if (coin.height < 0 || !util::unhex(coin.script_pubkey, spk_bytes)) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Invalid canonical UTXO creation metadata";
            return result;
        }

        dinero::consensus::UtreexoHash leaf_hash =
            dinero::consensus::HashUTXOForCreationHeight(
                txid_u256,
                vout,
                coin.amount,
                spk_bytes,
                static_cast<uint32_t>(coin.height),
                coin.coinbase
            );

        // The owned COW forest, coin and tip all describe the same selected
        // observation. Expensive proving holds no selected or forest lock.
        const auto leaf_position = captured->forest.findLeafPosition(leaf_hash);
        const auto proof_opt = leaf_position
            ? captured->forest.prove(*leaf_position)
            : std::optional<dinero::consensus::UtreexoProof>{};
        if (!proof_opt) {
            // Preserve the canonical coverage failure response. Classification
            // uses the captured canonical coin, never a later unrelated read.
            // The capture keeps service resources alive through notification;
            // mining joins occur after selected/forest locks were released.
            if (input.canonical) {
                chainstate->RequestChainstateRecovery(
                    "live UTXO missing from Utreexo forest for " + txid_hex + ":" +
                    std::to_string(vout), "[getutxoproof]");
            }
            result["error"]["code"] = -5;
            result["error"]["message"] = "Canonical UTXO leaf missing from captured forest";
            return result;
        }
        const auto& proof = *proof_opt;
        const auto position = proof.position;

        // 5. Return proof with leaf_hash, siblings, position, num_leaves,
        //    and the accumulator root + chain context the proof is valid for.
        //    Without root binding, callers cannot verify proof freshness.
        result["txid"] = txid_hex;
        result["vout"] = vout;
        result["leaf_hash"] = hashToHex(leaf_hash);  // CSN verifier needs this
        result["script_pubkey"] = coin.script_pubkey;
        result["amount_una"] = static_cast<Json::Int64>(coin.amount);
        result["created_height"] = static_cast<uint64_t>(coin.height);
        result["coinbase"] = coin.coinbase;
        result["position"] = position;
        result["num_leaves"] = proof.numLeaves;
        result["proof_size"] = static_cast<uint64_t>(proof.siblings.size());

        Json siblings = din::arr();
        for (const auto& sibling : proof.siblings) {
            siblings.append(hashToHex(sibling));
        }
        result["siblings"] = siblings;

        // Accumulator root this proof is valid against
        result["accumulator_root"] = hashToHex(captured->snapshot.commitment);

        // Chain context: which tip state the proof was generated at
        result["block_hash"] = captured->snapshot.block_hash.GetHex();
        result["height"] = captured->snapshot.height;

        dinero::g_logger.info("[getutxoproof] Generated proof for " + txid_hex + ":" + std::to_string(vout) +
                              " at position " + std::to_string(position));

    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
        dinero::g_logger.error("[getutxoproof] Exception: " + std::string(e.what()));
    }

    return result;
}

/**
 * blockchain.getproofupdates - Re-generate proofs for specific outpoints at current tip.
 *
 * This is the proof update protocol for clients that hold proofs bound to an
 * older root. Rather than delta-based proof transformation (which requires
 * forest simulation), this re-proves from the current forest state.
 *
 * Parameters:
 *   [
 *     {
 *       "root_from": hex (optional) — the root the client currently holds.
 *                    If it matches current root, returns "no_update_needed".
 *       "outpoints": [
 *         { "txid": hex, "vout": uint },
 *         ...
 *       ]
 *     }
 *   ]
 *
 * Response:
 *   {
 *     "status": "updated" | "no_update_needed",
 *     "root_from": hex,          // Echo back client's root
 *     "root_to": hex,            // Current accumulator root
 *     "height": uint,            // Current tip height
 *     "block_hash": hex,         // Current tip hash
 *     "stump_num_leaves": uint,  // Compact Utreexo state leaf count
 *     "stump_roots": [hex, ...], // Compact Utreexo state roots
 *     "proofs": [                // Only present when status == "updated"
 *       { "txid", "vout", "leaf_hash", "position", "num_leaves",
 *         "siblings": [...], "script_pubkey", "success": bool, "error": string }
 *     ]
 *   }
 */
Json rpc_getproofupdates(const ExecutionContext& ctx, const Json& params) {
    Json result;
    try {
        if (!params.isArray() || params.empty() || !params[0].isObject()) {
            result["error"]["code"] = -32602;
            result["error"]["message"] = "Expected {root_from, outpoints}"; return result;
        }
        const auto& request = params[0];
        const std::string client_root = request.isMember("root_from") && request["root_from"].isString()
            ? request["root_from"].asString() : "";
        const bool has_points = request.isMember("outpoints") && request["outpoints"].isArray();
        if (has_points && (request["outpoints"].empty() || request["outpoints"].size() > 100)) {
            result["error"]["code"] = -32602;
            result["error"]["message"] = "outpoints must contain 1-100 entries"; return result;
        }
        Json batch_params = arr(); batch_params.append(has_points ? request["outpoints"] : arr());
        const auto batch = rpc_getutxoproofs_batch(ctx, batch_params);
        if (batch.isMember("error")) return batch;
        const auto current_root = batch["utreexo_root"].asString();
        const bool unchanged = !client_root.empty() && client_root == current_root;
        if (!unchanged && !has_points) {
            result["error"]["code"] = -32602;
            result["error"]["message"] = "Missing 'outpoints' array"; return result;
        }
        result["root_from"] = client_root; result["root_to"] = current_root;
        for (const auto* key : {"height", "block_hash", "stump_num_leaves", "stump_roots",
                                "num_leaves", "num_roots", "roots"}) result[key] = batch[key];
        result["status"] = unchanged ? "no_update_needed" : "updated";
        if (unchanged) return result;
        Json proofs = arr();
        for (const auto& item : batch["proofs"]) {
            Json entry;
            for (const auto* key : {"txid", "vout", "success", "error_code", "error"})
                if (item.isMember(key)) entry[key] = item[key];
            if (item["success"].asBool()) {
                for (const auto* key : {"leaf_hash", "position", "num_leaves", "siblings",
                                        "script_pubkey", "created_height", "coinbase"})
                    entry[key] = item["proof"][key];
            }
            proofs.append(entry);
        }
        result["proofs"] = proofs;
    } catch (const std::exception& e) {
        result = Json(); result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Proof update failed: ") + e.what();
    }
    return result;
}

/**
 * blockchain.getutreexostats - Get Utreexo accumulator statistics
 *
 * Returns detailed statistics about the accumulator state.
 *
 * Params: none
 *
 * Returns:
 *   {
 *     "num_leaves": <n>,       // Total UTXOs
 *     "num_roots": <n>,        // Number of forest roots
 *     "total_size": <n>,       // Total memory usage (bytes)
 *     "avg_proof_size": <n>    // Average proof size (bytes)
 *   }
 */
Json rpc_getutreexostats(const ExecutionContext& ctx, const Json& params) {
    Json result;

    try {
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "ChainDB not available";
            return result;
        }

        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Chainstate service not available";
            return result;
        }

        auto* forest = chainstate->utreexoForest();
        if (!forest) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Utreexo forest not available";
            return result;
        }

        // Get statistics
        uint64_t num_leaves = chainstate->GetConsensusUTXOSet()->SnapshotForestLeafCount();
        std::vector<dinero::consensus::UtreexoHash> roots = chainstate->GetConsensusUTXOSet()->SnapshotForestRoots();

        // Calculate approximate proof size: log2(n) * 32 bytes
        uint64_t avg_proof_size = 0;
        if (num_leaves > 0) {
            uint64_t log_n = 0;
            uint64_t temp = num_leaves;
            while (temp > 0) {
                log_n++;
                temp >>= 1;
            }
            avg_proof_size = log_n * 32;  // Each sibling is 32 bytes
        }

        result["num_leaves"] = static_cast<Json::UInt64>(num_leaves);
        result["num_roots"] = static_cast<Json::UInt64>(roots.size());
        result["total_size"] = static_cast<Json::UInt64>(num_leaves * 32);  // Approximate
        result["avg_proof_size"] = static_cast<Json::UInt64>(avg_proof_size);

        dinero::g_logger.info("[getutreexostats] " + std::to_string(num_leaves) +
                             " leaves, " + std::to_string(roots.size()) + " roots");

    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
        dinero::g_logger.error("[getutreexostats] Exception: " + std::string(e.what()));
    }

    return result;
}

/**
 * blockchain.getutreexocachestats - Get bridge proof cache statistics
 *
 * Returns cache hit/miss counters and current block/tx cache occupancy for
 * bridge proof serving.
 *
 * Params: none
 */
Json rpc_getutreexocachestats(const ExecutionContext& ctx, const Json& params) {
    Json result;
    (void)params;

    try {
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Chainstate not available";
            return result;
        }

        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Chainstate service not available";
            return result;
        }

        auto bridge_node = chainstate->GetBridgeNode();
        result["bridge_enabled"] = static_cast<bool>(bridge_node);

        if (!bridge_node) {
            result["hits"] = static_cast<Json::UInt64>(0);
            result["misses"] = static_cast<Json::UInt64>(0);
            result["evictions"] = static_cast<Json::UInt64>(0);
            result["hit_rate"] = 0.0;
            result["block_cache_entries"] = static_cast<Json::UInt64>(0);
            result["block_cache_capacity"] = static_cast<Json::UInt64>(0);
            result["tx_cache_entries"] = static_cast<Json::UInt64>(0);
            result["tx_cache_capacity"] = static_cast<Json::UInt64>(0);
            result["indexed_heights"] = static_cast<Json::UInt64>(0);
            result["indexed_blocks"] = static_cast<Json::UInt64>(0);
            result["block_cache_ttl_seconds"] = static_cast<Json::UInt64>(0);
            result["tx_cache_ttl_seconds"] = static_cast<Json::UInt64>(0);
            result["proof_requests_total"] = static_cast<Json::UInt64>(0);
            result["proof_requests_rejected"] = static_cast<Json::UInt64>(0);
            result["proof_requests_coalesced"] = static_cast<Json::UInt64>(0);
            result["proof_tasks_completed"] = static_cast<Json::UInt64>(0);
            result["proof_tasks_failed"] = static_cast<Json::UInt64>(0);
            result["proof_queue_depth"] = static_cast<Json::UInt64>(0);
            result["proof_queue_capacity"] = static_cast<Json::UInt64>(0);
            result["proof_workers"] = static_cast<Json::UInt64>(0);
            result["active_generations"] = static_cast<Json::UInt64>(0);
            result["tip_priority_accepted"] = static_cast<Json::UInt64>(0);
            result["recent_priority_accepted"] = static_cast<Json::UInt64>(0);
            result["historical_priority_accepted"] = static_cast<Json::UInt64>(0);
            result["tip_priority_rejected"] = static_cast<Json::UInt64>(0);
            result["recent_priority_rejected"] = static_cast<Json::UInt64>(0);
            result["historical_priority_rejected"] = static_cast<Json::UInt64>(0);
            result["proof_generation_p50_ms"] = 0.0;
            result["proof_generation_p95_ms"] = 0.0;
            result["proof_generation_p99_ms"] = 0.0;
            result["queue_wait_p50_ms"] = 0.0;
            result["queue_wait_p95_ms"] = 0.0;
            result["queue_wait_p99_ms"] = 0.0;
            return result;
        }

        const auto snapshot = bridge_node->GetCacheSnapshot();

        result["hits"] = static_cast<Json::UInt64>(snapshot.hits);
        result["misses"] = static_cast<Json::UInt64>(snapshot.misses);
        result["evictions"] = static_cast<Json::UInt64>(snapshot.evictions);
        result["hit_rate"] = snapshot.hit_rate;
        result["block_cache_entries"] = static_cast<Json::UInt64>(snapshot.block_entries);
        result["block_cache_capacity"] = static_cast<Json::UInt64>(snapshot.block_capacity);
        result["tx_cache_entries"] = static_cast<Json::UInt64>(snapshot.tx_entries);
        result["tx_cache_capacity"] = static_cast<Json::UInt64>(snapshot.tx_capacity);
        result["indexed_heights"] = static_cast<Json::UInt64>(snapshot.indexed_heights);
        result["indexed_blocks"] = static_cast<Json::UInt64>(snapshot.indexed_blocks);
        result["block_cache_ttl_seconds"] = static_cast<Json::UInt64>(snapshot.block_ttl_seconds);
        result["tx_cache_ttl_seconds"] = static_cast<Json::UInt64>(snapshot.tx_ttl_seconds);
        result["proof_requests_total"] = static_cast<Json::UInt64>(snapshot.proof_requests_total);
        result["proof_requests_rejected"] = static_cast<Json::UInt64>(snapshot.proof_requests_rejected);
        result["proof_requests_coalesced"] = static_cast<Json::UInt64>(snapshot.proof_requests_coalesced);
        result["proof_tasks_completed"] = static_cast<Json::UInt64>(snapshot.proof_tasks_completed);
        result["proof_tasks_failed"] = static_cast<Json::UInt64>(snapshot.proof_tasks_failed);
        result["proof_queue_depth"] = static_cast<Json::UInt64>(snapshot.proof_queue_depth);
        result["proof_queue_capacity"] = static_cast<Json::UInt64>(snapshot.proof_queue_capacity);
        result["proof_workers"] = static_cast<Json::UInt64>(snapshot.proof_workers);
        result["active_generations"] = static_cast<Json::UInt64>(snapshot.active_generations);
        result["tip_priority_accepted"] = static_cast<Json::UInt64>(snapshot.tip_priority_accepted);
        result["recent_priority_accepted"] = static_cast<Json::UInt64>(snapshot.recent_priority_accepted);
        result["historical_priority_accepted"] = static_cast<Json::UInt64>(snapshot.historical_priority_accepted);
        result["tip_priority_rejected"] = static_cast<Json::UInt64>(snapshot.tip_priority_rejected);
        result["recent_priority_rejected"] = static_cast<Json::UInt64>(snapshot.recent_priority_rejected);
        result["historical_priority_rejected"] = static_cast<Json::UInt64>(snapshot.historical_priority_rejected);
        result["proof_generation_p50_ms"] = snapshot.proof_generation_p50_ms;
        result["proof_generation_p95_ms"] = snapshot.proof_generation_p95_ms;
        result["proof_generation_p99_ms"] = snapshot.proof_generation_p99_ms;
        result["queue_wait_p50_ms"] = snapshot.queue_wait_p50_ms;
        result["queue_wait_p95_ms"] = snapshot.queue_wait_p95_ms;
        result["queue_wait_p99_ms"] = snapshot.queue_wait_p99_ms;
    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
        dinero::g_logger.error("[getutreexocachestats] Exception: " + std::string(e.what()));
    }

    return result;
}

/**
 * blockchain.getutreexogossipstats - Get proof gossip health metrics
 *
 * Returns runtime counters for invproof/getproof/proofdata behavior,
 * recent-cache health, coalescing effectiveness, and abuse handling.
 *
 * Params: none
 */
Json rpc_getutreexogossipstats(const ExecutionContext& ctx, const Json& params) {
    Json result;
    (void)params;

    try {
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Chainstate not available";
            return result;
        }

        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Chainstate service not available";
            return result;
        }

        auto gossip = chainstate->GetProofGossipManager();
        result["gossip_enabled"] = static_cast<bool>(gossip);
        if (!gossip) {
            return result;
        }

        const auto stats = gossip->GetStats();
        result["invproofs_sent"] = static_cast<Json::UInt64>(stats.invproofs_sent);
        result["invproofs_received"] = static_cast<Json::UInt64>(stats.invproofs_received);
        result["invproofs_duplicate"] = static_cast<Json::UInt64>(stats.invproofs_duplicate);
        result["proofs_requested"] = static_cast<Json::UInt64>(stats.proofs_requested);
        result["proofs_delivered"] = static_cast<Json::UInt64>(stats.proofs_delivered);
        result["proofs_received"] = static_cast<Json::UInt64>(stats.proofs_received);
        result["proof_cache_hits"] = static_cast<Json::UInt64>(stats.proof_cache_hits);
        result["proof_cache_misses"] = static_cast<Json::UInt64>(stats.proof_cache_misses);
        result["proof_requests_coalesced"] = static_cast<Json::UInt64>(stats.proof_requests_coalesced);
        result["proof_provider_failures"] = static_cast<Json::UInt64>(stats.proof_provider_failures);
        result["proof_prewarmed"] = static_cast<Json::UInt64>(stats.proof_prewarmed);
        result["proofdata_unsolicited"] = static_cast<Json::UInt64>(stats.proofdata_unsolicited);
        result["proofdata_replayed"] = static_cast<Json::UInt64>(stats.proofdata_replayed);
        result["invalid_getproof_payloads"] = static_cast<Json::UInt64>(stats.invalid_getproof_payloads);
        result["invalid_proofdata_payloads"] = static_cast<Json::UInt64>(stats.invalid_proofdata_payloads);
        result["getproof_rate_limited"] = static_cast<Json::UInt64>(stats.getproof_rate_limited);
        result["proofdata_rate_limited"] = static_cast<Json::UInt64>(stats.proofdata_rate_limited);
        result["peers_disconnected_for_abuse"] = static_cast<Json::UInt64>(stats.peers_disconnected_for_abuse);
        result["proof_cache_entries"] = static_cast<Json::UInt64>(stats.proof_cache_entries);
        result["proof_cache_capacity"] = static_cast<Json::UInt64>(stats.proof_cache_capacity);
        result["proof_cache_ttl_seconds"] = static_cast<Json::UInt64>(stats.proof_cache_ttl_seconds);
        result["inflight_requests"] = static_cast<Json::UInt64>(stats.inflight_requests);

        const double cache_denom = static_cast<double>(stats.proof_cache_hits + stats.proof_cache_misses);
        const double cache_hit_rate = cache_denom > 0.0
            ? static_cast<double>(stats.proof_cache_hits) / cache_denom
            : 0.0;
        result["proof_cache_hit_rate"] = cache_hit_rate;

        const double delivery_denom = static_cast<double>(stats.proofs_requested);
        const double delivery_rate = delivery_denom > 0.0
            ? static_cast<double>(stats.proofs_received) / delivery_denom
            : 0.0;
        result["proof_delivery_rate"] = delivery_rate;
    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
        dinero::g_logger.error("[getutreexogossipstats] Exception: " + std::string(e.what()));
    }

    return result;
}

/**
 * blockchain.rebuildutreexo - Rebuild UTXO position index from live Utreexo forest
 *
 * Rebuilds the non-consensus (txid,vout) -> position mapping by walking the
 * current ChainDB and resolving each live UTXO against the active forest.
 * This must preserve the real forest; rebuilding a fresh sequential forest
 * produces positions that do not generate valid proofs against the live roots.
 *
 * Params: none
 *
 * Returns:
 *   {
 *     "success": <bool>,
 *     "num_leaves": <n>,           // Total leaves in active forest
 *     "num_roots": <n>,            // Roots in active forest
 *     "position_count": <n>,       // Live entries in rebuilt position index
 *     "matched": <n>,              // Live UTXOs resolved against forest
 *     "missing": <n>,              // Live UTXOs absent from forest
 *     "malformed": <n>,            // Malformed UTXO scripts skipped
 *     "skipped_unspendable": <n>   // OP_RETURN / provably unspendable outputs skipped
 *   }
 */
Json rpc_rebuildutreexo(const ExecutionContext& ctx, const Json& params) {
    Json result;

    try {
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Chainstate not available";
            return result;
        }

        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Chainstate service not available";
            return result;
        }

        auto* forest = chainstate->utreexoForest();
        if (!forest) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Utreexo forest not available";
            return result;
        }

        auto* chain_db = chainstate->GetChainDB();
        if (!chain_db) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "ChainDB not available";
            return result;
        }

        auto* position_index = chainstate->GetUTXOPositionIndex();

        dinero::g_logger.info("[rebuildutreexo] Starting live position index rebuild from ChainDB + active forest");

        if (!position_index) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Position index not available (bridge mode required)";
            return result;
        }

        // NOTE (audit: forest read-during-free UAF): this structural read walks
        // the live forest across a long ChainDB iteration. It is intentionally
        // NOT wrapped in LockForestShared() — holding the forest lock across
        // ChainDB locks would break the leaf-lock invariant and risk inversion
        // with the block-connect writer. rebuildutreexo is an operator-triggered
        // maintenance RPC; serialize it with connect (run under activation_mutex_)
        // or quiesce the node before running it. Tracked as follow-up.
        const auto report = position_index->Rebuild(*chain_db, *forest);

        result["success"] = report.success;
        result["num_leaves"] = static_cast<Json::UInt64>(chainstate->GetConsensusUTXOSet()->SnapshotForestLeafCount());
        result["num_roots"] = static_cast<Json::UInt64>(chainstate->GetConsensusUTXOSet()->SnapshotForestRootCount());
        result["utreexo_root"] = hashToHex(chainstate->GetConsensusUTXOSet()->SnapshotForestCommitment());
        result["position_count"] = static_cast<Json::UInt64>(position_index->GetPositionCount());
        result["matched"] = static_cast<Json::UInt64>(report.matched);
        result["missing"] = static_cast<Json::UInt64>(report.missing);
        result["malformed"] = static_cast<Json::UInt64>(report.malformed);
        result["skipped_unspendable"] = static_cast<Json::UInt64>(report.skipped_unspendable);
        result["forest_preserved"] = true;

        if (!report.success) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Live position index rebuild failed";
            dinero::g_logger.error("[rebuildutreexo] Live position index rebuild failed");
            return result;
        }

        if (report.missing > 0) {
            result["warning"] =
                std::to_string(report.missing) +
                " live UTXO(s) were missing from the active forest; proof serving is degraded";
        }

        dinero::g_logger.info("[rebuildutreexo] Position index rebuilt from live forest: " +
                              std::to_string(report.matched) + " matched, " +
                              std::to_string(report.missing) + " missing, " +
                              std::to_string(report.malformed) + " malformed");

    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
        dinero::g_logger.error("[rebuildutreexo] Exception: " + std::string(e.what()));
    }

    return result;
}

} // namespace din
