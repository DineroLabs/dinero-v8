#include "daemon/block_acceptor.h"
#include "mining/header_layout.h"  // 128-byte BlockHeader v1 constants
#include "db_meta_utils.hpp"
#include "common/sha256d.h"
#include "crypto/sha256.h"         // For CSHA256 — canonical hash computation
#include "consensus/target_helpers.h"
#include "consensus/consensus.hpp"
#include "consensus/asert.h"
#include "consensus/pow_context.h"
#include "consensus/pow.hpp"
#include "consensus/chainparams.h"  // For dinero::Params()
#include "consensus/utreexo_activation.h"
#include "consensus/block_status_generation.h"
#include "common/crash_injection.h"  // Phase 11a: IsUtreexoActive check
// Phase 39: chain_manager.h deleted (ChainManager removed)
#include "consensus/tx_parser.h"    // Phase 3D: Transaction parsing for wallet notifications
#include "consensus/parallel_block_validator.h"  // Phase 6B: Parallel script validation
#include "consensus/sigops.h"       // Mainnet Blocker: MAX_BLOCK_SIGOPS enforcement
#include "consensus/merkle_root.h"  // Phase 11a: Canonical merkle computation
#include "consensus/block_index.h"  // For FindBlockIndex, CBlockIndex::GetMedianTimePast (Reorg MTP fix)
#include "consensus/block_lifecycle.h"  // BLOCK_HAVE_DATA status flag
#include "consensus/header_chain.h"  // Fork-aware MTP: HeaderIndexEntry::GetMedianTimePast
#include "metrics/metrics_registry.h"
#include "primitives/block.h"
#include "wallet/wallet_manager.h"
#include "wallet/wallet_worker.h"
#include "daemon/ws_globals.h"
#include "daemon/gui_websocket_events.hpp"
#include "storage/chain_direct.h"
#include "storage/chain_db.h"
#include "storage/block_storage.h"
#include "storage/forest_restore.h"
#include "common/logger.h"
#include "daemon/services/p2p_service.h"  // Week 4: P2PService access via context
#include "daemon/p2p_manager.h"     // For P2PMessage
#include "common/address_script_builder.h"  // For BuildScriptPubKeyFromAddress
#include "common/hex_utils.h"              // For hex32_0x() - proper hex formatting
#include "daemon/daemon_context.h"         // Week 3: DaemonContext instead of globals
#include "daemon/services/chainstate_service.h"  // Week 3: ChainstateService access
#include "daemon/services/config_service.h"
#include "daemon/services/wallet_service.h"      // Week 3: WalletService access
#include <cmath>
#include <rocksdb/write_batch.h>
#include <json/writer.h>

// Forward declaration for wallet notification
extern void notifyWalletNewBlock(int height, const std::string& blockHash, const std::vector<dinero::Transaction>& transactions);

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <cstdlib>  // getenv, strtol — regtest fault-injection hook (#356 Task 3)
#include <cerrno>   // errno for strtol error checking
#include <cstdio>   // fflush
#ifndef _WIN32
#include <unistd.h> // _exit (POSIX)
#else
#include <process.h> // _exit (Windows CRT)
#endif

using namespace dinero;

// BlockRejectCodeToString moved to daemon/interfaces/ingress_types.cpp (Step 5)

// Week 3: Static context pointer (initialized by ChainstateService)
DaemonContext* BlockAcceptor::ctx_ = nullptr;

// Simple logging macros for now
#define LOG_INFO(msg) std::cout << "[BlockAcceptor INFO] " << msg << std::endl
#define LOG_ERROR(msg) std::cerr << "[BlockAcceptor ERROR] " << msg << std::endl

// Phase 3F: WebSocket notifications disabled (global g_subscriptions removed)
// Previously: Per-topic sequence managed by Subscriptions class
// TODO: Re-implement using ctx_->websocket_service in Phase 4+

// Size cap for WebSocket messages (256 KiB)
static constexpr size_t MAX_WS_MESSAGE_SIZE = 256 * 1024;

BlockAcceptResult BlockAcceptor::AcceptBlockFromRPC(const std::string& blockHex, const std::string& source) {
    try {
        // Use the canonical activation lock for the complete read/validate/store/
        // activate sequence.  A separate ingress-only mutex serialized peers
        // with each other but did not serialize them with AssumeUTXO promotion,
        // allowing a torn view (old ChainDB tip + promoted Utreexo forest).
        // ActivateBestChain takes this mutex recursively later in this method.
        auto* activation_ctx = DaemonContext::instance();
        auto activation_chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(
            activation_ctx ? activation_ctx->chainstate : nullptr);
        std::unique_lock<dinero::AnnotatedRecursiveMutex> activation_guard;
        if (activation_chainstate) {
            activation_guard = activation_chainstate->AcquireBlockIngressActivationLock();
        }

        LOG_INFO("🔍 BlockAcceptor: Processing " + std::to_string(blockHex.length()) + " hex chars from " + source);
        std::cout << "[ACCEPTOR-DEBUG] >>> AcceptBlockFromRPC ENTRY hex_size=" << blockHex.length() << " source=" << source << std::endl;

        // 1. Parse block from hex
        std::cout << "[ACCEPTOR-DEBUG] Step 1: Parsing block from hex..." << std::endl;
        ParsedBlock block = ParseBlockFromHex(blockHex);
        std::cout << "[ACCEPTOR-DEBUG] Block parsed: hash=" << block.blockHash.substr(0, 16) << "..."
                  << " prev=" << block.prevBlockHash.substr(0, 16) << "..."
                  << " txs=" << block.transactions.size() << std::endl;

        // Use canonical wire-bytes hash for structured result
        uint256 block_hash = block.blockHashRaw;

        // 2. Validate block header
        std::string error;
        std::cout << "[ACCEPTOR-DEBUG] Step 2: Validating block header..." << std::endl;
        if (!ValidateBlockHeader(block, error)) {
            std::cout << "[ACCEPTOR-DEBUG] REJECTED: Invalid header - " << error << std::endl;
            dinero::metrics::MetricsRegistry::IncrementBlocksRejected("invalid-header");
            return BlockAcceptResult::Rejected(BlockRejectCode::INVALID_HEADER, error, block_hash);
        }
        std::cout << "[ACCEPTOR-DEBUG] Header validated OK" << std::endl;

        // 3. Find parent block and validate chain link
        uint64_t parentHeight;
        std::string parentChainwork;
        std::cout << "[ACCEPTOR-DEBUG] Step 3: Finding parent block " << block.prevBlockHash.substr(0, 16) << "..." << std::endl;
        if (!FindParentBlock(block.prevBlockHash, parentHeight, parentChainwork, error)) {
            std::cout << "[ACCEPTOR-DEBUG] REJECTED: Missing parent - " << error << std::endl;
            dinero::metrics::MetricsRegistry::IncrementBlocksRejected("missing-parent");
            return BlockAcceptResult::Rejected(BlockRejectCode::MISSING_PARENT, error, block_hash);
        }
        std::cout << "[ACCEPTOR-DEBUG] Parent found at height " << parentHeight << std::endl;

        uint64_t newHeight = parentHeight + 1;

        // Validate PoW with correct height (moved here from ValidateBlockHeader)
        std::cout << "[ACCEPTOR-DEBUG] Step 3b: Validating PoW for height " << newHeight << "..." << std::endl;
        if (!ValidateProofOfWork(block, static_cast<uint32_t>(newHeight), error, block.prevBlockHash)) {
            std::cout << "[ACCEPTOR-DEBUG] REJECTED: Invalid PoW - " << error << std::endl;
            dinero::metrics::MetricsRegistry::IncrementBlocksRejected("invalid-pow");
            return BlockAcceptResult::Rejected(BlockRejectCode::INVALID_HEADER, error, block_hash, newHeight);
        }
        std::cout << "[ACCEPTOR-DEBUG] PoW validated OK for height " << newHeight << std::endl;

        if (!ValidateParentLink(block, parentHeight, error)) {
            dinero::metrics::MetricsRegistry::IncrementBlocksRejected("invalid-parent-link");
            return BlockAcceptResult::Rejected(BlockRejectCode::INVALID_PARENT_LINK, error, block_hash, newHeight);
        }

        // Determine if this extends the main chain or is a side-chain block
        // Side-chain blocks are stored but don't immediately update the tip
        bool isMainChainExtension = false;
        {
            auto* daemon_ctx = DaemonContext::instance();
            auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(daemon_ctx ? daemon_ctx->chainstate : nullptr);
            auto* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
            if (chain_db) {
                auto tip_result = chain_db->getTip();
                if (tip_result.status() == dinero::Status::Ok) {
                    // Compare raw uint256 — never compare display-order hex strings
                    dinero::uint256 prevHashRaw = dinero::uint256::FromHexUnsafe(block.prevBlockHash);
                    isMainChainExtension = (prevHashRaw == tip_result.value().hash);
                    if (!isMainChainExtension) {
                        LOG_INFO("🔍 Chain link check: prevHash=" + prevHashRaw.GetHex().substr(0, 16) +
                                 "... tipHash=" + tip_result.value().hash.GetHex().substr(0, 16) + "...");
                    }
                }
            }
        }
        if (!isMainChainExtension) {
            LOG_INFO("📌 Side-chain block detected: parent " + block.prevBlockHash.substr(0, 16) +
                    "... is not current tip (height " + std::to_string(newHeight) + ")");
        }

        // 4. Validate merkle root
        if (!ValidateMerkleRoot(block, error)) {
            dinero::metrics::MetricsRegistry::IncrementBlocksRejected("invalid-merkle");
            return BlockAcceptResult::Rejected(BlockRejectCode::INVALID_MERKLE_ROOT, error, block_hash, newHeight);
        }

        // 4.5. Validate signature operations (MAINNET BLOCKER FIX)
        // Enforce MAX_BLOCK_SIGOPS_COST to prevent DoS attacks
        if (!ValidateBlockSigops(block, error)) {
            dinero::metrics::MetricsRegistry::IncrementBlocksRejected("sigops-exceeded");
            return BlockAcceptResult::Rejected(BlockRejectCode::SIGOPS_LIMIT_EXCEEDED, error, block_hash, newHeight);
        }

        // 5. Validate contextual rules
        std::cout << "[ACCEPTOR-DEBUG] Step 5: Validating contextual rules..." << std::endl;
        if (!ValidateContextual(block, newHeight, error)) {
            std::cout << "[ACCEPTOR-DEBUG] REJECTED: Invalid coinbase - " << error << std::endl;
            dinero::metrics::MetricsRegistry::IncrementBlocksRejected("invalid-coinbase");
            return BlockAcceptResult::Rejected(BlockRejectCode::INVALID_COINBASE, error, block_hash, newHeight);
        }

        // 5.5. Validate checkpoint (prevent reorg past checkpoint blocks)
        std::cout << "[ACCEPTOR-DEBUG] Step 5.5: Validating checkpoint..." << std::endl;
        if (!ValidateCheckpoint(block, newHeight, error)) {
            std::cout << "[ACCEPTOR-DEBUG] REJECTED: Checkpoint violation - " << error << std::endl;
            dinero::metrics::MetricsRegistry::IncrementBlocksRejected("checkpoint-violation");
            return BlockAcceptResult::Rejected(BlockRejectCode::CHECKPOINT_VIOLATION, error, block_hash, newHeight);
        }

        // 5.6 Utreexo root sanity check (main-chain extensions only).
        //
        // Recompute the post-block utreexo root from the submitted
        // block's actual contents and compare it to the header's
        // `utreexo_root`. If they disagree, reject before the block
        // enters storage.
        //
        // This check USED to live inside `ConnectBlock` (gated on
        // `updateTip=true`), but the submitblock path below passes
        // `updateTip=false` to defer canonical tip writes — which
        // meant the check never ran on externally-submitted blocks.
        // The old `coinbase-modified-after-template` early-reject in
        // `rpc_submitblock_v14` used to catch tampered roots
        // incidentally; it was removed to unblock SV2-JD, which
        // exposed the missing check on this path.
        //
        // `ComputeUtreexoRootPure` is documented as a pure function
        // (temp forest snapshot, no state mutation), so running it
        // here — before ConnectBlock — does not conflict with the
        // "canonical state mutated only by ConnectTip" invariant.
        //
        // Main chain: use the live forest + ComputeUtreexoRootPure.
        //
        // Side chain whose parent IS in the main chain: build a
        // fork-aware UTXO overlay by walking main-chain undo data
        // from the current tip back to the parent's height,
        // restoring spent UTXOs and removing created ones. Then
        // load the utreexo checkpoint at the parent's height as the
        // starting forest and call ComputeUtreexoRootPureFromForest
        // with the overlay-backed lookup. This covers both
        // coinbase-only and multi-tx side-chain blocks.
        //
        // Side chain whose parent is ITSELF on a fork: we don't
        // have a cheap utreexo checkpoint for that parent, so we
        // skip the accept-time check and rely on the reorg-time
        // backstop (ConnectTip → block_validator_->ConnectBlock →
        // ValidateAndApplyBlock → ConnectBlockInternal(
        // verify_root=true) at consensus/block_validation.cpp:
        // 1668-1705).
        {
            auto* dctx = DaemonContext::instance();
            auto cs = std::dynamic_pointer_cast<dinero::ChainstateService>(
                dctx ? dctx->chainstate : nullptr);
            auto* bv = cs ? cs->GetBlockValidator() : nullptr;
            auto* chain_db_for_utreexo = cs ? cs->GetChainDB() : nullptr;
            const bool skip_utreexo_rpc = bv &&
                bv->getValidationMode() == consensus::ValidationMode::STATELESS;

            std::optional<dinero::uint256> computed_root_opt;
            std::string utreexo_error;

            if (bv && !skip_utreexo_rpc) {
                dinero::Block consensus_block = ConvertParsedBlockToBlock(block);

                if (isMainChainExtension) {
                    dinero::uint256 computed_root;
                    if (bv->ComputeUtreexoRootPure(consensus_block,
                                                   static_cast<uint32_t>(newHeight),
                                                   computed_root,
                                                   utreexo_error)) {
                        computed_root_opt = computed_root;
                    } else {
                        LOG_ERROR("⚠️  ComputeUtreexoRootPure failed on submitblock path: " +
                                  utreexo_error);
                    }
                } else if (chain_db_for_utreexo &&
                           !(cs->IsAssumeUTXOActive() &&
                             parentHeight < cs->GetAssumeUTXOBaseHeight())) {
                    // Side-chain. Confirm parent is on main chain.
                    //
                    // While AssumeUTXO background validation is replaying
                    // genesis..base, those historical bodies also traverse
                    // BlockAcceptor as non-tip blocks. Pre-base checkpoints
                    // and delta sidecars are deliberately not promoted into
                    // ChainDB until that replay proves the snapshot. Do not
                    // misroute them through this live side-chain precheck:
                    // AssumeUtxoReplayEngine verifies every historical root,
                    // and the below-base fork guard prevents activation.
                    const dinero::uint256 parent_hash =
                        dinero::uint256::FromHexUnsafe(block.prevBlockHash);
                    dinero::uint256 canonical_parent;
                    const bool parent_in_main_chain =
                        cs->ResolveCanonicalBlockHash(
                            static_cast<uint32_t>(parentHeight), canonical_parent) &&
                        canonical_parent == parent_hash;

                    if (parent_in_main_chain) {
                        consensus::UtreexoForest parent_forest;
                        std::string restore_error;
                        const dinero::storage::BlockHashAtHeightResolver resolve_canonical =
                            [cs](uint32_t height, dinero::uint256& out_hash) -> bool {
                                return cs->ResolveCanonicalBlockHash(height, out_hash);
                            };
                        const auto restore_status =
                            dinero::storage::RestoreHistoricalForest(
                                *chain_db_for_utreexo,
                                static_cast<uint32_t>(parentHeight),
                                parent_forest,
                                restore_error,
                                resolve_canonical);
                        if (restore_status == dinero::Status::Ok) {
                            try {
                                // Build a fork-aware UTXO overlay by
                                // walking main-chain undo data from the
                                // current tip back to `parentHeight`.
                                // For each main-chain block in that
                                // window: restore spent outputs (they
                                // existed at fork point) and remove
                                // created outputs (they did not).
                                std::unordered_map<dinero::OutPoint,
                                                   consensus::UTXOEntry> overlay_restored;
                                std::unordered_set<dinero::OutPoint> overlay_removed;

                                const uint32_t tip_h =
                                    dinero::storage::GetChainHeight(chain_db_for_utreexo);
                                bool overlay_ok = true;
                                for (uint32_t h = tip_h;
                                     h > static_cast<uint32_t>(parentHeight);
                                     --h) {
                                    dinero::uint256 mainchain_hash;
                                    if (!cs->ResolveCanonicalBlockHash(h, mainchain_hash)) {
                                        LOG_ERROR(
                                            "⚠️  fork-aware overlay: missing main-chain "
                                            "hash at height " +
                                            std::to_string(h));
                                        overlay_ok = false;
                                        break;
                                    }
                                    // Archival mode skips the ChainDB undo shadow
                                    // write, so ReadStoredUndoPublic (flatfile)
                                    // is the reliable path in both modes.
                                    auto undo_res = cs->ReadStoredUndoPublic(mainchain_hash);
                                    if (undo_res.status() != dinero::Status::Ok) {
                                        LOG_ERROR(
                                            "⚠️  fork-aware overlay: missing undo at "
                                            "height " +
                                            std::to_string(h));
                                        overlay_ok = false;
                                        break;
                                    }
                                    const dinero::UndoRecord& u = undo_res.value();

                                    for (const auto& s : u.spent) {
                                        dinero::OutPoint op(dinero::TxId(s.prev_txid),
                                                            s.prev_vout);
                                        overlay_removed.erase(op);
                                        consensus::UTXOEntry entry(
                                            dinero::AmountUna::Una(s.value),
                                            s.scriptPubKey,
                                            s.height,
                                            s.is_coinbase,
                                            s.is_confidential,
                                            s.commitment);
                                        overlay_restored.insert_or_assign(op, entry);
                                    }
                                    for (const auto& c : u.created) {
                                        dinero::OutPoint op(dinero::TxId(c.txid), c.vout);
                                        overlay_restored.erase(op);
                                        overlay_removed.insert(op);
                                    }
                                }

                                if (overlay_ok) {
                                    auto* live_utxo = bv->GetConsensusUTXOSet();
                                    auto lookup = [&overlay_restored, &overlay_removed,
                                                   live_utxo](const dinero::OutPoint& op)
                                        -> const consensus::UTXOEntry* {
                                        auto rit = overlay_restored.find(op);
                                        if (rit != overlay_restored.end()) {
                                            return &rit->second;
                                        }
                                        if (overlay_removed.count(op)) {
                                            return nullptr;
                                        }
                                        return live_utxo ? live_utxo->GetCoin(op) : nullptr;
                                    };

                                    dinero::uint256 computed_root;
                                    if (bv->ComputeUtreexoRootPureFromForest(
                                            consensus_block,
                                            static_cast<uint32_t>(newHeight),
                                            std::move(parent_forest),
                                            lookup,
                                            computed_root,
                                            utreexo_error)) {
                                        computed_root_opt = computed_root;
                                    } else {
                                        LOG_ERROR("⚠️  "
                                                  "ComputeUtreexoRootPureFromForest "
                                                  "failed: " +
                                                  utreexo_error);
                                    }
                                }
                            } catch (const std::exception& e) {
                                LOG_ERROR("⚠️  Failed to use restored Utreexo forest "
                                          "at height " +
                                          std::to_string(parentHeight) + ": " + e.what());
                            }
                        } else {
                            LOG_ERROR("⚠️  Failed to restore Utreexo forest at height " +
                                      std::to_string(parentHeight) + ": " +
                                      restore_error);
                        }
                    }
                }
            }

            if (computed_root_opt.has_value()) {
                dinero::uint256 header_root;
                if (block.utreexoRoot.size() == 64) {
                    dinero::uint256::FromHex(block.utreexoRoot, header_root);
                }
                if (computed_root_opt.value() != header_root) {
                    std::string err = "bad-utreexo-root: computed=" +
                        computed_root_opt->GetHex().substr(0, 16) +
                        "... header=" + header_root.GetHex().substr(0, 16) + "...";
                    LOG_ERROR(std::string("❌ UTREEXO VALIDATION FAILED (accept time, ") +
                              (isMainChainExtension ? "main-chain" : "side-chain") +
                              "): " + err);
                    dinero::metrics::MetricsRegistry::IncrementBlocksRejected(
                        "bad-utreexo-root");
                    return BlockAcceptResult::Rejected(
                        BlockRejectCode::INVALID_UTREEXO_ROOT, err, block_hash, newHeight);
                }
            }
        }

        // 6. Connect block metadata to database.
        // Canonical consensus state MUST be mutated only by ActivateBestChain->ConnectTip.
        // Passing updateTip=false here prevents pre-activation tip/UTXO contamination.
        std::cout << "[ACCEPTOR-DEBUG] Step 6: Connecting block to database at height " << newHeight << "..." << std::endl;
        if (!ConnectBlock(block, newHeight, parentChainwork, error, false)) {
            std::cout << "[ACCEPTOR-DEBUG] REJECTED: Connect failed - " << error << std::endl;
            dinero::metrics::MetricsRegistry::IncrementBlocksRejected("connect-failed");
            return BlockAcceptResult::Rejected(BlockRejectCode::CONNECT_FAILED, error, block_hash, newHeight);
        }
        std::cout << "[ACCEPTOR-DEBUG] Block connected successfully!" << std::endl;

        // Phase 40: Trigger chain activation after successful block acceptance
        // BEFORE NotifyBlockConnected so relay gating sees the real active tip.
        // This notifies services and (in Phase 41) will trigger reorg logic
        if (ctx_ && ctx_->chainstate) {
            auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
            if (chainstate) {
                chainstate->ActivateBestChain();
            }
        }

        // 7. Notify subsystems after activation.
        // This ensures canonical blocks are announced once they are active tip.
        std::cout << "[ACCEPTOR-DEBUG] Step 7: Notifying subsystems..." << std::endl;
        NotifyBlockConnected(block, newHeight);

        // Accepted result should always report the incoming block hash.
        // Canonical height-index may legitimately lag until ActivateBestChain runs.
        uint256 final_block_hash = block_hash;

        // Success - create accepted result with correct hash
        std::cout << "[ACCEPTOR-DEBUG] >>> BLOCK ACCEPTED at height " << newHeight
                  << " hash=" << final_block_hash.GetHex().substr(0, 16) << "..." << std::endl;
        BlockAcceptResult result = BlockAcceptResult::Accepted(final_block_hash, newHeight, true);

        // Update metrics on successful acceptance
        dinero::metrics::MetricsRegistry::IncrementBlocksAccepted();
        if (isMainChainExtension) {
            dinero::metrics::MetricsRegistry::SetChainHeight(newHeight);
        }

        // ========== IBD Detection and Sync Progress Broadcasting ==========
        // Detect Initial Block Download state and calculate sync progress
        {
            // Static variables to track sync state
            static std::chrono::steady_clock::time_point last_block_time = std::chrono::steady_clock::now();
            static std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>> sync_history; // (time, height) pairs
            static constexpr size_t MAX_HISTORY = 20; // Track last 20 blocks for sync rate calculation

            auto now = std::chrono::steady_clock::now();
            uint64_t current_time_unix = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

            // Add current block to sync history
            sync_history.push_back({now, newHeight});
            if (sync_history.size() > MAX_HISTORY) {
                sync_history.pop_front();
            }

            // Calculate sync rate (blocks per second) from history
            double sync_rate = 0.0;
            if (sync_history.size() >= 2) {
                auto& oldest = sync_history.front();
                auto& newest = sync_history.back();
                auto time_diff = std::chrono::duration_cast<std::chrono::seconds>(newest.first - oldest.first).count();
                if (time_diff > 0) {
                    uint64_t height_diff = newest.second - oldest.second;
                    sync_rate = static_cast<double>(height_diff) / static_cast<double>(time_diff);
                }
            }

            // IBD Detection: We're in IBD if block timestamp is significantly old (> 3600 seconds behind)
            // This means we're syncing historical blocks, not near the chain tip
            constexpr uint64_t IBD_THRESHOLD_SECONDS = 3600; // 1 hour
            uint64_t time_behind = (current_time_unix > block.timestamp) ? (current_time_unix - block.timestamp) : 0;
            bool is_ibd = (time_behind > IBD_THRESHOLD_SECONDS);

            // Calculate progress and ETA
            double progress = 1.0;
            int eta_seconds = 0;

            if (is_ibd && time_behind > 0) {
                // Progress calculation: How far have we caught up?
                // Assume genesis was launched around Jan 2025 (adjust if needed)
                // For now, use a simple heuristic: progress = (block_time - genesis_approx) / (current_time - genesis_approx)
                constexpr uint64_t GENESIS_APPROX_TIME = 1704067200; // Jan 1, 2024 00:00:00 UTC (approximate)
                uint64_t total_time_span = current_time_unix - GENESIS_APPROX_TIME;
                uint64_t synced_time_span = (block.timestamp > GENESIS_APPROX_TIME) ? (block.timestamp - GENESIS_APPROX_TIME) : 0;

                if (total_time_span > 0) {
                    progress = std::clamp(static_cast<double>(synced_time_span) / static_cast<double>(total_time_span), 0.0, 1.0);
                }

                // ETA calculation: time_behind / sync_rate (in seconds per block)
                if (sync_rate > 0.01) { // Avoid division by near-zero
                    // Estimate remaining blocks: We need to catch up "time_behind" seconds
                    // Assume blocks come every ~120 seconds on average (2 minutes)
                    constexpr double AVG_BLOCK_TIME_SEC = 120.0;
                    double remaining_blocks = time_behind / AVG_BLOCK_TIME_SEC;
                    eta_seconds = static_cast<int>(remaining_blocks / sync_rate);

                    // Cap ETA at 7 days to avoid absurd values
                    constexpr int MAX_ETA = 7 * 24 * 3600;
                    eta_seconds = std::min(eta_seconds, MAX_ETA);
                }
            }

            // Broadcast sync progress to GUI
            dinero_daemon::gui_events::BroadcastSyncProgress(is_ibd, progress, eta_seconds);

            // Update tracking variables
            last_block_time = now;
        }
        // ========== End IBD Detection ==========

        LOG_INFO("✅ Block accepted at height " + std::to_string(newHeight) + ", hash: " + block.blockHash.substr(0, 16) + "...");

        return result;

    } catch (const std::exception& e) {
        std::string error_message = std::string("Exception: ") + e.what();
        std::cout << "[ACCEPTOR-DEBUG] >>> EXCEPTION: " << error_message << std::endl;

        // Update metrics on rejection
        dinero::metrics::MetricsRegistry::IncrementBlocksRejected("parse-error");

        LOG_ERROR("❌ BlockAcceptor exception: " + error_message);
        return BlockAcceptResult::Rejected(BlockRejectCode::PARSE_ERROR, error_message);
    }
}

