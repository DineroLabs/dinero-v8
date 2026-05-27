/**
 * Blockchain RPC Methods - Context-Aware (Week 2 Migration)
 *
 * This file demonstrates the migration from legacy globals to DaemonContext.
 * Compare with methods_blockchain_legacy.cpp to see the difference.
 *
 * OLD PATTERN (legacy):
 *   extern ChainDB* g_chain_db_direct;
 *   uint32_t height = dinero::storage::GetChainHeight(dinero::legacy::g_chain_db_direct());
 *
 * NEW PATTERN (context-aware):
 *   auto chainstate = ctx.daemon->chainstate;
 *   uint32_t height = chainstate->getBlockHeight();
 *
 * Benefits:
 * - No dependency on global variables
 * - Testable with mock services
 * - Clear dependency tracking
 * - Type-safe service access
 */

// Order matters: include daemon_context.h AFTER rpc_registry.h
// so the forward declaration gets replaced with the full definition
#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"  // Full DaemonContext definition (replaces forward decl)
#include "daemon/services/chainstate_service.h"
#include "daemon/services/p2p_service.h"
#include "daemon/services/wallet_service.h"  // Phase W.1.1: For WalletService access
#include "daemon/services/config_service.h"
#include "daemon/services/prune_service.h"   // Phase P.2: For PruneService RPC wiring
#include "consensus/block_download_scheduler.h"
#include "consensus/header_store.h"
#include "consensus/header_sync_p2p.h"
#include "common/logger.h"
#include <memory>  // For std::dynamic_pointer_cast
#include "storage/chain_db.h"
#include "storage/archival_block_reader.h"
#include "storage/chain_direct.h"
#include "consensus/subsidy.h"
#include <cstring>
#include "daemon/block_acceptor.h"
#include "consensus/chainparams.h"
#include "consensus/merkle_root.h"  // For ComputeMerkleRoot (Phase 11a.2)
#include "consensus/shielded/shielded_output_feed.h"  // M2: shielded outputs RPC
// Phase 39: chain_manager.h deleted (ChainManager removed)
#include "primitives/uint256.h"  // Phase M.0: For uint256 type
#include "common/address_script_builder.h"  // Phase W.1.1: For BuildScriptPubKeyFromAddress
#include "mining/block_assembler.h"  // For canonical CalculateMerkleRoot
#include "common/sha256d.h"  // For Dinero::Common::reverse_hex
#include "consensus/block_filter.h"  // BIP158 GCS block filters
#include "consensus/filter_commitment.h"  // DNRF commitment validation for served filters
#include "consensus/merkle_proof.h"  // Merkle inclusion proofs for coinbase
#include "consensus/outpoint.h"          // For OutPoint
#include "primitives/hash_domains.h"     // For TxId
#include "consensus/utxo_entry.h"        // For UTXOEntry
#include <sstream>
#include <iomanip>
#include <cstring>  // Phase W.1.1: For std::memset
#include <ctime>    // Phase W.1.1: For std::time
#include <filesystem>
#include <algorithm>

using dinero::uint256;  // Phase M.0: Make uint256 available without namespace prefix

// Forward declaration for RPC metadata registration
namespace dinero {
namespace rpc {
    void registerBlockchainRPCMetadata();
}
}

// Forward declarations: UTXO set methods (defined in methods_utxoset.cpp)
namespace din {
    din::Json rpc_gettxoutsetinfo(const ExecutionContext& ctx, const din::Json& params);
    din::Json rpc_scantxoutset(const ExecutionContext& ctx, const din::Json& params);
}

// Forward declarations: Address-indexed query methods (defined in methods_address_index.cpp)
namespace din {
    din::Json rpc_getaddressbalance(const ExecutionContext& ctx, const din::Json& params);
    din::Json rpc_getaddressmempool(const ExecutionContext& ctx, const din::Json& params);
    din::Json rpc_getaddresshistory(const ExecutionContext& ctx, const din::Json& params);
    din::Json rpc_reindextx(const ExecutionContext& ctx, const din::Json& params);
    din::Json rpc_getcheckpoints(const ExecutionContext& ctx, const din::Json& params);
}

namespace {
static dinero::StatusOr<dinero::Block> ReadRpcBlock(
    dinero::ChainstateService* chainstate,
    dinero::ChainDB* chain_db,
    dinero::BlockStorage* block_storage,
    const uint256& block_hash)
{
    if (chainstate) {
        return chainstate->getBlockByHash(block_hash);
    }
    if (chain_db) {
        return dinero::storage::ReadArchivalBlock(*chain_db, block_storage, block_hash);
    }
    return dinero::Status::Internal;
}

struct PeerSyncTelemetrySummary {
    size_t connected{0};
    uint32_t advertised_best_height{0};
    uint32_t max_synced_headers{0};
    uint32_t max_synced_blocks{0};
};

struct ArchivalCoverageSummary {
    uint64_t expected_body_count{0};
    uint64_t readable_body_count{0};
    uint64_t flatfile_body_count{0};
    uint64_t legacy_fallback_body_count{0};
    int lowest_readable_body_height{-1};
    int lowest_flatfile_body_height{-1};
    int highest_contiguous_flatfile_body_height{-1};
    int first_missing_readable_body_height{-1};
    int first_missing_flatfile_body_height{-1};
    bool genesis_readable_body_present{false};
    bool genesis_flatfile_body_present{false};
    bool tip_readable_body_present{false};
    bool tip_flatfile_body_present{false};
};

PeerSyncTelemetrySummary CollectPeerSyncTelemetry(const ExecutionContext& ctx) {
    PeerSyncTelemetrySummary summary;

    if (!ctx.daemon || !ctx.daemon->p2p) {
        return summary;
    }

    auto p2p = std::dynamic_pointer_cast<dinero::P2PService>(ctx.daemon->p2p);
    if (!p2p) {
        return summary;
    }

    const auto peers = p2p->GetConnectedPeers();
    summary.connected = peers.size();

    for (const auto& peer : peers) {
        summary.advertised_best_height = std::max(
            summary.advertised_best_height,
            std::max(peer.best_known_height, peer.start_height));
        summary.max_synced_headers = std::max(summary.max_synced_headers, peer.synced_headers);
        summary.max_synced_blocks = std::max(summary.max_synced_blocks, peer.synced_blocks);
    }

    return summary;
}

dinero::consensus::HeaderSyncState EffectiveHeaderSyncState(
    const dinero::consensus::HeaderSyncManager::SyncStats& stats,
    uint32_t effective_peer_best_height
) {
    const bool caught_up_against_peers = effective_peer_best_height <= stats.local_best_height;
    if (caught_up_against_peers &&
        (stats.state == dinero::consensus::HeaderSyncState::IDLE ||
         stats.state == dinero::consensus::HeaderSyncState::REQUESTING_HEADERS ||
         stats.state == dinero::consensus::HeaderSyncState::PROCESSING_HEADERS)) {
        return dinero::consensus::HeaderSyncState::CAUGHT_UP;
    }

    return stats.state;
}

std::string RawBytesToHex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

std::string UtreexoRootRawHex(const uint256& root) {
    return RawBytesToHex(root.begin(), 32);
}

std::string BytesToHex(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) {
        return "";
    }
    return RawBytesToHex(bytes.data(), bytes.size());
}

std::string ClassifyPrivacyFlow(bool has_conf_inputs, bool has_conf_outputs) {
    if (has_conf_inputs && has_conf_outputs) {
        return "ct_transfer";
    }
    if (has_conf_inputs) {
        return "unshield";
    }
    if (has_conf_outputs) {
        return "shield";
    }
    return "transparent";
}

ArchivalCoverageSummary ComputeArchivalCoverage(
    const dinero::ChainstateService& chainstate,
    const dinero::ChainDB& chain_db,
    uint32_t active_height
) {
    ArchivalCoverageSummary summary;
    summary.expected_body_count = static_cast<uint64_t>(active_height) + 1;

    bool contiguous_flatfiles = true;
    for (uint64_t height = 0; height <= active_height; ++height) {
        auto hash_result = chain_db.getBlockHashByHeight(static_cast<int>(height));
        const bool have_hash = hash_result.ok();

        bool readable = false;
        bool flatfile = false;
        if (have_hash) {
            const auto& hash = hash_result.value();
            readable = chainstate.hasReadableBlockByHash(hash);
            flatfile = chainstate.hasFlatfileBlockByHash(hash);
        }

        if (readable) {
            summary.readable_body_count++;
            if (summary.lowest_readable_body_height < 0) {
                summary.lowest_readable_body_height = static_cast<int>(height);
            }
        } else if (summary.first_missing_readable_body_height < 0) {
            summary.first_missing_readable_body_height = static_cast<int>(height);
        }

        if (flatfile) {
            summary.flatfile_body_count++;
            if (summary.lowest_flatfile_body_height < 0) {
                summary.lowest_flatfile_body_height = static_cast<int>(height);
            }
            if (readable) {
                summary.highest_contiguous_flatfile_body_height = contiguous_flatfiles
                    ? static_cast<int>(height)
                    : summary.highest_contiguous_flatfile_body_height;
            }
        } else {
            if (summary.first_missing_flatfile_body_height < 0) {
                summary.first_missing_flatfile_body_height = static_cast<int>(height);
            }
            contiguous_flatfiles = false;
        }

        if (readable && !flatfile) {
            summary.legacy_fallback_body_count++;
        }

        if (height == 0) {
            summary.genesis_readable_body_present = readable;
            summary.genesis_flatfile_body_present = flatfile;
        }
        if (height == active_height) {
            summary.tip_readable_body_present = readable;
            summary.tip_flatfile_body_present = flatfile;
        }
    }

    return summary;
}
} // namespace

// Helper to format DIN amounts
static std::string formatDIN(uint64_t una) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(8)
        << (double)una / (double)dinero::ConsensusSubsidy::UNA_PER_DIN;
    return oss.str();
}

template <typename OutputLike>
void PopulateExplorerOutputFields(din::Json& output_obj, const OutputLike& output) {
    output_obj["is_confidential"] = output.is_confidential;
    output_obj["amount_hidden"] = output.is_confidential;
    if (output.is_confidential) {
        output_obj["display_amount"] = "confidential";
        output_obj["commitment"] = BytesToHex(output.commitment);
        output_obj["range_proof_bytes"] = static_cast<Json::UInt64>(output.range_proof.size());
        output_obj["nonce_bytes"] = static_cast<Json::UInt64>(output.nonce.size());
    } else {
        output_obj["value_una"] = static_cast<double>(output.value.GetUna());
        output_obj["value_din"] = formatDIN(output.value.GetUna());
        output_obj["display_amount"] = formatDIN(output.value.GetUna());
    }
}

static std::string backgroundValidationStatusToString(
    dinero::ChainstateService::BackgroundValidationStatus status
) {
    switch (status) {
        case dinero::ChainstateService::BackgroundValidationStatus::NotStarted:
            return "NotStarted";
        case dinero::ChainstateService::BackgroundValidationStatus::InProgress:
            return "InProgress";
        case dinero::ChainstateService::BackgroundValidationStatus::Completed:
            return "Completed";
        case dinero::ChainstateService::BackgroundValidationStatus::Failed:
            return "Failed";
    }
    return "Unknown";
}

static std::string ibdStatusToString(dinero::ChainstateService::IBDStatus status) {
    switch (status) {
        case dinero::ChainstateService::IBDStatus::NotInIBD:
            return "NotInIBD";
        case dinero::ChainstateService::IBDStatus::InIBD:
            return "InIBD";
        case dinero::ChainstateService::IBDStatus::SnapshotBootstrap:
            return "SnapshotBootstrap";
        case dinero::ChainstateService::IBDStatus::IBDComplete:
            return "IBDComplete";
    }
    return "Unknown";
}

static std::string headerSyncStateToString(dinero::consensus::HeaderSyncState state) {
    switch (state) {
        case dinero::consensus::HeaderSyncState::IDLE:
            return "idle";
        case dinero::consensus::HeaderSyncState::REQUESTING_HEADERS:
            return "requesting_headers";
        case dinero::consensus::HeaderSyncState::PROCESSING_HEADERS:
            return "processing_headers";
        case dinero::consensus::HeaderSyncState::STALLED:
            return "stalled";
        case dinero::consensus::HeaderSyncState::CAUGHT_UP:
            return "caught_up";
    }
    return "unknown";
}

static std::string syncPhaseToString(dinero::SyncPhase phase) {
    switch (phase) {
        case dinero::SyncPhase::IBD:
            return "ibd";
        case dinero::SyncPhase::CATCHING_UP:
            return "catching_up";
        case dinero::SyncPhase::STEADY_STATE:
            return "steady_state";
    }
    return "unknown";
}

