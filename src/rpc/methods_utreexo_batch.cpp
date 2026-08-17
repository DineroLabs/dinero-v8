/**
 * Phase 11a.1: Utreexo Batch RPC Methods
 *
 * Implements high-performance batch RPC methods for Utreexo proofs:
 * - getutxoproofs_batch: Generate proofs for multiple UTXOs in one call
 * - verifyutxoproofs_batch: Verify multiple proofs against accumulator
 *
 * Performance target: 1000+ proofs/second
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "primitives/uint256.h"
#include "primitives/hash_domains.h"
#include "storage/chain_db.h"
#include "common/status.h"
#include "common/logger.h"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <vector>

namespace din {
using ExecutionContext = ::ExecutionContext;  // Global ExecutionContext from rpc/rpc_registry.h

// Helper: Convert UtreexoHash to hex string
std::string hashToHexBatch(const dinero::consensus::UtreexoHash& hash) {
    std::ostringstream oss;
    for (uint8_t byte : hash) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

// Helper: Convert hex string to UtreexoHash
std::optional<dinero::consensus::UtreexoHash> hexToHashBatch(const std::string& hex) {
    if (hex.size() != 64) {  // 32 bytes = 64 hex chars
        return std::nullopt;
    }

    dinero::consensus::UtreexoHash hash(32);
    for (size_t i = 0; i < 32; ++i) {
        std::string byte_str = hex.substr(i * 2, 2);
        try {
            hash[i] = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
        } catch (...) {
            return std::nullopt;
        }
    }
    return hash;
}

// Helper: Validate hex string format
bool isValidHex(const std::string& hex) {
    if (hex.empty()) return false;
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

// Helper: Parse and validate txid from hex
std::optional<dinero::TxId> parseTxId(const std::string& txid_hex) {
    // Validate length (32 bytes = 64 hex chars)
    if (txid_hex.size() != 64) {
        return std::nullopt;
    }

    // Validate hex characters
    if (!isValidHex(txid_hex)) {
        return std::nullopt;
    }

    // Parse to uint256
    dinero::uint256 txid_uint256;
    if (!dinero::uint256::FromHex(txid_hex, txid_uint256)) {
        return std::nullopt;
    }

    return dinero::TxId(txid_uint256);
}

void MaybeScheduleProofCoverageRecovery(dinero::ChainstateService& chainstate,
                                        dinero::ChainDB& chain_db,
                                        const dinero::TxId& txid,
                                        uint32_t vout,
                                        const std::string& source_tag) {
    auto coin_result = chain_db.getCoin(txid.AsUint256(), vout);
    if (coin_result.status() != dinero::Status::Ok) {
        return;
    }

    const std::string outpoint =
        txid.AsUint256().GetHex() + ":" + std::to_string(vout);
    dinero::g_logger.error(source_tag +
                           " position index missing live UTXO " + outpoint +
                           " — scheduling chainstate recovery");
    chainstate.RequestChainstateRecovery(
        "live UTXO missing from Utreexo position index for " + outpoint,
        source_tag);
}

/**
 * blockchain.getutxoproofs_batch - Generate proofs for multiple UTXOs
 *
 * Creates compact Merkle proofs for multiple UTXOs in a single efficient call.
 * Significantly faster than calling getutxoproof individually.
 *
 * Params:
 *   [0] utxos (array, required): Array of {txid, vout} objects
 *
 * Returns:
 *   {
 *     "utreexo_root": "<hash>",  // Current Utreexo commitment (32 bytes hex)
 *     "proofs": [
 *       {
 *         "txid": "<txid>",
 *         "vout": <n>,
 *         "proof": {
 *           "siblings": ["<hash>", ...],  // Merkle path hashes
 *           "position": <n>,               // Leaf position in forest
 *           "num_leaves": <n>              // Total leaves in forest
 *         },
 *         "proof_size": <n>,  // Proof size in bytes
 *         "success": true
 *       },
 *       {
 *         "txid": "<txid>",
 *         "vout": <n>,
 *         "success": false,
 *         "error_code": "utxo-not-found",
 *         "error": "UTXO not found in position index"
 *       },
 *       ...
 *     ],
 *     "batch_size": <n>,
 *     "successful": <n>,
 *     "failed": <n>,
 *     "generation_time_ms": <n>
 *   }
 *
 * Notes:
 *   - Results are returned in the same order as requested UTXOs
 *   - Proofs may be temporarily unavailable during reorgs (transient failures)
 *   - Batch size is capped at 1000 UTXOs for DoS protection
 *   - Each UTXO proof is independent (partial success is allowed)
 */