AcceptResult BlockAcceptor::AcceptBlockFromPeer(const Block& block, const std::string& peer_id) {
    // Serialize the Block object to binary format
    std::string blockBinary = block.Serialize();

    // FIX: Convert binary bytes to hex string for AcceptBlockFromRPC
    // Block::Serialize() returns raw binary bytes, not hex
    std::string blockHex = BytesToHex(
        reinterpret_cast<const uint8_t*>(blockBinary.data()),
        blockBinary.size()
    );

    // Route to standard acceptance flow with peer source attribution
    std::string source = "peer:" + peer_id;
    return AcceptBlockFromRPC(blockHex, source);
}

ParsedBlock BlockAcceptor::ParseBlockFromHex(const std::string& blockHex) {
    if (blockHex.length() % 2 != 0) {
        throw std::runtime_error("Invalid hex string length");
    }
    
    std::vector<uint8_t> blockBytes = HexToBytes(blockHex);
    const size_t HEADER_V1_SIZE = 128;  // Phase W.1.1: New 128-byte header format
    if (blockBytes.size() < HEADER_V1_SIZE + 1) { // 128-byte header + 1-byte tx count minimum
        throw std::runtime_error("Block too small (need " + std::to_string(HEADER_V1_SIZE + 1) +
                                " bytes, got " + std::to_string(blockBytes.size()) + ")");
    }

    ParsedBlock block;
    const uint8_t* data = blockBytes.data();
    size_t offset = 0;

    // Parse block header (128 bytes - Dinero BlockHeader v1)
    // Phase W.1.1 fix: Updated to match new 128-byte header layout
    // Layout (see primitives/block.cpp:SerializeForHash):
    //   0x00 (4 bytes):  version
    //   0x04 (32 bytes): prev_block_hash
    //   0x24 (32 bytes): merkle_root
    //   0x44 (32 bytes): utreexo_root
    //   0x64 (8 bytes):  timestamp
    //   0x6C (4 bytes):  difficulty
    //   0x70 (4 bytes):  nonce
    //   0x74 (12 bytes): reserved

    // Offset 0: version (4 bytes)
    block.version = ReadUint32LE(data + offset); offset += 4;

    // Offset 4: Previous block hash (32 bytes, reverse for display)
    std::vector<uint8_t> prevHash(data + offset, data + offset + 32); offset += 32;
    std::reverse(prevHash.begin(), prevHash.end());
    block.prevBlockHash = BytesToHex(prevHash.data(), 32);

    // Offset 36: Merkle root (32 bytes, reverse for display)
    std::vector<uint8_t> merkleRoot(data + offset, data + offset + 32); offset += 32;
    std::reverse(merkleRoot.begin(), merkleRoot.end());
    block.merkleRoot = BytesToHex(merkleRoot.data(), 32);

    // Offset 68: Utreexo commitment (32 bytes) - AFTER-state Utreexo root
    std::vector<uint8_t> utreexoRoot(data + offset, data + offset + 32);
    std::reverse(utreexoRoot.begin(), utreexoRoot.end());
    block.utreexoRoot = BytesToHex(utreexoRoot.data(), 32);
    offset += 32;

    // Offset 100: timestamp (8 bytes) - uint64_t in new format
    uint64_t ts = 0;
    for (int i = 0; i < 8; i++) {
        ts |= (static_cast<uint64_t>(data[offset + i]) << (i * 8));
    }
    block.timestamp = static_cast<uint32_t>(ts);  // Store as uint32 for compatibility
    offset += 8;

    // Offset 108: bits/difficulty (4 bytes)
    block.bits = ReadUint32LE(data + offset); offset += 4;

    // Offset 112: nonce (4 bytes)
    block.nonce = ReadUint32LE(data + offset); offset += 4;

    // Offset 116: reserved (12 bytes) - skip
    offset += 12;

    // Compute block hash (double SHA-256 of ALL 128 header bytes)
    std::string headerBytes(reinterpret_cast<const char*>(blockBytes.data()), HEADER_V1_SIZE);

    // [DEBUG] Dump header bytes in validator (for miner vs validator comparison)
    std::string header_hex_validator;
    header_hex_validator.reserve(HEADER_V1_SIZE * 2);
    for (size_t i = 0; i < HEADER_V1_SIZE; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", (unsigned char)headerBytes[i]);
        header_hex_validator += buf;
    }
    LOG_INFO("[VALIDATOR] header_bytes_128 = " + header_hex_validator);

    // Compute block hash as canonical uint256 from raw wire header bytes.
    // Uses crypto::CSHA256 — same implementation as BlockHeader::GetHash().
    // Result stored as raw uint256 (consensus) + display-order hex (logging).
    {
        uint8_t h1[32], h2[32];
        dinero::crypto::CSHA256()
            .Write(reinterpret_cast<const uint8_t*>(blockBytes.data()), HEADER_V1_SIZE)
            .Finalize(h1);
        dinero::crypto::CSHA256().Write(h1, 32).Finalize(h2);
        // h2 is big-endian SHA-256 output (MSB at h2[0]).
        // uint256 uses LE storage (data[0]=LSB, data[31]=MSB).
        // Reverse bytes so operator< and GetHex() work correctly.
        for (int i = 0; i < 32; ++i)
            block.blockHashRaw.data[i] = h2[31 - i];
        block.blockHash = block.blockHashRaw.GetHex();  // Display-order for logging/PoW compat
    }

    // [DEBUG] Log computed hash
    LOG_INFO("[VALIDATOR] hash_computed = " + block.blockHash);
    
    // Parse transaction count (variable length integer, simplified to 1 byte)
    // Parse transactions from block data
    if (offset >= blockBytes.size()) {
        throw std::runtime_error("Block data too short for transaction count");
    }
    
    // Read transaction count (varint)
    uint64_t txCount = 0;
    if (!ReadVarInt(data, blockBytes.size(), offset, txCount)) {
        throw std::runtime_error("Failed to read transaction count");
    }
    
    LOG_INFO("📋 Parsing " + std::to_string(txCount) + " transactions");
    
    // Parse each transaction
    block.transactions.clear();
    for (uint64_t i = 0; i < txCount; ++i) {
        if (offset >= blockBytes.size()) {
            throw std::runtime_error("Block data too short for transaction " + std::to_string(i));
        }

        // Record starting position
        size_t tx_start = offset;

        // Parse transaction (this advances offset)
        dinero::Transaction tx;
        if (!ParseTransaction(data, blockBytes.size(), offset, tx)) {
            throw std::runtime_error("Failed to parse transaction " + std::to_string(i));
        }

        // Use EXACT bytes from block stream (no serialize round-trip)
        size_t tx_len = offset - tx_start;
        std::string txHex = BytesToHex(data + tx_start, tx_len);
        block.transactions.push_back(txHex);

        LOG_INFO("✅ Parsed transaction " + std::to_string(i) + " (" + std::to_string(tx_len) + " bytes): " +
                 tx.GetTxid().AsUint256().GetHex().substr(0, 16) + "...");  // Phase M.0: Convert to hex first
    }
    
    block.txCount = txCount;
    LOG_INFO("✅ Successfully parsed " + std::to_string(txCount) + " transactions");

    // ════════════════════════════════════════════════════════════════════
    // PARSE OPTIONAL UTREEXO DATA (after transactions)
    // ════════════════════════════════════════════════════════════════════
    // Format: 1 byte flag + optional BlockUtreexoData
    // 0x00 = no Utreexo data (backward compatibility)
    // 0x01 = has Utreexo data
    if (offset < blockBytes.size()) {
        uint8_t utreexo_flag = data[offset++];
        if (utreexo_flag == 0x01) {
            // Parse BlockUtreexoData
            std::vector<uint8_t> remaining(data + offset, data + blockBytes.size());
            try {
                block.utreexo_data = dinero::consensus::BlockUtreexoData::deserialize(remaining);
                LOG_INFO("✅ Parsed Utreexo data: " +
                        std::to_string(block.utreexo_data->spent_outputs.size()) + " spent outputs");
            } catch (const std::exception& e) {
                LOG_ERROR("⚠️ Failed to parse Utreexo data: " + std::string(e.what()));
                // Continue without utreexo data - validation will fail if needed
            }
        } else if (utreexo_flag != 0x00) {
            LOG_ERROR("⚠️ Invalid Utreexo flag: 0x" + BytesToHex(&utreexo_flag, 1));
        }
    }

    block.blockSize = blockBytes.size();

    return block;
}