static din::Json buildSnapshotBootstrapDiagnostics(
    const ExecutionContext& ctx,
    const std::shared_ptr<dinero::ChainstateService>& chainstate
) {
    din::Json snapshot;
    snapshot["assumeutxo_active"] = chainstate->IsAssumeUTXOActive();
    snapshot["background_validation_complete"] = chainstate->IsBackgroundValidationComplete();

    if (chainstate->IsAssumeUTXOActive()) {
        snapshot["snapshot_base_height"] = chainstate->GetAssumeUTXOBaseHeight();
        snapshot["snapshot_base_block"] = chainstate->GetAssumeUTXOBaseBlock().GetHex();
    }

    const auto validation_progress = chainstate->GetBackgroundValidationProgress();
    snapshot["background_validation_status"] = backgroundValidationStatusToString(validation_progress.status);
    snapshot["background_validation_progress_percent"] = validation_progress.progress_percent;
    snapshot["background_validation_current_height"] = validation_progress.current_height;
    snapshot["background_validation_target_height"] = validation_progress.target_height;
    if (!validation_progress.error_message.empty()) {
        snapshot["background_validation_error"] = validation_progress.error_message;
    }

    const uint64_t default_max_snapshot_mb = 64ULL * 1024ULL;
    uint64_t max_snapshot_mb = default_max_snapshot_mb;
    bool manifest_required = false;
    std::string configured_snapshot_path;
    std::string configured_manifest_path;

    if (ctx.daemon && ctx.daemon->config) {
        auto config = ctx.daemon->config;
        max_snapshot_mb = static_cast<uint64_t>(std::max(
            config->GetInt("assumeutxo_snapshot_max_mb", static_cast<int>(default_max_snapshot_mb)), 0));
        manifest_required = config->GetBool("assumeutxo_require_manifest", false);
        configured_snapshot_path = config->GetString("assumeutxo_snapshot", "");
        configured_manifest_path = config->GetString("assumeutxo_manifest", "");
    }

    snapshot["snapshot_transport_max_mb"] = static_cast<Json::UInt64>(max_snapshot_mb);
    snapshot["manifest_required"] = manifest_required;
    snapshot["configured_snapshot_path"] = configured_snapshot_path;
    snapshot["configured_manifest_path"] = configured_manifest_path;
    snapshot["configured_snapshot"] = !configured_snapshot_path.empty();
    snapshot["configured_manifest"] = !configured_manifest_path.empty();

    std::filesystem::path resolved_snapshot_path;
    std::error_code ec;
    bool snapshot_exists = false;
    bool snapshot_regular = false;
    bool snapshot_symlink = false;
    uint64_t snapshot_size = 0;
    if (!configured_snapshot_path.empty()) {
        resolved_snapshot_path = std::filesystem::path(configured_snapshot_path);
        const auto status = std::filesystem::symlink_status(resolved_snapshot_path, ec);
        if (!ec) {
            snapshot_exists = std::filesystem::exists(status);
            snapshot_regular = std::filesystem::is_regular_file(status);
            snapshot_symlink = std::filesystem::is_symlink(status);
            if (snapshot_exists && snapshot_regular) {
                snapshot_size = std::filesystem::file_size(resolved_snapshot_path, ec);
                if (ec) {
                    snapshot_size = 0;
                }
            }
        }
    }
    snapshot["configured_snapshot_exists"] = snapshot_exists;
    snapshot["configured_snapshot_regular_file"] = snapshot_regular;
    snapshot["configured_snapshot_symlink"] = snapshot_symlink;
    snapshot["configured_snapshot_bytes"] = static_cast<Json::UInt64>(snapshot_size);

    std::filesystem::path resolved_manifest_path;
    std::string manifest_source = "none";
    if (!configured_manifest_path.empty()) {
        resolved_manifest_path = std::filesystem::path(configured_manifest_path);
        manifest_source = "config";
    } else if (!configured_snapshot_path.empty()) {
        resolved_manifest_path = resolved_snapshot_path;
        resolved_manifest_path += ".manifest.json";
        manifest_source = "sibling";
    }

    bool manifest_present = false;
    bool manifest_regular = false;
    if (!resolved_manifest_path.empty()) {
        const auto manifest_status = std::filesystem::symlink_status(resolved_manifest_path, ec);
        if (!ec) {
            manifest_present = std::filesystem::exists(manifest_status);
            manifest_regular = std::filesystem::is_regular_file(manifest_status);
        }
    }

    snapshot["manifest_source"] = manifest_source;
    snapshot["manifest_path"] = resolved_manifest_path.empty() ? "" : resolved_manifest_path.string();
    snapshot["manifest_present"] = manifest_present;
    snapshot["manifest_regular_file"] = manifest_regular;

    std::string trust_gate_mode = "disabled";
    if (manifest_required) {
        trust_gate_mode = "required";
    } else if (manifest_present || !configured_manifest_path.empty()) {
        trust_gate_mode = "optional";
    }
    snapshot["trust_gate_mode"] = trust_gate_mode;

    if (manifest_required && !manifest_present) {
        snapshot["next_action"] = "Manifest is required; provide assumeutxo_manifest or <snapshot>.manifest.json before loading.";
    } else if (!configured_snapshot_path.empty() && !snapshot_exists) {
        snapshot["next_action"] = "Configured assumeutxo_snapshot path does not exist; update path or deploy snapshot file.";
    } else if (!configured_snapshot_path.empty() && snapshot_symlink) {
        snapshot["next_action"] = "Snapshot path is a symlink; use a regular file path.";
    } else if (chainstate->IsAssumeUTXOActive() && !chainstate->IsBackgroundValidationComplete()) {
        snapshot["next_action"] = "Snapshot loaded; monitor getbackgroundvalidationprogress until status=Completed.";
    } else if (chainstate->IsBackgroundValidationComplete()) {
        snapshot["next_action"] = "Background validation complete; snapshot trust has been fully verified.";
    } else if (configured_snapshot_path.empty()) {
        snapshot["next_action"] = "Set assumeutxo_snapshot in config or use loadtxoutset <path> for fast bootstrap.";
    } else {
        snapshot["next_action"] = "Snapshot bootstrap configuration appears ready.";
    }

    return snapshot;
}

// ═══════════════════════════════════════════════════════════════
// CONTEXT-AWARE RPC HANDLERS (Week 2 Pattern)
// ═══════════════════════════════════════════════════════════════

/**
 * blockchain.getblockcount - Get current blockchain height
 *
 * OLD: extern ChainDB* g_chain_db_direct;
 *      dinero::storage::GetChainHeight(dinero::legacy::g_chain_db_direct())
 *
 * NEW: ctx.daemon->chainstate->getBlockHeight()
 */
din::Json rpc_context_getblockcount(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Week 2: Access chainstate via DaemonContext instead of global
    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    uint32_t height = chainstate->getBlockHeight();
    result = static_cast<int>(height);
    return result;
}

/**
 * blockchain.getblockhash - Get block hash by height
 *
 * OLD: dinero::storage::GetBlockHash(dinero::legacy::g_chain_db_direct(), height)
 * NEW: ctx.daemon->chainstate->chainDB()->...
 */
din::Json rpc_context_getblockhash(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<int>()) {
        result["error"] = "Height parameter required (integer)";
        return result;
    }

    // Week 2: Access services via DaemonContext
    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    uint32_t height = params[0].as<int>();
    uint32_t current_height = chainstate->getBlockHeight();

    if (height > current_height) {
        result["error"] = "Block height out of range";
        return result;
    }

    // Phase 39: Get chain database via ChainstateService (ChainManager deleted)
    auto* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        result["error"] = "Chain database not available";
        return result;
    }

    std::string block_hash = dinero::storage::GetBlockHash(chain_db, height);
    if (block_hash.empty()) {
        result["error"] = "Block not found";
        return result;
    }

    result = block_hash;
    return result;
}

/**
 * blockchain.getblock - Get block by hash
 *
 * Demonstrates accessing ChainDB through ChainstateService
 */
din::Json rpc_context_getblock(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: getblock <hash>";
        return result;
    }

    // Week 2: Service access via context
    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    // Phase 39: Get chain database via ChainstateService (ChainManager deleted)
    auto* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        result["error"] = "Chain database not available";
        return result;
    }

    std::string block_hash = params[0].as<std::string>();
    uint256 block_hash_uint256 = uint256::FromHexUnsafe(block_hash);  // Phase M.0: Convert hex to uint256

    auto block_result = chainstate->getBlockByHash(block_hash_uint256);
    if (block_result.status() != dinero::Status::Ok) {
        result["error"] = "Block not found";
        return result;
    }

    const dinero::Block& block = block_result.value();

    // Verbosity 0: return raw hex (like Bitcoin Core getblock <hash> 0)
    int verbosity = 1;
    if (params.size() >= 2 && params[1].is<int>()) {
        verbosity = params[1].as<int>();
    }
    if (verbosity == 0) {
        std::string binary = block.Serialize();
        std::ostringstream hex_stream;
        for (unsigned char c : binary) {
            hex_stream << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(c);
        }
        return din::Json(hex_stream.str());
    }

    auto height_result = chain_db->getBlockHeight(block_hash_uint256);
    uint32_t height = (height_result.status() == dinero::Status::Ok) ? height_result.value() : 0;

    result["hash"] = block_hash;
    result["height"] = static_cast<int>(height);
    result["version"] = static_cast<int>(block.header.version);
    result["previousblockhash"] = block.header.prev_block_hash.GetHex();  // Consensus→RPC: hex encode
    result["merkleroot"] = block.header.merkle_root.GetHex();  // Consensus→RPC: hex encode
    result["time"] = static_cast<Json::UInt64>(block.header.timestamp);
    result["bits"] = static_cast<Json::UInt64>(block.header.difficulty);
    result["nonce"] = static_cast<Json::UInt64>(block.header.nonce);

    // Backward-compat field: display-order uint256 hex.
    if (!block.header.utreexo_root.IsNull()) {
        result["utreexocommitment"] = block.header.utreexo_root.GetHex();
        // Explicit raw byte order matching header bytes 68..99, proof bundles,
        // and blockchain.getutreexocommitment.
        result["utreexocommitment_raw"] = UtreexoRootRawHex(block.header.utreexo_root);
    }

    din::Json tx_array = din::arr();
    for (const auto& tx : block.vtx) {
        tx_array.append(tx.GetTxid().AsUint256().GetHex());  // Consensus→RPC: TxId to hex
    }
    result["tx"] = tx_array;
    result["nTx"] = static_cast<int>(block.vtx.size());

    return result;
}

/**
 * blockchain.getinfo - Get comprehensive blockchain info
 *
 * Demonstrates multi-service access (chainstate, p2p, mempool)
 */
din::Json rpc_context_getblockchaininfo(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Week 2: Access multiple services via context
    if (!ctx.daemon) {
        result["error"] = "DaemonContext not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    // Phase 39: Get chain database via ChainstateService (ChainManager deleted)
    auto* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        result["error"] = "Chain database not available";
        return result;
    }

    // Get actual chain name from chain params (mainnet/testnet/regtest)
    result["chain"] = dinero::Params().name;
    uint32_t height = chainstate->getBlockHeight();
    const auto peer_sync = CollectPeerSyncTelemetry(ctx);
    result["blocks"] = static_cast<int>(height);

    // Header height: report the highest locally known selector/header-sync view.
    uint32_t header_height = height;
    if (ctx.daemon->header_chain) {
        if (const auto* best = ctx.daemon->header_chain->GetBestHeader()) {
            header_height = std::max(header_height, best->height);
        }
    }
    if (ctx.daemon->header_sync) {
        auto stats = ctx.daemon->header_sync->GetStats();
        header_height = std::max(
            header_height,
            std::max(stats.peer_best_height, peer_sync.advertised_best_height));
    }
    result["headers"] = static_cast<int>(header_height);

    result["bestblockhash"] = chainstate->getBestBlockHash();
    result["difficulty"] = dinero::storage::GetDifficulty(chain_db, height + 1);
    result["mediantime"] = static_cast<int>(std::time(nullptr));

    // Verification progress: ratio of validated blocks to best known header height
    double verification_progress = 1.0;
    if (header_height > 0) {
        verification_progress = static_cast<double>(height) / static_cast<double>(header_height);
        if (verification_progress > 1.0) verification_progress = 1.0;
    }
    result["verificationprogress"] = verification_progress;

    // IBD: use ChainstateService::IsInIBD() for actual state
    result["initialblockdownload"] = chainstate->IsInIBD();
    // Chainwork: prefer active_tip_ (authoritative in AssumeUTXO mode where
    // chaindb tip lags at genesis while the UTXO set is at snapshot height).
    {
        std::string work_hex;
        const dinero::CBlockIndex* tip_idx = chainstate->GetActiveTip();
        if (tip_idx && !tip_idx->chainwork.empty()) {
            work_hex = tip_idx->chainwork;
        } else {
            auto tip_result = chain_db->getTip();
            if (tip_result.ok()) {
                work_hex = tip_result.value().work.GetHex();
            }
        }
        result["chainwork"] = work_hex.empty()
            ? "0x0000000000000000000000000000000000000000000000000000000000000000"
            : "0x" + work_hex;
    }
    result["size_on_disk"] = 0;

    // Phase P.2: Pruning stats (wired to PruneService)
    bool is_pruned = false;
    uint32_t prune_height = 0;
    uint64_t saved_space_mb = 0;

    if (ctx.daemon->prune) {
        auto prune_stats = ctx.daemon->prune->getStats();
        is_pruned = ctx.daemon->prune->isEnabled();
        prune_height = prune_stats.lowest_block_height;
        saved_space_mb = prune_stats.bytes_pruned / (1024 * 1024);
    }
    result["pruned"] = is_pruned;

    // If pruned, add pruning stats (Bitcoin Core compatible)
    if (is_pruned) {
        result["pruneheight"] = static_cast<int>(prune_height);
        result["prune_target_size"] = 0;  // Target size in bytes (0 = keep N blocks mode)
    }

    // Additional pruning info
    if (ctx.daemon->prune && is_pruned) {
        auto config = ctx.daemon->prune->getConfig();
        result["keep_blocks"] = static_cast<int>(config.keep_blocks);
        result["automatic_pruning"] = config.auto_prune;
    }

    result["moneysupply"] = formatDIN(dinero::storage::GetTotalCoinsMined(chain_db));

    // SegWit is active from genesis
    din::Json softforks;
    softforks["csv"]["type"] = "buried";
    softforks["csv"]["active"] = true;
    softforks["csv"]["height"] = 0;
    softforks["segwit"]["type"] = "buried";
    softforks["segwit"]["active"] = true;
    softforks["segwit"]["height"] = 0;
    result["softforks"] = softforks;

    return result;
}

/**
 * blockchain.getsynchealth - Operator-focused sync diagnostics
 *
 * Returns a truthful view of the local active chain, header selector,
 * persisted header store, peer advertisements, and body-download scheduler.
 */