Json rpc_getutxoproofs_batch(const ExecutionContext& ctx, const Json& params) {
    Json result;
    auto start_time = std::chrono::high_resolution_clock::now();

    try {
        // Validate context
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

        auto* position_index = chainstate->GetUTXOPositionIndex();
        if (!position_index) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Position index not available";
            return result;
        }

        auto* chain_db = chainstate->GetChainDB();
        if (!chain_db) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "ChainDB not available";
            return result;
        }

        // Parse parameters
        if (params.empty() || !params[0].isArray()) {
            result["error"]["code"] = -32602;
            result["error"]["message"] = "Invalid params: expected array of {txid, vout} objects";
            return result;
        }

        const Json& utxos = params[0];
        size_t batch_size = utxos.size();

        // Enforce batch size limit (prevent DoS)
        constexpr size_t MAX_BATCH_SIZE = 1000;
        if (batch_size > MAX_BATCH_SIZE) {
            result["error"]["code"] = -32600;
            result["error"]["message"] = "Batch size exceeds limit (max " + std::to_string(MAX_BATCH_SIZE) + ")";
            return result;
        }

        // Get current Utreexo root for batch result
        // Snapshot the live forest once under its shared lock; derive the root
        // and all batch proofs from the local clone — consistent AND free of a
        // race with the block-connect writer (audit: forest read-during-free UAF).
        dinero::consensus::UtreexoForest forest_view;
        {
            auto forest_lock = chainstate->GetConsensusUTXOSet()->LockForestShared();
            forest_view = forest->clone();
        }
        dinero::consensus::UtreexoHash utreexo_root = forest_view.getCommitment();

        // Process each UTXO
        Json proofs_array(::Json::arrayValue);
        size_t successful = 0;
        size_t failed = 0;

        for (const auto& utxo : utxos) {
            Json proof_result;

            // Parse txid and vout
            if (!utxo.isObject() || !utxo.isMember("txid") || !utxo.isMember("vout")) {
                proof_result["success"] = false;
                proof_result["error_code"] = "invalid-format";
                proof_result["error"] = "Invalid UTXO format: expected {txid, vout}";
                failed++;
                proofs_array.append(proof_result);
                continue;
            }

            std::string txid_hex = utxo["txid"].asString();

            // Validate vout is unsigned integer
            if (!utxo["vout"].isUInt()) {
                proof_result["txid"] = txid_hex;
                proof_result["success"] = false;
                proof_result["error_code"] = "invalid-vout";
                proof_result["error"] = "Invalid vout: must be unsigned integer";
                failed++;
                proofs_array.append(proof_result);
                continue;
            }

            uint32_t vout = utxo["vout"].asUInt();
            proof_result["txid"] = txid_hex;
            proof_result["vout"] = vout;

            // Parse and validate txid
            auto txid_opt = parseTxId(txid_hex);
            if (!txid_opt.has_value()) {
                proof_result["success"] = false;
                proof_result["error_code"] = "invalid-txid";
                proof_result["error"] = "Invalid txid format (expected 64 hex characters)";
                failed++;
                proofs_array.append(proof_result);
                continue;
            }

            dinero::TxId txid = txid_opt.value();

            // Look up position from index
            auto position_opt = position_index->GetPosition(txid, vout);
            if (!position_opt.has_value()) {
                MaybeScheduleProofCoverageRecovery(*chainstate, *chain_db, txid, vout,
                                                  "[getutxoproofs_batch]");
                proof_result["success"] = false;
                proof_result["error_code"] = "chainstate-recovery-required";
                proof_result["error"] = "Live UTXO missing from position index; chainstate recovery scheduled";
                failed++;
                proofs_array.append(proof_result);
                continue;
            }

            uint64_t position = position_opt.value();

            // Generate proof from forest
            auto proof_opt = forest_view.prove(position);
            if (!proof_opt.has_value()) {
                proof_result["success"] = false;
                proof_result["error_code"] = "proof-generation-failed";
                proof_result["error"] = "Failed to generate proof from forest (may be transient during reorg)";
                failed++;
                proofs_array.append(proof_result);
                continue;
            }

            const auto& proof = proof_opt.value();

            // Serialize proof
            Json proof_json;
            Json siblings_array(::Json::arrayValue);
            for (const auto& sibling : proof.siblings) {
                siblings_array.append(hashToHexBatch(sibling));
            }
            proof_json["siblings"] = siblings_array;
            proof_json["position"] = static_cast<Json::Int64>(proof.position);
            proof_json["num_leaves"] = static_cast<Json::Int64>(proof.numLeaves);

            proof_result["proof"] = proof_json;
            proof_result["proof_size"] = static_cast<Json::Int64>(proof.siblings.size() * 32 + 16);
            proof_result["success"] = true;

            successful++;
            proofs_array.append(proof_result);
        }

        // Calculate timing
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        // Return results
        result["utreexo_root"] = hashToHexBatch(utreexo_root);
        result["proofs"] = proofs_array;
        result["batch_size"] = static_cast<Json::Int64>(batch_size);
        result["successful"] = static_cast<Json::Int64>(successful);
        result["failed"] = static_cast<Json::Int64>(failed);
        result["generation_time_ms"] = static_cast<Json::Int64>(duration.count());

        dinero::g_logger.info("[getutxoproofs_batch] Generated " + std::to_string(successful) +
                             "/" + std::to_string(batch_size) + " proofs in " +
                             std::to_string(duration.count()) + "ms");

    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
        dinero::g_logger.error("[getutxoproofs_batch] Exception: " + std::string(e.what()));
    }

    return result;
}