bool BlockAcceptor::ValidateBlockHeader(const ParsedBlock& block, std::string& error) {
    LOG_INFO("🔍 Header validation: version=" + std::to_string(block.version) +
             ", bits=" + std::to_string(block.bits) +
             ", timestamp=" + std::to_string(block.timestamp) +
             ", prevhash=" + block.prevBlockHash.substr(0, 16) + "..." +
             ", utreexo=" + block.utreexoRoot.substr(0, 16) + "...");

    // Basic header validation
    if (block.version == 0) {
        error = "bad-version";
        LOG_ERROR("❌ Header validation failed: " + error);
        return false;
    }

    if (block.prevBlockHash.length() != 64) {
        error = "bad-prevblk";
        LOG_ERROR("❌ Header validation failed: " + error + " (length=" + std::to_string(block.prevBlockHash.length()) + ")");
        return false;
    }

    if (block.merkleRoot.length() != 64) {
        error = "bad-merkleroot";
        LOG_ERROR("❌ Header validation failed: " + error + " (length=" + std::to_string(block.merkleRoot.length()) + ")");
        return false;
    }

    // Validate Utreexo commitment (32 bytes = 64 hex chars)
    if (block.utreexoRoot.length() != 64) {
        error = "bad-utreexo-commitment";
        LOG_ERROR("❌ Header validation failed: " + error + " (length=" + std::to_string(block.utreexoRoot.length()) + ")");
        return false;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // MAINNET READINESS: Header Size Enforcement (Consensus-Critical)
    // ═══════════════════════════════════════════════════════════════════════════
    // Reject blocks with incorrect header size to prevent legacy miners (80-byte)
    // from submitting blocks. This is the final safety gate against format errors.
    //
    // Dinero uses 128-byte headers (BlockHeader v1 - CONSENSUS-FINAL)
    // Any deviation indicates:
    //   - Legacy Bitcoin miner (80 bytes)
    //   - Old transitional format (112 bytes)
    //   - Broken GPU miner (if not rebuilt)
    //   - Corrupted Stratum pool
    //
    // This check must execute BEFORE PoW verification to fail fast.
    // See: CONSENSUS_LOCK.md for why this is immutable
    // ═══════════════════════════════════════════════════════════════════════════
    {
        // Phase M.4: Use ToBlockHeader() for clean boundary conversion
        BlockHeader header = ToBlockHeader(block);

        auto serialized_header = header.SerializeForHash();
        if (serialized_header.size() != DINERO_HEADER_SIZE_BYTES) {
            error = "bad-header-size";
            LOG_ERROR("❌ Header validation failed: " + error +
                     " (got=" + std::to_string(serialized_header.size()) +
                     ", expected=" + std::to_string(DINERO_HEADER_SIZE_BYTES) + ")");
            return false;
        }

        LOG_INFO("✅ Header size validated: " + std::to_string(DINERO_HEADER_SIZE_BYTES) + " bytes");

        // ═══════════════════════════════════════════════════════════════════════════
        // BlockHeader v1: Reserved Field Validation (Consensus-Critical)
        // ═══════════════════════════════════════════════════════════════════════════
        // BlockHeader v1 requires reserved[12] to be all zeros.
        // Any non-zero byte in reserved field makes the block invalid.
        // This is a hard consensus rule and cannot be softened without a hard fork.
        //
        // Why this matters:
        //   - Prevents protocol pollution (no accidental data in reserved space)
        //   - Ensures future extensibility (reserved field can be repurposed cleanly)
        //   - Enforces clean header format (no undefined behavior)
        //
        // See: docs/BLOCKHEADER_V1_FINALIZATION_PLAN.md
        // ═══════════════════════════════════════════════════════════════════════════
        if (!header.IsReservedValid()) {
            error = "bad-header-reserved";
            LOG_ERROR("❌ Header validation failed: " + error +
                     " (reserved field contains non-zero bytes - BlockHeader v1 violation)");
            return false;
        }

        LOG_INFO("✅ Reserved field validated: all zeros (BlockHeader v1)");
    }

    // NOTE: PoW validation moved to AcceptBlockFromRPC AFTER FindParentBlock
    // This ensures we use the correct block height for difficulty calculation
    // during P2P sync (when receiving blocks ahead of our local tip)

    // Validate timestamp
    if (!ValidateTimestamp(block, error)) {
        LOG_ERROR("❌ Header validation failed: " + error);
        return false;
    }

    LOG_INFO("✅ Header validation passed");
    return true;
}

bool BlockAcceptor::ValidateProofOfWork(const ParsedBlock& block, uint32_t blockHeight, std::string& error, const std::string& parentHashHex) {
    try {
        // ═══════════════════════════════════════════════════════════════════════════
        // PATH A: Regtest PoW Skip (Phase W.1.1)
        // ═══════════════════════════════════════════════════════════════════════════
        // In regtest mode, blocks are mined deterministically (nonce=0, instant).
        // Skip PoW validation entirely to allow instant block generation for testing.
        // This matches Bitcoin Core's regtest behavior where PoW is not enforced.
        // ═══════════════════════════════════════════════════════════════════════════
        const auto& chain_params = dinero::Params();
        if (chain_params.name == "regtest") {
            LOG_INFO("✅ [PATH A] Skipping PoW validation for regtest block");
            return true;  // Accept without PoW check
        }

        // P2P FIX: Use the explicitly passed blockHeight instead of local chain height
        // This is critical for P2P sync where we receive blocks ahead of our local tip
        uint32_t nextHeight = blockHeight;
        std::cout << "[POW-DEBUG] Validating PoW for block at height " << nextHeight << std::endl;

        // Create consensus parameters
        Consensus consensus;

        // ✅ CRITICAL FIX: Apply regtest override (must match GBT in block_assembler.cpp)
        // This ensures validator uses same params as block template generator
        if (dinero::Params().name != "mainnet") {
            uint32_t network_pow_limit = dinero::Params().pow_limit_bits;  // 0x207fffff for regtest
            consensus.genesisBits = network_pow_limit;
            consensus.asertAnchorBits = network_pow_limit;
            consensus.powLimitBits = network_pow_limit;
        }

        uint32_t requiredBits = 0;

        // Special case: Genesis block (height 0)
        // Genesis uses the pre-mined difficulty from chainparams
        if (nextHeight == 0) {
            // For genesis, we'd need to check chainparams
            // For now, accept any genesis block (validated elsewhere)
            requiredBits = block.bits;
            LOG_INFO("📊 Genesis block: accepting bits " + dinero::hex32_0x(requiredBits));
        }
        // Block 1+: Use unified difficulty calculation (matches GetBlockTemplate)
        else {
            auto* daemon_ctx = DaemonContext::instance();
            auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(
                (daemon_ctx && daemon_ctx->chainstate) ? daemon_ctx->chainstate : nullptr);
            auto* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
            const dinero::uint256 pow_parent_hash = dinero::uint256::FromHexUnsafe(parentHashHex);
            auto* parent_index = chainstate ? chainstate->FindBlockIndex(pow_parent_hash) : nullptr;
            int64_t known_parent_mtp = 0;
            int64_t known_block1_time = 0;
            if (parent_index) {
                known_parent_mtp = static_cast<int64_t>(parent_index->GetMedianTimePast());
                known_block1_time = dinero::GetKnownAncestryTimestamp(
                    parent_index,
                    /*parent_entry=*/nullptr,
                    1);
            } else if (daemon_ctx && daemon_ctx->header_chain) {
                // #441: derive both ancestry-dependent values inside the
                // selector lock. A raw HeaderIndexEntry pointer here could be
                // freed by side-branch eviction during the MTP/anchor walk.
                dinero::consensus::HeaderAsertContext header_context;
                if (daemon_ctx->header_chain->GetAsertContextByHash(
                        pow_parent_hash, header_context)) {
                    if (header_context.parent_height + 1 != nextHeight) {
                        error = "bad-diffbits: header parent height " +
                            std::to_string(header_context.parent_height) +
                            " does not precede candidate height " +
                            std::to_string(nextHeight);
                        LOG_ERROR("❌ " + error);
                        return false;
                    }
                    known_parent_mtp = header_context.parent_mtp;
                    known_block1_time = header_context.block1_time;
                }
            }

            const auto asert_input = dinero::BuildAsertInputForCandidateTimes(
                known_parent_mtp,
                known_block1_time,
                chain_db,
                nextHeight,
                static_cast<int64_t>(block.timestamp),
                consensus);

            if (asert_input.has_value()) {
                LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
                LOG_INFO("⚖️  CONSENSUS ASERT INPUT:");
                LOG_INFO("  target_height:      " + std::to_string(asert_input->target_height));
                LOG_INFO("  reference_time:     " + std::to_string(asert_input->reference_time));
                LOG_INFO("  anchor.height:      " + std::to_string(asert_input->anchor.height));
                LOG_INFO("  anchor.time:        " + std::to_string(asert_input->anchor.time));
                LOG_INFO("  anchor.bits:        " + dinero::hex32_0x(asert_input->anchor.bits));
                LOG_INFO("  block.timestamp:    " + std::to_string(block.timestamp));
                LOG_INFO("  block.bits:         " + dinero::hex32_0x(block.bits));
                LOG_INFO("  consensus.genesisBits: " + dinero::hex32_0x(consensus.genesisBits));
                LOG_INFO("  consensus.powLimitBits:  " + dinero::hex32_0x(consensus.powLimitBits));

                requiredBits = ComputeAsertBits(*asert_input);

                LOG_INFO("⚖️  CONSENSUS ASERT OUTPUT:");
                LOG_INFO("  calculated_bits:    " + dinero::hex32_0x(requiredBits));
                LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
                LOG_INFO("📊 Difficulty calculated for height " + std::to_string(nextHeight) +
                         ": " + dinero::hex32_0x(requiredBits));
            }
        }

        // Guard: if we couldn't compute difficulty (no chain DB), reject
        if (requiredBits == 0 && nextHeight > 0) {
            error = "bad-diffbits: unable to compute required difficulty for height " + std::to_string(nextHeight);
            LOG_ERROR("❌ " + error);
            return false;
        }

        // Validate that block's difficulty bits match required bits
        if (block.bits != requiredBits) {
            error = "bad-diffbits: block has " + dinero::hex32_0x(block.bits) +
                    ", required " + dinero::hex32_0x(requiredBits);
            LOG_ERROR("❌ " + error);
            return false;
        }

        // Convert bits to target (big-endian byte array from TargetFromBitsBE)
        auto targetBE = dinero::TargetFromBits(requiredBits);

        // Build target as uint256 (LE storage): reverse BE array into uint256.data
        // uint256.data[0]=LSB, data[31]=MSB; targetBE[0]=MSB, targetBE[31]=LSB
        dinero::uint256 targetU;
        for (int i = 0; i < 32; ++i)
            targetU.data[i] = targetBE[31 - i];

        // Use blockHashRaw (uint256) — the canonical wire-bytes hash.
        // NEVER derive the comparison value from the display string.
        const dinero::uint256& hashU = block.blockHashRaw;

        // DEBUG: both uint256::GetHex() output display-order (MSB first) for human reading
        std::cout << "[POW-DEBUG] =====================================" << std::endl;
        std::cout << "[POW-DEBUG] Hash  (uint256): " << hashU.GetHex() << std::endl;
        std::cout << "[POW-DEBUG] Target(uint256): " << targetU.GetHex() << std::endl;
        std::cout << "[POW-DEBUG] Block bits: " << dinero::hex32_0x(block.bits)
                  << "  Required: " << dinero::hex32_0x(requiredBits) << std::endl;
        std::cout << "[POW-DEBUG] =====================================" << std::endl;

        LOG_INFO("PoW check: height=" + std::to_string(nextHeight) +
                 ", bits=" + dinero::hex32_0x(block.bits) +
                 " (required=" + dinero::hex32_0x(requiredBits) + ")" +
                 ", hash=" + hashU.GetHex().substr(0, 16) + "...");

        // uint256::operator< compares from data[31] down (MSB-first),
        // which is the correct 256-bit integer comparison for both
        // LE-stored hash and LE-stored target.
        // Use !(target < hash) ≡ hash <= target — only depends on operator<.
        if (!(targetU < hashU)) {
            LOG_INFO("PoW valid: hash <= target");
            return true;
        }

        error = "bad-pow";
        LOG_ERROR("PoW invalid: hash > target  hash=" + hashU.GetHex() +
                  "  target=" + targetU.GetHex());
        return false;

    } catch (const std::exception& e) {
        error = "bad-bits";
        LOG_ERROR("❌ PoW validation exception: " + std::string(e.what()));
        return false;
    }
}

bool BlockAcceptor::ValidateTimestamp(const ParsedBlock& block, std::string& error) {
    // Week 3: MIGRATED - Now uses ctx_->chainstate instead of dinero::legacy::g_chain_db_direct()
    auto now = std::chrono::system_clock::now();
    auto nowTimestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    // BIP113: Block timestamp must be > Median Time Past (MTP)
    // This prevents timestamp manipulation attacks
    //
    // FORK-AWARE MTP (Consensus-correct):
    // MTP is computed from the block's PARENT chain, not the active chain.
    // This allows proper validation of blocks on competing forks.
    uint32_t median_time_past = 0;
    bool mtp_computed = false;
    const dinero::uint256 parent_hash = dinero::uint256::FromHexUnsafe(block.prevBlockHash);

    // Try fork-aware MTP from BlockIndex first.
    // BlockIndex tracks side-chain parents even when HeaderChainSelector only tracks active headers.
    auto* daemon_ctx = DaemonContext::instance();
    if (daemon_ctx && daemon_ctx->chainstate) {
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(daemon_ctx->chainstate);
        if (chainstate) {
            if (auto* parent_index = chainstate->FindBlockIndex(parent_hash)) {
                median_time_past = static_cast<uint32_t>(parent_index->GetMedianTimePast());
                mtp_computed = true;
                LOG_INFO("⏰ [FORK-AWARE/BLOCKINDEX] MTP=" + std::to_string(median_time_past) +
                         " (from parent height=" + std::to_string(parent_index->height) + ")");
            }
        }
    }

    // Fallback 1: HeaderChainSelector
    if (!mtp_computed && daemon_ctx && daemon_ctx->header_chain) {
        // #441: MTP walks the parent chain, which the *Copy accessors cannot
        // expose (they null the copy's parent). Compute it inside the selector,
        // under its lock, instead of dereferencing an escaped pointer.
        uint32_t parent_mtp = 0;
        uint32_t parent_height = 0;
        if (daemon_ctx->header_chain->GetMedianTimePastByHash(parent_hash, parent_mtp,
                                                              parent_height)) {
            // Fork-aware MTP: computed from parent's ancestry
            median_time_past = parent_mtp;
            mtp_computed = true;
            LOG_INFO("⏰ [FORK-AWARE] MTP=" + std::to_string(median_time_past) +
                     " (from parent height=" + std::to_string(parent_height) + ")");
        }
    }

    // Fallback 2: use active chain MTP ONLY when this block extends active tip.
    // Do not validate side-chain/orphan blocks against active-tip MTP.
    if (!mtp_computed && ctx_ && ctx_->chainstate) {
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(daemon_ctx ? daemon_ctx->chainstate : nullptr);
        auto* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
        if (chain_db) {
            auto tip_result = chain_db->getTip();
            if (tip_result.ok()) {
                const auto& tip = tip_result.value();
                if (tip.hash == parent_hash) {
                    bool is_block_1 = (tip.height == 0);
                    if (is_block_1) {
                        const auto& params = Params();
                        median_time_past = params.genesis.nTime;
                        mtp_computed = true;
                        LOG_INFO("⏰ Block 1 validation: Using genesis timestamp as MTP: " + std::to_string(median_time_past));
                    } else {
                        median_time_past = dinero::storage::GetMedianTimePast(chain_db);
                        mtp_computed = true;
                    }
                } else {
                    LOG_INFO("⏰ [MTP-DEFER] Parent is not active tip; skipping active-chain fallback");
                }
            } else {
                const auto& params = Params();
                median_time_past = params.genesis.nTime;
                mtp_computed = true;
            }
        }
    }

    if (mtp_computed) {
        LOG_INFO("⏰ MTP=" + std::to_string(median_time_past) +
                 ", block.time=" + std::to_string(block.timestamp) +
                 ", diff=" + std::to_string(static_cast<int64_t>(block.timestamp) - median_time_past) + "s");

        // Reject if block timestamp ≤ median time past
        if (block.timestamp <= median_time_past) {
            error = "time-too-old (timestamp must be > median time past)";
            LOG_ERROR("❌ Timestamp ≤ MTP: " + std::to_string(block.timestamp) +
                     " ≤ " + std::to_string(median_time_past));
            return false;
        }
    }

    // Also check against network time (max 2 hours in future)
    if (block.timestamp > nowTimestamp + 7200) {
        error = "time-too-new";
        LOG_ERROR("❌ Timestamp too far in future: " + std::to_string(block.timestamp) + 
                 " > " + std::to_string(nowTimestamp + 7200));
        return false;
    }
    
    LOG_INFO("✅ Timestamp validation passed (MTP check enabled)");
    return true;
}

bool BlockAcceptor::FindParentBlock(const std::string& prevHash, uint64_t& parentHeight, std::string& parentChainwork, std::string& error) {
    try {
        // Week 3: MIGRATED - Now uses ctx_->chainstate instead of dinero::legacy::g_chain_db_direct()
        // Get current chain state from RocksDB
        uint32_t height = 0;
        std::string tipHash;
        std::string chainwork;
        auto* daemon_ctx = DaemonContext::instance();
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(daemon_ctx ? daemon_ctx->chainstate : nullptr);
        auto* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;

        if (chain_db) {
            height = dinero::storage::GetChainHeight(chain_db);
            tipHash = dinero::storage::GetBestBlockHash(chain_db);

            // Get chainwork from tip info
            auto tip_result = chain_db->getTip();
            if (tip_result.status() == dinero::Status::Ok) {
                auto tip_info = tip_result.value();
                // Convert arith_uint256 to hex string
                chainwork = tip_info.work.GetHex();
            }
        }

        LOG_INFO("🔗 Parent check (RocksDB): prevHash=" + prevHash.substr(0, 16) + "..." +
                 ", tipHash=" + (tipHash.empty() ? "none" : tipHash.substr(0, 16) + "...") +
                 ", height=" + std::to_string(height));

        // For genesis block (prevhash = all zeros)
        if (prevHash == std::string(64, '0')) {
            LOG_INFO("✅ Genesis block parent (all zeros)");
            parentHeight = 0; // Genesis will be height 1
            parentChainwork = std::string(64, '0'); // Zero chainwork for genesis parent
            return true;
        }

        const uint256 parent_hash = uint256::FromHexUnsafe(prevHash);

        // In AssumeUTXO/header-first replay, ChainDB height indexes can lag far behind the
        // already-loaded parent ancestry. Trust in-memory ancestry first.
        if (chainstate) {
            if (auto* parent_index = chainstate->FindBlockIndex(parent_hash)) {
                parentHeight = parent_index->height;
                parentChainwork = parent_index->chainwork;
                LOG_INFO("✅ Parent found in BlockIndex at height " + std::to_string(parentHeight));
                return true;
            }
        }
        if (daemon_ctx && daemon_ctx->header_chain) {
            // #441: copy under the selector's lock.
            dinero::consensus::HeaderIndexEntry parent_copy{};
            if (daemon_ctx->header_chain->GetHeaderCopy(parent_hash, parent_copy)) {
                parentHeight = parent_copy.height;
                parentChainwork = parent_copy.chainwork.GetHex();
                LOG_INFO("✅ Parent found in HeaderChain at height " + std::to_string(parentHeight));
                return true;
            }
        }

        // If chain is truly empty (no tip known at all), accept the first block.
        if (tipHash.empty()) {
            LOG_INFO("✅ Empty chain, accepting first block");
            parentHeight = 0;
            parentChainwork = std::string(64, '0');
            return true;
        }

        // Check if prevhash matches current tip (fast path - extending main chain)
        if (prevHash == tipHash) {
            parentHeight = height;
            parentChainwork = chainwork.empty() ? std::string(64, '0') : chainwork;
            LOG_INFO("✅ Parent matches tip at height " + std::to_string(parentHeight));
            return true;
        }

        // REORG SUPPORT: Parent doesn't match tip - check if it's a known block
        // This enables fork selection and reorganization to longer chains
        if (chain_db) {
            // Look up the parent block by hash
            auto height_result = chain_db->getBlockHeight(parent_hash);

            if (height_result.status() == dinero::Status::Ok) {
                // Parent exists in our chain - this could be a competing fork
                parentHeight = height_result.value();

                // Get parent's chainwork from header metadata
                auto meta_result = chain_db->getHeaderMetadata(parent_hash);
                if (meta_result.status() == dinero::Status::Ok) {
                    parentChainwork = meta_result.value().chainwork.GetHex();
                } else {
                    // Fallback - chainwork not critical for acceptance, reorg logic will recalculate
                    parentChainwork = std::string(64, '0');
                }

                LOG_INFO("✅ Parent found at height " + std::to_string(parentHeight) +
                        " (competing fork - ActivateBestChain will handle reorg)");
                return true;
            }
        }

        // Parent not found - this is an orphan block (missing parent)
        error = "missing-parent: Block parent " + prevHash.substr(0, 16) + "... not found in chain";
        LOG_INFO("⚠️ " + error + " (will be added to orphan pool)");
        return false;

    } catch (const std::exception& e) {
        error = std::string("Failed to find parent: ") + e.what();
        LOG_ERROR("❌ Parent lookup failed: " + error);
        return false;
    }
}

bool BlockAcceptor::ValidateParentLink(const ParsedBlock& block, uint64_t parentHeight, std::string& error) {
    // For regtest, minimal validation
    // Basic parent link validation for regtest mode
    // Full implementation would validate the complete chain of blocks
    return true;
}

bool BlockAcceptor::ValidateMerkleRoot(const ParsedBlock& block, std::string& error) {
    if (block.transactions.empty()) {
        error = "No transactions in block";
        return false;
    }

    // Phase 11a: Use canonical merkle computation
    // Convert ParsedBlock to Block for proper Transaction objects
    dinero::Block consensus_block = ConvertParsedBlockToBlock(block);

    // Compute merkle root using canonical API
    bool merkle_mutated = false;
    uint256 computed_merkle = dinero::consensus::ComputeMerkleRoot(consensus_block.vtx, &merkle_mutated);
    // CVE-2012-2459: a duplicated subtree forges another valid block's merkle
    // root/hash. A valid block cannot contain a duplicated transaction
    // (double-spend), so this never rejects a valid block; checked before the
    // root comparison because a mutated block's root matches the header.
    if (merkle_mutated) {
        error = "bad-txns-duplicate: duplicated transaction in merkle tree (CVE-2012-2459)";
        LOG_ERROR("❌ " + error);
        return false;
    }
    std::string computedMerkleRoot = computed_merkle.GetHex();

    // Compare with block header merkle root
    if (computedMerkleRoot != block.merkleRoot) {
        error = "Merkle root mismatch: computed=" + computedMerkleRoot + ", header=" + block.merkleRoot;
        LOG_ERROR("❌ " + error);
        return false;
    }

    LOG_INFO("✅ Merkle root validation passed");
    return true;
}

bool BlockAcceptor::ValidateBlockSigops(const ParsedBlock& block, std::string& error) {
    // MAINNET BLOCKER FIX: Enforce MAX_BLOCK_SIGOPS_COST (80,000)
    // Prevents DoS attacks via blocks with excessive signature operations

    if (block.transactions.empty()) {
        error = "No transactions in block for sigops validation";
        return false;
    }

    // Convert ParsedBlock to Block structure for sigops validation
    Block consensus_block = ConvertParsedBlockToBlock(block);

    unsigned int sigop_cost = 0;
    if (!dinero::consensus::CheckBlockSigops(consensus_block, sigop_cost, error)) {
        LOG_ERROR("❌ Block sigops validation failed: " + error);
        return false;
    }

    // Log sigops count for monitoring
    if (sigop_cost > 10000) {
        LOG_INFO("Block sigops: " + std::to_string(sigop_cost) + " / " +
                std::to_string(dinero::consensus::MAX_BLOCK_SIGOPS_COST));
    }

    LOG_INFO("✅ Block sigops validation passed (" + std::to_string(sigop_cost) + " sigops)");
    return true;
}

bool BlockAcceptor::ValidateContextual(const ParsedBlock& block, uint64_t height, std::string& error) {
    if (block.txCount == 0) {
        error = "Block has no transactions";
        return false;
    }

    if (block.transactions.empty()) {
        error = "Missing transaction data";
        return false;
    }

    // Compute total fee budget from non-coinbase transactions.
    // For CT/ring txs (HasExplicitFee): use the committed explicit_fee field.
    // For transparent txs: exact fee requires the coin view (not available here);
    //   use a conservative per-tx budget as an upper bound.
    uint64_t total_explicit_fees = 0;
    size_t transparent_tx_count = 0;

    for (size_t i = 1; i < block.transactions.size(); i++) {
        const std::string& txHex = block.transactions[i];
        if (txHex.empty()) continue;

        std::vector<uint8_t> txBytes = HexToBytes(txHex);
        size_t offset = 0;
        dinero::Transaction tx;
        if (ParseTransaction(txBytes.data(), txBytes.size(), offset, tx)) {
            if (tx.HasExplicitFee()) {
                total_explicit_fees += tx.GetExplicitFee();
            } else {
                transparent_tx_count++;
            }
        }
    }

    // Budget for transparent txs: 10 una/vbyte * 100KB max tx size = 1M una per tx.
    // This is a loose upper bound — exact accounting requires the coin view.
    const uint64_t TRANSPARENT_FEE_PER_TX = 1000000;
    uint64_t total_fee_budget = total_explicit_fees + transparent_tx_count * TRANSPARENT_FEE_PER_TX;

    // Validate coinbase (BIP34 height + subsidy check with computed fee budget)
    if (!ValidateCoinbase(block.transactions[0], height, total_fee_budget, error)) {
        return false;
    }

    return true;
}

bool BlockAcceptor::ValidateCheckpoint(const ParsedBlock& block, uint64_t height, std::string& error) {
    // Get active chain parameters with checkpoints
    const auto& params = dinero::Params();

    // Check if this height has a checkpoint
    auto it = params.vCheckpoints.find(static_cast<uint32_t>(height));
    if (it != params.vCheckpoints.end()) {
        // This height has a checkpoint - verify hash matches
        const std::string& checkpointHash = it->second;

        if (block.blockHash != checkpointHash) {
            error = "Checkpoint mismatch at height " + std::to_string(height) +
                    ": expected " + checkpointHash +
                    ", got " + block.blockHash;
            LOG_ERROR("❌ " + error);
            return false;
        }

        LOG_INFO("✅ Checkpoint validated at height " + std::to_string(height));
    }

    return true;
}

bool BlockAcceptor::ValidateCoinbase(const std::string& coinbaseTx, uint64_t expectedHeight, uint64_t total_fee_budget, std::string& error) {
    if (coinbaseTx.empty()) {
        error = "Empty coinbase transaction";
        return false;
    }
    
    // Parse coinbase transaction to extract height
    std::vector<uint8_t> txBytes = HexToBytes(coinbaseTx);
    size_t offset = 0;
    
    // Skip version (4 bytes)
    if (txBytes.size() < 4) {
        error = "Coinbase transaction too short";
        return false;
    }
    offset += 4;

    // Check for SegWit marker (0x00 0x01) - skip if present
    // SegWit: [version:4][0x00][0x01][input_count:varint]...
    // Legacy: [version:4][input_count:varint]...
    if (offset + 2 <= txBytes.size() && txBytes[offset] == 0x00 && txBytes[offset + 1] == 0x01) {
        offset += 2;  // Skip SegWit marker and flag
    }

    // Read input count (should be 1 for coinbase)
    uint64_t inputCount = 0;
    if (!ReadVarInt(txBytes.data(), txBytes.size(), offset, inputCount) || inputCount != 1) {
        error = "Coinbase must have exactly one input";
        return false;
    }
    
    // Read first input (coinbase input)
    if (offset + 36 > txBytes.size()) {
        error = "Coinbase input too short";
        return false;
    }
    
    // Skip previous output (32 bytes + 4 bytes)
    offset += 36;
    
    // Read script length
    uint64_t scriptLen = 0;
    if (!ReadVarInt(txBytes.data(), txBytes.size(), offset, scriptLen)) {
        error = "Failed to read coinbase script length";
        return false;
    }
    
    // Validate script length (BIP34: height must be encoded in first few bytes)
    if (scriptLen < 2 || scriptLen > 100) {
        error = "Invalid coinbase script length: " + std::to_string(scriptLen);
        return false;
    }
    
    // Read script
    if (offset + scriptLen > txBytes.size()) {
        error = "Coinbase script extends beyond transaction";
        return false;
    }
    
    // Extract height from coinbase script (BIP34)
    uint64_t scriptHeight = 0;
    if (scriptLen >= 2) {
        uint8_t heightLen = txBytes[offset];
        if (heightLen > 0 && heightLen <= 5 && offset + 1 + heightLen <= txBytes.size()) {
            // Read height as little-endian integer
            for (int i = 0; i < heightLen; i++) {
                scriptHeight |= static_cast<uint64_t>(txBytes[offset + 1 + i]) << (i * 8);
            }
        }
    }
    
    // Validate height matches expected height (strict BIP34).
    if (scriptHeight != expectedHeight) {
        error = "BIP34 height mismatch: script=" + std::to_string(scriptHeight) +
                ", expected=" + std::to_string(expectedHeight);
        LOG_ERROR("❌ " + error);
        return false;
    }
    LOG_INFO("✅ BIP34 height validation passed: height=" + std::to_string(expectedHeight));

    // =========================================================================
    // CRITICAL CONSENSUS: Validate coinbase subsidy amount
    // =========================================================================
    // Skip to outputs section: coinbase input script + sequence (4 bytes)
    offset += scriptLen + 4;

    // Read output count
    uint64_t outputCount = 0;
    if (!ReadVarInt(txBytes.data(), txBytes.size(), offset, outputCount) || outputCount == 0) {
        error = "Coinbase must have at least one output";
        return false;
    }

    // Read first output value (amount in una)
    if (offset + 8 > txBytes.size()) {
        error = "Coinbase output too short";
        return false;
    }

    uint64_t coinbase_value = 0;
    for (int i = 0; i < 8; i++) {
        coinbase_value |= static_cast<uint64_t>(txBytes[offset + i]) << (i * 8);
    }

    // Calculate expected subsidy for this height
    uint64_t expected_subsidy = 0;
    if (expectedHeight == 0) {
        // Genesis: 100 DIN (unspendable)
        expected_subsidy = dinero::ConsensusSubsidy::GENESIS_UNSPENDABLE_UNA;
    } else {
        // PoW blocks (height 1+): Standard subsidy with halving + tail emission
        expected_subsidy = dinero::ConsensusSubsidy::GetBlockSubsidy(static_cast<uint32_t>(expectedHeight)).GetUna();
    }

    // Validate: coinbase output must not exceed subsidy + block fees.
    // total_fee_budget is computed by ValidateContextual:
    //   - CT/ring txs: uses explicit_fee field (exact)
    //   - Transparent txs: conservative per-tx estimate (exact requires coin view)
    if (coinbase_value > expected_subsidy + total_fee_budget) {
        error = "Coinbase output (" + std::to_string(coinbase_value) + " una) exceeds maximum subsidy + fees (" +
                std::to_string(expected_subsidy + total_fee_budget) + " una) at height " + std::to_string(expectedHeight);
        LOG_ERROR("❌ " + error);
        return false;
    }

    LOG_INFO("✅ Coinbase subsidy validation passed: " + std::to_string(coinbase_value) +
             " una (subsidy=" + std::to_string(expected_subsidy) +
             " + fees=" + std::to_string(total_fee_budget) + " una)");

    return true;
}

dinero::UndoRecord BlockAcceptor::BuildUndoForBlock(
    const ParsedBlock& block,
    uint64_t height,
    ChainDB* chain_db
) {
    dinero::UndoRecord undo;

    LOG_INFO("📦 Building undo record for block at height " + std::to_string(height) +
             " with " + std::to_string(block.transactions.size()) + " transactions");

    // Process all transactions in block
    for (size_t tx_idx = 0; tx_idx < block.transactions.size(); tx_idx++) {
        std::vector<uint8_t> txBytes = HexToBytes(block.transactions[tx_idx]);
        size_t offset = 0;
        dinero::Transaction tx;

        if (!ParseTransaction(txBytes.data(), txBytes.size(), offset, tx)) {
            LOG_ERROR("⚠️ Failed to parse transaction " + std::to_string(tx_idx) + " for undo record");
            continue;
        }

        TxId txid = tx.GetTxid();  // Phase M.4: GetTxid() returns TxId

        // STEP 1: Record all spent UTXOs (skip coinbase inputs)
        if (tx_idx > 0) {  // Skip coinbase transaction
            for (const auto& input : tx.vin) {
                // Query ChainDB coins CF to get the UTXO being spent
                // Phase M.4: Extract uint256 for ChainDB API
                auto coin_result = chain_db->getCoinWithConfidentialFallback(input.prevout.txid.AsUint256(), input.prevout.vout);

                if (coin_result.ok()) {
                    const auto& coin = coin_result.value();

                    // Save spent UTXO data for restoration during reorg
                    dinero::SpentCoin spent;
                    // Phase M.4: SpentCoin.prev_txid is uint256, extract from TxId
                    spent.prev_txid = input.prevout.txid.AsUint256();
                    spent.prev_vout = input.prevout.vout;
                    spent.value = coin.amount;
                    // Phase M.0: Convert hex string to binary vector
                    std::string hex = coin.script_pubkey;
                    spent.scriptPubKey.reserve(hex.size() / 2);
                    for (size_t i = 0; i < hex.size(); i += 2) {
                        uint8_t byte = static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16));
                        spent.scriptPubKey.push_back(byte);
                    }
                    spent.is_coinbase = coin.coinbase;
                    spent.height = coin.height;
                    spent.is_confidential = coin.is_confidential;
                    spent.commitment = coin.commitment;

                    undo.spent.push_back(spent);

                    LOG_INFO("  💰 Captured spent coin: " + input.prevout.txid.AsUint256().GetHex().substr(0, 16) + "..." +
                             ":" + std::to_string(input.prevout.vout) +
                             " (value=" + std::to_string(coin.amount) + ", height=" + std::to_string(coin.height) + ")");
                } else {
                    LOG_ERROR("⚠️ CRITICAL: Could not find UTXO for input " + input.prevout.txid.AsUint256().GetHex().substr(0, 16) +
                             ":" + std::to_string(input.prevout.vout) + " - this should never happen!");
                    // This is a critical error - the transaction is spending a non-existent UTXO
                    // In production, this would indicate database corruption or consensus failure
                }
            }
        }

        // STEP 2: Record all created outputs
        for (uint32_t vout = 0; vout < tx.vout.size(); vout++) {
            dinero::CreatedOut created;
            // Phase M.4: CreatedOut.txid is uint256, extract from TxId
            created.txid = txid.AsUint256();
            created.vout = vout;
            undo.created.push_back(created);
        }
    }

    LOG_INFO("📦 Undo record complete: " + std::to_string(undo.spent.size()) +
             " spent coins, " + std::to_string(undo.created.size()) + " created outputs");

    return undo;
}