din::Json rpc_context_getsynchealth(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon) {
        result["error"] = "DaemonContext not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    result["chain"] = dinero::Params().name;

    const uint32_t active_height = chainstate->getBlockHeight();
    const auto peer_sync = CollectPeerSyncTelemetry(ctx);
    result["active_height"] = static_cast<int>(active_height);
    result["active_best_hash"] = chainstate->getBestBlockHash();
    result["initialblockdownload"] = chainstate->IsInIBD();

    if (const auto* chain_db = chainstate->GetChainDB()) {
        auto tip_result = chain_db->getTip();
        if (tip_result.status() == dinero::Status::Ok) {
            result["chaindb_tip_height"] = tip_result.value().height;
            result["chaindb_tip_hash"] = tip_result.value().hash.GetHex();
        } else {
            result["chaindb_tip_height"] = 0;
            result["chaindb_tip_hash"] = "";
            result["chaindb_tip_status"] = static_cast<int>(tip_result.status());
        }

        const auto checkpoint_result = chain_db->getLatestUtreexoCheckpoint();
        const bool checkpoint_found = checkpoint_result.status() == dinero::Status::Ok;
        result["latest_utreexo_checkpoint_found"] = checkpoint_found;
        if (checkpoint_found) {
            const int checkpoint_height = checkpoint_result.value().first;
            result["latest_utreexo_checkpoint_height"] = checkpoint_height;
            auto checksum_result = chain_db->getUtreexoChecksum(checkpoint_height);
            result["latest_utreexo_checkpoint_has_checksum"] =
                checksum_result.status() == dinero::Status::Ok;
            if (checksum_result.status() != dinero::Status::Ok &&
                checksum_result.status() != dinero::Status::NotFound) {
                result["latest_utreexo_checkpoint_checksum_status"] =
                    static_cast<int>(checksum_result.status());
            }
        }

        auto checksum_version = chain_db->getUtreexoMeta("CHECKSUM_VERSION");
        if (checksum_version.status() == dinero::Status::Ok) {
            result["utreexo_checksum_version"] = checksum_version.value();
        } else {
            result["utreexo_checksum_version"] = "";
        }

        const auto marker_result = chain_db->getForestTipMarker();
        const bool marker_found = marker_result.status() == dinero::Status::Ok;
        result["forest_tip_marker_found"] = marker_found;
        if (marker_found) {
            result["forest_tip_marker_height"] = marker_result.value().height;
            result["forest_tip_marker_hash"] = marker_result.value().block_hash.GetHex();
            result["forest_tip_marker_root"] = marker_result.value().forest_root.GetHex();
        } else if (marker_result.status() != dinero::Status::NotFound) {
            result["forest_tip_marker_status"] = static_cast<int>(marker_result.status());
        }

        const auto shielded_marker_result = chain_db->getShieldedTipMarker();
        const bool shielded_marker_found = shielded_marker_result.status() == dinero::Status::Ok;
        result["shielded_tip_marker_found"] = shielded_marker_found;
        if (shielded_marker_found) {
            result["shielded_tip_marker_height"] = shielded_marker_result.value().height;
            result["shielded_tip_marker_hash"] = shielded_marker_result.value().block_hash.GetHex();
            result["shielded_tip_marker_root"] = shielded_marker_result.value().shielded_root.GetHex();
            result["shielded_tip_marker_tree_size"] =
                static_cast<Json::UInt64>(shielded_marker_result.value().tree_size);
            result["shielded_tip_marker_nullifier_count"] =
                static_cast<Json::UInt64>(shielded_marker_result.value().nullifier_count);
        } else if (shielded_marker_result.status() != dinero::Status::NotFound) {
            result["shielded_tip_marker_status"] =
                static_cast<int>(shielded_marker_result.status());
        }
    }

    if (chainstate) {
        if (const auto* tree = chainstate->GetShieldedCommitmentTree()) {
            const auto root = tree->Root();
            uint256 root_u256;
            std::memcpy(root_u256.data, root.data(), root.size());
            result["shielded_frontier_root"] = root_u256.GetHex();
            result["shielded_tree_size"] = static_cast<Json::UInt64>(tree->Size());
        }
        if (const auto* nullifiers = chainstate->GetShieldedNullifierSet()) {
            result["shielded_nullifier_count"] = static_cast<Json::UInt64>(nullifiers->Size());
        }
    }

    std::string alignment_reason;
    const bool aligned = chainstate->IsCanonicalStateAligned(&alignment_reason);
    result["canonical_state_aligned"] = aligned;
    result["canonical_alignment_reason"] = aligned ? "" : alignment_reason;

    din::Json selector;
    selector["available"] = static_cast<bool>(ctx.daemon->header_chain);
    if (ctx.daemon->header_chain) {
        selector["header_count"] = static_cast<Json::UInt64>(ctx.daemon->header_chain->GetHeaderCount());
        if (const auto* best = ctx.daemon->header_chain->GetBestHeader()) {
            selector["best_height"] = static_cast<int>(best->height);
            selector["best_hash"] = best->hash.GetHex();
        } else {
            selector["best_height"] = 0;
            selector["best_hash"] = "";
        }
    }
    result["header_selector"] = selector;

    din::Json store;
    store["available"] = static_cast<bool>(ctx.daemon->header_store);
    if (ctx.daemon->header_store) {
        auto header_store = ctx.daemon->header_store;
        store["persisted_header_count"] = static_cast<Json::UInt64>(header_store->GetHeaderCount());
        store["legacy_entries_detected"] = header_store->HasLegacyEntries();
        store["schema_metadata_present"] = header_store->HasSchemaMetadata();
        store["schema_compatible"] = header_store->IsSchemaCompatible();
        store["schema_recovery_required"] = header_store->NeedsSchemaRecovery();
        store["schema_recovery_reason"] = header_store->GetSchemaRecoveryReason();

        const auto expected_schema = header_store->GetExpectedSchemaMetadata();
        store["current_schema_version"] = static_cast<int>(expected_schema.version);
        store["current_network"] = expected_schema.network;
        store["current_header_size"] = static_cast<int>(expected_schema.header_size);

        if (const auto persisted_schema = header_store->GetPersistedSchemaMetadata()) {
            store["persisted_schema_version"] = static_cast<int>(persisted_schema->version);
            store["persisted_network"] = persisted_schema->network;
            store["persisted_header_size"] = static_cast<int>(persisted_schema->header_size);
        } else {
            store["persisted_schema_version"] = 0;
            store["persisted_network"] = "";
            store["persisted_header_size"] = 0;
        }

        uint256 persisted_best_hash;
        if (header_store->LoadBestHeader(persisted_best_hash)) {
            store["persisted_best_hash"] = persisted_best_hash.GetHex();

            dinero::consensus::HeaderIndexEntry persisted_best_entry;
            if (header_store->LoadHeader(persisted_best_hash, persisted_best_entry)) {
                store["persisted_best_height"] = static_cast<int>(persisted_best_entry.height);
            }
        } else {
            store["persisted_best_hash"] = "";
        }
    }
    result["header_store"] = store;

    din::Json peers_json;
    peers_json["connected"] = 0;
    peers_json["peer_advertised_best_height"] = static_cast<int>(active_height);
    peers_json["max_synced_headers"] = 0;
    peers_json["max_synced_blocks"] = 0;
    peers_json["connected"] = static_cast<Json::UInt64>(peer_sync.connected);
    peers_json["peer_advertised_best_height"] = static_cast<int>(
        std::max(active_height, peer_sync.advertised_best_height));
    peers_json["max_synced_headers"] = static_cast<int>(peer_sync.max_synced_headers);
    peers_json["max_synced_blocks"] = static_cast<int>(peer_sync.max_synced_blocks);
    result["peers"] = peers_json;

    din::Json header_sync;
    header_sync["available"] = static_cast<bool>(ctx.daemon->header_sync);
    if (ctx.daemon->header_sync) {
        const auto stats = ctx.daemon->header_sync->GetStats();
        const uint32_t effective_peer_best_height =
            std::max(stats.peer_best_height, peer_sync.advertised_best_height);
        const auto effective_state = EffectiveHeaderSyncState(stats, effective_peer_best_height);
        const uint32_t effective_headers_behind =
            (effective_peer_best_height > stats.local_best_height)
                ? (effective_peer_best_height - stats.local_best_height)
                : 0;

        header_sync["state"] = headerSyncStateToString(effective_state);
        header_sync["local_best_height"] = static_cast<int>(stats.local_best_height);
        header_sync["peer_best_height"] = static_cast<int>(effective_peer_best_height);
        header_sync["headers_behind"] = static_cast<int>(effective_headers_behind);
        header_sync["active_peers"] = static_cast<int>(stats.active_peers);
        header_sync["stalled_peers"] = static_cast<int>(stats.stalled_peers);
        header_sync["current_sync_peer"] = static_cast<Json::UInt64>(stats.current_sync_peer);
    }
    result["header_sync"] = header_sync;

    din::Json block_download;
    block_download["available"] = static_cast<bool>(ctx.daemon->block_download);
    if (ctx.daemon->block_download) {
        const size_t in_flight = ctx.daemon->block_download->GetInFlightCount();
        const size_t queued = ctx.daemon->block_download->GetQueuedBlockCount();
        const size_t queued_not_in_flight = queued > in_flight ? (queued - in_flight) : 0;

        block_download["local_tip_height"] = static_cast<int>(ctx.daemon->block_download->GetLocalTipHeight());
        block_download["headers_synced"] = ctx.daemon->block_download->HeadersSynced();
        block_download["send_getdata_ready"] = ctx.daemon->block_download->HasSendGetDataCallback();
        block_download["missing"] = static_cast<Json::UInt64>(ctx.daemon->block_download->GetMissingBlockCount());
        block_download["in_flight"] = static_cast<Json::UInt64>(in_flight);
        block_download["queued"] = static_cast<Json::UInt64>(queued);
        block_download["queued_not_in_flight"] = static_cast<Json::UInt64>(queued_not_in_flight);
        block_download["is_fully_synchronized"] = ctx.daemon->block_download->IsFullySynchronized();
        block_download["phase"] = syncPhaseToString(ctx.daemon->block_download->GetCurrentPhase());
    }
    result["block_download"] = block_download;

    return result;
}

/**
 * blockchain.auditundometadata - Dry-run/apply repair for legacy missing
 * BLOCK_HAVE_UNDO metadata flags when the referenced undo bytes are still
 * readable and structurally match the active-chain block body.
 */
din::Json rpc_context_auditundometadata(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    uint32_t max_blocks = 1024;
    bool apply = false;
    bool include_ok = false;
    if (!params.empty()) {
        if (params[0].isObject()) {
            const auto& opts = params[0];
            if (opts.isMember("max_blocks")) {
                max_blocks = static_cast<uint32_t>(std::max(0, opts["max_blocks"].asInt()));
            }
            if (opts.isMember("apply")) {
                apply = opts["apply"].asBool();
            }
            if (opts.isMember("include_ok")) {
                include_ok = opts["include_ok"].asBool();
            }
        } else {
            if (params.size() > 0 && params[0].isInt()) {
                max_blocks = static_cast<uint32_t>(std::max(0, params[0].asInt()));
            }
            if (params.size() > 1 && params[1].isBool()) {
                apply = params[1].asBool();
            }
            if (params.size() > 2 && params[2].isBool()) {
                include_ok = params[2].asBool();
            }
        }
    }

    const auto report = chainstate->AuditUndoMetadataForRestamp(max_blocks, apply, include_ok);
    result["apply"] = report.apply;
    result["scanned"] = static_cast<int>(report.scanned);
    result["restampable"] = static_cast<int>(report.restampable);
    result["repaired"] = static_cast<int>(report.repaired);
    result["failed"] = static_cast<int>(report.failed);

    din::Json entries = din::arr();
    for (const auto& e : report.entries) {
        din::Json item;
        item["height"] = static_cast<int>(e.height);
        item["hash"] = e.hash.GetHex();
        item["status_flags"] = static_cast<Json::UInt64>(e.status_flags);
        item["undo_file"] = static_cast<Json::UInt64>(e.undo_file);
        item["undo_pos"] = static_cast<Json::UInt64>(e.undo_pos);
        item["undo_size"] = static_cast<Json::UInt64>(e.undo_size);
        item["has_undo_flag"] = e.has_undo_flag;
        item["undo_readable"] = e.undo_readable;
        item["undo_decodable"] = e.undo_decodable;
        item["block_readable"] = e.block_readable;
        item["restampable"] = e.restampable;
        item["repaired"] = e.repaired;
        item["reason"] = e.reason;
        entries.append(item);
    }
    result["entries"] = entries;
    return result;
}

/**
 * blockchain.debugclearundoflag - regtest-only fixture helper.
 */
din::Json rpc_context_debugclearundoflag(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (dinero::Params().network_id != "regtest") {
        result["error"] = "debugclearundoflag is regtest-only";
        result["code"] = -32601;
        return result;
    }
    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: blockchain.debugclearundoflag <blockhash>";
        result["code"] = -32602;
        return result;
    }
    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    const std::string blockhash = params[0].as<std::string>();
    if (blockhash.size() != 64) {
        result["error"] = "Invalid blockhash: must be 64 hex characters";
        result["code"] = -32602;
        return result;
    }

    std::string error;
    if (!chainstate->DebugClearUndoFlagForBlock(uint256::FromHexUnsafe(blockhash), error)) {
        result["error"] = error;
        result["code"] = -32603;
        return result;
    }

    result["cleared"] = true;
    result["hash"] = blockhash;
    return result;
}

/**
 * blockchain.getarchivalstatus - Report whether local block bodies are replayable
 *
 * This is the operator-facing truth source for archival health:
 * - flatfile coverage on the active chain
 * - readable coverage including legacy ChainDB fallback
 * - whether genesis-to-tip local replay is currently possible from blk*.dat
 */
din::Json rpc_context_getarchivalstatus(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    auto* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        result["error"] = "Chain database not available";
        return result;
    }

    const uint32_t active_height = chainstate->getBlockHeight();
    const ArchivalCoverageSummary coverage = ComputeArchivalCoverage(*chainstate, *chain_db, active_height);

    bool archival_mode_configured = true;
    if (ctx.daemon->config) {
        auto config = std::dynamic_pointer_cast<dinero::ConfigService>(ctx.daemon->config);
        if (config) {
            archival_mode_configured = config->GetBool("storage.archival", true);
        }
    }

    bool pruned = false;
    uint32_t prune_height = 0;
    if (ctx.daemon->prune) {
        pruned = ctx.daemon->prune->isEnabled();
        if (pruned) {
            prune_height = ctx.daemon->prune->getStats().lowest_block_height;
        }
    }

    const bool full_replay_possible =
        coverage.expected_body_count > 0 &&
        coverage.flatfile_body_count == coverage.expected_body_count &&
        coverage.first_missing_flatfile_body_height < 0;

    result["chain"] = dinero::Params().name;
    result["active_height"] = static_cast<int>(active_height);
    result["bestblockhash"] = chainstate->getBestBlockHash();
    result["archival_mode_configured"] = archival_mode_configured;
    result["strict_flatfile_reads_enabled"] = chainstate->strictArchivalReadsEnabled();
    result["legacy_fallback_allowed"] = !chainstate->strictArchivalReadsEnabled();
    result["pruned"] = pruned;
    if (pruned) {
        result["pruneheight"] = static_cast<int>(prune_height);
    }

    result["replay_source"] = "flatfiles";
    result["expected_body_count"] = static_cast<Json::UInt64>(coverage.expected_body_count);
    result["readable_body_count"] = static_cast<Json::UInt64>(coverage.readable_body_count);
    result["flatfile_body_count"] = static_cast<Json::UInt64>(coverage.flatfile_body_count);
    result["legacy_fallback_body_count"] = static_cast<Json::UInt64>(coverage.legacy_fallback_body_count);
    result["runtime_legacy_body_fallback_reads"] =
        static_cast<Json::UInt64>(chainstate->getLegacyBodyFallbackReadCount());
    result["runtime_legacy_undo_fallback_reads"] =
        static_cast<Json::UInt64>(chainstate->getLegacyUndoFallbackReadCount());
    result["genesis_readable_body_present"] = coverage.genesis_readable_body_present;
    result["genesis_flatfile_body_present"] = coverage.genesis_flatfile_body_present;
    result["tip_readable_body_present"] = coverage.tip_readable_body_present;
    result["tip_flatfile_body_present"] = coverage.tip_flatfile_body_present;
    result["full_replay_possible"] = full_replay_possible;

    if (coverage.lowest_readable_body_height >= 0) {
        result["lowest_readable_body_height"] = coverage.lowest_readable_body_height;
    } else {
        result["lowest_readable_body_height"] = Json::nullValue;
    }
    if (coverage.lowest_flatfile_body_height >= 0) {
        result["lowest_flatfile_body_height"] = coverage.lowest_flatfile_body_height;
    } else {
        result["lowest_flatfile_body_height"] = Json::nullValue;
    }
    if (coverage.highest_contiguous_flatfile_body_height >= 0) {
        result["highest_contiguous_flatfile_body_height"] = coverage.highest_contiguous_flatfile_body_height;
    } else {
        result["highest_contiguous_flatfile_body_height"] = Json::nullValue;
    }
    if (coverage.first_missing_readable_body_height >= 0) {
        result["first_missing_readable_body_height"] = coverage.first_missing_readable_body_height;
    } else {
        result["first_missing_readable_body_height"] = Json::nullValue;
    }
    if (coverage.first_missing_flatfile_body_height >= 0) {
        result["first_missing_flatfile_body_height"] = coverage.first_missing_flatfile_body_height;
    } else {
        result["first_missing_flatfile_body_height"] = Json::nullValue;
    }

    din::Json notes = din::arr();
    if (pruned) {
        notes.append("Pruning is enabled, so full genesis-to-tip flatfile replay is not expected.");
    }
    if (coverage.legacy_fallback_body_count > 0) {
        notes.append("Some active-chain blocks are readable only through legacy ChainDB fallback and are not yet guaranteed replayable from blk*.dat alone.");
    }
    if (!full_replay_possible && coverage.first_missing_flatfile_body_height >= 0) {
        notes.append("Flatfile replay gap detected on the active chain.");
    }
    result["notes"] = notes;

    return result;
}

/**
 * blockchain.getinfo - Simplified blockchain info for GUI
 * 
 * Returns: { "blocks": N, "headers": N }
 * Used by GUI for sync progress display
 */
din::Json rpc_context_getinfo(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    uint32_t height = chainstate->getBlockHeight();

    // For now, blocks == headers (no headers-first sync yet)
    // When we add headers-first sync, query chain_db->getHeaderCount()
    result["blocks"] = static_cast<int>(height);
    result["headers"] = static_cast<int>(height);

    // Add chain type for GUI network display (mainnet/testnet/regtest)
    if (ctx.daemon->config) {
        auto config = std::dynamic_pointer_cast<dinero::ConfigService>(ctx.daemon->config);
        if (config) {
            if (config->IsRegtest()) {
                result["chain"] = "regtest";
            } else if (config->IsTestnet()) {
                result["chain"] = "testnet";
            } else {
                result["chain"] = "main";
            }
        }
    }

    return result;
}