/**
 * blockchain.verifyutxoproofs_batch - Verify multiple proofs against accumulator
 *
 * Verifies cryptographic proofs for multiple UTXOs in a single efficient call.
 * Checks that each proof is valid against the current Utreexo commitment.
 *
 * Params:
 *   [0] proofs (array, required): Array of proof objects with UTXO data
 *        Each object: {txid, vout, proof: {siblings, position, num_leaves}}
 *
 * Returns:
 *   {
 *     "utreexo_root": "<hash>",  // Current Utreexo commitment used for verification
 *     "results": [
 *       {
 *         "txid": "<txid>",
 *         "vout": <n>,
 *         "valid": true
 *       },
 *       {
 *         "txid": "<txid>",
 *         "vout": <n>,
 *         "valid": false,
 *         "error_code": "proof-invalid",
 *         "error": "Proof verification failed: root mismatch"
 *       },
 *       ...
 *     ],
 *     "batch_size": <n>,
 *     "valid": <n>,
 *     "invalid": <n>,
 *     "verification_time_ms": <n>
 *   }
 *
 * Notes:
 *   - Results are returned in the same order as requested proofs
 *   - Requires UTXO data from chainstate (authoritative UTXO set) to compute leaf hashes
 *   - Verification is performed against current forest state
 *   - Batch size is capped at 1000 proofs for DoS protection
 */