// ============================================================================
// Phase 3D: Convert ParsedBlock (hex transaction strings) → dinero::Block (parsed objects)
// ============================================================================
dinero::Block BlockAcceptor::ConvertParsedBlockToBlock(const ParsedBlock& parsed_block) {
    dinero::Block block;

    // Phase M.4: Single source of truth for ParsedBlock → BlockHeader conversion
    block.header = ToBlockHeader(parsed_block);

    // Parse transaction hex strings into Transaction objects
    block.vtx.reserve(parsed_block.transactions.size());

    for (size_t i = 0; i < parsed_block.transactions.size(); ++i) {
        const std::string& tx_hex = parsed_block.transactions[i];
        dinero::Transaction tx;
        std::string parse_error;

        // Use TransactionSerializer::Deserialize (handles CT outputs correctly)
        // This must match the parser used in ParseBlockFromHex to produce identical txids.
        std::vector<uint8_t> tx_bytes;
        tx_bytes.reserve(tx_hex.size() / 2);
        for (size_t j = 0; j < tx_hex.size(); j += 2) {
            tx_bytes.push_back(static_cast<uint8_t>(
                std::stoi(tx_hex.substr(j, 2), nullptr, 16)));
        }
        size_t consumed = 0;
        bool success = TransactionSerializer::Deserialize(tx, tx_bytes, consumed);
        if (!success) {
            parse_error = "TransactionSerializer::Deserialize failed";
        }

        if (!success) {
            LOG_ERROR("Failed to parse transaction " + std::to_string(i) + ": " + parse_error);
            // Add empty transaction to maintain index alignment
            block.vtx.push_back(dinero::Transaction{});
        } else {
            // DEBUG: Check if witness was preserved after parsing
            bool has_witness = false;
            for (size_t j = 0; j < tx.vin.size(); j++) {
                if (!tx.vin[j].witness.empty()) {
                    has_witness = true;
                    LOG_INFO("[ConvertParsedBlock] tx[" + std::to_string(i) + "].vin[" + std::to_string(j) +
                             "] has witness: " + std::to_string(tx.vin[j].witness.size()) + " elements, " +
                             "first size: " + std::to_string(tx.vin[j].witness.empty() ? 0 : tx.vin[j].witness[0].size()) + " bytes");
                }
            }
            if (i > 0 && !has_witness) {
                LOG_ERROR("[ConvertParsedBlock] tx[" + std::to_string(i) + "] has NO witness after parsing!");
            }
            block.vtx.push_back(tx);
        }
    }

    // Copy utreexo proof data if present
    if (parsed_block.utreexo_data.has_value()) {
        block.utreexo = parsed_block.utreexo_data;
        LOG_INFO("✅ Copied Utreexo data: " +
                std::to_string(block.utreexo->spent_outputs.size()) + " spent outputs");
    }

    LOG_INFO("✅ Converted ParsedBlock → Block with " + std::to_string(block.vtx.size()) + " transactions");

    return block;
}