/**
 * blockchain.getbestblockhash - Get hash of the best (tip) block
 *
 * NEW: ctx.daemon->chainstate->getBestBlockHash()
 */
din::Json rpc_context_getbestblockhash(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    result = chainstate->getBestBlockHash();
    return result;
}

/**
 * Phase P.2: blockchain.pruneblockchain - Prune blocks up to target height
 *
 * Parameters:
 *   - height (uint32_t): Prune all eligible blocks below this height
 *
 * Returns:
 *   {
 *     "blocks_pruned": N,
 *     "bytes_recovered": N,
 *     "lowest_block_height": N,
 *     "size_on_disk_mb": N
 *   }
 */
din::Json rpc_pruneblockchain(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Validate parameters
    if (params.empty() || !params[0].is<int>()) {
        result["error"] = "Height parameter required (integer)";
        return result;
    }

    uint32_t target_height = static_cast<uint32_t>(params[0].as<int>());

    // Access PruneService through context
    if (!ctx.daemon || !ctx.daemon->prune) {
        result["error"]["code"] = -32603;
        result["error"]["message"] = "PruneService not available";
        return result;
    }

    // Check if pruning is enabled
    if (!ctx.daemon->prune->isEnabled()) {
        result["error"]["code"] = -1;
        result["error"]["message"] = "Pruning is disabled. Start with -prune=<N> to enable.";
        return result;
    }

    // Execute pruning
    auto prune_result = ctx.daemon->prune->pruneToHeight(target_height);

    if (!prune_result.success()) {
        result["error"]["code"] = -1;
        result["error"]["message"] = "Pruning failed";
        din::Json errors(Json::arrayValue);
        for (const auto& err : prune_result.errors) {
            errors.append(err);
        }
        result["errors"] = errors;
        return result;
    }

    result["blocks_pruned"] = static_cast<int>(prune_result.blocks_pruned);
    result["bytes_recovered"] = Json::Int64(prune_result.bytes_recovered);
    result["lowest_block_height"] = static_cast<int>(target_height);
    // result["size_on_disk_mb"] = static_cast<int>(prune_service->getDiskUsage() / (1024 * 1024));

    return result;
}

/**
 * blockchain.getdifficulty - Get current network difficulty
 *
 * NEW: ctx.daemon->chainstate->chainDB()->...
 */
din::Json rpc_context_getdifficulty(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    // Phase 39: Get chain database via ChainstateService (ChainManager deleted)
    auto* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        result["error"] = "Chain database not available";
        return result;
    }

    uint32_t height = chainstate->getBlockHeight();
    double difficulty = dinero::storage::GetDifficulty(chain_db, height + 1);
    result = difficulty;
    return result;
}

/**
 * blockchain.getblockheader - Get block header by hash
 *
 * NEW: ctx.daemon->chainstate->chainDB()->...
 */
din::Json rpc_context_getblockheader(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: getblockheader <hash>";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    // Phase 39: Get chain database via ChainstateService (ChainManager deleted)
    auto* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        result["error"] = "Chain database not available";
        return result;
    }

    std::string block_hash = params[0].as<std::string>();
    uint256 block_hash_uint256 = uint256::FromHexUnsafe(block_hash);  // Phase M.0: Convert hex to uint256

    auto block_result = chainstate->getBlockByHash(block_hash_uint256);
    if (block_result.status() != dinero::Status::Ok) {
        result["error"] = "Block not found";
        return result;
    }

    const dinero::Block& block = block_result.value();
    auto height_result = chain_db->getBlockHeight(block_hash_uint256);
    uint32_t height = (height_result.status() == dinero::Status::Ok) ? height_result.value() : 0;

    result["hash"] = block_hash;
    result["height"] = static_cast<int>(height);
    result["version"] = static_cast<int>(block.header.version);
    result["previousblockhash"] = block.header.prev_block_hash.GetHex();  // Consensus→RPC
    result["merkleroot"] = block.header.merkle_root.GetHex();  // Consensus→RPC
    result["time"] = static_cast<Json::UInt64>(block.header.timestamp);
    result["bits"] = static_cast<Json::UInt64>(block.header.difficulty);
    result["nonce"] = static_cast<Json::UInt64>(block.header.nonce);

    // Backward-compat field: display-order uint256 hex.
    // Always include even if null - light clients need to verify field exists.
    result["utreexo_root"] = block.header.utreexo_root.GetHex();
    // Explicit raw byte order matching header bytes 68..99, proof bundles,
    // and blockchain.getutreexocommitment.
    result["utreexo_root_raw"] = UtreexoRootRawHex(block.header.utreexo_root);

    // Chainwork (cumulative proof-of-work at this block)
    auto work_result = chain_db->getBlockWork(block_hash_uint256);
    if (work_result.status() == dinero::Status::Ok) {
        result["chainwork"] = "0x" + work_result.value().GetHex();
    }

    if (auto* block_index = chainstate->FindBlockIndex(block_hash_uint256)) {
        const uint32_t status = block_index->status;
        result["status_flags"] = static_cast<Json::UInt64>(status);
        result["failed_valid"] = (status & dinero::BLOCK_FAILED_VALID) != 0;
        result["failed_child"] = (status & dinero::BLOCK_FAILED_CHILD) != 0;
    }

    return result;
}

/**
 * blockchain.getmininginfo - Get mining statistics
 *
 * NEW: ctx.daemon->chainstate->chainDB()->...
 */
din::Json rpc_context_getmininginfo(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    // Phase 39: Get chain database via ChainstateService (ChainManager deleted)
    auto* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        result["error"] = "Chain database not available";
        return result;
    }

    try {
        uint32_t height = chainstate->getBlockHeight();
        uint32_t next_height = height + 1;
        const auto& chainparams = dinero::Params();
        const double difficulty = dinero::storage::GetDifficulty(chain_db, next_height);
        const double target_spacing =
            chainparams.target_spacing > 0 ? static_cast<double>(chainparams.target_spacing) : 300.0;
        const double network_hashrate = std::max(0.0, difficulty * 4294967296.0 / target_spacing);

        result["blocks"] = static_cast<int>(height);
        result["currentblocksize"] = 0;
        result["currentblocktx"] = 0;
        result["difficulty"] = difficulty;
        result["networkhashps"] = network_hashrate;
        result["pooledtx"] = 0;
        result["chain"] = "main";
        result["testnet"] = false;
        result["generate"] = false;
        result["genproclimit"] = -1;
        result["hashespersec"] = network_hashrate;

        // Phase M.6.2: Extract raw value from AmountUna
        uint64_t reward = dinero::ConsensusSubsidy::GetBlockSubsidy(next_height).GetUna();
        result["blocksubsidy"] = formatDIN(reward);

    } catch (const std::exception& e) {
        result["error"] = std::string("getmininginfo error: ") + e.what();
    }

    return result;
}

/**
 * blockchain.submitblock - Submit mined block
 *
 * NEW: Uses BlockAcceptor (which already uses context-aware services)
 */
din::Json rpc_context_submitblock(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Missing block hex data";
        return result;
    }

    std::string block_hex = params[0].as<std::string>();

    try {
        auto accept_result = dinero::BlockAcceptor::AcceptBlockFromRPC(block_hex, "rpc");

        if (accept_result.accepted()) {
            std::string hash_hex = accept_result.block_hash.GetHex();
            dinero::g_logger.info("Block accepted at height " + std::to_string(accept_result.height) +
                                ", hash " + hash_hex.substr(0, 16) + "...");
            return din::null();
        } else {
            result["error"] = accept_result.reason;
            result["code"] = std::string(dinero::BlockRejectCodeToString(accept_result.code));
        }
    } catch (const std::exception& e) {
        result["error"] = std::string("Block acceptance error: ") + e.what();
    }

    return result;
}

/**
 * blockchain.invalidateblock - Mark a block as invalid and disconnect if in active chain
 *
 * Works on all networks. Marks the block and all descendants as invalid,
 * disconnects them from the active chain if necessary, and re-evaluates
 * the best chain (may switch to a fork).
 */
din::Json rpc_context_invalidateblock(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: invalidateblock <blockhash>";
        result["code"] = -32602;
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    std::string blockhash = params[0].as<std::string>();
    if (blockhash.length() != 64) {
        result["error"] = "Invalid blockhash: must be 64 hex characters";
        result["code"] = -32602;
        return result;
    }

    uint32_t old_height = chainstate->getBlockHeight();
    uint256 hash = uint256::FromHexUnsafe(blockhash);

    std::string error;
    if (!chainstate->InvalidateBlock(hash, error)) {
        result["error"] = "invalidateblock failed: " + error;
        result["code"] = -32603;
        return result;
    }

    uint32_t new_height = chainstate->getBlockHeight();
    result["old_height"] = static_cast<int>(old_height);
    result["new_height"] = static_cast<int>(new_height);
    result["invalidated"] = blockhash;
    result["new_tip"] = chainstate->getBestBlockHash();
    return result;
}

/**
 * blockchain.reconsiderblock - Clear invalid flags and re-evaluate chain
 *
 * Clears BLOCK_FAILED_VALID / BLOCK_FAILED_CHILD from the specified block
 * and all its descendants, then re-evaluates the best chain. If the
 * reconsidered branch has more work, a reorg will occur.
 */
din::Json rpc_context_reconsiderblock(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: reconsiderblock <blockhash>";
        result["code"] = -32602;
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    std::string blockhash = params[0].as<std::string>();
    if (blockhash.length() != 64) {
        result["error"] = "Invalid blockhash: must be 64 hex characters";
        result["code"] = -32602;
        return result;
    }

    uint32_t old_height = chainstate->getBlockHeight();
    uint256 hash = uint256::FromHexUnsafe(blockhash);

    std::string error;
    if (!chainstate->ReconsiderBlock(hash, error)) {
        result["error"] = "reconsiderblock failed: " + error;
        result["code"] = -32603;
        return result;
    }

    uint32_t new_height = chainstate->getBlockHeight();
    result["old_height"] = static_cast<int>(old_height);
    result["new_height"] = static_cast<int>(new_height);
    result["reconsidered"] = blockhash;
    result["new_tip"] = chainstate->getBestBlockHash();
    return result;
}

/**
 * utreexo.dumpforestinternal - Diagnostic-only forest internal-state dump.
 *
 * Writes the full UtreexoForest internal state (numLeaves, roots[],
 * nodes_, deleted_positions_, leaf_positions_, canonical_empty_roots_)
 * to a file for off-line diff. Used by the height-9290 paired-snapshot
 * comparison plan: getCommitment() folds nullopt and optional(ZERO_HASH)
 * to identical bytes, so two forests can match commitments while differing
 * internally; this RPC exposes the difference.
 *
 * Params: [output_path] (string, required)
 */
din::Json rpc_context_utreexo_dumpforestinternal(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: utreexo.dumpforestinternal <output_path>";
        result["code"] = -32602;
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    auto* utxo_set = chainstate->GetConsensusUTXOSet();
    if (!utxo_set) {
        result["error"] = "Consensus UTXO set unavailable";
        return result;
    }

    const std::string output_path = params[0].as<std::string>();
    const auto dump = utxo_set->GetForest().dumpInternalState();

    std::ofstream out(output_path, std::ios::trunc);
    if (!out) {
        result["error"] = "Failed to open output path: " + output_path;
        result["code"] = -32603;
        return result;
    }
    out << dump;
    if (!out.good()) {
        result["error"] = "Failed to write dump: " + output_path;
        result["code"] = -32603;
        return result;
    }
    out.close();

    result["path"] = output_path;
    result["bytes"] = static_cast<int64_t>(dump.size());
    result["numLeaves"] = static_cast<int64_t>(utxo_set->GetForest().getNumLeaves());
    result["activeLeaves"] = static_cast<int64_t>(utxo_set->GetForest().getActiveLeaves());
    result["tip_height"] = static_cast<int>(chainstate->getBlockHeight());
    return result;
}

// ═══════════════════════════════════════════════════════════════
// AssumeUTXO: Fast Sync with Snapshot
// ═══════════════════════════════════════════════════════════════

/**
 * blockchain.dumptxoutset - Export UTXO snapshot for AssumeUTXO
 *
 * Params:
 *   [0] path (string): Output file path for snapshot
 *
 * Returns:
 *   {
 *     "coins_written": <n>,
 *     "base_height": <n>,
 *     "base_hash": "<hash>",
 *     "path": "<path>",
 *     "bytes_written": <n>
 *   }
 */
static din::Json rpc_context_dumptxoutset(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        // Validate context
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Chainstate not available";
            return result;
        }

        // Parse parameters
        if (!params.isArray() || params.size() < 1 || !params[0].isString()) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Usage: blockchain.dumptxoutset <path>";
            return result;
        }

        std::string snapshot_path = params[0].asString();

        // Get ChainManager
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Failed to access chainstate service";
            return result;
        }

        // Phase 42: AssumeUTXO snapshot export (restored)
        auto export_result = chainstate->ExportSnapshot(snapshot_path);

        if (!export_result.success) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Snapshot export failed: " + export_result.error_message;
            return result;
        }

        // Return success result (use snapshot header values, not current chainstate)
        result["coins_written"] = export_result.utxos_exported;
        result["base_height"] = export_result.block_height;
        result["base_hash"] = export_result.block_hash.GetHex();
        result["path"] = snapshot_path;
        result["bytes_written"] = export_result.bytes_written;

    } catch (const std::exception& e) {
        result["error"]["code"] = -32603;
        result["error"]["message"] = std::string("dumptxoutset failed: ") + e.what();
    }

    return result;
}

/**
 * blockchain.loadtxoutset - Import UTXO snapshot (AssumeUTXO fast sync)
 *
 * Params:
 *   [0] path (string): Path to snapshot file
 *
 * Returns:
 *   {
 *     "coins_loaded": <n>,
 *     "base_height": <n>,
 *     "base_hash": "<hash>",
 *     "path": "<path>",
 *     "snapshot_valid": true/false,
 *     "snapshot_bootstrap": { ... trust-gate + validation diagnostics ... }
 *   }
 */
static din::Json rpc_context_loadtxoutset(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        // Validate context
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Chainstate not available";
            return result;
        }

        // Parse parameters
        if (!params.isArray() || params.size() < 1 || !params[0].isString()) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Usage: blockchain.loadtxoutset <path>";
            return result;
        }

        std::string snapshot_path = params[0].asString();

        // Get ChainManager
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Failed to access chainstate service";
            return result;
        }

        // Phase 42: AssumeUTXO snapshot import (restored)
        auto import_result = chainstate->LoadSnapshot(snapshot_path);

        if (!import_result.success) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Snapshot import failed: " + import_result.error_message;
            result["path"] = snapshot_path;
            result["snapshot_bootstrap"] = buildSnapshotBootstrapDiagnostics(ctx, chainstate);
            return result;
        }

        // Return success result
        result["coins_loaded"] = import_result.utxos_imported;
        result["base_height"] = import_result.block_height;
        result["base_hash"] = import_result.block_hash.GetHex();
        result["path"] = snapshot_path;
        result["snapshot_valid"] = import_result.checksum_valid;
        result["bytes_read"] = import_result.bytes_read;
        result["snapshot_bootstrap"] = buildSnapshotBootstrapDiagnostics(ctx, chainstate);

    } catch (const std::exception& e) {
        result["error"]["code"] = -32603;
        result["error"]["message"] = std::string("loadtxoutset failed: ") + e.what();
    }

    return result;
}