Json rpc_verifyutxoproofs_batch(const ExecutionContext& ctx, const Json& params) {
    Json result;
    auto start_time = std::chrono::high_resolution_clock::now();

    try {
        // Validate context
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

        // Phase 11a: Verification queries chainstate (UTXO set + forest), not ChainDB (storage)
        // ChainDB is removed here - we only need chainstate for consensus queries

        // Parse parameters
        if (params.empty() || !params[0].isArray()) {
            result["error"]["code"] = -32602;
            result["error"]["message"] = "Invalid params: expected array of proof objects";
            return result;
        }

        const Json& proofs_param = params[0];
        size_t batch_size = proofs_param.size();

        // Enforce batch size limit
        constexpr size_t MAX_BATCH_SIZE = 1000;
        if (batch_size > MAX_BATCH_SIZE) {
            result["error"]["code"] = -32600;
            result["error"]["message"] = "Batch size exceeds limit (max " + std::to_string(MAX_BATCH_SIZE) + ")";
            return result;
        }

        // Get current forest roots and commitment for verification
        // Snapshot the live forest once under its shared lock; derive the root
        // and all batch proofs from the local clone — consistent AND free of a
        // race with the block-connect writer (audit: forest read-during-free UAF).
        dinero::consensus::UtreexoForest forest_view;
        {
            auto forest_lock = chainstate->GetConsensusUTXOSet()->LockForestShared();
            forest_view = forest->clone();
        }
        std::vector<dinero::consensus::UtreexoHash> roots = forest_view.getRoots();
        dinero::consensus::UtreexoHash utreexo_root = forest_view.getCommitment();

        // Process each proof
        Json results_array(::Json::arrayValue);
        size_t valid_count = 0;
        size_t invalid_count = 0;

        for (const auto& proof_obj : proofs_param) {
            Json verify_result;

            // Parse txid, vout, and proof
            if (!proof_obj.isObject() || !proof_obj.isMember("txid") ||
                !proof_obj.isMember("vout") || !proof_obj.isMember("proof")) {
                verify_result["valid"] = false;
                verify_result["error_code"] = "invalid-format";
                verify_result["error"] = "Invalid format: expected {txid, vout, proof}";
                invalid_count++;
                results_array.append(verify_result);
                continue;
            }

            std::string txid_hex = proof_obj["txid"].asString();

            // Validate vout is unsigned integer
            if (!proof_obj["vout"].isUInt()) {
                verify_result["txid"] = txid_hex;
                verify_result["valid"] = false;
                verify_result["error_code"] = "invalid-vout";
                verify_result["error"] = "Invalid vout: must be unsigned integer";
                invalid_count++;
                results_array.append(verify_result);
                continue;
            }

            uint32_t vout = proof_obj["vout"].asUInt();
            verify_result["txid"] = txid_hex;
            verify_result["vout"] = vout;

            // Parse and validate txid
            auto txid_opt = parseTxId(txid_hex);
            if (!txid_opt.has_value()) {
                verify_result["valid"] = false;
                verify_result["error_code"] = "invalid-txid";
                verify_result["error"] = "Invalid txid format (expected 64 hex characters)";
                invalid_count++;
                results_array.append(verify_result);
                continue;
            }

            dinero::TxId txid = txid_opt.value();
            dinero::uint256 txid_uint256 = txid.AsUint256();

            // Phase 11a FIX: Query chainstate (authoritative UTXO set), NOT ChainDB (storage layer)
            // ChainDB is persistence; chainstate is consensus authority
            auto* utxo_index = chainstate->getUTXOIndex();
            if (!utxo_index) {
                verify_result["valid"] = false;
                verify_result["error_code"] = "utxo-index-unavailable";
                verify_result["error"] = "UTXO index not available";
                invalid_count++;
                results_array.append(verify_result);
                continue;
            }

            std::optional<dinero::WalletUTXO> utxo_opt = utxo_index->GetUTXO(txid, vout);
            if (!utxo_opt.has_value()) {
                verify_result["valid"] = false;
                verify_result["error_code"] = "utxo-not-found";
                verify_result["error"] = "UTXO not found in chainstate (spent or never existed)";
                invalid_count++;
                results_array.append(verify_result);
                continue;
            }

            const dinero::WalletUTXO& utxo = utxo_opt.value();

            dinero::consensus::UtreexoHash leaf_hash = dinero::consensus::HashUTXOForCreationHeight(
                txid_uint256,
                vout,
                utxo.value.GetUna(),  // AmountUna -> uint64_t
                utxo.spk,              // scriptPubKey bytes
                static_cast<uint32_t>(utxo.height),
                utxo.is_coinbase
            );

            // Parse proof structure
            const Json& proof_json = proof_obj["proof"];
            if (!proof_json.isObject() || !proof_json.isMember("siblings") ||
                !proof_json.isMember("position") || !proof_json.isMember("num_leaves")) {
                verify_result["valid"] = false;
                verify_result["error_code"] = "invalid-proof-structure";
                verify_result["error"] = "Invalid proof structure";
                invalid_count++;
                results_array.append(verify_result);
                continue;
            }

            // Build UtreexoProof structure
            dinero::consensus::UtreexoProof proof;
            proof.position = proof_json["position"].asUInt64();
            proof.numLeaves = proof_json["num_leaves"].asUInt64();

            const Json& siblings_json = proof_json["siblings"];
            if (!siblings_json.isArray()) {
                verify_result["valid"] = false;
                verify_result["error_code"] = "invalid-siblings";
                verify_result["error"] = "Invalid siblings array";
                invalid_count++;
                results_array.append(verify_result);
                continue;
            }

            for (const auto& sibling_hex : siblings_json) {
                auto sibling_opt = hexToHashBatch(sibling_hex.asString());
                if (!sibling_opt.has_value()) {
                    verify_result["valid"] = false;
                    verify_result["error_code"] = "invalid-sibling-hash";
                    verify_result["error"] = "Invalid sibling hash format";
                    invalid_count++;
                    results_array.append(verify_result);
                    goto next_proof;  // Break out of inner loop and continue outer
                }
                proof.siblings.push_back(sibling_opt.value());
            }

            // Verify proof
            if (proof.verify(leaf_hash, roots)) {
                verify_result["valid"] = true;
                valid_count++;
            } else {
                verify_result["valid"] = false;
                verify_result["error_code"] = "proof-invalid";
                verify_result["error"] = "Proof verification failed: root mismatch";
                invalid_count++;
            }

            next_proof:
            results_array.append(verify_result);
        }

        // Calculate timing
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        // Return results
        result["utreexo_root"] = hashToHexBatch(utreexo_root);
        result["results"] = results_array;
        result["batch_size"] = static_cast<Json::Int64>(batch_size);
        result["valid"] = static_cast<Json::Int64>(valid_count);
        result["invalid"] = static_cast<Json::Int64>(invalid_count);
        result["verification_time_ms"] = static_cast<Json::Int64>(duration.count());

        dinero::g_logger.info("[verifyutxoproofs_batch] Verified " + std::to_string(valid_count) +
                             "/" + std::to_string(batch_size) + " proofs in " +
                             std::to_string(duration.count()) + "ms");

    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
        dinero::g_logger.error("[verifyutxoproofs_batch] Exception: " + std::string(e.what()));
    }

    return result;
}

} // namespace din