bool BlockAcceptor::ConnectBlock(const ParsedBlock& block, uint64_t height, const std::string& parentChainwork, std::string& error, bool updateTip) {
    // Capture the operator-decision generation BEFORE any of this function's
    // work. Storing, indexing and metadata writes all take time; if an
    // invalidate or reconsider lands meanwhile, this result is stale and must
    // not write or preserve failure flags — otherwise a continuously
    // re-announced block undoes the operator's decision on the next relay.
    // See consensus/block_status_generation.h.
    dinero::consensus::GenerationRead accept_status_generation{};
    {
        auto* gen_ctx = DaemonContext::instance();
        auto gen_cs = std::dynamic_pointer_cast<dinero::ChainstateService>(
            gen_ctx ? gen_ctx->chainstate : nullptr);
        if (gen_cs) accept_status_generation = gen_cs->ReadBlockStatusGeneration();
        // No chainstate leaves it Error-by-default, which preserves flags
        // rather than clearing them.
    }
    try {
        // Authorization token for ChainDB writes (compile-time enforced)
        ChainWriteToken token;

        // Calculate new chainwork
        std::string newChainwork = CalculateChainwork(parentChainwork, block.bits);

        // ============================================================================
        // ANTI-SELF-CHAIN SAFEGUARDS
        // ============================================================================
        const auto& params = dinero::Params();

        // 4d-1: nMinimumChainWork is NOT enforced per-block here.
        //
        // The previous SAFEGUARD-1 rejected any individual block whose *cumulative*
        // chainwork was below nMinimumChainWork. That is incorrect: during IBD the
        // real chain's early blocks legitimately have cumulative work below the
        // threshold, so a non-zero nMinimumChainWork would reject them and brick
        // sync at genesis (which is why the value had to stay zero).
        //
        // Bitcoin-style enforcement gates the best *header* chain's total work
        // before block download/activation (so a sub-threshold/forged-low-work
        // chain is never followed), then accepts ALL blocks on a qualifying chain.
        // That chain-level gate lives at the header-sync activation trigger; here we
        // only keep the IBD/AssumeValid heuristic below (which uses params).

        // ====================================================================
        // F.10.9: ASSUMEVALID (IBD Performance Optimization)
        // ====================================================================
        // Skip signature verification for blocks below assumeValidHeight during IBD
        // Safe because: (1) minimum chainwork ensures we're on real chain (F.10.10)
        //               (2) still validates: PoW, merkle roots, UTXOs, structure
        // Performance gain: 5-10x faster sync during IBD
        // ====================================================================

        bool skip_sig_check = false;

        // ────────────────────────────────────────────────────────────────────
        // IBD Detection (Bitcoin Core model - NEVER use time alone)
        // ────────────────────────────────────────────────────────────────────
        // IBD means: "I do not yet trust I'm on the real chain"
        //
        // CORRECT model (chainwork-anchored):
        //   1. Chainwork < minimum (HARD GATE - F.10.10)
        //   2. Height < assumeValidHeight (progress indicator)
        //   3. Tip staleness (optional heuristic, NEVER alone)
        //
        // WHY NOT TIME ALONE:
        //   - Block timestamps are miner-controlled (within drift rules)
        //   - Attackers can manipulate timestamps to trigger/prevent AssumeValid
        //   - Time is NOT a reliable indicator of chain state
        //
        // Bitcoin Core: Uses chainwork + height + tip age (never time alone)
        // Dinero: Uses chainwork + height (stronger than Bitcoin Core)
        // ────────────────────────────────────────────────────────────────────

        // Primary IBD indicator: chainwork below minimum (hard security gate)
        bool chainwork_insufficient = false;
        if (!params.nMinimumChainWork.empty()) {
            int cmp = dinero::CompareChainwork(newChainwork, params.nMinimumChainWork);
            chainwork_insufficient = (cmp < 0);
        }

        // Secondary IBD indicator: height below assumeValidHeight (progress gate)
        bool height_below_assume_valid = (height < params.assumeValidHeight);

        // Tertiary IBD indicator (optional heuristic): tip staleness
        // Only use as weak signal, NEVER as sole condition
        constexpr uint64_t DINERO_BLOCK_TIME = 120; // 2 minutes
        constexpr uint64_t TIP_STALE_THRESHOLD = 30 * DINERO_BLOCK_TIME; // 30 blocks ≈ 1 hour
        uint64_t current_time_unix = std::time(nullptr);
        uint64_t time_behind = (current_time_unix > block.timestamp) ? (current_time_unix - block.timestamp) : 0;
        bool tip_is_stale = (time_behind > TIP_STALE_THRESHOLD);

        // Combine indicators (chainwork OR height, optionally AND tip staleness)
        // Note: Chainwork is the HARD GATE. Height provides progress confidence.
        bool is_ibd = chainwork_insufficient || height_below_assume_valid || tip_is_stale;

        LOG_INFO("💾 Connecting block at height " + std::to_string(height));

        // Week 3: MIGRATED - Now uses ctx_->chainstate instead of dinero::legacy::g_chain_db_direct()
        if (!ctx_ || !ctx_->chainstate) {
            error = "Chainstate service not available";
            LOG_ERROR("❌ " + error);
            return false;
        }

        auto* daemon_ctx = DaemonContext::instance();
        // Phase 39: Get ChainDB via ChainstateService (ChainManager deleted)
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(daemon_ctx ? daemon_ctx->chainstate : nullptr);
        auto* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
        if (!chain_db) {
            error = "ChainDB not initialized";
            LOG_ERROR("❌ " + error);
            return false;
        }

        // ────────────────────────────────────────────────────────────────────
        // TIP PROTECTION: Never skip validation for the active tip
        // ────────────────────────────────────────────────────────────────────
        // Critical: The tip block must ALWAYS be fully verified, even during IBD
        // This prevents silent acceptance of invalid current blocks
        // Bitcoin Core enforces this implicitly - we enforce it explicitly
        //
        // Rule: AssumeValid only applies to blocks strictly below active chain height
        // ────────────────────────────────────────────────────────────────────

        // Get current active chain height
        uint32_t active_chain_height = 0;
        auto tip_result = chain_db->getTip();
        if (tip_result.ok()) {
            active_chain_height = tip_result.value().height;
        }

        // Tip protection: block extends or equals current tip → FULL VALIDATION
        bool is_extending_tip = (height >= active_chain_height);

        // Apply ASSUMEVALID optimization during IBD (with tip protection)
        if (is_ibd &&
            height < params.assumeValidHeight &&      // Below assumeValid height
            params.assumeValidHeight > 0 &&            // AssumeValid enabled
            !is_extending_tip) {                       // NOT the tip (critical)
            skip_sig_check = true;
            LOG_INFO("⚡ F.10.9 ASSUMEVALID: Skipping script verification for block " + std::to_string(height));
            LOG_INFO("⚡ IBD indicators: chainwork_low=" + std::string(chainwork_insufficient ? "true" : "false") +
                     ", height_below_assume=" + std::string(height_below_assume_valid ? "true" : "false") +
                     ", tip_stale=" + std::string(tip_is_stale ? "true" : "false"));
            LOG_INFO("⚡ Height=" + std::to_string(height) + " < assumeValidHeight=" + std::to_string(params.assumeValidHeight) +
                     ", active_chain_height=" + std::to_string(active_chain_height));
        } else if (is_extending_tip && is_ibd) {
            LOG_INFO("🔒 TIP PROTECTION: Block " + std::to_string(height) +
                     " extends tip (active=" + std::to_string(active_chain_height) + ") → FULL VALIDATION");
        }
        (void)skip_sig_check; // IBD sig-skip optimization not yet wired

        // Use canonical wire-bytes hash (computed at parse time from raw 128-byte header)
        dinero::uint256 blockHash = block.blockHashRaw;

        // Use cumulative chainwork, not per-block placeholder work.
        dinero::arith_uint256 blockWork;
        try {
            blockWork = dinero::ChainworkFromHex(newChainwork);
        } catch (const std::exception& e) {
            error = "Invalid chainwork calculation: " + std::string(e.what());
            LOG_ERROR("❌ " + error);
            return false;
        }

        // ════════════════════════════════════════════════════════════════════════════
        // PHASE 11a: PURE ROOT COMPUTATION (NO STATE MUTATION)
        // ════════════════════════════════════════════════════════════════════════════
        // PATH A (Mining): Computes utreexo_root WITHOUT mutating chainstate
        //   - ComputeUtreexoRootPure() = pure function, temp forest snapshot
        //   - Store block with computed root
        //   - ActivateBestChain() → ConnectTip() → ValidateAndApplyBlock() = actual mutation
        //
        // PATH B (Sync): ActivateBestChain() → ConnectTip() → ValidateAndApplyBlock()
        //
        // CRITICAL INVARIANT: Consensus state mutated exactly once, only by ConnectTip()
        // This ensures position index, undo data, and forest are all updated together.
        // ════════════════════════════════════════════════════════════════════════════

        auto chainstate_service = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
        if (!chainstate_service) {
            error = "ChainstateService not available";
            LOG_ERROR("❌ " + error);
            return false;
        }

        // Convert ParsedBlock → Block for BlockValidator
        dinero::Block consensus_block = ConvertParsedBlockToBlock(block);

        // ════════════════════════════════════════════════════════════════════
        // P2P FIX: PRESERVE miner's utreexo_root after PoW validation passes
        // ════════════════════════════════════════════════════════════════════
        // The miner computed utreexo_root using getblocktemplate and found a valid nonce.
        // PoW validation (above) verified: hash(header_with_miner_utreexo_root) < target
        //
        // CRITICAL: We MUST NOT replace the miner's utreexo_root!
        // If we replace it, the block hash changes, breaking PoW for peers.
        //
        // Previous bug: Daemon recomputed utreexo_root, stored different hash.
        // Peers computed hash from received block → didn't match target → rejected.
        // ════════════════════════════════════════════════════════════════════

        // Phase M.4: Use ToBlockHeader() for clean conversion
        // This preserves the miner's utreexo_root from the wire format
        dinero::BlockHeader header = ToBlockHeader(block);

        // Canonical hash is from wire bytes (computed in ParseBlockFromHex).
        // Verify ToBlockHeader round-trip produces identical hash — catches serialization bugs.
        {
            dinero::uint256 structHash = header.GetHash();
            if (structHash != blockHash) {
                LOG_ERROR("🔴 HASH DIVERGENCE at height " + std::to_string(height) +
                          ": wire=" + blockHash.GetHex().substr(0, 16) +
                          "... struct=" + structHash.GetHex().substr(0, 16) + "...");
                // Wire hash is authoritative (matches what peers see)
            }
        }

        LOG_INFO("✅ Using miner's utreexo_root (PoW validated): " + block.utreexoRoot.substr(0, 16) + "...");
        LOG_INFO("✅ Block hash (wire): " + blockHash.GetHex().substr(0, 16) + "...");

        // ════════════════════════════════════════════════════════════════════
        // UTREEXO ROOT VALIDATION (BEFORE STORAGE)
        // ════════════════════════════════════════════════════════════════════
        // REORG FIX: Only validate Utreexo for main chain extensions.
        // Side-chain blocks (competing forks) have different UTXO state - their
        // utreexo roots are computed against a different fork's UTXO set.
        // ActivateBestChain will validate them AFTER disconnecting our chain.
        // ════════════════════════════════════════════════════════════════════
        auto* block_validator = chainstate_service->GetBlockValidator();
        // In STATELESS mode, StatelessNode already validated the utreexo proof
        // via the utxoblk handler — skip redundant (and incorrect) recomputation
        // since the BlockAcceptor's forest is a different instance.
        bool skip_utreexo = block_validator &&
            block_validator->getValidationMode() == consensus::ValidationMode::STATELESS;
        if (updateTip && block_validator && !skip_utreexo) {
            // Main chain extension: validate Utreexo root now
            dinero::uint256 computed_root;
            std::string utreexo_error;

            if (block_validator->ComputeUtreexoRootPure(consensus_block, height, computed_root, utreexo_error)) {
                // Convert block's utreexo_root hex string to uint256 for comparison
                dinero::uint256 block_root;
                if (!block.utreexoRoot.empty() && block.utreexoRoot.size() == 64) {
                    dinero::uint256::FromHex(block.utreexoRoot, block_root);
                }

                // ═══════════════════════════════════════════════════════════════════════════
                // 🔍 DIAGNOSTIC: Utreexo root comparison debug logging
                // ═══════════════════════════════════════════════════════════════════════════
                LOG_INFO("🔍 [UTREEXO DEBUG] Height: " + std::to_string(height));
                LOG_INFO("🔍 [UTREEXO DEBUG] computed_root (GetHex): " + computed_root.GetHex());
                LOG_INFO("🔍 [UTREEXO DEBUG] block.utreexoRoot (raw string): " + block.utreexoRoot);
                LOG_INFO("🔍 [UTREEXO DEBUG] block_root (GetHex after FromHex): " + block_root.GetHex());
                // Show raw bytes for computed_root
                std::ostringstream computed_raw;
                for (int i = 0; i < 32; ++i) {
                    computed_raw << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(computed_root.data[i]);
                }
                LOG_INFO("🔍 [UTREEXO DEBUG] computed_root raw bytes [0..31]: " + computed_raw.str());
                // Show raw bytes for block_root
                std::ostringstream block_raw;
                for (int i = 0; i < 32; ++i) {
                    block_raw << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(block_root.data[i]);
                }
                LOG_INFO("🔍 [UTREEXO DEBUG] block_root raw bytes [0..31]: " + block_raw.str());
                // ═══════════════════════════════════════════════════════════════════════════

                if (computed_root != block_root) {
                    error = "bad-utreexo-root: computed=" + computed_root.GetHex().substr(0, 16) +
                            "... expected=" + block_root.GetHex().substr(0, 16) + "...";
                    LOG_ERROR("❌ UTREEXO VALIDATION FAILED (before storage): " + error);
                    return false;
                }
                LOG_INFO("✅ Utreexo root validated: " + computed_root.GetHex().substr(0, 16) + "...");
            } else {
                LOG_ERROR("⚠️  Failed to compute utreexo root: " + utreexo_error);
                // Continue for now - ActivateBestChain will catch it
            }
        } else if (!updateTip) {
            LOG_INFO("🔀 SIDE-CHAIN BLOCK: Skipping Utreexo validation (will validate after reorg disconnect)");
        }

        // 🔍 DIAGNOSTIC: Log what we're storing
        LOG_INFO("💾 Storing header at height " + std::to_string(height) +
                 ": bits=" + dinero::hex32_0x(block.bits) +
                 ", timestamp=" + std::to_string(block.timestamp) +
                 ", utreexo_root=" + block.utreexoRoot.substr(0, 16) + "...");

        // Use RocksDB WriteBatch for atomic updates
        rocksdb::WriteBatch batch;
        // Canonical tip/UTXO mutations are deferred to ActivateBestChain->ConnectTip.
        // BlockAcceptor persists block/header metadata and candidate graph entries only.
        const bool apply_canonical_writes = false;
        if (updateTip && !apply_canonical_writes) {
            LOG_INFO("⏭️ Main-chain candidate detected: deferring canonical tip/UTXO writes to ConnectTip");
        }

        // ========================================================================
        // CONVERT BLOCKUNDO → UNDORECORD FOR STORAGE
        // ========================================================================
        // Canonical undo/UTXO state is for the active chain only.
        // Side-chain blocks are stored for possible future activation, but must
        // not mutate canonical rollback or UTXO state until ConnectTip.
        std::optional<FilePosition> undo_flatfile_pos;
        const bool archival_flatfiles_available = ctx_ && ctx_->block_storage;
        if (!archival_flatfiles_available) {
            error = "Archival block acceptance requires BlockStorage";
            LOG_ERROR("❌ " + error);
            return false;
        }
        if (apply_canonical_writes) {
            dinero::UndoRecord undo = BuildUndoForBlock(block, height, chain_db);
            std::vector<uint8_t> undoBytes = undo.Serialize();

            if (ctx_ && ctx_->block_storage) {
                auto undo_pos_result = ctx_->block_storage->writeUndo(blockHash, undoBytes);
                if (undo_pos_result.status() != dinero::Status::Ok) {
                    error = "Failed to store undo data in BlockStorage";
                    LOG_ERROR("❌ " + error);
                    return false;
                }
                undo_flatfile_pos = undo_pos_result.value();
                if (undo_flatfile_pos->offset > std::numeric_limits<uint32_t>::max()) {
                    error = "Undo file offset exceeds persisted metadata range";
                    LOG_ERROR("❌ " + error);
                    return false;
                }
                LOG_INFO("📦 Undo flatfile write complete: file=" + std::to_string(undo_flatfile_pos->file_number) +
                         " offset=" + std::to_string(undo_flatfile_pos->offset) +
                         " size=" + std::to_string(undo_flatfile_pos->size));
            }

            LOG_INFO("📦 Archival mode: skipping legacy ChainDB undo shadow write");

            LOG_INFO("📦 Undo record serialized: " + std::to_string(undoBytes.size()) + " bytes");
        } else {
            LOG_INFO("📌 Side-chain block: skipping canonical undo persistence (will generate on activation)");
        }

        // ========================================================================
        // UTXO SET UPDATE - ChainDB UTXO storage for mempool validation
        // ========================================================================
        // BlockValidator::ConnectBlock() updates UTXOIndex (wallet SQLite DB).
        // However, mempool uses ChainDB::getCoin() (RocksDB) for tx validation.
        // We MUST also store UTXOs in ChainDB to enable mempool acceptance.
        //
        // For each transaction:
        // - Delete spent UTXOs (inputs) from ChainDB
        // - Add created UTXOs (outputs) to ChainDB
        // ========================================================================
        if (apply_canonical_writes) {
            for (size_t tx_idx = 0; tx_idx < consensus_block.vtx.size(); tx_idx++) {
                const auto& tx = consensus_block.vtx[tx_idx];
                const TxId txid = tx.GetTxid();
                const bool is_coinbase = (tx_idx == 0);

                // Delete spent UTXOs (skip coinbase - no inputs to spend)
                if (!is_coinbase) {
                    for (const auto& input : tx.vin) {
                        auto del_status = chain_db->deleteCoin(token, input.prevout.txid.AsUint256(), input.prevout.vout, &batch);
                        if (del_status != dinero::Status::Ok) {
                            LOG_ERROR("❌ Failed to delete spent UTXO: " + input.prevout.txid.AsUint256().GetHex().substr(0, 16) +
                                      ":" + std::to_string(input.prevout.vout));
                            // Continue - undo data has the spent coins for recovery
                        }
                    }
                }

                // Add created UTXOs (outputs)
                for (uint32_t vout = 0; vout < tx.vout.size(); vout++) {
                    const auto& output = tx.vout[vout];

                    // Build Coin struct for ChainDB
                    dinero::Coin coin;
                    coin.amount = output.value.GetUna();  // Extract raw una value
                    // Convert scriptPubKey bytes to hex string (ChainDB stores as hex)
                    std::ostringstream spk_hex;
                    for (uint8_t byte : output.scriptPubKey) {
                        spk_hex << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
                    }
                    coin.script_pubkey = spk_hex.str();
                    coin.height = static_cast<uint32_t>(height);
                    coin.coinbase = is_coinbase;
                    coin.is_confidential = output.is_confidential;
                    coin.commitment = output.commitment;

                    auto put_status = chain_db->putCoin(token, txid.AsUint256(), vout, coin, &batch);
                    if (put_status != dinero::Status::Ok) {
                        LOG_ERROR("❌ Failed to store UTXO: " + txid.AsUint256().GetHex().substr(0, 16) +
                                  ":" + std::to_string(vout));
                        // This is critical - transaction won't be spendable
                    }
                }
            }
            LOG_INFO("💰 Stored " + std::to_string(consensus_block.vtx.size()) + " tx UTXOs in ChainDB");
        } else {
            LOG_INFO("📌 Side-chain block: skipping canonical UTXO mutation (will apply on activation)");
        }

        // ========================================================================
        // Canonical archival write: accepted block bodies live in flatfiles.
        // ========================================================================
        std::optional<FilePosition> flatfile_pos;
        if (archival_flatfiles_available) {
            auto pos_result = ctx_->block_storage->writeBlock(blockHash, consensus_block);
            if (pos_result.status() != dinero::Status::Ok) {
                error = "Failed to store block body in BlockStorage";
                LOG_ERROR("❌ " + error);
                return false;
            }
            flatfile_pos = pos_result.value();
            LOG_INFO("📦 BlockStorage write complete: file=" + std::to_string(flatfile_pos->file_number) +
                     " offset=" + std::to_string(flatfile_pos->offset) +
                     " size=" + std::to_string(flatfile_pos->size));
        } else {
            error = "Archival block acceptance requires BlockStorage for block bodies";
            LOG_ERROR("❌ " + error);
            return false;
        }

        // ========================================================================
        // Legacy compatibility write: keep ChainDB block bodies until remaining
        // direct readers are migrated to the flatfile-backed path.
        // ========================================================================
        LOG_INFO("📦 Storing block at height " + std::to_string(height) + " with hash=" + blockHash.GetHex());
        LOG_INFO("📦 Archival mode: skipping legacy ChainDB block-body shadow write");

        // Store header in ChainDB
        // Week 3: MIGRATED - Use chain_db from context
        std::ostringstream header_root_debug;
        for (size_t i = 0; i < 32; ++i) {
            header_root_debug << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(header.utreexo_root.data[i]);
        }
        LOG_INFO("🔍 DEBUG: About to store header.utreexo_root=" + header_root_debug.str().substr(0, 16) + "...");
        auto status = chain_db->putHeader(token, blockHash, header, static_cast<int>(height), blockWork, &batch);
        if (status != dinero::Status::Ok) {
            error = "Failed to store block header";
            LOG_ERROR("❌ " + error);
            return false;
        }

        if (flatfile_pos.has_value()) {
            if (flatfile_pos->offset > std::numeric_limits<uint32_t>::max()) {
                error = "Block file offset exceeds persisted metadata range";
                LOG_ERROR("❌ " + error);
                return false;
            }

            ChainDB::PersistedHeaderMetadata metadata;
            metadata.parent_hash = header.prev_block_hash;
            metadata.height = static_cast<int32_t>(height);
            metadata.chainwork = blockWork;
            metadata.status_flags = dinero::BLOCK_VALID_HEADER |
                                    dinero::BLOCK_VALID_TREE |
                                    dinero::BLOCK_VALID_TRANSACTIONS |
                                    dinero::BLOCK_VALID_CHAIN |
                                    dinero::BLOCK_HAVE_DATA |
                                    (apply_canonical_writes ? dinero::BLOCK_HAVE_UNDO : 0u);
            metadata.file_number = flatfile_pos->file_number;
            metadata.data_pos = static_cast<uint32_t>(flatfile_pos->offset);
            metadata.data_size = flatfile_pos->size;
            if (undo_flatfile_pos.has_value()) {
                metadata.undo_file = undo_flatfile_pos->file_number;
                metadata.undo_pos = static_cast<uint32_t>(undo_flatfile_pos->offset);
                metadata.undo_size = undo_flatfile_pos->size;
            }

            // BlockAcceptor may see the same block twice: first as the block
            // ActivateBestChain connects, then again through a relay/side-chain
            // path after the tip moved. This writer owns block-body/header
            // placement, but ConnectTip owns canonical undo metadata. Preserve
            // an existing BLOCK_HAVE_UNDO + undo_file/pos/size row so a
            // duplicate relay cannot regress the active tip's rollback data.
            status = chain_db->putHeaderMetadataPreservingExistingUndo(
                token, blockHash, metadata, &batch);
            if (status != dinero::Status::Ok) {
                error = "Failed to store block header metadata";
                LOG_ERROR("❌ " + error);
                return false;
            }
        }

        if (apply_canonical_writes) {
            // Update height index for canonical active chain only.
            status = chain_db->putHeightIndex(token, static_cast<int>(height), blockHash, &batch);
            if (status != dinero::Status::Ok) {
                error = "Failed to update height index";
                LOG_ERROR("❌ " + error);
                return false;
            }

            // Update chain tip (side-chain blocks are handled by ActivateBestChain).
            status = chain_db->setTip(token, blockHash, static_cast<int>(height), blockWork, &batch);
            if (status != dinero::Status::Ok) {
                error = "Failed to update chain tip";
                LOG_ERROR("❌ " + error);
                return false;
            }
        } else {
            LOG_INFO("📌 Side-chain block: not updating canonical tip/height index (ActivateBestChain will decide)");
        }

        // Commit all changes atomically (includes undo data)
        status = chain_db->writeBatch(token, std::move(batch), true);
        if (status != dinero::Status::Ok) {
            error = "Failed to commit block to database";
            LOG_ERROR("❌ " + error);
            return false;
        }

        LOG_INFO("✅ Block metadata committed atomically at height " + std::to_string(height));

        // Regtest-only fault-injection: abort AFTER the block body + header +
        // metadata (BLOCK_HAVE_DATA, file/pos/size) are durably committed to
        // ChainDB via the atomic writeBatch above — i.e. the block is stored
        // AND indexed — but BEFORE it is connected (ConnectBlock is always
        // called with updateTip=false from AcceptBlockFromRPC; the caller's
        // subsequent ActivateBestChain()->ConnectTip() call is what actually
        // advances the tip). This deterministically reproduces the
        // crash-between-store-and-connect state that drives ConnectTip's
        // stateless recovery branch (#356). The BlockIndex population below
        // this point is in-memory bookkeeping only — irrelevant to what a
        // restarted process observes, since recovery rebuilds it from the
        // ChainDB metadata just committed. Inert unless we are on regtest AND
        // the env var is set AND it matches this height. The regtest gate means
        // this debug facility cannot fire on mainnet/testnet even if the env
        // var leaks into a production environment. _exit(70) bypasses
        // destructors so nothing else commits/flushes.
        if (dinero::Params().name == "regtest") {
        if (const char* abort_height_env = std::getenv("DINERO_DEBUG_ABORT_AFTER_STORE_HEIGHT")) {
            errno = 0;
            char* parse_end = nullptr;
            const long want_height = std::strtol(abort_height_env, &parse_end, 10);
            // Require the ENTIRE value to be a clean integer (parse_end at NUL),
            // so "6garbage" does not fire at height 6.
            if (parse_end != abort_height_env && *parse_end == '\0' && errno == 0 &&
                want_height >= 0 && static_cast<long>(height) == want_height) {
                LOG_ERROR("💥 [DEBUG] DINERO_DEBUG_ABORT_AFTER_STORE_HEIGHT=" +
                          std::string(abort_height_env) + " — aborting after block store+index at height " +
                          std::to_string(height) + " (pre-connect) for #356 recovery test");
                std::fflush(nullptr);
                _exit(70);
            }
        }
        }

        // ========================================================================
        // Phase 41: Populate BlockIndex for automatic fork selection
        // ========================================================================
        // After successful block connection, create BlockIndex entry to enable
        // ChainstateService to perform automatic fork selection via chainwork comparison.
        // This is pure metadata tracking - no consensus validation happens here.
        if (ctx_ && ctx_->chainstate) {
            auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
            if (chainstate) {
                // ════════════════════════════════════════════════════════════════════
                // CRITICAL INVARIANT: Block hash identity must be single-source-of-truth
                // ════════════════════════════════════════════════════════════════════
                // Reuse the SAME header that was stored (line 1392), which includes:
                // - utreexo_root (computed by ApplyBlock)
                // - All other consensus fields
                //
                // DO NOT recreate header from ParsedBlock - it won't have utreexo_root!
                // This would cause header.GetHash() to differ from blockHash.
                //
                // Consequence: ConnectTip can't find blocks, position index never populates
                // ════════════════════════════════════════════════════════════════════

                uint256 header_hash = header.GetHash();
                LOG_INFO("🔗 Creating BlockIndex with hash from header: " + header_hash.GetHex());

                // Enforce invariant: block hash identity
                if (header_hash != blockHash) {
                    error = "FATAL: Block hash identity violation: header.GetHash()=" +
                           header_hash.GetHex() + " != blockHash=" + blockHash.GetHex();
                    LOG_ERROR("❌ " + error);
                    return false;
                }

                // Add to BlockIndex graph (using the SAME header with utreexo_root)
                auto* block_index = chainstate->AddBlockIndex(header, height);

                if (block_index) {
                    // Apr 14 2026 (Bug #6 / #38) — preserve any pre-existing
                    // BLOCK_FAILED_VALID / BLOCK_FAILED_CHILD flag. Without
                    // this, a peer re-relaying a previously-invalidated block
                    // silently overwrites the failure flag here and the block
                    // re-enters the candidate set on the next AddCandidate
                    // call, defeating the entire persistent invalidate fix.
                    // Generation check (consensus/block_status_generation.h).
                    // Preserving a failure flag is correct ONLY if no operator
                    // decision landed while this block was being processed. A
                    // continuously re-announced block always has a relay in
                    // flight, so without this a reconsiderblock is undone by
                    // the very next relay — measured at 650 re-assertions after
                    // a reconsider, with the tip never recovering.
                    //
                    // Fails CLOSED on an unreadable counter: a 0 read never
                    // equals a bumped generation, so the flags are dropped
                    // rather than wrongly preserved.
                    // The compare below and the write that follows must be
                    // atomic w.r.t. an operator decision. Enforced here so
                    // dropping the serialization is caught by every test that
                    // re-accepts a block, not only by a race that reproduces.
                    chainstate->AssertActivationLockHeld("ConnectBlock compare+write");
                    const auto gen_now = chainstate->ReadBlockStatusGeneration();
                    // Unreadable on EITHER side means the operator decision is
                    // unknown, not unchanged. Treating it as unchanged would
                    // re-assert flags over a reconsider; treating it as changed
                    // would CLEAR persisted invalidity. Neither is safe, so the
                    // rule below declines to act: it carries forward exactly
                    // what is already on disk.
                    const bool reads_usable =
                        accept_status_generation.usable() && gen_now.usable();
                    const bool decision_unchanged =
                        reads_usable &&
                        dinero::consensus::GenerationStillCurrent(
                            accept_status_generation.value, gen_now.value);
                    // DETERMINISTIC RACE BARRIER (regtest only, inert
                    // otherwise). Sits exactly between the generation
                    // COMPARISON above and the flag WRITE below — the TOCTOU
                    // window. A test parks the daemon here, invokes
                    // reconsiderblock concurrently, and releases:
                    //   with activation_mutex_ on ReconsiderBlock, the
                    //     reconsider must WAIT, so this write lands first and
                    //     the reconsider then clears it;
                    //   without it, the reconsider runs ahead and this write
                    //     re-asserts flags it just cleared — which the test
                    //     detects.
                    // Deterministic in both directions; no sleep decides
                    // correctness.
                    dinero::testing::MaybeBarrierAt(
                        "connectblock_after_generation_compare",
                        dinero::testing::CrashHooksEnabled().load());

                    // Same decision as consensus/block_status_generation.h's
                    // PreservedFailureFlags, which exists so this rule is
                    // unit-testable without a daemon.
                    const uint32_t preserved_failure_flags =
                        dinero::consensus::PreservedFailureFlagsFromReads(
                            accept_status_generation, gen_now,
                            block_index->status &
                                (dinero::BLOCK_FAILED_VALID | dinero::BLOCK_FAILED_CHILD));
                    if (!reads_usable) {
                        LOG_ERROR("⚠️  Block-status generation unreadable while "
                                  "accepting " + block.blockHash.substr(0, 16) +
                                  "... — PRESERVING existing failure flags "
                                  "rather than clearing persisted invalidity. "
                                  "An operator decision made in this window may "
                                  "need to be re-issued.");
                    }
                    if (!decision_unchanged) {
                        LOG_INFO("♻️  Stale acceptance (generation " +
                                 std::to_string(accept_status_generation.value) + " -> " +
                                 std::to_string(gen_now.value) +
                                 "): NOT re-asserting failure flags for " +
                                 block.blockHash.substr(0, 16) +
                                 "... — a newer operator decision wins");
                    }

                    // Mark as fully validated (all consensus checks passed)
                    // BLOCK_HAVE_DATA: block body stored in flatfiles.
                    block_index->status = dinero::BLOCK_VALID_HEADER |
                                         dinero::BLOCK_VALID_TREE |
                                         dinero::BLOCK_VALID_SCRIPTS |
                                         dinero::BLOCK_VALID_TRANSACTIONS |
                                         dinero::BLOCK_VALID_CHAIN |
                                         dinero::BLOCK_HAVE_DATA |
                                         (apply_canonical_writes ? dinero::BLOCK_HAVE_UNDO : 0u) |
                                         preserved_failure_flags;
                    if (flatfile_pos.has_value()) {
                        block_index->file_number = flatfile_pos->file_number;
                        block_index->data_pos = static_cast<uint32_t>(flatfile_pos->offset);
                        block_index->data_size = flatfile_pos->size;
                    }
                    if (undo_flatfile_pos.has_value()) {
                        block_index->undo_file = undo_flatfile_pos->file_number;
                        block_index->undo_pos = static_cast<uint32_t>(undo_flatfile_pos->offset);
                        block_index->undo_size = undo_flatfile_pos->size;
                    }

                    // Add to candidate tips for fork selection — but skip if
                    // the block has been persistently invalidated. AddCandidate
                    // also checks IsEligibleForCandidacy, but logging the skip
                    // here makes the relay-loop case obvious in the journal.
                    if (preserved_failure_flags == 0) {
                        chainstate->AddCandidate(block_index);
                    } else {
                        LOG_INFO("⛔ Block " + block.blockHash.substr(0, 16) +
                                 "... has persistent BLOCK_FAILED_VALID flag — skipping AddCandidate");
                    }

                    LOG_INFO("✅ BlockIndex created: height=" + std::to_string(height) +
                            ", chainwork=..." + block_index->chainwork.substr(48, 16));
                }
            }
        }

        LOG_INFO("✅ Block accepted: hash=" + block.blockHash.substr(0, 16) + "..., height=" + std::to_string(height));

        // ========================================================================
        // Phase 3D: Wallet notifications — REMOVED (duplicate)
        // ========================================================================
        // Wallet is now notified via ConnectTip → notifyBlockConnected → WalletWorker.
        // The old direct call here was a DUPLICATE (ActivateBestChain at line 219
        // already triggers ConnectTip → notifyBlockConnected for this block).
        // Having both caused double-processing and data races on WalletManager.
        // ========================================================================

        return true;

    } catch (const std::exception& e) {
        error = std::string("Database error: ") + e.what();
        LOG_ERROR("❌ ConnectBlock failed: " + error);
        return false;
    }
}