/**
 * blockchain.getbackgroundvalidationprogress - Get AssumeUTXO background validation progress (Phase 44)
 *
 * Params: None
 *
 * Returns:
 * {
 *   "status": "NotStarted"|"InProgress"|"Completed"|"Failed",
 *   "current_height": <current height being validated>,
 *   "target_height": <snapshot base height>,
 *   "blocks_validated": <total blocks validated>,
 *   "progress_percent": <percentage complete>,
 *   "error_message": "<error if failed>"
 * }
 */
static din::Json rpc_context_getbackgroundvalidationprogress(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -32603;
            result["error"]["message"] = "Chainstate service not available";
            return result;
        }

        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            result["error"]["code"] = -32603;
            result["error"]["message"] = "Chainstate service not available";
            return result;
        }

        auto progress = chainstate->GetBackgroundValidationProgress();

        result["status"] = backgroundValidationStatusToString(progress.status);
        result["current_height"] = progress.current_height;
        result["target_height"] = progress.target_height;
        result["blocks_validated"] = progress.blocks_validated;
        result["progress_percent"] = progress.progress_percent;
        result["error_message"] = progress.error_message;

        // Additional info
        result["assumeutxo_active"] = chainstate->IsAssumeUTXOActive();
        if (chainstate->IsAssumeUTXOActive()) {
            result["snapshot_base_block"] = chainstate->GetAssumeUTXOBaseBlock().GetHex();
            result["snapshot_base_height"] = chainstate->GetAssumeUTXOBaseHeight();
        }

    } catch (const std::exception& e) {
        result["error"]["code"] = -32603;
        result["error"]["message"] = std::string("getbackgroundvalidationprogress failed: ") + e.what();
    }

    return result;
}

/**
 * blockchain.getibdprogress - Get Initial Block Download progress (Phase 45)
 *
 * Params: None
 *
 * Returns:
 * {
 *   "status": "NotInIBD"|"InIBD"|"SnapshotBootstrap"|"IBDComplete",
 *   "local_height": <our current chain height>,
 *   "network_height": <estimated network height>,
 *   "blocks_remaining": <blocks left to sync>,
 *   "sync_percent": <percentage synced>,
 *   "snapshot_loaded": <true if bootstrapped from snapshot>,
 *   "services_ready": <true if node can serve RPC/mining/wallets>,
 *   "snapshot_bootstrap": { ... trust-gate + validation diagnostics ... }
 * }
 */
static din::Json rpc_context_getibdprogress(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -32603;
            result["error"]["message"] = "Chainstate service not available";
            return result;
        }

        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            result["error"]["code"] = -32603;
            result["error"]["message"] = "Chainstate service not available";
            return result;
        }

        auto progress = chainstate->GetIBDProgress();

        const std::string status_str = ibdStatusToString(progress.status);

        result["status"] = status_str;
        result["local_height"] = progress.local_height;
        result["network_height"] = progress.network_height;
        result["blocks_remaining"] = progress.blocks_remaining;
        result["sync_percent"] = progress.sync_percent;
        result["snapshot_loaded"] = progress.snapshot_loaded;
        result["services_ready"] = progress.services_ready;
        result["snapshot_bootstrap"] = buildSnapshotBootstrapDiagnostics(ctx, chainstate);

        if (ctx.daemon->block_download) {
            const size_t in_flight = ctx.daemon->block_download->GetInFlightCount();
            const size_t queued = ctx.daemon->block_download->GetQueuedBlockCount();
            const size_t queued_not_in_flight = (queued > in_flight) ? (queued - in_flight) : 0;

            din::Json scheduler;
            scheduler["in_flight"] = static_cast<uint64_t>(in_flight);
            scheduler["queued"] = static_cast<uint64_t>(queued);
            scheduler["queued_not_in_flight"] = static_cast<uint64_t>(queued_not_in_flight);
            scheduler["missing"] = static_cast<uint64_t>(ctx.daemon->block_download->GetMissingBlockCount());
            scheduler["is_fully_synchronized"] = ctx.daemon->block_download->IsFullySynchronized();
            result["scheduler"] = scheduler;

            dinero::g_logger.debug("RPC getibdprogress: status=" + status_str +
                                   ", in_flight=" + std::to_string(in_flight) +
                                   ", queued=" + std::to_string(queued) +
                                   ", queued_not_in_flight=" + std::to_string(queued_not_in_flight));
        } else {
            dinero::g_logger.debug("RPC getibdprogress: status=" + status_str +
                                   ", scheduler=unavailable");
        }

    } catch (const std::exception& e) {
        result["error"]["code"] = -32603;
        result["error"]["message"] = std::string("getibdprogress failed: ") + e.what();
    }

    return result;
}

/**
 * blockchain.getsnapshotbootstrapstatus - Snapshot bootstrap + trust-gate diagnostics
 *
 * Returns operator-focused status for AssumeUTXO bootstrap configuration:
 * - configured snapshot/manifest paths and presence checks
 * - trust gate mode (required/optional/disabled)
 * - transport limits and file safety checks
 * - live AssumeUTXO/background-validation state
 */
static din::Json rpc_context_getsnapshotbootstrapstatus(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    (void)params;

    try {
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -32603;
            result["error"]["message"] = "Chainstate service not available";
            return result;
        }

        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            result["error"]["code"] = -32603;
            result["error"]["message"] = "Chainstate service not available";
            return result;
        }

        const auto ibd = chainstate->GetIBDProgress();
        result["ibd_status"] = ibdStatusToString(ibd.status);
        result["ibd_local_height"] = ibd.local_height;
        result["ibd_network_height"] = ibd.network_height;
        result["ibd_blocks_remaining"] = ibd.blocks_remaining;
        result["ibd_sync_percent"] = ibd.sync_percent;
        result["services_ready"] = ibd.services_ready;
        result["snapshot_bootstrap"] = buildSnapshotBootstrapDiagnostics(ctx, chainstate);
    } catch (const std::exception& e) {
        result["error"]["code"] = -32603;
        result["error"]["message"] = std::string("getsnapshotbootstrapstatus failed: ") + e.what();
    }

    return result;
}

/**
 * blockchain.getpruninginfo - Get pruning status and statistics
 *
 * Returns comprehensive pruning state from PruneService:
 * - mode: "disabled", "manual", "auto"
 * - pruning_enabled: boolean
 * - pruned_height: lowest block height with data (prune frontier)
 * - keep_blocks: configured number of blocks to keep
 * - min_undo_depth: safety margin (288 blocks)
 * - blocks_pruned/bytes_freed: cumulative stats
 * - can_prune: whether pruning is currently possible
 */
static din::Json rpc_context_getpruninginfo(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        // Check PruneService availability
        if (!ctx.daemon || !ctx.daemon->prune) {
            result["error"]["code"] = -32603;
            result["error"]["message"] = "PruneService not available";
            return result;
        }

        auto config = ctx.daemon->prune->getConfig();
        auto stats = ctx.daemon->prune->getStats();

        // Determine mode string
        std::string mode_str = "disabled";
        if (config.enabled) {
            mode_str = config.auto_prune ? "auto" : "manual";
        }

        result["mode"] = mode_str;
        result["pruning_enabled"] = config.enabled;
        result["pruned_height"] = static_cast<int>(stats.lowest_block_height);
        result["keep_blocks"] = static_cast<int>(config.keep_blocks);
        result["min_undo_depth"] = 288;  // MIN_UNDO_DEPTH constant

        // Storage stats
        result["current_disk_usage_mb"] = static_cast<double>(stats.current_disk_usage_mb);
        result["target_size_mb"] = static_cast<double>(config.target_size_mb);
        result["blocks_pruned"] = static_cast<double>(stats.blocks_pruned);
        result["bytes_freed"] = static_cast<double>(stats.bytes_pruned);

        // Current state
        result["blocks_kept"] = static_cast<double>(stats.blocks_kept);
        result["headers_kept"] = static_cast<double>(stats.headers_kept);
        result["is_pruned"] = stats.is_pruned;

        // Pruning capability
        uint32_t tip_height = 0;
        if (ctx.daemon->chainstate) {
            auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
            if (chainstate) {
                tip_height = chainstate->getBlockHeight();
            }
        }
        // Can prune if enabled, auto_prune is on, and we have enough blocks
        bool can_prune = config.enabled && config.auto_prune &&
                         (tip_height > config.keep_blocks + 288);
        result["can_prune"] = can_prune;

        // Status message
        if (!config.enabled) {
            result["status"] = "Pruning disabled. Start with -prune=<N> to enable.";
        } else if (stats.is_pruned) {
            result["status"] = "Pruned to height " + std::to_string(stats.lowest_block_height);
        } else {
            result["status"] = "Pruning enabled, no blocks pruned yet";
        }

    } catch (const std::exception& e) {
        result["error"]["code"] = -32603;
        result["error"]["message"] = std::string("getpruninginfo failed: ") + e.what();
    }

    return result;
}

/**
 * blockchain.pruneblockchain <height> - Manually prune blockchain up to specified height (Phase 46)
 */
static din::Json rpc_context_pruneblockchain(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -32603;
            result["error"]["message"] = "Chainstate service not available";
            return result;
        }

        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            result["error"]["code"] = -32603;
            result["error"]["message"] = "Chainstate service not available";
            return result;
        }

        // Parse height parameter
        if (params.isNull() || !params.isArray() || params.size() < 1) {
            result["error"]["code"] = -32602;
            result["error"]["message"] = "Missing required parameter: height";
            return result;
        }

        uint32_t target_height = 0;

        if (params[0].isNumeric()) {
            target_height = static_cast<uint32_t>(params[0].asDouble());
        } else if (params[0].isString()) {
            try {
                target_height = std::stoul(params[0].asString());
            } catch (...) {
                result["error"]["code"] = -32602;
                result["error"]["message"] = "Invalid height parameter";
                return result;
            }
        } else {
            result["error"]["code"] = -32602;
            result["error"]["message"] = "Height must be a number";
            return result;
        }

        // Execute pruning
        uint32_t blocks_pruned = chainstate->PruneBlockchain(target_height);

        // Get updated pruning info
        auto info = chainstate->GetPruningInfo();

        result["success"] = (blocks_pruned > 0);
        result["blocks_pruned"] = blocks_pruned;
        result["pruned_height"] = info.pruned_height;
        result["current_disk_usage_mb"] = static_cast<double>(info.current_disk_usage_mb);
        result["bytes_freed"] = static_cast<double>(info.bytes_freed);

    } catch (const std::exception& e) {
        result["error"]["code"] = -32603;
        result["error"]["message"] = std::string("pruneblockchain failed: ") + e.what();
    }

    return result;
}

/**
 * GetPrevoutInfo - Helper to lookup previous output information
 *
 * Searches for a specific output from a previous transaction.
 * Tries multiple sources in order of efficiency:
 * 1. Block scan (currently the only method, but safe and reliable)
 *
 * @param chain_db Blockchain database
 * @param current_height Current chain height for search bounds
 * @param prev_txid Transaction ID of the previous transaction
 * @param vout Output index within the previous transaction
 * @param out_value [OUT] Value of the output in una (una)
 * @param out_script [OUT] scriptPubKey of the output
 * @return true if prevout was found, false otherwise
 */
template<typename ChainDBType>
static bool GetPrevoutInfo(
    dinero::ChainstateService* chainstate,
    ChainDBType* chain_db,
    dinero::BlockStorage* block_storage,
    uint32_t current_height,
    const uint256& prev_txid,
    uint32_t vout,
    int64_t& out_value,
    std::vector<uint8_t>& out_script)
{
    // Search through blocks to find the previous transaction
    for (int32_t height = current_height; height >= 0; --height) {
        std::string block_hash_str = dinero::storage::GetBlockHash(chain_db, height);
        if (block_hash_str.empty()) continue;

        uint256 block_hash = uint256::FromHexUnsafe(block_hash_str);
        auto block_result = ReadRpcBlock(
            chainstate,
            chain_db,
            block_storage,
            block_hash);
        if (block_result.status() != dinero::Status::Ok) continue;

        const dinero::Block& block = block_result.value();

        // Check each transaction in the block
        for (const auto& tx : block.vtx) {
            if (tx.GetTxid().AsUint256() == prev_txid) {  // Phase M.4: Unwrap TxId for comparison
                // Found the transaction, now get the specific output
                if (vout >= tx.vout.size()) {
                    // Invalid vout index
                    return false;
                }

                // Phase M.6.2: Extract raw value from AmountUna for RPC boundary
                out_value = tx.vout[vout].value.GetUna();
                out_script = tx.vout[vout].scriptPubKey;
                return true;
            }
        }
    }

    return false;  // Transaction not found
}

/**
 * blockchain.gettransaction - Get transaction details by TXID
 *
 * Provides chain-level truth about transaction status, location, and contents.
 * Independent of wallet state - shows exact on-chain data.
 *
 * Usage: blockchain.gettransaction <txid>
 *
 * Returns:
 * - blockhash: Hash of containing block (if confirmed)
 * - blockheight: Height of containing block (if confirmed)
 * - confirmations: Number of confirmations
 * - inputs: Array of inputs with prevout and amounts
 * - outputs: Array of outputs with addresses and amounts
 * - status: "confirmed" or "not_found"
 * - txid: Transaction ID
 */