// ========================================================================
// Phase 4B: DisconnectBlock - Reverse of ConnectBlock
// ========================================================================
bool BlockAcceptor::DisconnectBlock(const ParsedBlock& block, uint64_t height, std::string& error) {
    try {
        // Authorization token for ChainDB writes (compile-time enforced)
        ChainWriteToken token;
        LOG_INFO("🔄 Disconnecting block at height " + std::to_string(height));

        // Week 3: MIGRATED - Now uses ctx_->chainstate instead of dinero::legacy::g_chain_db_direct()
        if (!ctx_ || !ctx_->chainstate) {
            error = "Chainstate service not available";
            LOG_ERROR("❌ " + error);
            return false;
        }

        auto* daemon_ctx = DaemonContext::instance();
        // Phase 39: Get ChainDB via ChainstateService (ChainManager deleted)
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(daemon_ctx ? daemon_ctx->chainstate : nullptr);
        auto* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
        if (!chain_db) {
            error = "ChainDB not initialized";
            LOG_ERROR("❌ " + error);
            return false;
        }

        // ========================================================================
        // STEP 1: LOAD UNDO RECORD from RocksDB
        // ========================================================================
        // CRITICAL FIX: Must use header.GetHash() to match storage key
        // The block was stored with hash from header.GetHash() (includes utreexo_root)
        // NOT block.blockHash (original wire format which may differ)
        dinero::BlockHeader header = ToBlockHeader(block);
        std::string correctHash = header.GetHash().GetHex();
        std::string undoKey = "U:" + correctHash;
        std::string undoValue;

        auto status = chain_db->getRaw(undoKey, undoValue);
        if (status != dinero::Status::Ok) {
            error = "Failed to load undo record for block " + correctHash.substr(0, 16) + "...";
            LOG_ERROR("❌ " + error);
            return false;
        }

        // Deserialize undo record
        std::vector<uint8_t> undoBytes(undoValue.begin(), undoValue.end());
        dinero::UndoRecord undo = dinero::UndoRecord::Deserialize(undoBytes);

        LOG_INFO("📦 Undo record loaded: " + std::to_string(undoBytes.size()) + " bytes, " +
                 std::to_string(undo.spent.size()) + " spent outputs to restore");

        // Use RocksDB WriteBatch for atomic updates
        rocksdb::WriteBatch batch;

        // ========================================================================
        // STEP 2: REVERSE UTXO SET CHANGES using ChainDB
        // ========================================================================
        // Use undo.spent to restore all spent UTXOs directly (don't need to loop through transactions)
        LOG_INFO("💰 Restoring " + std::to_string(undo.spent.size()) + " spent UTXOs...");

        for (const auto& spentCoin : undo.spent) {
            // Build Coin struct to restore
            Coin coin;
            coin.amount = spentCoin.value;
            // Phase M.0: Convert binary scriptPubKey to hex string for Coin
            coin.script_pubkey.reserve(spentCoin.scriptPubKey.size() * 2);
            for (uint8_t byte : spentCoin.scriptPubKey) {
                char buf[3];
                snprintf(buf, sizeof(buf), "%02x", byte);
                coin.script_pubkey += buf;
            }
            coin.height = static_cast<int>(spentCoin.height);
            coin.coinbase = spentCoin.is_coinbase;
            coin.is_confidential = spentCoin.is_confidential;
            coin.commitment = spentCoin.commitment;

            // Restore the spent UTXO to ChainDB
            // Phase M.0: prev_txid is already uint256 (no conversion needed)
            auto status = chain_db->putCoin(token, spentCoin.prev_txid, spentCoin.prev_vout, coin, &batch);
            if (status != Status::Ok) {
                error = "Failed to restore spent UTXO: " + spentCoin.prev_txid.GetHex().substr(0, 16) + "...:" +
                        std::to_string(spentCoin.prev_vout) + " - " + std::string(StatusToString(status));
                LOG_ERROR("❌ " + error);
                return false;
            }

            LOG_INFO("  ✅ Restored spent UTXO: " + spentCoin.prev_txid.GetHex().substr(0, 16) + "...:" +
                     std::to_string(spentCoin.prev_vout) + " (value=" + std::to_string(coin.amount) + ")");
        }

        // Delete all created UTXOs from undo.created
        LOG_INFO("💰 Deleting " + std::to_string(undo.created.size()) + " created UTXOs...");

        for (const auto& createdOut : undo.created) {
            // Remove UTXO from ChainDB
            // Phase M.0: txid is already uint256 (no conversion needed)
            auto status = chain_db->deleteCoin(token, createdOut.txid, createdOut.vout, &batch);
            if (status != Status::Ok) {
                // Not critical if UTXO already spent/missing
                LOG_INFO("  ⚠️  UTXO already removed: " + createdOut.txid.GetHex().substr(0, 16) + "...:" + std::to_string(createdOut.vout));
            } else {
                LOG_INFO("  🗑️ Removed created UTXO: " + createdOut.txid.GetHex().substr(0, 16) + "...:" + std::to_string(createdOut.vout));
            }
        }

        LOG_INFO("✅ UTXO set reversed successfully");

        // ========================================================================
        // STEP 3: REMOVE TX INDEX ENTRIES (Reorg Safety)
        // ========================================================================
        // Critical: TX index must be rolled back to prevent orphaned txs from being found
        LOG_INFO("📇 Removing TX index entries for " + std::to_string(block.transactions.size()) + " transactions...");

        for (size_t tx_idx = 0; tx_idx < block.transactions.size(); tx_idx++) {
            std::vector<uint8_t> txBytes = HexToBytes(block.transactions[tx_idx]);
            size_t offset = 0;
            dinero::Transaction tx;

            if (ParseTransaction(txBytes.data(), txBytes.size(), offset, tx)) {
                TxId txid = tx.GetTxid();  // Phase M.4: GetTxid() returns TxId

                // Delete TX index entry
                // Phase M.4: Extract uint256 for ChainDB API
                auto tx_status = chain_db->deleteTxIndex(token, txid.AsUint256(), &batch);
                if (tx_status != dinero::Status::Ok) {
                    LOG_ERROR("⚠️ Failed to remove TX index for " + txid.AsUint256().GetHex().substr(0, 16) + "... (non-critical)");
                } else {
                    LOG_INFO("  🗑️ Removed TX index: " + txid.AsUint256().GetHex().substr(0, 16) + "...");
                }
            } else {
                LOG_ERROR("⚠️ Failed to parse transaction " + std::to_string(tx_idx) + " during disconnect (non-critical)");
            }
        }

        LOG_INFO("✅ TX index entries removed");

        // ========================================================================
        // STEP 4: UPDATE CHAIN TIP to parent block
        // ========================================================================
        dinero::uint256 parentHash = dinero::uint256::FromHexUnsafe(block.prevBlockHash);  // Phase M.0: Convert from hex

        // Get parent block header (contains bits for work calculation)
        auto parentHeaderResult = chain_db->getHeader(parentHash);
        if (!parentHeaderResult.ok()) {
            error = "Failed to retrieve parent block header for " + block.prevBlockHash.substr(0, 16) + "...";
            LOG_ERROR("❌ " + error);
            return false;
        }
        dinero::BlockHeader parentHeader = parentHeaderResult.value();

        // Get parent block height
        auto parentHeightResult = chain_db->getBlockHeight(parentHash);
        if (!parentHeightResult.ok()) {
            error = "Failed to retrieve parent block height for " + block.prevBlockHash.substr(0, 16) + "...";
            LOG_ERROR("❌ " + error);
            return false;
        }
        int parentHeight = parentHeightResult.value();

        // Restore parent's cumulative chainwork from metadata when available.
        dinero::arith_uint256 parentWork = dinero::GetBlockProof(parentHeader.difficulty);
        auto parent_meta = chain_db->getHeaderMetadata(parentHash);
        if (parent_meta.ok()) {
            parentWork = parent_meta.value().chainwork;
        }

        // Update chain tip to parent
        status = chain_db->setTip(token, parentHash, parentHeight, parentWork, &batch);
        if (status != dinero::Status::Ok) {
            error = "Failed to update chain tip to parent";
            LOG_ERROR("❌ " + error);
            return false;
        }

        // ========================================================================
        // STEP 5: DELETE UNDO RECORD
        // ========================================================================
        // Delete undo record (no longer needed after rollback)
        batch.Delete(undoKey);

        // Note: We keep block headers for historical purposes (allows querying orphaned blocks)
        // But TX index is removed to prevent confusion (orphaned txs should not be findable)

        // ========================================================================
        // STEP 6: COMMIT ALL CHANGES ATOMICALLY
        // ========================================================================
        status = chain_db->writeBatch(token, std::move(batch), true);
        if (status != dinero::Status::Ok) {
            error = "Failed to commit block disconnect to database";
            LOG_ERROR("❌ " + error);
            return false;
        }

        LOG_INFO("✅ Block disconnected atomically at height " + std::to_string(height));

        // ========================================================================
        // Phase 4B: Notify registered wallets of disconnected block
        // ========================================================================
        if (ctx_ && ctx_->chainstate) {
            auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
            if (chainstate) {
                // Convert ParsedBlock → dinero::Block (parses hex transactions)
                dinero::Block converted_block = ConvertParsedBlockToBlock(block);

                // Notify all registered wallets (triggers automatic UTXO rollback)
                chainstate->notifyBlockDisconnected(converted_block, height);

                LOG_INFO("✅ Wallet disconnect notifications dispatched for block " + std::to_string(height));
            } else {
                LOG_ERROR("⚠️  ChainstateService not available for wallet notifications");
            }
        } else {
            // No context or chainstate - headless mode or startup
            LOG_INFO("ℹ️  No chainstate context, wallet notifications skipped");
        }
        // ========================================================================

        return true;

    } catch (const std::exception& e) {
        error = std::string("Database error: ") + e.what();
        LOG_ERROR("❌ DisconnectBlock failed: " + error);
        return false;
    }
}