din::Json rpc_context_gettransaction(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: blockchain.gettransaction <txid>";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    auto* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        result["error"] = "Chain database not available";
        return result;
    }

    std::string txid_str = params[0].as<std::string>();
    uint256 target_txid = uint256::FromHexUnsafe(txid_str);

    // Search through blocks to find the transaction
    uint32_t current_height = chainstate->getBlockHeight();
    bool found = false;
    uint256 found_block_hash;
    uint32_t found_block_height = 0;
    dinero::Transaction found_tx;

    // Search from newest to oldest (most recent transactions first)
    for (int32_t height = current_height; height >= 0; --height) {
        std::string block_hash_str = dinero::storage::GetBlockHash(chain_db, height);
        if (block_hash_str.empty()) continue;

        uint256 block_hash = uint256::FromHexUnsafe(block_hash_str);
        auto block_result = ReadRpcBlock(
            chainstate.get(),
            chain_db,
            ctx.daemon ? ctx.daemon->block_storage.get() : nullptr,
            block_hash);
        if (block_result.status() != dinero::Status::Ok) continue;

        const dinero::Block& block = block_result.value();

        // Check each transaction in the block
        for (const auto& tx : block.vtx) {
            if (tx.GetTxid().AsUint256() == target_txid) {  // Phase M.4: Unwrap TxId for comparison
                found = true;
                found_block_hash = block_hash;
                found_block_height = height;
                found_tx = tx;
                break;
            }
        }

        if (found) break;
    }

    if (!found) {
        result["error"] = "Transaction not found in blockchain";
        result["txid"] = txid_str;
        result["status"] = "not_found";
        return result;
    }

    // Build detailed response
    result["txid"] = txid_str;
    result["blockhash"] = found_block_hash.GetHex();
    result["blockheight"] = static_cast<int>(found_block_height);
    result["confirmations"] = static_cast<int>(current_height - found_block_height + 1);
    result["status"] = "confirmed";

    bool has_conf_inputs = false;
    bool has_conf_outputs = false;
    for (const auto& output : found_tx.vout) {
        has_conf_outputs = has_conf_outputs || output.is_confidential;
    }

    // Add input details
    din::Json inputs_array = din::arr();
    int64_t total_input_value = 0;
    bool all_inputs_visible = true;
    for (const auto& input : found_tx.vin) {
        din::Json input_obj;
        input_obj["prevout_txid"] = input.prevout.txid.AsUint256().GetHex();
        input_obj["prevout_vout"] = static_cast<int>(input.prevout.vout);
        input_obj["sequence"] = static_cast<int>(input.sequence);

        // Lookup prevout amount (if not coinbase)
        if (!found_tx.IsCoinbase()) {
            int64_t prevout_value = 0;
            std::vector<uint8_t> prevout_script;
            bool prevout_is_confidential = false;
            std::vector<uint8_t> prevout_commitment;
            auto prev_tx_result = chain_db->getTransaction(input.prevout.txid.AsUint256());
            if (prev_tx_result.ok() && input.prevout.vout < prev_tx_result.value().vout.size()) {
                const auto& prevout = prev_tx_result.value().vout[input.prevout.vout];
                prevout_is_confidential = prevout.is_confidential;
                prevout_commitment = prevout.commitment;
                has_conf_inputs = has_conf_inputs || prevout_is_confidential;
            }

            if (GetPrevoutInfo(
                    chainstate.get(),
                    chain_db,
                    ctx.daemon ? ctx.daemon->block_storage.get() : nullptr,
                    current_height,
                    input.prevout.txid.AsUint256(),
                    input.prevout.vout,
                    prevout_value,
                    prevout_script)) {  // Phase M.4: Unwrap TxId
                input_obj["amount_hidden"] = prevout_is_confidential;
                input_obj["is_confidential_prevout"] = prevout_is_confidential;
                if (prevout_is_confidential) {
                    input_obj["display_amount"] = "confidential";
                    input_obj["commitment"] = BytesToHex(prevout_commitment);
                    all_inputs_visible = false;
                } else {
                    input_obj["value_una"] = static_cast<double>(prevout_value);
                    input_obj["value_din"] = formatDIN(prevout_value);
                    input_obj["display_amount"] = formatDIN(prevout_value);
                    total_input_value += prevout_value;
                }

                // Add prevout scriptPubKey hex
                std::ostringstream prevout_spk_hex;
                for (uint8_t byte : prevout_script) {
                    prevout_spk_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
                }
                input_obj["prevout_scriptPubKey"] = prevout_spk_hex.str();
            }
            // If prevout not found, amount fields are omitted
        }

        inputs_array.append(input_obj);
    }
    result["inputs"] = inputs_array;
    result["input_count"] = static_cast<int>(found_tx.vin.size());

    // Add output details
    din::Json outputs_array = din::arr();
    int64_t total_output_value = 0;
    bool all_outputs_visible = true;
    for (size_t i = 0; i < found_tx.vout.size(); ++i) {
        const auto& output = found_tx.vout[i];
        din::Json output_obj;
        output_obj["vout"] = static_cast<int>(i);
        PopulateExplorerOutputFields(output_obj, output);
        if (output.is_confidential) {
            all_outputs_visible = false;
        } else {
            total_output_value += output.value.GetUna();
        }

        // Identify output type
        if (output.IsTaproot()) {
            output_obj["type"] = "taproot";
        } else if (output.IsSegWitV0()) {
            output_obj["type"] = "segwit_v0";
        } else if (output.IsWitness()) {
            output_obj["type"] = "witness";
        } else {
            output_obj["type"] = "legacy";
        }

        // Add scriptPubKey hex
        std::ostringstream spk_hex;
        for (uint8_t byte : output.scriptPubKey) {
            spk_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        }
        output_obj["scriptPubKey"] = spk_hex.str();

        outputs_array.append(output_obj);
    }
    result["outputs"] = outputs_array;
    result["output_count"] = static_cast<int>(found_tx.vout.size());

    // Add transaction metadata
    result["version"] = static_cast<int>(found_tx.version);
    result["locktime"] = static_cast<int>(found_tx.lockTime);
    result["is_coinbase"] = found_tx.IsCoinbase();
    result["classification"] = ClassifyPrivacyFlow(has_conf_inputs, has_conf_outputs);
    result["has_confidential_inputs"] = has_conf_inputs;
    result["has_confidential_outputs"] = has_conf_outputs;

    if (found_tx.IsTaproot()) {
        result["witness_version"] = "taproot";
    } else if (found_tx.IsSegWitV0()) {
        result["witness_version"] = "segwit_v0";
    } else if (found_tx.HasWitness()) {
        result["witness_version"] = "witness";
    } else {
        result["witness_version"] = "legacy";
    }

    // Add value totals and fee (if not coinbase and inputs were resolved)
    if (all_outputs_visible) {
        result["total_output_value_una"] = static_cast<double>(total_output_value);
        result["total_output_value_din"] = formatDIN(total_output_value);
    } else {
        result["total_output_value_hidden"] = true;
        result["total_output_display"] = "confidential";
    }

    if (!found_tx.IsCoinbase() && total_input_value > 0 && all_inputs_visible && all_outputs_visible) {
        result["total_input_value_una"] = static_cast<double>(total_input_value);
        result["total_input_value_din"] = formatDIN(total_input_value);

        int64_t fee = total_input_value - total_output_value;
        result["fee_una"] = static_cast<double>(fee);
        result["fee_din"] = formatDIN(fee);
    } else if (!found_tx.IsCoinbase() && (has_conf_inputs || has_conf_outputs)) {
        result["fee_hidden"] = true;
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// PATH A: DETERMINISTIC REGTEST MINING (Phase W.1.1)
// ═══════════════════════════════════════════════════════════════
/**
 * generate - Mine blocks deterministically (regtest only)
 *
 * This is Bitcoin Core's deterministic block generation for regtest.
 * NO PoW loop, NO mining service dependency, instant block creation.
 *
 * Critical for: wallet tests, Lightning tests, CI determinism
 */
din::Json rpc_context_generate(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Phase W.1.1: HARD REQUIREMENT - regtest only
    const auto& chain_params = dinero::Params();
    if (chain_params.name != "regtest") {
        result["error"]["code"] = -1;
        result["error"]["message"] = "generate is regtest-only (use mining.start for testnet/mainnet)";
        return result;
    }

    dinero::g_logger.info("[PATH A] ✅ Deterministic regtest mining invoked");

    // Parse parameters
    int nblocks = 1;  // default
    if (params.isArray() && params.size() > 0 && params[0].isInt()) {
        nblocks = params[0].asInt();
    }

    if (nblocks <= 0 || nblocks > 1000) {
        result["error"]["code"] = -8;
        result["error"]["message"] = "nblocks must be between 1 and 1000";
        return result;
    }

    // Phase W.1.1: Get mining address from wallet (or use fallback)
    std::string address = "";
    if (ctx.daemon && ctx.daemon->wallet) {
        try {
            auto wallet_svc = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
            if (wallet_svc) {
                auto& mgr = wallet_svc->get();
                address = mgr.getNewAddress("mining", "taproot");
            }
        } catch (...) {
            // Wallet not available
        }
    }

    // Fallback to default address if wallet unavailable
    if (address.empty()) {
        address = "din1pegrzhlug8ak32yd89fu2p8e6zl9kwd8ee6z5874xdalrsr2c6xmss6h8k0";
    }

    dinero::g_logger.info("[PATH A] Mining " + std::to_string(nblocks) + " blocks to " + address);

    // Get chainDB
    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    dinero::ChainDB* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        result["error"] = "ChainDB not available";
        return result;
    }

    // Generate blocks (Path A - deterministic, no PoW)
    din::Json block_hashes(Json::arrayValue);

    // Track timestamp to ensure it advances for each block (median time past validation)
    uint32_t last_timestamp = 0;

    for (int i = 0; i < nblocks; ++i) {
        try {
            dinero::g_logger.info("[PATH A] Generating deterministic block " + std::to_string(i+1) + "/" + std::to_string(nblocks));

            // Get current height
            uint32_t current_height = dinero::storage::GetChainHeight(chain_db);
            uint32_t height = current_height + 1;
            std::string prevHash = dinero::storage::GetBestBlockHash(chain_db);

            // Get previous block's timestamp to ensure new timestamp advances
            uint32_t prev_timestamp = 0;
            if (height > 1) {  // Genesis has no previous block
                auto prev_header_result = chain_db->getHeader(dinero::uint256::FromHexUnsafe(prevHash));
                if (prev_header_result.ok()) {
                    prev_timestamp = prev_header_result.value().timestamp;
                }
            }

            // Create block (deterministic - nonce = 0) - exact pattern from MiningExtrasHandlers
            dinero::Block block;
            block.header.version = 1;
            block.header.prev_block_hash = dinero::uint256::FromHexUnsafe(prevHash);

            // Timestamp must be > median time past (which includes previous block)
            // Ensure timestamp is at least prev_timestamp + 1
            uint32_t current_time = static_cast<uint32_t>(std::time(nullptr));
            uint32_t min_timestamp = (prev_timestamp > last_timestamp) ? prev_timestamp : last_timestamp;
            if (current_time <= min_timestamp) {
                current_time = min_timestamp + 1;  // Advance by at least 1 second
            }
            block.header.timestamp = current_time;
            last_timestamp = current_time;

            block.header.nonce = 0;  // Deterministic: no PoW needed
            block.header.difficulty = 0x207fffff;  // Regtest minimum difficulty

            // Initialize utreexo_root and reserved (may be implicitly zero but explicit is safer)
            block.header.utreexo_root = dinero::uint256();
            std::memset(block.header.reserved, 0, sizeof(block.header.reserved));

            // Create coinbase
            dinero::Transaction coinbase;
            coinbase.version = 1;
            coinbase.lockTime = 0;

            dinero::TxInput input;
            input.prevout.txid = dinero::TxId(dinero::uint256());
            input.prevout.vout = 0xffffffff;

            // BIP34 height - must encode as: [length_byte] [height_bytes_little_endian]
            // The validator expects: first byte = length, then height value in LE
            std::vector<uint8_t> scriptSig;
            if (height >= 2) {
                if (height < 0x7f) {  // Fits in 1 byte
                    scriptSig.push_back(0x01);  // length = 1
                    scriptSig.push_back(static_cast<uint8_t>(height));
                } else if (height <= 0x7fff) {  // Fits in 2 bytes
                    scriptSig.push_back(0x02);  // length = 2
                    scriptSig.push_back(static_cast<uint8_t>(height & 0xff));
                    scriptSig.push_back(static_cast<uint8_t>((height >> 8) & 0xff));
                } else if (height <= 0x7fffff) {  // Fits in 3 bytes
                    scriptSig.push_back(0x03);  // length = 3
                    scriptSig.push_back(static_cast<uint8_t>(height & 0xff));
                    scriptSig.push_back(static_cast<uint8_t>((height >> 8) & 0xff));
                    scriptSig.push_back(static_cast<uint8_t>((height >> 16) & 0xff));
                }
            }
            scriptSig.push_back('D');
            scriptSig.push_back('N');
            scriptSig.push_back('R');

            input.scriptSig = scriptSig;
            input.sequence = 0xffffffff;
            coinbase.vin.push_back(input);

            // Coinbase output
            dinero::TxOutput output;
            output.value = dinero::ConsensusSubsidy::GetBlockSubsidy(height);

            // Phase W.1.1: Create scriptPubKey from wallet address
            // Use BuildScriptPubKeyFromAddress to decode the bech32m address
            std::string addr_error;
            if (!dinero::BuildScriptPubKeyFromAddress(address, output.scriptPubKey, addr_error)) {
                dinero::g_logger.error("[PATH A] Failed to build scriptPubKey from address: " + address + " error: " + addr_error);
                result["error"] = "Failed to decode mining address: " + addr_error;
                return result;
            }

            coinbase.vout.push_back(output);
            block.vtx.push_back(coinbase);

            // Merkle root - Use canonical merkle calculation (Phase 11a.2)
            // ComputeMerkleRoot() returns uint256 directly (internal format, no hex conversion)
            // For single-tx blocks: merkle_root == txid (guaranteed by Phase 11a.2 invariants)
            // This eliminates endianness bugs from hex round-tripping
            block.header.merkle_root = dinero::consensus::ComputeMerkleRoot(block.vtx);

            // DEBUG: Check what's in the block
            dinero::g_logger.info("[PATH A DEBUG] block.vtx.size() = " + std::to_string(block.vtx.size()));
            for (size_t i = 0; i < block.vtx.size(); i++) {
                dinero::g_logger.info(
                    "[PATH A DEBUG] tx[" + std::to_string(i) + "] vin=" +
                    std::to_string(block.vtx[i].vin.size()) + " vout=" +
                    std::to_string(block.vtx[i].vout.size())
                );
            }

            // DEBUG: Check header fields before serialization
            std::ostringstream diffHex;
            diffHex << "0x" << std::hex << std::setfill('0') << std::setw(8) << block.header.difficulty;
            dinero::g_logger.info("[PATH A DEBUG] header.difficulty = " + diffHex.str());
            dinero::g_logger.info("[PATH A DEBUG] header.nonce = " + std::to_string(block.header.nonce));
            dinero::g_logger.info("[PATH A DEBUG] header.timestamp = " + std::to_string(block.header.timestamp));

            // Submit block - convert binary to hex
            std::string blockBinary = block.Serialize();

            // Convert binary to hex string (required by ParseBlockFromHex)
            std::ostringstream hexStream;
            hexStream << std::hex << std::setfill('0');
            for (size_t i = 0; i < blockBinary.length(); ++i) {
                hexStream << std::setw(2) << static_cast<unsigned>(static_cast<unsigned char>(blockBinary[i]));
            }
            std::string blockHex = hexStream.str();

            dinero::g_logger.info("[PATH A DEBUG] Block: " + std::to_string(blockBinary.length()) + " bytes -> " + std::to_string(blockHex.length()) + " hex chars");

            // DEBUG: Show first 256 chars (128 bytes) of block hex (the header)
            std::string headerHex = blockHex.substr(0, std::min(size_t(256), blockHex.length()));
            dinero::g_logger.info("[PATH A DEBUG] Header hex (first 128 bytes): " + headerHex);

            auto accept_result = dinero::BlockAcceptor::AcceptBlockFromRPC(blockHex, "generate-deterministic");

            if (accept_result.rejected()) {
                dinero::g_logger.error("[PATH A] Block rejected: " + accept_result.reason);
                result["error"] = "Block acceptance failed: " + accept_result.reason;
                return result;
            }

            dinero::g_logger.info("[PATH A] ✅ Block " + std::to_string(i+1) + " accepted at height " + std::to_string(accept_result.height));
            block_hashes.append(accept_result.block_hash.GetHex());

        } catch (const std::exception& e) {
            dinero::g_logger.error("[PATH A] Block generation failed: " + std::string(e.what()));
            result["error"] = "Block generation failed: " + std::string(e.what());
            return result;
        }
    }

    dinero::g_logger.info("[PATH A] ✅ Generated " + std::to_string(nblocks) + " deterministic blocks");
    result = block_hashes;
    return result;
}

/**
 * blockchain.announcetip - Announce current tip to all peers (Phase G.X: Fork Resolution)
 *
 * Used after mining stops or when tips diverge to ensure peers sync to our chain.
 * This helps resolve situations where nodes have different tips due to rapid
 * block production and missed INV messages.
 *
 * Returns: { "tip": "<hash>", "height": <n>, "announced": true }
 */