std::string BlockAcceptor::CalculateChainwork(const std::string& parentChainwork, uint32_t bits) {
    try {
        // Calculate block proof (work done for this block) using proper arithmetic
        dinero::arith_uint256 blockProof = dinero::GetBlockProof(bits);

        // Parse parent chainwork or start from zero
        dinero::arith_uint256 parentWork;
        if (!parentChainwork.empty() && parentChainwork != std::string(64, '0')) {
            parentWork = dinero::ChainworkFromHex(parentChainwork);
        } else {
            parentWork = dinero::arith_uint256::Zero();
        }

        // New chainwork = parent chainwork + block proof
        dinero::arith_uint256 newChainwork = parentWork + blockProof;

        // Convert to hex string
        return dinero::ChainworkToHex(newChainwork);

    } catch (const std::exception& e) {
        LOG_ERROR("❌ Chainwork calculation error: " + std::string(e.what()));
        // Fallback: return parent chainwork + minimum work
        if (!parentChainwork.empty() && parentChainwork != std::string(64, '0')) {
            return parentChainwork;
        }
        return "0000000000000000000000000000000000000000000000000000000000000001";
    }
}

std::string BlockAcceptor::ComputeBlockHash(const ParsedBlock& block) {
    // This is already computed in ParseBlockFromHex
    return block.blockHash;
}

void BlockAcceptor::NotifyBlockConnected(const ParsedBlock& block, uint64_t height) {
    LOG_INFO("📡 Block connected notifications sent for height " + std::to_string(height));
    
    // Notify wallet of new block
    try {
        // Phase M.4: Convert ParsedBlock to Block format for wallet notification
        dinero::Block block_obj;
        block_obj.header = ToBlockHeader(block);

        // Parse transactions from hex strings
        uint32_t tx_index = 0;
        for (const auto& tx_hex : block.transactions) {
            std::vector<uint8_t> tx_bytes = HexToBytes(tx_hex);
            size_t offset = 0;
            dinero::Transaction tx;
            if (ParseTransaction(tx_bytes.data(), tx_bytes.size(), offset, tx)) {
                block_obj.vtx.push_back(tx);
                
                // ═══════════════════════════════════════════════════════════
                // MINING REWARD INDEXING: Index coinbase transactions
                // ═══════════════════════════════════════════════════════════
                // Note: Coinbase indexing is handled by WalletWorker::OnBlockConnected()
                // which processes all transactions including coinbase. The wallet worker
                // will automatically index coinbase outputs that match registered addresses.
                // No explicit indexing needed here - wallet worker handles it.
                if (tx_index == 0 && !tx.vout.empty()) {
                    // Coinbase transaction - wallet worker will process it
                    LOG_INFO("  ⛏️ Coinbase transaction detected - wallet worker will index mining rewards");
                }
                
                tx_index++;
            }
        }

        // ========================================================================
        // WALLET NOTIFICATION — REMOVED (duplicate, caused data race)
        // ========================================================================
        // Wallet is now notified exclusively via ConnectTip → notifyBlockConnected
        // → WalletNotify::OnBlockConnected → WalletWorker (single async thread).
        // The old direct call here was a SECOND notification for the same block
        // (ActivateBestChain already triggered ConnectTip → notifyBlockConnected).
        // Having both paths active caused concurrent access to WalletManager
        // (which has no mutex), leading to "double free or corruption" crashes.
        // ========================================================================

    } catch (const std::exception& e) {
        LOG_ERROR("❌ Failed to parse transactions or notify wallet worker: " + std::string(e.what()));
    }

    // ========================================================================
    // Phase 3F: WebSocket notification via DaemonContext (g_subscriptions global removed)
    // ========================================================================
    if (ctx_ && ctx_->websocket_subscriptions) {
        try {
            // Get current timestamp for event
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;

            std::stringstream ts_stream;
            ts_stream << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
            ts_stream << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
            std::string ts = ts_stream.str();

            // Get next sequence from subscription manager via context
            uint64_t topic_seq = ctx_->websocket_subscriptions->get_next_topic_seq("newBlocks");

            // Build event with metadata
            Json::Value block_event;
            block_event["type"] = "event";
            block_event["topic"] = "newBlocks";
            block_event["seq"] = static_cast<Json::Value::UInt64>(topic_seq);
            block_event["ts"] = ts;
            block_event["schema"] = "dinero.block.v1";
            block_event["source"] = "dinerod";

            Json::Value data;
            data["height"] = static_cast<Json::Value::UInt64>(height);
            data["hash"] = block.blockHash;
            data["timestamp"] = static_cast<Json::Value::UInt64>(block.timestamp);
            data["difficulty"] = static_cast<Json::Value::UInt64>(block.bits);
            block_event["data"] = data;

            // Serialize
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            std::string json_str = Json::writeString(builder, block_event);

            // Size cap: trim data if > 256 KiB
            if (json_str.size() > MAX_WS_MESSAGE_SIZE) {
                block_event["data"] = "[TRIMMED: payload exceeds 256 KiB]";
                block_event["warn"] = "Data payload trimmed due to size limit";
                block_event["original_size"] = static_cast<Json::Value::UInt64>(json_str.size());
                json_str = Json::writeString(builder, block_event);
            }

            // Broadcast to subscribers via context
            ctx_->websocket_subscriptions->enqueue("newBlocks", json_str);

            LOG_INFO("📡 WebSocket: Broadcast new block at height " + std::to_string(height));
        } catch (const std::exception& e) {
            LOG_ERROR("❌ Failed to broadcast WebSocket notification: " + std::string(e.what()));
        }
    }

    // ========================================================================
    // P2P BLOCK ANNOUNCEMENT - Critical for network propagation!
    // ========================================================================
    // Phase C.1.5: Restore single choke point - route through ChainstateService
    // All block relay (mined or received) must go through ChainstateService::BroadcastNewBlock()
    // This ensures centralized, observable, and consistent block announcement
    if (ctx_ && ctx_->chainstate) {
        try {
            auto chainstate_service = std::dynamic_pointer_cast<ChainstateService>(ctx_->chainstate);
            if (chainstate_service) {
                // ChainstateService will handle P2P broadcast via wired P2PService
                // CRITICAL FIX: Use getBestBlockHash() instead of block.blockHash
                // The block.blockHash is from wire format (may have wrong utreexo_root).
                // The tip hash in ChainDB has the correct hash computed from header.GetHash()
                // after setting the computed utreexo_root in ConnectBlock().
                std::string active_tip_hash = chainstate_service->getBestBlockHash();
                std::string active_hash_at_height;
                if (auto* db = chainstate_service->GetChainDB()) {
                    auto hash_at_height = db->getBlockHashByHeight(static_cast<int>(height));
                    if (hash_at_height.status() == dinero::Status::Ok) {
                        active_hash_at_height = hash_at_height.value().GetHex();
                    }
                }

                // Activation gate: announce only when this callback height matches
                // the active tip hash. This prevents stale/side-chain accepts from
                // being advertised as canonical new blocks.
                if (!active_tip_hash.empty() && active_tip_hash == active_hash_at_height) {
                    chainstate_service->BroadcastNewBlock(active_tip_hash);
                    LOG_INFO("✅ Block announced via ChainstateService (active tip) hash=" +
                             active_tip_hash.substr(0, 16) + "...");
                } else {
                    LOG_INFO("ℹ️ Block connected but not announced (not active tip): height=" +
                             std::to_string(height) + ", active_at_height=" +
                             (active_hash_at_height.empty() ? std::string("none") : active_hash_at_height.substr(0, 16) + "...") +
                             ", active_tip=" +
                             (active_tip_hash.empty() ? std::string("none") : active_tip_hash.substr(0, 16) + "..."));
                }
            } else {
                LOG_INFO("⚠️ ChainstateService not available, block not broadcast");
            }
        } catch (const std::exception& e) {
            LOG_ERROR("❌ Failed to broadcast block via ChainstateService: " + std::string(e.what()));
        }
    } else {
        LOG_INFO("⚠️ Chainstate service not available, block not broadcast to P2P network");
    }

    LOG_INFO("📡 Block connected notifications completed for height " + std::to_string(height));
}