din::Json rpc_context_announcetip(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    // Get current tip info
    std::string tip_hash = chainstate->getBestBlockHash();
    uint32_t height = chainstate->getBlockHeight();

    // Announce to all peers
    chainstate->AnnounceTip();

    result["tip"] = tip_hash;
    result["height"] = static_cast<int>(height);
    result["announced"] = true;
    result["message"] = "Tip announced to all peers for fork resolution";

    return result;
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION FUNCTION
// ═══════════════════════════════════════════════════════════════

/**
 * Register context-aware blockchain methods (Week 2)
 *
 * These methods replace the legacy versions and use DaemonContext
 * instead of global variables for service access.
 *
 * To migrate a namespace:
 * 1. Keep legacy methods registered (backward compat)
 * 2. Register new context-aware versions with RegisterMode::Overwrite
 * 3. Test thoroughly
 * 4. Remove legacy registrations in Week 3
 */

/**
 * blockchain.gettxout - Query a single UTXO by txid + vout
 *
 * Params: [txid (hex string), vout (int)]
 * Returns: null if spent/missing, or object with value, scriptPubKey, height, confirmations, coinbase
 */
din::Json rpc_context_gettxout(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Validate params: need txid (string) and vout (int)
    if (params.size() < 2) {
        result["error"] = "Usage: gettxout <txid> <vout>";
        return result;
    }

    if (!params[0].is<std::string>()) {
        result["error"] = "txid must be a hex string";
        return result;
    }
    std::string txid_hex = params[0].as<std::string>();
    if (txid_hex.size() != 64) {
        result["error"] = "txid must be 64 hex characters";
        return result;
    }

    if (!params[1].is<int>()) {
        result["error"] = "vout must be an integer";
        return result;
    }
    int vout = params[1].as<int>();
    if (vout < 0) {
        result["error"] = "vout must be non-negative";
        return result;
    }

    // Access ChainDB directly (in-memory ConsensusUTXOSet may be empty)
    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    dinero::ChainDB* chain_db = ctx.daemon->chainstate->GetChainDB();
    if (!chain_db) {
        result["error"] = "ChainDB not initialized";
        return result;
    }

    // Point lookup in RocksDB
    uint256 txid_raw = uint256::FromHexUnsafe(txid_hex);
    auto coin_result = chain_db->getCoin(txid_raw, static_cast<uint32_t>(vout));

    if (!coin_result.ok()) {
        // UTXO not found (spent or never existed) — return JSON null
        return din::Json();
    }

    const auto& coin = coin_result.value();

    // Get current tip for confirmations
    auto tip_result = chain_db->getTip();
    int current_height = 0;
    std::string bestblock;
    if (tip_result.ok()) {
        current_height = tip_result.value().height;
        bestblock = tip_result.value().hash.GetHex();
    }

    int confirmations = (current_height >= coin.height) ? (current_height - coin.height + 1) : 0;

    result["bestblock"] = bestblock;
    result["confirmations"] = confirmations;
    result["value"] = formatDIN(coin.amount);
    result["value_una"] = static_cast<double>(coin.amount);
    result["coinbase"] = coin.coinbase;
    result["height"] = coin.height;

    // scriptPubKey as hex
    din::Json spk_obj;
    spk_obj["hex"] = coin.script_pubkey;

    // Detect script type from raw hex
    // P2TR: OP_1 (0x51) + PUSH32 (0x20) + 32 bytes = 68 hex chars
    // P2WPKH: OP_0 (0x00) + PUSH20 (0x14) + 20 bytes = 44 hex chars
    // P2WSH: OP_0 (0x00) + PUSH32 (0x20) + 32 bytes = 68 hex chars
    if (coin.script_pubkey.size() == 68 && coin.script_pubkey.substr(0, 4) == "5120") {
        spk_obj["type"] = "witness_v1_taproot";
    } else if (coin.script_pubkey.size() == 44 && coin.script_pubkey.substr(0, 4) == "0014") {
        spk_obj["type"] = "witness_v0_keyhash";
    } else if (coin.script_pubkey.size() == 68 && coin.script_pubkey.substr(0, 4) == "0020") {
        spk_obj["type"] = "witness_v0_scripthash";
    } else {
        spk_obj["type"] = "unknown";
    }

    result["scriptPubKey"] = spk_obj;

    return result;
}

/**
 * blockchain.getheaders — Batch raw header fetch for lightweight clients.
 *
 * Params: { "from_height": int, "count": int (max 2000) }
 *   or positional: [from_height, count]
 *
 * Returns concatenated 128-byte raw headers as a single hex string,
 * enabling efficient header-chain sync (2000 headers = 256 KB hex).
 */
static din::Json rpc_context_getheaders(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"]["code"] = -1;
        result["error"]["message"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    dinero::ChainDB* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
    if (!chain_db) {
        result["error"]["code"] = -1;
        result["error"]["message"] = "ChainDB not initialized";
        return result;
    }

    auto tip_result = chain_db->getTip();
    if (!tip_result.ok()) {
        result["error"]["code"] = -1;
        result["error"]["message"] = "Failed to get chain tip";
        return result;
    }

    int tip_height = tip_result.value().height;

    // Parse parameters: positional [from_height, count] or named
    int from_height = 0;
    int count = 2000;
    static constexpr int MAX_BATCH = 2000;

    if (params.isArray()) {
        if (params.size() > 0 && params[0].isInt()) {
            from_height = params[0].asInt();
        }
        if (params.size() > 1 && params[1].isInt()) {
            count = params[1].asInt();
        }
    } else if (params.isObject()) {
        if (params.isMember("from_height") && params["from_height"].isInt()) {
            from_height = params["from_height"].asInt();
        }
        if (params.isMember("count") && params["count"].isInt()) {
            count = params["count"].asInt();
        }
    }

    if (from_height < 0) from_height = 0;
    if (count < 1) count = 1;
    if (count > MAX_BATCH) count = MAX_BATCH;

    // Clamp to available chain height
    int end_height = from_height + count - 1;
    if (end_height > tip_height) {
        end_height = tip_height;
    }

    int actual_count = (end_height >= from_height) ? (end_height - from_height + 1) : 0;

    // Build concatenated hex of raw 128-byte headers
    std::ostringstream hex_stream;
    hex_stream << std::hex << std::setfill('0');

    int fetched = 0;
    for (int h = from_height; h <= end_height; ++h) {
        auto hash_result = chain_db->getBlockHashByHeight(h);
        if (!hash_result.ok()) break;

        auto header_result = chain_db->getHeader(hash_result.value());
        if (!header_result.ok()) break;

        auto raw_bytes = header_result.value().SerializeForHash();
        for (uint8_t b : raw_bytes) {
            hex_stream << std::setw(2) << static_cast<int>(b);
        }
        ++fetched;
    }

    result["headers"] = hex_stream.str();
    result["from_height"] = from_height;
    result["count"] = fetched;
    result["tip_height"] = tip_height;

    return result;
}

/**
 * blockchain.getblockfilters — Batch GCS filter fetch for lightweight clients.
 *
 * Params: { "from_height": int, "count": int (max 2000) }
 *   or positional: [from_height, count]
 *
 * Returns array of filter objects with block_hash, filter hex, element_count,
 * and filter_hash (SHA256d of filter data) for coinbase commitment verification.
 */
static din::Json rpc_context_getblockfilters(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"]["code"] = -1;
        result["error"]["message"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    dinero::ChainDB* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
    if (!chain_db) {
        result["error"]["code"] = -1;
        result["error"]["message"] = "ChainDB not initialized";
        return result;
    }

    auto tip_result = chain_db->getTip();
    if (!tip_result.ok()) {
        result["error"]["code"] = -1;
        result["error"]["message"] = "Failed to get chain tip";
        return result;
    }

    int tip_height = tip_result.value().height;

    // Parse parameters: positional [from_height, count] or named
    int from_height = 0;
    int count = 2000;
    static constexpr int MAX_BATCH = 2000;

    if (params.isArray()) {
        if (params.size() > 0 && params[0].isInt()) {
            from_height = params[0].asInt();
        }
        if (params.size() > 1 && params[1].isInt()) {
            count = params[1].asInt();
        }
    } else if (params.isObject()) {
        if (params.isMember("from_height") && params["from_height"].isInt()) {
            from_height = params["from_height"].asInt();
        }
        if (params.isMember("count") && params["count"].isInt()) {
            count = params["count"].asInt();
        }
    }

    if (from_height < 0) from_height = 0;
    if (count < 1) count = 1;
    if (count > MAX_BATCH) count = MAX_BATCH;

    int end_height = from_height + count - 1;
    if (end_height > tip_height) {
        end_height = tip_height;
    }

    din::Json filters_array = din::arr();
    int fetched = 0;

    for (int h = from_height; h <= end_height; ++h) {
        auto hash_result = chain_db->getBlockHashByHeight(h);
        if (!hash_result.ok()) break;

        const uint256& block_hash = hash_result.value();
        auto block_result = ReadRpcBlock(
            chainstate.get(),
            chain_db,
            ctx.daemon ? ctx.daemon->block_storage.get() : nullptr,
            block_hash);
        if (!block_result.ok() || block_result.value().vtx.empty()) {
            continue;
        }
        const auto& block = block_result.value();

        auto filter_result = chain_db->getBlockFilter(block_hash);

        dinero::consensus::GCSFilter filter_obj;
        bool filter_ready = false;
        auto commitment_validates = [&](const dinero::consensus::GCSFilter& candidate,
                                        std::string* error_out = nullptr) -> bool {
            if (!dinero::consensus::RequiresFilterCommitment(h)) {
                if (error_out) error_out->clear();
                return true;
            }
            std::string local_error;
            const bool ok = dinero::consensus::ValidateFilterCommitment(
                block.vtx[0], candidate.GetHash(), h, local_error);
            if (error_out) {
                *error_out = local_error;
            }
            return ok;
        };
        auto build_filter = [&](bool include_spent_inputs) -> std::optional<dinero::consensus::GCSFilter> {
            std::vector<std::vector<uint8_t>> scripts;

            // Output scriptPubKeys (excluding OP_RETURN)
            for (const auto& tx : block.vtx) {
                for (const auto& out : tx.vout) {
                    if (!out.scriptPubKey.empty() && out.scriptPubKey[0] != 0x6a) {
                        scripts.push_back(out.scriptPubKey);
                    }
                }
            }

            // Spent input scriptPubKeys from undo data
            if (include_spent_inputs) {
                auto undo_res = dinero::storage::ReadArchivalUndo(
                    *chain_db,
                    ctx.daemon ? ctx.daemon->block_storage.get() : nullptr,
                    block_hash);
                if (undo_res.ok()) {
                    for (const auto& sc : undo_res.value().spent) {
                        if (!sc.scriptPubKey.empty()) {
                            scripts.push_back(sc.scriptPubKey);
                        }
                    }
                }
            }

            auto candidate = dinero::consensus::GCSFilter::Build(scripts, block.header.prev_block_hash);
            if (candidate.IsEmpty()) {
                return std::nullopt;
            }
            return candidate;
        };
        auto persist_filter = [&](const dinero::consensus::GCSFilter& candidate) -> bool {
            chain_db->putBlockFilter(block_hash, candidate.encoded_data, candidate.element_count);
            filter_obj = candidate;
            filter_ready = true;
            return true;
        };
        auto rebuild_filter = [&]() -> bool {
            if (auto full_filter = build_filter(true); full_filter.has_value()) {
                std::string full_error;
                if (commitment_validates(*full_filter, &full_error)) {
                    return persist_filter(*full_filter);
                }
                if (dinero::consensus::RequiresFilterCommitment(h)) {
                    dinero::g_logger.warning(
                        "[getblockfilters] Rebuilt full filter mismatches DNRF commitment at height " +
                        std::to_string(h) + ": " + full_error);
                }
            }

            // Historical compatibility: some early post-activation blocks committed an
            // outputs-only filter hash before spent-input scripts were consistently
            // available on the filter-building path. If the full rebuild does not
            // match the on-chain DNRF commitment, try the outputs-only form and
            // persist it only if it verifies against the coinbase commitment.
            if (dinero::consensus::RequiresFilterCommitment(h)) {
                if (auto outputs_only_filter = build_filter(false); outputs_only_filter.has_value()) {
                    std::string compat_error;
                    if (commitment_validates(*outputs_only_filter, &compat_error)) {
                        dinero::g_logger.warning(
                            "[getblockfilters] Serving historical outputs-only compatibility filter at height " +
                            std::to_string(h));
                        return persist_filter(*outputs_only_filter);
                    }
                }
            }

            return false;
        };

        if (filter_result.ok()) {
            // Filter already stored
            const auto& stored = filter_result.value();
            filter_obj = dinero::consensus::GCSFilter::FromEncoded(
                stored.data, stored.element_count, block.header.prev_block_hash);
            filter_ready = true;

            // Self-heal stale legacy cache entries before serving them to light clients.
            if (dinero::consensus::RequiresFilterCommitment(h)) {
                std::string filter_error;
                if (!dinero::consensus::ValidateFilterCommitment(
                        block.vtx[0], filter_obj.GetHash(), h, filter_error)) {
                    filter_ready = rebuild_filter();
                }
            }
        } else {
            // Backfill: build filter on-the-fly from block + undo data.
            filter_ready = rebuild_filter();
        }

        if (!filter_ready || filter_obj.IsEmpty()) continue;

        uint256 filter_hash = filter_obj.GetHash();

        // Encode filter data as hex
        std::ostringstream hex_stream;
        hex_stream << std::hex << std::setfill('0');
        for (uint8_t b : filter_obj.encoded_data) {
            hex_stream << std::setw(2) << static_cast<int>(b);
        }

        din::Json entry;
        entry["height"] = h;
        entry["block_hash"] = block_hash.GetHex();
        entry["filter"] = hex_stream.str();
        entry["element_count"] = static_cast<int>(filter_obj.element_count);
        entry["filter_hash"] = filter_hash.GetHex();

        // Include coinbase TX and merkle proof for commitment verification
        // Serialized coinbase transaction (hex) and txid (non-witness hash for merkle proof)
        entry["coinbase_tx"] = block.vtx[0].SerializeHex(dinero::TxSerializationMode::WithWitness);
        entry["coinbase_txid"] = block.vtx[0].GetTxid().AsUint256().GetHex();

        // Merkle proof: sibling hashes from coinbase (index 0) to root
        auto proof = dinero::consensus::GenerateMerkleProof(block.vtx, 0);
        din::Json proof_array = din::arr();
        for (const auto& hash : proof) {
            proof_array.append(hash.GetHex());
        }
        entry["merkle_proof"] = proof_array;
        entry["tx_count"] = static_cast<int>(block.vtx.size());

        filters_array.append(entry);
        ++fetched;
    }

    result["filters"] = filters_array;
    result["from_height"] = from_height;
    result["count"] = fetched;
    result["tip_height"] = tip_height;

    return result;
}

/**
 * blockchain.shielded.outputs — Public shielded output feed (M2).
 *
 * Returns the public shielded outputs and spend nullifiers in the
 * requested height range. Thin clients trial-decrypt the encrypted
 * notes locally against their own viewing keys; the daemon performs
 * no recipient-derived filtering (the wire format provides no
 * client-precomputable recipient clue).
 *
 * Params: { "from_height": int, "count": int (max 2000) }
 *   or positional: [from_height, count]
 *
 * Response shape — see
 * docs/superpowers/specs/2026-05-27-trustless-light-client-shielded-m2-design.md.
 * Blocks with zero shielded outputs AND zero shielded spends are
 * omitted from the `blocks` array.
 */
static din::Json rpc_context_shieldedoutputs(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"]["code"] = -1;
        result["error"]["message"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    dinero::ChainDB* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
    if (!chain_db) {
        result["error"]["code"] = -1;
        result["error"]["message"] = "ChainDB not initialized";
        return result;
    }

    auto tip_result = chain_db->getTip();
    if (!tip_result.ok()) {
        result["error"]["code"] = -1;
        result["error"]["message"] = "Failed to get chain tip";
        return result;
    }
    const int tip_height = tip_result.value().height;

    // ── Param parsing (same shape as blockchain.getblockfilters) ──
    int from_height = 0;
    int count = 2000;
    static constexpr int MAX_BATCH = 2000;

    if (params.isArray()) {
        if (params.size() > 0 && params[0].isInt()) from_height = params[0].asInt();
        if (params.size() > 1 && params[1].isInt()) count = params[1].asInt();
    } else if (params.isObject()) {
        if (params.isMember("from_height") && params["from_height"].isInt()) {
            from_height = params["from_height"].asInt();
        }
        if (params.isMember("count") && params["count"].isInt()) {
            count = params["count"].asInt();
        }
    }
    if (from_height < 0) from_height = 0;
    if (count < 1)        count = 1;
    if (count > MAX_BATCH) count = MAX_BATCH;

    int end_height = from_height + count - 1;
    if (end_height > tip_height) end_height = tip_height;

    // ── Establish leaf-index basis for from_height ──
    // Lookup closure: ChainDB → uint256 → Block via ReadRpcBlock.
    ::dinero::BlockStorage* block_storage =
        ctx.daemon ? ctx.daemon->block_storage.get() : nullptr;
    auto block_lookup = [chainstate, chain_db, block_storage](uint32_t h) -> std::optional<::dinero::Block> {
        auto hash_result = chain_db->getBlockHashByHeight(static_cast<int>(h));
        if (!hash_result.ok()) return std::nullopt;
        auto block_result = ReadRpcBlock(chainstate.get(), chain_db, block_storage, hash_result.value());
        if (!block_result.ok()) return std::nullopt;
        return block_result.value();
    };

    const uint32_t activation_height = dinero::Params().shielded_activation_height;

    auto first_leaf_result = ::dinero::consensus::shielded::CountShieldedOutputsBeforeHeight(
        static_cast<uint32_t>(from_height),
        activation_height,
        block_lookup);
    if (!first_leaf_result.ok()) {
        result["error"]["code"] = -1;
        result["error"]["message"] = "Failed to derive first_leaf_index: " +
            std::string(::dinero::StatusToString(first_leaf_result.status()));
        return result;
    }
    uint64_t next_leaf_index = first_leaf_result.value();

    // ── Walk requested height range ──
    din::Json blocks_array = din::arr();

    for (int h = from_height; h <= end_height; ++h) {
        if (static_cast<uint32_t>(h) < activation_height) continue;

        auto hash_result = chain_db->getBlockHashByHeight(h);
        if (!hash_result.ok()) break;
        const uint256& block_hash = hash_result.value();

        auto block_result = ReadRpcBlock(chainstate.get(), chain_db, block_storage, block_hash);
        if (!block_result.ok()) continue;
        const auto& block = block_result.value();

        ::dinero::consensus::shielded::ShieldedOutputFeedResult feed{};
        const auto feed_status = ::dinero::consensus::shielded::ExtractShieldedOutputFeed(
            block, static_cast<uint32_t>(h), next_leaf_index, &feed);
        if (feed_status != ::dinero::consensus::shielded::ShieldedOutputFeedError::Ok) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Shielded output feed extraction failed at height " +
                std::to_string(h);
            return result;
        }

        // Skip blocks that contributed no shielded data to the feed.
        if (feed.outputs.empty() && feed.spent_nullifiers.empty()) {
            // next_leaf_index is unchanged when no outputs were emitted.
            continue;
        }

        din::Json block_obj;
        block_obj["height"]                = h;
        block_obj["block_hash"]             = block_hash.GetHex();
        block_obj["shielded_spend_count"]   = static_cast<int>(feed.spent_nullifiers.size());
        block_obj["shielded_output_count"]  = static_cast<int>(feed.outputs.size());

        din::Json spends_array = din::arr();
        for (const auto& nf : feed.spent_nullifiers) {
            din::Json o;
            o["txid"]        = nf.txid.AsUint256().GetHex();
            o["tx_index"]    = static_cast<int>(nf.tx_index);
            o["spend_index"] = static_cast<int>(nf.spend_index);
            o["nullifier"]   = RawBytesToHex(nf.nullifier.data(), nf.nullifier.size());
            spends_array.append(o);
        }
        block_obj["spent_nullifiers"] = spends_array;

        din::Json outputs_array = din::arr();
        for (const auto& e : feed.outputs) {
            din::Json o;
            o["txid"]         = e.txid.AsUint256().GetHex();
            o["tx_index"]     = static_cast<int>(e.tx_index);
            o["output_index"] = static_cast<int>(e.output_index);
            // leaf_index is monotonically advancing; JSON integers stay safe
            // for chain heights up to ~2^53 outputs.
            o["leaf_index"]   = static_cast<Json::UInt64>(e.leaf_index);
            o["commitment"]   = RawBytesToHex(e.commitment.data(), e.commitment.size());
            o["encrypted_note"] = BytesToHex(e.encrypted_note);
            outputs_array.append(o);
        }
        block_obj["outputs"] = outputs_array;

        blocks_array.append(block_obj);
        next_leaf_index = feed.next_leaf_index;
    }

    result["from_height"] = from_height;
    result["count"]       = count;
    result["tip_height"]  = tip_height;
    result["blocks"]      = blocks_array;
    return result;
}

void registerBlockchainMethodsContext() {
    extern RpcRegistry g_rpcRegistry;

    // Note: Using RegisterMode::Overwrite to replace legacy handlers
    // The method names stay the same, but implementation uses context

    // Core blockchain queries
    g_rpcRegistry.registerHandler("blockchain.getblockcount",
                                 rpc_context_getblockcount,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getblockcount", "blockchain.getblockcount");

    g_rpcRegistry.registerHandler("blockchain.getblockhash",
                                 rpc_context_getblockhash,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getblockhash", "blockchain.getblockhash");

    g_rpcRegistry.registerHandler("blockchain.getblock",
                                 rpc_context_getblock,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getblock", "blockchain.getblock");

    g_rpcRegistry.registerHandler("blockchain.gettransaction",
                                 rpc_context_gettransaction,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("gettransaction", "blockchain.gettransaction");

    g_rpcRegistry.registerHandler("blockchain.getblockchaininfo",
                                 rpc_context_getblockchaininfo,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getblockchaininfo", "blockchain.getblockchaininfo");

    g_rpcRegistry.registerHandler("blockchain.getsynchealth",
                                 rpc_context_getsynchealth,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getsynchealth", "blockchain.getsynchealth");

    g_rpcRegistry.registerHandler("blockchain.auditundometadata",
                                 rpc_context_auditundometadata,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("auditundometadata", "blockchain.auditundometadata");

    g_rpcRegistry.registerHandler("blockchain.debugclearundoflag",
                                 rpc_context_debugclearundoflag,
                                 RegisterMode::Overwrite,
                                 "regtest-only");

    g_rpcRegistry.registerHandler("blockchain.getarchivalstatus",
                                 rpc_context_getarchivalstatus,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getarchivalstatus", "blockchain.getarchivalstatus");

    g_rpcRegistry.registerHandler("blockchain.getinfo",
                                 rpc_context_getinfo,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getinfo", "blockchain.getinfo");

    g_rpcRegistry.registerHandler("blockchain.getbestblockhash",
                                 rpc_context_getbestblockhash,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getbestblockhash", "blockchain.getbestblockhash");

    g_rpcRegistry.registerHandler("blockchain.getdifficulty",
                                 rpc_context_getdifficulty,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getdifficulty", "blockchain.getdifficulty");

    // Phase P.2: Pruning RPC
    g_rpcRegistry.registerHandler("blockchain.pruneblockchain",
                                 rpc_pruneblockchain,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("pruneblockchain", "blockchain.pruneblockchain");

    g_rpcRegistry.registerHandler("blockchain.getblockheader",
                                 rpc_context_getblockheader,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getblockheader", "blockchain.getblockheader");

    // Mining methods
    g_rpcRegistry.registerHandler("blockchain.getmininginfo",
                                 rpc_context_getmininginfo,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getmininginfo", "blockchain.getmininginfo");

    g_rpcRegistry.registerHandler("blockchain.submitblock",
                                 rpc_context_submitblock,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("submitblock", "blockchain.submitblock");

    // Admin methods
    g_rpcRegistry.registerHandler("blockchain.invalidateblock",
                                 rpc_context_invalidateblock,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("invalidateblock", "blockchain.invalidateblock");

    g_rpcRegistry.registerHandler("blockchain.reconsiderblock",
                                 rpc_context_reconsiderblock,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("reconsiderblock", "blockchain.reconsiderblock");

    // Diagnostic — see comment on rpc_context_utreexo_dumpforestinternal.
    g_rpcRegistry.registerHandler("utreexo.dumpforestinternal",
                                 rpc_context_utreexo_dumpforestinternal,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // AssumeUTXO: Fast sync with snapshots
    g_rpcRegistry.registerHandler("blockchain.dumptxoutset",
                                 rpc_context_dumptxoutset,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("dumptxoutset", "blockchain.dumptxoutset");

    g_rpcRegistry.registerHandler("blockchain.loadtxoutset",
                                 rpc_context_loadtxoutset,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("loadtxoutset", "blockchain.loadtxoutset");

    g_rpcRegistry.registerHandler("blockchain.getbackgroundvalidationprogress",
                                 rpc_context_getbackgroundvalidationprogress,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getbackgroundvalidationprogress", "blockchain.getbackgroundvalidationprogress");

    g_rpcRegistry.registerHandler("blockchain.getibdprogress",
                                 rpc_context_getibdprogress,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getibdprogress", "blockchain.getibdprogress");

    g_rpcRegistry.registerHandler("blockchain.getsnapshotbootstrapstatus",
                                 rpc_context_getsnapshotbootstrapstatus,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getsnapshotbootstrapstatus", "blockchain.getsnapshotbootstrapstatus");

    g_rpcRegistry.registerHandler("blockchain.getpruninginfo",
                                 rpc_context_getpruninginfo,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getpruninginfo", "blockchain.getpruninginfo");

    // NOTE: blockchain.pruneblockchain already registered above (line 1688) using PruneService.
    // The Phase 46 version (rpc_context_pruneblockchain) used ChainstateService which is deprecated.

    // Phase W.1.1: Deterministic regtest mining (registered early, unconditional)
    // This MUST be registered here (core blockchain RPC) to ensure availability before
    // MiningService initialization. Path A (deterministic regtest) does NOT depend on
    // mining service - it creates blocks instantly with nonce=0.
    g_rpcRegistry.registerHandler("generate",
                                 rpc_context_generate,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    dinero::g_logger.info("[RPC Context] ✅ Registered 'generate' RPC (Path A - deterministic regtest mining, always available)");

    // Phase G.X: Fork resolution - announce current tip to peers
    g_rpcRegistry.registerHandler("blockchain.announcetip",
                                 rpc_context_announcetip,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("announcetip", "blockchain.announcetip");
    dinero::g_logger.info("[RPC Context] ✅ Registered 'announcetip' RPC (Phase G.X - fork resolution)");

    // UTXO set info (defined in methods_utxoset.cpp)
    g_rpcRegistry.registerHandler("blockchain.gettxoutsetinfo",
                                 [](const ExecutionContext& ctx, const din::Json& params) {
                                     return din::rpc_gettxoutsetinfo(ctx, params);
                                 },
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("gettxoutsetinfo", "blockchain.gettxoutsetinfo");

    // Address Explorer: scan UTXO set by address (defined in methods_utxoset.cpp)
    g_rpcRegistry.registerHandler("blockchain.scantxoutset",
                                 [](const ExecutionContext& ctx, const din::Json& params) {
                                     return din::rpc_scantxoutset(ctx, params);
                                 },
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("scantxoutset", "blockchain.scantxoutset");

    // Address-indexed queries (defined in methods_address_index.cpp)
    g_rpcRegistry.registerHandler("blockchain.getaddressbalance",
                                 [](const ExecutionContext& ctx, const din::Json& params) {
                                     return din::rpc_getaddressbalance(ctx, params);
                                 },
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getaddressbalance", "blockchain.getaddressbalance");

    g_rpcRegistry.registerHandler("blockchain.getaddressmempool",
                                 [](const ExecutionContext& ctx, const din::Json& params) {
                                     return din::rpc_getaddressmempool(ctx, params);
                                 },
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getaddressmempool", "blockchain.getaddressmempool");

    g_rpcRegistry.registerHandler("blockchain.getaddresshistory",
                                 [](const ExecutionContext& ctx, const din::Json& params) {
                                     return din::rpc_getaddresshistory(ctx, params);
                                 },
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getaddresshistory", "blockchain.getaddresshistory");

    g_rpcRegistry.registerHandler("blockchain.reindextx",
                                 [](const ExecutionContext& ctx, const din::Json& params) {
                                     return din::rpc_reindextx(ctx, params);
                                 },
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("reindextx", "blockchain.reindextx");

    g_rpcRegistry.registerHandler("blockchain.getcheckpoints",
                                 [](const ExecutionContext& ctx, const din::Json& params) {
                                     return din::rpc_getcheckpoints(ctx, params);
                                 },
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getcheckpoints", "blockchain.getcheckpoints");

    // Batch raw header fetch for lightweight header-chain sync
    g_rpcRegistry.registerHandler("blockchain.getheaders",
                                 rpc_context_getheaders,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getheaders", "blockchain.getheaders");

    // Batch GCS filter fetch for lightweight filter-chain sync
    g_rpcRegistry.registerHandler("blockchain.getblockfilters",
                                 rpc_context_getblockfilters,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getblockfilters", "blockchain.getblockfilters");

    // M2: public shielded output feed for thin-client receive scanning
    g_rpcRegistry.registerHandler("blockchain.shielded.outputs",
                                 rpc_context_shieldedoutputs,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("shieldedoutputs", "blockchain.shielded.outputs");

    // gettxout: query single UTXO by txid + vout
    g_rpcRegistry.registerHandler("blockchain.gettxout",
                                 rpc_context_gettxout,
                                 RpcMethodMeta{
                                     "blockchain.gettxout",
                                     "blockchain",
                                     "Returns details about an unspent transaction output (UTXO). "
                                     "Returns null (JSON null) if the output is spent or was never created.",
                                     {
                                         {"txid", "string", "The transaction id (64 hex chars)", true},
                                         {"vout", "integer", "The output index (0-based)", true}
                                     },
                                     {"object|null",
                                      "A JSON object with value, scriptPubKey, height, confirmations, "
                                      "coinbase, and mature fields — or null if the UTXO does not exist "
                                      "(spent or never created)"},
                                     "dinero-cli gettxout <txid> 0"
                                 },
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("gettxout", "blockchain.gettxout");

    // Phase RPC-UX: Metadata registration remains disabled until handler symbol
    // names are unified between context methods and metadata module.

    dinero::g_logger.info("[RPC Context] Registered 22 blockchain context-aware methods (including AssumeUTXO + IBD + snapshot bootstrap status + Pruning + Generate + AnnounceTip + ScanTxOutSet + GetTxOut + GetHeaders)");
}