// Wallet notification function moved back to main.cpp

// Utility functions
std::vector<uint8_t> BlockAcceptor::HexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.length() / 2);
    
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byteString, nullptr, 16));
        bytes.push_back(byte);
    }
    
    return bytes;
}

std::string BlockAcceptor::BytesToHex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; i++) {
        oss << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return oss.str();
}

uint32_t BlockAcceptor::ReadUint32LE(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

void BlockAcceptor::WriteUint32LE(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value & 0xff);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    data[2] = static_cast<uint8_t>((value >> 16) & 0xff);
    data[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

bool BlockAcceptor::ReadVarInt(const uint8_t* data, size_t dataSize, size_t& offset, uint64_t& value) {
    if (offset >= dataSize) return false;
    
    uint8_t first = data[offset++];
    if (first < 0xfd) {
        value = first;
        return true;
    } else if (first == 0xfd) {
        if (offset + 2 > dataSize) return false;
        value = static_cast<uint64_t>(data[offset]) | (static_cast<uint64_t>(data[offset + 1]) << 8);
        offset += 2;
        return true;
    } else if (first == 0xfe) {
        if (offset + 4 > dataSize) return false;
        value = static_cast<uint64_t>(data[offset]) |
                (static_cast<uint64_t>(data[offset + 1]) << 8) |
                (static_cast<uint64_t>(data[offset + 2]) << 16) |
                (static_cast<uint64_t>(data[offset + 3]) << 24);
        offset += 4;
        return true;
    } else if (first == 0xff) {
        if (offset + 8 > dataSize) return false;
        value = static_cast<uint64_t>(data[offset]) |
                (static_cast<uint64_t>(data[offset + 1]) << 8) |
                (static_cast<uint64_t>(data[offset + 2]) << 16) |
                (static_cast<uint64_t>(data[offset + 3]) << 24) |
                (static_cast<uint64_t>(data[offset + 4]) << 32) |
                (static_cast<uint64_t>(data[offset + 5]) << 40) |
                (static_cast<uint64_t>(data[offset + 6]) << 48) |
                (static_cast<uint64_t>(data[offset + 7]) << 56);
        offset += 8;
        return true;
    }
    return false;
}

bool BlockAcceptor::ParseTransaction(const uint8_t* data, size_t dataSize, size_t& offset, dinero::Transaction& tx) {
    // ✅ BULLETPROOF DESERIALIZATION (Bitcoin Core pattern)
    // Never re-serialize to determine consumed bytes!
    if (offset > dataSize) {
        LOG_ERROR("❌ [ParseTx] offset > dataSize: " + std::to_string(offset) + " > " + std::to_string(dataSize));
        return false;
    }
    std::vector<uint8_t> remaining(data + offset, data + dataSize);

    // DEBUG: Log transaction hex
    std::ostringstream hex_stream;
    for (size_t i = 0; i < std::min(remaining.size(), size_t(200)); i++) {
        hex_stream << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(remaining[i]);
    }
    LOG_INFO("🔍 [ParseTx] offset=" + std::to_string(offset) + " remaining=" + std::to_string(remaining.size()) + " bytes");
    LOG_INFO("🔍 [ParseTx] tx_hex_start: " + hex_stream.str());

    size_t consumed = 0;
    if (!TransactionSerializer::Deserialize(tx, remaining, consumed)) {
        LOG_ERROR("❌ [ParseTx] TransactionSerializer::Deserialize failed at offset " + std::to_string(offset));
        return false;
    }

    if (offset + consumed > dataSize) {
        LOG_ERROR("❌ [ParseTx] offset + consumed > dataSize: " + std::to_string(offset + consumed) + " > " + std::to_string(dataSize));
        return false;
    }
    offset += consumed;
    LOG_INFO("✅ [ParseTx] Success, consumed=" + std::to_string(consumed) + " bytes, new_offset=" + std::to_string(offset));
    return true;
}

// ============================================================================
// Reorg Support: ApplyTipInvalidation() - Disconnect chain tip (regtest-only)
// ============================================================================

bool BlockAcceptor::ApplyTipInvalidation(const std::string& blockhash, std::string& error) {
    try {
        // Authorization token for ChainDB writes (compile-time enforced)
        ChainWriteToken token;

        // Week 3: MIGRATED - Now uses ctx_->chainstate instead of dinero::legacy::g_chain_db_direct()
        if (!ctx_ || !ctx_->chainstate) {
            error = "Chainstate service not available";
            LOG_ERROR("❌ " + error);
            return false;
        }

        auto* daemon_ctx = DaemonContext::instance();
        // Phase 39: Get ChainDB via ChainstateService (ChainManager deleted)
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(daemon_ctx ? daemon_ctx->chainstate : nullptr);
        auto* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
        if (!chain_db) {
            error = "ChainDB not initialized";
            LOG_ERROR("❌ " + error);
            return false;
        }

        LOG_INFO("🔄 Starting tip invalidation for block " + blockhash.substr(0, 16) + "...");

        // Step 1: Load undo data from RocksDB
        std::string undoKey = "U:" + blockhash;
        std::string undoData;
        auto status = chain_db->getRaw(undoKey, undoData);

        if (status != dinero::Status::Ok) {
            error = "No undo data found for block " + blockhash.substr(0, 16) + "...";
            LOG_ERROR("❌ " + error);
            return false;
        }

        // Step 2: Deserialize undo record
        std::vector<uint8_t> undoBytes(undoData.begin(), undoData.end());
        dinero::UndoRecord undo = dinero::UndoRecord::Deserialize(undoBytes);

        LOG_INFO("📦 Loaded undo record: " + std::to_string(undo.spent.size()) +
                 " spent coins, " + std::to_string(undo.created.size()) + " created outputs");

        // Step 3+4 (#586, hardening of an UNWIRED path): a bare invalidateblock
        // RPC routes to ChainstateService::InvalidateBlock (which received the
        // #586 guard + activation-mutex serialization); registerBlockInvalidation
        // has no live callers, and the rig confirmed 18/18 harness invalidations
        // hit the ChainstateService path, 0 this one. Hardened anyway so future
        // wiring cannot resurrect the stale-authority defect.
        // Resolve the target block and its parent by IDENTITY,
        // not the height index. The old code assumed the target IS the current
        // tip (newHeight = ChainDB-tip - 1) and looked the parent up via
        // getBlockHashByHeight — both go stale the moment a concurrent connect
        // advances the chain between the RPC arriving and this running, which
        // produced an out-of-order rollback plan (DisconnectTip(57) while the
        // tip was 58) and a corrupted forest (utreexo-add-failed livelock).
        // The target's own header names its parent; its own height record
        // names its height. Neither can go stale.
        uint256 target_hash;
        if (!uint256::FromHex(blockhash, target_hash)) {
            error = "Invalid block hash hex: " + blockhash.substr(0, 16) + "...";
            LOG_ERROR("❌ " + error);
            return false;
        }
        auto target_header_result = chain_db->getHeader(target_hash);
        if (target_header_result.status() != dinero::Status::Ok) {
            error = "No header found for invalidation target " + blockhash.substr(0, 16) + "...";
            LOG_ERROR("❌ " + error);
            return false;
        }
        auto target_height_result = chain_db->getBlockHeight(target_hash);
        if (target_height_result.status() != dinero::Status::Ok || target_height_result.value() <= 0) {
            error = "No height record for invalidation target " + blockhash.substr(0, 16) + "...";
            LOG_ERROR("❌ " + error);
            return false;
        }
        const uint32_t targetHeight = static_cast<uint32_t>(target_height_result.value());
        const uint32_t tipHeight = dinero::storage::GetChainHeight(chain_db);
        if (targetHeight != tipHeight) {
            LOG_INFO("⚠️ Invalidation target height " + std::to_string(targetHeight) +
                     " != ChainDB tip " + std::to_string(tipHeight) +
                     " (concurrent connect?) — proceeding with identity-resolved parent");
        }
        uint32_t newHeight = targetHeight - 1;

        LOG_INFO("📊 Target height: " + std::to_string(targetHeight) +
                 " → new height: " + std::to_string(newHeight));

        uint256 newTipHash = target_header_result.value().prev_block_hash;

        LOG_INFO("🔗 New tip will be: " + newTipHash.GetHex().substr(0, 16) + "... at height " +
                 std::to_string(newHeight));

        // Step 5: Apply undo (atomic batch)
        rocksdb::WriteBatch batch;

        // 5a. Delete all outputs created by this block using ChainDB
        LOG_INFO("🗑️ Deleting " + std::to_string(undo.created.size()) + " outputs created by disconnected block...");
        for (const auto& created : undo.created) {
            // Remove UTXO from ChainDB
            // Phase M.0: created.txid is already uint256 (no conversion needed)
            auto status = chain_db->deleteCoin(token, created.txid, created.vout, &batch);
            if (status != Status::Ok) {
                LOG_ERROR("  ⚠️ Failed to delete UTXO: " + created.txid.GetHex().substr(0, 16) + "...:" + std::to_string(created.vout));
                // This could happen if the UTXO was already spent in a later block (which shouldn't happen for tip invalidation)
            } else {
                LOG_INFO("  ✅ Deleted UTXO: " + created.txid.GetHex().substr(0, 16) + "...:" + std::to_string(created.vout));
            }
        }

        // 5b. Restore all spent UTXOs using ChainDB
        LOG_INFO("♻️ Restoring " + std::to_string(undo.spent.size()) + " UTXOs spent by disconnected block...");
        for (const auto& spent : undo.spent) {
            // Build Coin struct to restore
            Coin coin;
            coin.amount = spent.value;
            // Phase M.0: Convert binary scriptPubKey to hex string for Coin
            coin.script_pubkey.reserve(spent.scriptPubKey.size() * 2);
            for (uint8_t byte : spent.scriptPubKey) {
                char buf[3];
                snprintf(buf, sizeof(buf), "%02x", byte);
                coin.script_pubkey += buf;
            }
            coin.height = static_cast<int>(spent.height);
            coin.coinbase = spent.is_coinbase;
            coin.is_confidential = spent.is_confidential;
            coin.commitment = spent.commitment;

            // Restore the spent UTXO to ChainDB
            // Phase M.0: spent.prev_txid is already uint256 (no conversion needed)
            auto status = chain_db->putCoin(token, spent.prev_txid, spent.prev_vout, coin, &batch);
            if (status != Status::Ok) {
                error = "Failed to restore UTXO: " + spent.prev_txid.GetHex().substr(0, 16) + "...:" + std::to_string(spent.prev_vout) + " - " + std::string(StatusToString(status));
                LOG_ERROR("❌ " + error);
                return false;
            }

            LOG_INFO("  ✅ Restored UTXO: " + spent.prev_txid.GetHex().substr(0, 16) + "...:" +
                     std::to_string(spent.prev_vout) + " (value=" + std::to_string(spent.value) +
                     ", height=" + std::to_string(spent.height) + ")");
        }

        // 5c. Get parent block header and chainwork
        // Week 3: MIGRATED - Use chain_db from context
        auto parentHeaderResult = chain_db->getHeader(newTipHash);  // Phase M.0: Already uint256
        if (parentHeaderResult.status() != dinero::Status::Ok) {
            error = "Failed to get parent header";
            LOG_ERROR("❌ " + error);
            return false;
        }

        // Get parent's tip info to retrieve chainwork
        // For simplified regtest, we'll use a basic chainwork calculation
        dinero::arith_uint256 parentWork(newHeight);  // Simplified: chainwork = height

        // 5d. Update chain tip to parent
        status = chain_db->setTip(token, newTipHash, newHeight, parentWork, &batch);  // Phase M.0: Already uint256
        if (status != dinero::Status::Ok) {
            error = "Failed to update tip to parent";
            LOG_ERROR("❌ " + error);
            return false;
        }

        LOG_INFO("✅ Tip updated to parent: " + newTipHash.GetHex().substr(0, 16) + "... at height " +
                 std::to_string(newHeight));

        // 5e. Keep undo data for debugging (don't delete)
        // This allows re-invalidating if needed and helps with testing
        LOG_INFO("📦 Keeping undo data for block " + blockhash.substr(0, 16) + "...");

        // Step 6: Commit atomically
        status = chain_db->writeBatch(token, std::move(batch), true);
        if (status != dinero::Status::Ok) {
            error = "Failed to commit tip invalidation";
            LOG_ERROR("❌ " + error);
            return false;
        }

        LOG_INFO("✅ Tip invalidation complete: height " + std::to_string(tipHeight) +
                 " → " + std::to_string(newHeight) + ", new tip: " + newTipHash.GetHex().substr(0, 16) + "...");

        // Phase 4B: Synchronize in-memory chainstate (forest + active tip) and notify wallet.
        if (ctx_ && ctx_->chainstate) {
            try {
                auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
                if (chainstate) {
                    dinero::uint256 blockHashU256 = dinero::uint256::FromHexUnsafe(blockhash);

                    // Mark invalidated tip as failed so candidate selection does not
                    // immediately re-activate it on the next ActivateBestChain pass.
                    if (auto* invalid_idx = chainstate->FindBlockIndex(blockHashU256)) {
                        invalid_idx->status |= dinero::BLOCK_FAILED_VALID;
                        chainstate->RemoveCandidate(invalid_idx);
                    }

                    // Restore forest + active tip to the same parent height we just wrote to ChainDB.
                    std::string sync_error;
                    if (!chainstate->ReloadConsensusUTXOFromDB(sync_error)) {
                        error = "Failed to reload in-memory consensus UTXO state: " + sync_error;
                        LOG_ERROR("❌ " + error);
                        return false;
                    }
                    if (!chainstate->RestoreUtreexoCheckpoint(newHeight, sync_error)) {
                        error = "Failed to restore in-memory Utreexo checkpoint: " + sync_error;
                        LOG_ERROR("❌ " + error);
                        return false;
                    }
                    if (!chainstate->ForceSetActiveTip(newTipHash, sync_error)) {
                        error = "Failed to update in-memory active tip: " + sync_error;
                        LOG_ERROR("❌ " + error);
                        return false;
                    }
                    if (auto* parent_idx = chainstate->FindBlockIndex(newTipHash)) {
                        chainstate->AddCandidate(parent_idx);
                    }

                    // Reconstruct disconnected block for downstream wallet/mempool notifications.
                    auto blockResult = chainstate->getBlockByHash(blockHashU256);

                    if (blockResult.ok()) {
                        dinero::Block disconnected_block = blockResult.value();
                        chainstate->notifyBlockDisconnected(disconnected_block, tipHeight);
                        LOG_INFO("✅ Wallet disconnect notifications dispatched for block " + std::to_string(tipHeight));
                    } else {
                        if (ctx_->wallet) {
                            auto& wallet = ctx_->wallet->get();
                            wallet.setBlockchainHeight(newHeight);
                            LOG_INFO("⚠️  Block not found in DB, fallback to basic height update: " + std::to_string(newHeight));
                        }
                    }
                } else {
                    LOG_ERROR("⚠️  ChainstateService not available for wallet notifications");
                }
            } catch (const std::exception& e) {
                LOG_ERROR("⚠️  Failed to notify wallet of reorg: " + std::string(e.what()));
                // Don't fail the reorg if wallet notification fails
            }
        } else {
            LOG_INFO("⚠️  Chainstate service not available, skipping reorg notification");
        }

        return true;

    } catch (const std::exception& e) {
        error = std::string("Exception during tip invalidation: ") + e.what();
        LOG_ERROR("❌ " + error);
        return false;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Phase M.4: Clean Boundary Conversion (ParsedBlock → BlockHeader)
// ═════════════════════════════════════════════════════════════════════════════
// SINGLE SOURCE OF TRUTH for converting wire format to consensus format.
// This eliminates duplicate assignments, legacy compatibility hacks, and
// ensures consistent Phase 3 field names throughout the codebase.
//
// Architecture:
//   ParsedBlock (wire/display)  →  BlockHeader (consensus)
//   - camelCase strings         →  - snake_case uint256
//   - bits                      →  - difficulty
//   - utreexoRoot               →  - utreexo_root
// ═════════════════════════════════════════════════════════════════════════════
BlockHeader BlockAcceptor::ToBlockHeader(const ParsedBlock& parsed) {
    BlockHeader header{};  // Zero-initialize (including reserved[12])

    // Version (consensus field)
    header.version = parsed.version;

    // Phase M.0: Convert hex strings to uint256
    header.prev_block_hash = uint256::FromHexUnsafe(parsed.prevBlockHash);
    header.merkle_root = uint256::FromHexUnsafe(parsed.merkleRoot);
    header.utreexo_root = uint256::FromHexUnsafe(parsed.utreexoRoot);

    // Timestamp (Phase 3: 64-bit, but ParsedBlock still uses 32-bit for wire compat)
    header.timestamp = static_cast<uint64_t>(parsed.timestamp);

    // Phase 3: bits → difficulty (semantic rename)
    header.difficulty = parsed.bits;

    // Nonce
    header.nonce = parsed.nonce;

    // reserved[12] is already zero-initialized

    return header;
}
