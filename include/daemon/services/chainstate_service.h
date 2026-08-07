#pragma once
#include "daemon/iservice.h"
#include "storage/chain_db.h"
#include "wallet/utxo_index.h"
#include "interfaces/wallet_notifier.h"  // Phase 3D: Wallet event notifications
#include "consensus/utreexo_accumulator.h"  // v0.14.0.4: Utreexo enforcement
#include "consensus/header_convergence.h"  // #439: HeaderConvergence + pure rule
#include "consensus/block_index.h"  // Phase 41: BlockIndex graph for reorg logic
#include "consensus/utxo_snapshot.h"  // Phase 42: AssumeUTXO snapshot structures
#include "daemon/services/assumeutxo_lifecycle.h"  // AssumeUTXO fatal state machine
#include "daemon/services/unreadable_block_set.h"  // thread-safe unreadable-block tracking
#include "daemon/services/stateless_replay_shielded_decision.h"  // #356: marker-guard decision
#include "daemon/sync_stats_recorder.h"  // Forest checkpoint delta campaign phase 0
#include "daemon/reorg_log.h"
#include "consensus/block_validation.h"  // Reorg fix: Production consensus validator
#include "consensus/consensus_utxo_set.h"  // Phase 2: Pure in-memory UTXO set
#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "indexing/utxo_position_index.h"  // Phase 11a: Global UTXO → Utreexo position mapping
#include "p2p/orphan_block_pool.h"  // Phase C.1 v2: Orphan block handling for P2P relay
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <set>  // Phase 39: For std::set<std::string> completed_blocks_
#include <unordered_map>  // Phase 41: For BlockIndex storage
#include <unordered_set>
#include <map>  // Phase C.3: For in_flight_blocks_
#include <filesystem>  // Phase 42: For snapshot file paths
#include <thread>  // Phase 44: For background validation worker
#include <atomic>  // Phase 44: For thread-safe stop signal
#include <mutex>   // Phase 44: For background validation state protection
#include <condition_variable>  // #298: wake-on-store for background validation
#include <algorithm>

// Phase C.1.5: Forward declaration for P2P message handlers
struct P2PMessage;

namespace dinero {

// Phase 39 Step 2: Forward declaration (header deleted)
class ChainManager;
class BlockStorage;
class WalletManager;  // Snapshot wallet rescan (see RescanWalletFromSnapshotUTXOs)
struct FilePosition;  // #309: storage/block_storage.h

namespace consensus {
    class WalletUTXOAdapter;  // v2.2.0: Forward declare adapter (breaks header dependency)
    class ConsensusUTXOSet;   // Phase 2: Pure in-memory UTXO set (owns forest)
    class IConsensusUTXOSet;  // Phase 2: Consensus UTXO set interface
    class HeaderChainSelector;  // P2P fix: Header chain tracking for sync
    struct HeaderIndexEntry;    // Header-first sync index entry
    class ProofGossipManager;   // Phase 9.3+: Proof gossip prewarm/metrics
    enum class ConnectBlockResult : int;  // Scheduler drain connection classification
}

// Phase 9.2: Forward declarations for IPC oracle clients
namespace ipc {
    class ChainOracleClient;
    class TimeOracleClient;
    class TransactionOracleClient;
}

// Phase P.2: Forward declaration for BridgeNode (Utreexo proof serving)
namespace network {
    class BridgeNode;
    class StatelessNode;  // CSN reorg support
}

// AssumeUTXO mode exit: forward declaration of the genesis->base replay engine
// (full type included by chainstate_service.cpp for PromoteValidatedHistory).
namespace assumeutxo {
    class AssumeUtxoReplayEngine;
}
namespace pool {
    class PoolManager;  // Pool accounting lifecycle callbacks
}

/**
 * ChainstateService - Consensus-critical blockchain state service
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * ARCHITECTURAL LAW (ONE DB Definition)
 * ═══════════════════════════════════════════════════════════════════════════
 * ChainstateService is a pure validation context.
 * It does NOT own, construct, or expose ChainDB.
 * All storage authority lives in ChainManager.
 *
 * ChainDB is constructed by DaemonApp and transferred to ChainManager.
 * ChainstateService receives read-only access via ChainManager.
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Manages blockchain state coordination and validation.
 * Handles block storage, chain validation, and UTXO management.
 *
 * Dependencies: Logger, Config
 *
 * Initialization order:
 * - Init() creates ChainManager (DaemonApp provides ChainDB separately)
 * - Start() initializes genesis block and loads chain state
 * - Stop() performs clean shutdown and flushes data
 */
class ChainstateService : public IService {
public:
    ChainstateService();  // v2.2.4: Out-of-line (WalletUTXOAdapter incomplete type)
    ~ChainstateService() override;  // v2.2.4: Out-of-line (WalletUTXOAdapter incomplete type)

    std::string Name() const override { return "Chainstate"; }

    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    // Service health
    bool IsHealthy() const override;
    std::string GetMetrics() const override;

    // Phase 39: ChainManager accessor (temporary - callers being migrated)
    ChainManager& chainManager();
    const ChainManager& chainManager() const;

    // Phase 39: ChainDB accessor (re-enabled after ChainManager deletion)
    // Callers should use this instead of chainManager()->GetChainDB()
    ChainDB* GetChainDB();
    const ChainDB* GetChainDB() const;

    // Phase 39: Set ChainDB pointer (called by DaemonApp during initialization)
    void setChainDB(ChainDB* db) { chain_db_ = db; }

    // Phase 11a: BlockValidator accessor (for BlockAcceptor to update Utreexo)
    consensus::BlockValidator* GetBlockValidator() { return block_validator_.get(); }
    const consensus::BlockValidator* GetBlockValidator() const { return block_validator_.get(); }

    // Phase 2: Direct access to consensus UTXO set
    consensus::IConsensusUTXOSet* GetConsensusUTXOSet() { return consensus_utxo_set_.get(); }
    const consensus::IConsensusUTXOSet* GetConsensusUTXOSet() const { return consensus_utxo_set_.get(); }

    // Scan the in-memory snapshot (AssumeUTXO) UTXO set for coins owned by
    // `wallet` and record them into the wallet's local UTXO table, advancing the
    // wallet's scan watermark to `base_height`. Complements the block-replay
    // rescan, which cannot see pre-base coins (no block bodies in headers-only
    // mode). Idempotent. Returns the number of owned coins recorded, or -1 if no
    // snapshot UTXO set is loaded. Used by the LoadSnapshot completion hook,
    // WalletService startup sweep, and wallet.importmnemonic when the mnemonic
    // is imported only after snapshot activation.
    int RescanWalletFromSnapshotUTXOs(WalletManager& wallet, uint32_t base_height);

    // v7 shielded pool state accessors.
    consensus::shielded::CommitmentTree* GetShieldedCommitmentTree() { return &shielded_tree_; }
    const consensus::shielded::CommitmentTree* GetShieldedCommitmentTree() const { return &shielded_tree_; }
    consensus::shielded::NullifierSet* GetShieldedNullifierSet() { return &shielded_nullifiers_; }
    const consensus::shielded::NullifierSet* GetShieldedNullifierSet() const { return &shielded_nullifiers_; }
    const consensus::shielded::AnchorHistory& GetShieldedAnchorHistory() const { return shielded_anchor_history_; }

    // Composite state hash: SHA256 over the bytes of every container
    // that crosses the reorg boundary (utreexo forest commitment +
    // numLeaves; shielded tree root + size; nullifier set size; anchor
    // history size, last height, last root). Used by the
    // ShieldedReorgInvertibility property test to assert byte-equality
    // across Connect/Disconnect/Connect cycles. Reproducible across
    // restarts and across nodes at the same tip — any drift in any
    // tracked container changes the hash.
    uint256 ComputeShieldedReorgStateHash() const;

    // Phase 3b step 3 part 2 — startup verification of the journal
    // row §1.4 names. After ActivateBestChain settles on the
    // canonical tip and the in-memory shielded state is loaded,
    // this method looks up
    //   consensus_journal:<height_be_hex>:<tip_hash_hex>
    // in ChainDB. If the row exists, it compares the stored DSRH v2
    // hex against the live state's ComputeShieldedReorgStateHash().
    //   - match  → log info, return true
    //   - absent → log info (pre-flag-on blocks don't have rows;
    //              not an error). Return true.
    //   - mismatch → enter consensus safe mode with reason
    //                "consensus_journal_state_mismatch", refuse
    //                template generation + block connect, require
    //                operator safemode.exit. Return false.
    //
    // Returns true if state is consistent (or unverifiable due to
    // absent row), false if a mismatch was detected and safe mode
    // was entered.
    bool VerifyConsensusJournalAtActiveTip();

    // Phase 11a: Utreexo forest accessor (for extracting root hash)
    // Phase 2: Forest now owned by ConsensusUTXOSet
    consensus::UtreexoForest* GetUtreexoForest() {
        return consensus_utxo_set_ ? &consensus_utxo_set_->GetForest() : nullptr;
    }
    const consensus::UtreexoForest* GetUtreexoForest() const {
        return consensus_utxo_set_ ? &consensus_utxo_set_->GetForest() : nullptr;
    }

    // Phase 8: Set validation mode (called by DaemonApp during initialization)
    void setValidationMode(consensus::ValidationMode mode);

    // Phase 40: Chain Activation (foundation for reorg logic)
    // Activates the best chain tip and notifies dependent services
    // Phase 41: Now includes full reorg logic via BlockIndex graph
    void ActivateBestChain();

    // Phase 41: BlockIndex Management
    // Create or find block index entry in the chain graph
    class CBlockIndex* FindBlockIndex(const uint256& hash);
    class CBlockIndex* AddBlockIndex(const BlockHeader& header, uint32_t height);
    void UpdateChainwork(class CBlockIndex* block_index);
    void AddCandidate(class CBlockIndex* block_index);
    void RemoveCandidate(class CBlockIndex* block_index);
    class CBlockIndex* GetBestCandidate();

    // #309: candidate eligibility for reorg targets. A not-yet-validated side
    // branch is a legitimate reorg target if its whole branch back to a connected
    // (BLOCK_VALID_CHAIN) base has block bodies present; full per-block validation
    // is deferred to the reorg ConnectTip walk (the Bitcoin Core model). Requiring
    // BLOCK_VALID_CHAIN up front wedged a node on its own minority fork because a
    // multi-block side branch can never pre-validate (its parent's UTXO state is
    // not applied until connected).
    bool IsReorgCandidateEligible(class CBlockIndex* block_index);
    bool HasBranchDataToConnectedBase(class CBlockIndex* block_index);

    // Phase 41: Reorg Execution
    // Find common ancestor of two chains
    class CBlockIndex* FindFork(class CBlockIndex* a, class CBlockIndex* b);
    // Disconnect blocks from old chain
    bool DisconnectTip(class CBlockIndex* tip_to_disconnect);
    // Connect blocks to new chain
    // out_consensus_invalid (#309/I2): set true ONLY when the failure came from
    // BlockValidator::ConnectBlock (a consensus-rule violation), so the reorg
    // driver can permanently reject a consensus-invalid speculative branch
    // (mark BLOCK_FAILED_VALID) instead of looping, while leaving operational
    // failures (missing-utxo, I/O) un-poisoned.
    bool ConnectTip(class CBlockIndex* tip_to_connect, std::string* out_error = nullptr,
                    bool* out_consensus_invalid = nullptr);

    // CSN reorg: Bookkeeping-only connect (no ConnectBlock, no forest mutation).
    // Writes coin changes, an UndoRecord (spent/created + shielded fields),
    // shielded frontier/anchor/marker/nullifier state, tip pointer, height
    // index, notifications. Used after StatelessNode::ReplayBlock() has
    // already advanced the forest. `shielded_undo` is the BlockUndo the
    // caller built while applying this block's shielded section (pass
    // nullptr when no shielded apply preceded this call, e.g. ConnectTip's
    // stateless-replay branch) — its pre_block_shielded_frontier /
    // pre_reset_shielded_epoch feed the persisted UndoRecord so a later
    // DisconnectTip-CSN can roll this block back. For a shielded-bearing
    // block with no usable shielded undo (nullptr, or frontier unset), the
    // undo write is SKIPPED with a loud warning so a later disconnect of
    // that block fails loudly ("Missing undo data") instead of silently
    // skipping the shielded rollback.
    bool CommitConnectedBlockBookkeeping(class CBlockIndex* block_index, const Block& block,
                                         const consensus::BlockUndo* shielded_undo,
                                         std::string* out_error = nullptr);

    // Phase 41: Active chain tip (replaces ChainDB getTip for consensus)
    class CBlockIndex* GetActiveTip() const { return active_tip_; }

    // Block invalidation (production-safe, all networks)
    // Mark a block and all descendants as invalid; disconnect from active chain if needed.
    bool InvalidateBlock(const uint256& hash, std::string& error);
    // Clear invalid flags from a block and descendants; re-evaluate for best chain.
    bool ReconsiderBlock(const uint256& hash, std::string& error);

    // Regtest maintenance helpers (used by invalidateblock path)
    // Rebind in-memory active tip pointer to a specific block hash.
    bool ForceSetActiveTip(const uint256& hash, std::string& error);
    // Restore in-memory Utreexo forest from persisted checkpoint at given height.
    bool RestoreUtreexoCheckpoint(uint32_t height, std::string& error);
    // Reload in-memory consensus UTXO cache from ChainDB tip state.
    bool ReloadConsensusUTXOFromDB(std::string& error);
    // Verify canonical chainstate alignment (active tip vs persisted ChainDB tip).
    bool IsCanonicalStateAligned(std::string* reason = nullptr) const;

    // Operator repair tool for legacy metadata drift surfaced by the
    // startup undo audit. Dry-run by default: scans active-chain tail
    // blocks and reports whether missing BLOCK_HAVE_UNDO flags can be
    // safely re-stamped from already-readable flatfile undo metadata.
    // Apply mode only sets BLOCK_HAVE_UNDO when the existing metadata
    // still has a non-zero undo position/size and the referenced bytes
    // read + deserialize + structurally match the block body.
    struct UndoMetadataRestampEntry {
        uint32_t height{0};
        uint256 hash;
        uint32_t status_flags{0};
        uint32_t undo_file{0};
        uint32_t undo_pos{0};
        uint32_t undo_size{0};
        bool has_undo_flag{false};
        bool undo_readable{false};
        bool undo_decodable{false};
        bool block_readable{false};
        bool restampable{false};
        bool repaired{false};
        std::string reason;
    };

    struct UndoMetadataRestampReport {
        bool apply{false};
        uint32_t scanned{0};
        uint32_t restampable{0};
        uint32_t repaired{0};
        uint32_t failed{0};
        std::vector<UndoMetadataRestampEntry> entries;
    };

    UndoMetadataRestampReport AuditUndoMetadataForRestamp(uint32_t max_blocks_back,
                                                          bool apply,
                                                          bool include_ok);

    // Regtest-only test hook: clears only the BLOCK_HAVE_UNDO bit while
    // preserving undo_file/undo_pos/undo_size, so integration tests can
    // prove the auditor detects and repairs the exact historical drift.
    bool DebugClearUndoFlagForBlock(const uint256& hash, std::string& error);

    // CSN reorg support: Wire StatelessNode for forest checkpoint restore
    void setStatelessNode(std::shared_ptr<network::StatelessNode> sn) { stateless_node_ = std::move(sn); }
    // Pool accounting support: Wire PoolManager for confirmation/orphan updates
    void setPoolManager(std::shared_ptr<pool::PoolManager> manager) { pool_manager_ = std::move(manager); }

    // CSN reorg reset signal: OnUtxoBlock handler checks this to reset next_validate_height
    uint32_t ConsumeCSNReorgResetHeight() {
        return csn_reorg_reset_height_.exchange(0);
    }

    // Phase 43: Safe Mode Management (Deep Reorg Protection)
    // Safe mode prevents mining during dangerous chain conditions
    bool IsInSafeMode() const { return safe_mode_active_; }
    std::string GetSafeModeReason() const { return safe_mode_reason_; }

    /// Read-only observability: reorganisations recorded this process lifetime.
    /// Exposed over RPC as reorg.status. Never consulted by consensus logic.
    const ReorgLog& GetReorgLog() const { return reorg_log_; }
    void EnterSafeMode(const std::string& reason);
    void RequestChainstateRecovery(const std::string& reason,
                                   const std::string& source_tag = "[external]");
    void ExitSafeMode();

    // Phase 43: Reorg depth threshold for safe mode
    static constexpr int DEEP_REORG_THRESHOLD = 100; // Blocks

    // Crash Safety: Incomplete reorg detection
    // Returns true if an incomplete reorg was detected on startup
    // Signals that wallet should be rescanned to verify balance integrity
    bool WasIncompleteReorgDetected() const { return incomplete_reorg_detected_; }

    // Rebuild transaction index for all blocks (one-time backfill)
    // Returns {indexed_blocks, indexed_txs} or error
    std::pair<uint64_t, uint64_t> RebuildTxIndex();

    // Phase 42: AssumeUTXO Snapshot Export/Import
    // Export current UTXO set to snapshot file
    consensus::SnapshotExportResult ExportSnapshot(const std::filesystem::path& snapshot_path);
    // Load UTXO set from snapshot file (fast sync)
    consensus::SnapshotImportResult LoadSnapshot(const std::filesystem::path& snapshot_path);

    // Phase 42: AssumeUTXO State Management
    bool IsAssumeUTXOActive() const { return assumeutxo_active_; }
    uint256 GetAssumeUTXOBaseBlock() const { return assumeutxo_base_block_; }
    uint32_t GetAssumeUTXOBaseHeight() const { return assumeutxo_base_height_; }
    // Wallet recovery must remain available after background validation
    // promotes the snapshot and clears assumeutxo_active_. Snapshot-bootstrapped
    // nodes still lack pre-base block bodies after promotion, so a wallet opened
    // or imported later must scan the configured snapshot UTXO section.
    uint32_t GetSnapshotWalletRecoveryBaseHeight() const;

    // Fatal-state-machine lifecycle (docs/design/assumeutxo-fatal-state-machine.md).
    // Never nullptr after Initialize(); guarded internally.
    assumeutxo::AssumeUtxoLifecycle* GetAssumeUtxoLifecycle();

    // Operator reset for fatal_mismatch: atomically calls lifecycle OperatorReset
    // then clears persisted AssumeUTXO metadata so post-reset restarts don't
    // recreate the legacy-upgrade hole. Returns false if the token is wrong or
    // the lifecycle is not in FatalMismatch; true on success.
    bool ResetAssumeUtxoFatalState(const std::string& confirm_token);

    // Phase 44: Background Validation (makes AssumeUTXO production-safe)
    enum class BackgroundValidationStatus {
        NotStarted,     // No validation in progress
        InProgress,     // Validating blocks from genesis
        Completed,      // Validation succeeded, UTXO sets match
        Failed          // Validation failed, snapshot invalid
    };

    struct BackgroundValidationProgress {
        BackgroundValidationStatus status;
        uint32_t current_height;      // Height currently being validated
        uint32_t target_height;       // Snapshot base height (validation goal)
        uint32_t blocks_validated;    // Total blocks validated so far
        double progress_percent;      // Percentage complete (0.0 - 100.0)
        std::string error_message;    // Error details if status == Failed
    };

    // Start background validation of assumed UTXO set
    void StartBackgroundValidation();
    // Get current validation progress
    BackgroundValidationProgress GetBackgroundValidationProgress() const;
    // Check if background validation is complete and successful
    bool IsBackgroundValidationComplete() const;

    // #298 no-progress hang watchdog. Call from an always-running periodic
    // loop that is INDEPENDENT of the bg-validation worker thread (the p2p
    // scheduler tick loop). Emits one loud, timestamped ERROR marker when
    // NEITHER the bg-validation height NOR the foreground tip height has
    // advanced for > kHangWatchdogMinutes while validation is in progress —
    // turning a silent multi-hour wedge into a capturable gdb hint. Diagnose
    // only; never restarts anything. Cheap and lock-light; safe to call ~5s.
    static constexpr int kHangWatchdogMinutes = 15;
    void CheckHangWatchdog();

    // Phase 45: Snapshot-accelerated IBD (Initial Block Download fast sync)
    enum class IBDStatus {
        NotInIBD,           // Node is synced, not in IBD
        InIBD,              // Node is syncing (traditional IBD)
        SnapshotBootstrap,  // Node loaded snapshot, immediately usable
        IBDComplete         // IBD finished successfully
    };

    struct IBDProgress {
        IBDStatus status;
        uint32_t local_height;        // Our current chain height
        uint32_t network_height;      // Estimated network height (0 if unknown)
        uint32_t blocks_remaining;    // Blocks left to sync (0 if synced)
        double sync_percent;          // Percentage synced (0.0 - 100.0)
        bool snapshot_loaded;         // True if bootstrapped from snapshot
        bool services_ready;          // True if node can serve RPC/mining/wallets
    };

    // Detect if node is in Initial Block Download
    bool IsInIBD() const;
    // Get IBD progress information
    IBDProgress GetIBDProgress() const;

    // ═══════════════════════════════════════════════════════════════════════
    // Canonical sync snapshot (issue #439)
    //
    // Header-chain convergence is a DISTINCT state from the three that already
    // exist here. Do not conflate them; see
    // docs/architecture/sync-state-behavior-matrix.md.
    //
    //   1. header convergence  — this snapshot: does the active chain match the
    //                            best known header chain?
    //   2. initial-download    — IsInIBD(): should we be downloading rather
    //      policy                than trusting our tip? (network-height and
    //                            snapshot aware; UNCHANGED by #439)
    //   3. service readiness   — AreServicesReady()
    //   4. AssumeUTXO ready    — assumeutxo + IsBackgroundValidationComplete()
    //                            (e.g. CanPruneNow())
    //
    // A snapshot node at convergence with background validation still running
    // must simultaneously report Converged, services READY, and prune-unsafe.
    // Collapsing these would break at least one consumer.
    // ═══════════════════════════════════════════════════════════════════════

    // The convergence vocabulary and rule live in the tiny standalone header
    // consensus/header_convergence.h so the behavior-matrix test can exercise
    // them without pulling in RocksDB and the rest of the daemon. Aliased here
    // so consumers keep using ChainstateService::HeaderConvergence.
    using HeaderConvergence = dinero::consensus::HeaderConvergence;

    // The snapshot VALUE TYPE lives in consensus/header_convergence.h alongside
    // the rule, so tests exercise the production IsConverged() directly rather
    // than re-implementing it. Aliased so consumers keep using
    // ChainstateService::SyncSnapshot.
    using SyncSnapshot = dinero::consensus::SyncSnapshot;

    /**
     * @brief Single canonical source of header-chain sync facts.
     *
     * Value-returning and lock-safe: the best header is copied out of
     * HeaderChainSelector under its mutex (GetBestHeaderCopy), so no pointer
     * escapes and no field is read outside the lock. Consumers must use this
     * rather than reaching into HeaderChainSelector themselves.
     *
     * Convergence compares HASHES, not heights: an equal-height reorg
     * (same height, different hash) is NOT convergence.
     */
    SyncSnapshot GetSyncSnapshot() const;

    /**
     * @brief The convergence rule, as a pure function.
     *
     * Extracted so the behavior matrix
     * (docs/architecture/sync-state-behavior-matrix.md) can be tested directly
     * — cold start, headers-ahead, convergence, equal-height reorg, missing
     * inputs and restart are all decided here, without standing up a
     * ChainstateService, a ChainDB or a peer.
     *
     * Fails closed: any missing input yields Unknown.
     * Compares hashes, never heights.
     */
    static HeaderConvergence ComputeConvergence(bool has_best_header,
                                                const uint256& best_header_hash,
                                                bool has_active_tip,
                                                const uint256& active_tip_hash) {
        return dinero::consensus::ComputeHeaderConvergence(
            has_best_header, best_header_hash, has_active_tip, active_tip_hash);
    }

    // Forest checkpoint delta campaign phase 0
    // (docs/design/forest-checkpoint-deltas.md): per-block connect latency +
    // forest-checkpoint write volume, for getsynchealth /
    // getsnapshotbootstrapstatus and the campaign's A/B baseline.
    SyncStatsRecorder::Snapshot GetSyncStatsSnapshot() const {
        return sync_stats_.GetSnapshot();
    }
    // CSN validation worker's extra full-forest checkpoint write
    // (daemon_app CSN reorg-support path).
    void RecordCsnForestCheckpoint(uint64_t bytes) {
        sync_stats_.RecordCsnCheckpoint(bytes);
    }
    // Update estimated network height (called by P2PService on peer connect)
    void UpdateNetworkHeight(uint32_t peer_height) {
        if (peer_height > ibd_network_height_) {
            ibd_network_height_ = peer_height;
        }
    }
    // Attempt to bootstrap from snapshot during IBD
    bool TrySnapshotBootstrap(const std::filesystem::path& snapshot_path);

    // FIX 2 (issue #186): deferred snapshot bootstrap. A configured
    // `assumeutxo_snapshot` cannot be loaded at startup (the base block isn't on
    // our header chain yet) and must not pre-empt block download from genesis.
    // The base height/hash are PEEKED at startup and a "pending" state is set;
    // block download is deferred until headers reach the EXACT base hash, then
    // the snapshot loads. If headers pass the base height without the base hash
    // appearing (stale/orphaned snapshot), pending is cleared and the node falls
    // back to full IBD — it never blocks forever.
    // Defer block download while the bootstrap is Pending (awaiting base hash)
    // OR Loading (a thread is importing the snapshot) — never connect blocks
    // concurrently with the load.
    bool IsSnapshotBootstrapPending() const {
        const auto s = snapshot_bootstrap_state_.load();
        return s == SnapshotBootstrapState::Pending || s == SnapshotBootstrapState::Loading;
    }
    // Drive the deferred bootstrap: load if the base hash is now on the header
    // chain; give up (→ full IBD) if headers passed the base height without it.
    // Safe no-op when no bootstrap is pending.
    void TryDeferredSnapshotBootstrap();

    // Check if node services are ready (RPC, mining, wallets)
    bool AreServicesReady() const;

    // Phase 46: Snapshot-based Pruning (disk optimization after snapshot bootstrap)
    enum class PruningMode {
        Disabled,   // No pruning
        Manual,     // Manual pruning via RPC
        Auto        // Automatic pruning based on disk usage
    };

    struct PruningInfo {
        PruningMode mode;
        bool pruning_enabled;
        uint32_t pruned_height;        // Highest block that's been pruned
        uint64_t target_disk_usage_mb; // Target disk usage in MB
        uint64_t current_disk_usage_mb;// Current disk usage in MB
        uint64_t blocks_pruned;        // Number of blocks pruned
        uint64_t bytes_freed;          // Disk space freed by pruning
        bool can_prune;                // True if safe to prune now
        std::string prune_status;      // Human-readable status message
    };

    // Get current pruning status and statistics
    PruningInfo GetPruningInfo() const;
    // Manually prune blockchain up to specified height (returns blocks pruned)
    uint32_t PruneBlockchain(uint32_t target_height);
    // Check if pruning is safe (snapshot loaded + validation complete)
    bool CanPruneNow() const;
    // Enable/disable automatic pruning
    void SetPruningMode(PruningMode mode, uint64_t target_disk_mb = 10000);

    // Access to UTXO index (for legacy code compatibility)
    UTXOIndex* utxoIndex() { return utxo_index_.get(); }
    const UTXOIndex* utxoIndex() const { return utxo_index_.get(); }

    // Aliases for global shim compatibility
    UTXOIndex* getUTXOIndex() { return utxoIndex(); }
    const UTXOIndex* getUTXOIndex() const { return utxoIndex(); }

    // v0.14.0.4: Access to Utreexo accumulator (consensus-critical state)
    // Phase 2: Forest now owned by ConsensusUTXOSet
    consensus::UtreexoForest* utreexoForest() {
        return consensus_utxo_set_ ? &consensus_utxo_set_->GetForest() : nullptr;
    }
    const consensus::UtreexoForest* utreexoForest() const {
        return consensus_utxo_set_ ? &consensus_utxo_set_->GetForest() : nullptr;
    }

    // Phase 11a: Global UTXO position index for proof generation
    indexing::UTXOPositionIndex* GetUTXOPositionIndex() { return utxo_position_index_.get(); }
    const indexing::UTXOPositionIndex* GetUTXOPositionIndex() const { return utxo_position_index_.get(); }

    // Aliases for consistency
    consensus::UtreexoForest* getUtreexoForest() { return utreexoForest(); }
    const consensus::UtreexoForest* getUtreexoForest() const { return utreexoForest(); }

    // Forward commonly used methods (ONE DB: Delegate to global g_chain_manager)
    // CRITICAL FIX (Nov 7, 2025): Query ChainDB (RocksDB), not legacy blockchain_ (SQLite)
    // RocksDB is the source of truth; SQLite (ExplorerDB) is read-only analytics
    uint32_t getBlockHeight() const;
    std::string getBestBlockHash() const;

    /**
     * @brief Announce current tip to all peers (Phase G.X: Fork Resolution)
     *
     * Called after mining stops or periodically to ensure peers know about
     * our best chain. This helps resolve diverged tips after rapid block production.
     */
    void AnnounceTip();

    // Block query methods (ChainDB-backed legacy compatibility helpers)
    bool hasBlock(uint32_t height) const;
    std::string getBlock(uint32_t height) const;
    bool hasBlockByHash(const uint256& hash) const;
    bool hasReadableBlockByHash(const uint256& hash) const;
    // #309: record a stored body's flatfile position (file/pos/size) + BLOCK_HAVE_DATA
    // in the block's existing ChainDB header metadata, so a stored-but-not-yet-
    // connected block (competing side-branch above the active tip) is recognized
    // by HasArchivalBlockBody / the import loop. No-op if header metadata is absent
    // or the body position is already recorded. Wired from the scheduler's
    // SetPersistBodyPositionCallback (invoked outside the scheduler mutex).
    void PersistStoredBodyPosition(const uint256& hash, const FilePosition& pos);
    bool hasFlatfileBlockByHash(const uint256& hash) const;
    StatusOr<Block> getBlockByHash(const uint256& hash) const;
    uint64_t getLegacyBodyFallbackReadCount() const;
    uint64_t getLegacyUndoFallbackReadCount() const;
    bool strictArchivalReadsEnabled() const;

    // ========================================================================
    // Phase C.1: P2P Block Relay Integration
    // Connect P2P layer to consensus layer (BlockAcceptor)
    // ========================================================================

    /**
     * @brief Process incoming block from P2P network (hex format)
     *
     * Called by the active P2P message router when a BLOCK message is received from a peer.
     * Validates the block using BlockAcceptor, and broadcasts to other peers if accepted.
     *
     * @param blockHex Hex-encoded block data
     * @param peer_id Identifier of the peer that sent the block
     * @return true if block was accepted, false if rejected
     */
    bool ProcessIncomingBlockHex(const std::string& blockHex, const std::string& peer_id);

    /**
     * @brief Process incoming block from P2P network (Block struct)
     *
     * Called by the active P2P message router when a BLOCK message is received from a peer.
     * Validates the block using BlockAcceptor, and broadcasts to other peers if accepted.
     *
     * @param block The block to process
     * @param peer_id Identifier of the peer that sent the block
     * @return true if block was accepted, false if rejected
     */
    bool ProcessIncomingBlock(const Block& block, const std::string& peer_id);

    /**
     * @brief Process a stored block and return classified connection result.
     *
     * Used by BlockDownloadScheduler drain loop to decide deterministic recovery:
     * - request parent vs wait vs retry vs mark invalid.
     */
    consensus::ConnectBlockResult ProcessIncomingStoredBlock(const Block& block, const std::string& source);

    /**
     * @brief Broadcast new block to P2P network
     *
     * Sends INV message to all connected peers announcing a new block.
     * Called after successfully accepting a block (either mined locally or received from peer).
     *
     * @param block_hash Hash of the block to broadcast
     */
    void BroadcastNewBlock(const std::string& block_hash);

    /**
     * @brief Check if a block was explicitly requested by RequestBlocks()
     * Used by the P1 reorg fix to bypass the IBD guard for fork blocks.
     */
    bool IsBlockInFlight(const std::string& block_hash) const;

    /**
     * Record which peer announced which headers so branch body requests can be
     * sent back to peers that actually advertised them.
     */
    void RecordHeaderAnnouncements(const std::string& peer_addr,
                                   const std::vector<BlockHeader>& headers);

    /**
     * Handle NOTFOUND response from a peer.
     * Clears only the matching block in-flight requests assigned to that peer.
     */
    void HandleNotFoundFromPeer(const std::string& peer_addr,
                                const std::vector<uint8_t>& payload);

    /**
     * @brief Set P2P service for block broadcasting
     * Called during daemon startup to wire services together
     *
     * @param p2p_service P2P service for network broadcasting
     */
    void setP2PService(std::shared_ptr<class P2PService> p2p_service);

    /**
     * @brief Set BlockRelayManager for Phase G.2 block announcements
     * Called during daemon startup to wire block relay
     *
     * @param block_relay BlockRelayManager for announcing blocks
     */
    void setBlockRelayManager(std::shared_ptr<class BlockRelayManager> block_relay);

    /**
     * @brief Set HeaderChainSelector for header chain tracking
     * When blocks are connected, their headers are added to keep header chain in sync
     */
    void setHeaderChainSelector(std::shared_ptr<dinero::consensus::HeaderChainSelector> header_chain);

    /**
     * @brief Set ChainOracleClient for Phase 9.2 Lightning event forwarding
     * Called during daemon startup if lightningd is enabled
     *
     * @param oracle Chain oracle client for forwarding block events to lightningd
     */
    void setChainOracleClient(std::unique_ptr<ipc::ChainOracleClient> oracle);

    /**
     * @brief Set TimeOracleClient for Phase 9.2 Lightning block height tracking
     * Called during daemon startup if lightningd is enabled
     *
     * @param oracle Time oracle client for forwarding block height updates to lightningd
     */
    void setTimeOracleClient(std::unique_ptr<ipc::TimeOracleClient> oracle);

    /**
     * @brief Set TransactionOracleClient for Phase 9.2 Lightning TX confirmation tracking
     * Called during daemon startup if lightningd is enabled
     *
     * Phase 9.3: Now accepts shared_ptr (shared with WatchRegistrationServer)
     *
     * @param oracle Transaction oracle client for forwarding TX confirmations to lightningd
     */
    void setTransactionOracleClient(std::shared_ptr<ipc::TransactionOracleClient> oracle);

    /**
     * @brief Set BridgeNode for Phase P.2 Utreexo proof caching
     * Called during daemon startup to enable proof pre-caching at connect time
     *
     * @param node BridgeNode instance for generating and caching Utreexo proofs
     */
    void setBridgeNode(std::shared_ptr<network::BridgeNode> node);

    /**
     * @brief Get wired BridgeNode (if proof serving is enabled)
     */
    std::shared_ptr<network::BridgeNode> GetBridgeNode() const { return bridge_node_; }

    /**
     * @brief Set ProofGossipManager for tip-proof prewarm hooks.
     *
     * When connected, freshly generated block proofs are prewarmed into the
     * gossip recent-cache during ConnectTip().
     */
    void setProofGossipManager(std::shared_ptr<consensus::ProofGossipManager> manager);

    /**
     * @brief Get wired ProofGossipManager (if gossip is enabled)
     */
    std::shared_ptr<consensus::ProofGossipManager> GetProofGossipManager() const {
        return proof_gossip_manager_;
    }

    // ========================================================================
    // Phase C.1.5: P2P Message Handlers
    // Handle incoming P2P protocol messages for block relay
    // ========================================================================

    /**
     * @brief Handle INV message (inventory announcement)
     * Requests blocks via GETDATA if we don't have them
     *
     * @param peer_addr Peer address that sent the INV
     * @param msg P2P message containing inventory hashes
     */
    void OnInv(const std::string& peer_addr, const P2PMessage& msg);

    /**
     * @brief Handle GETDATA message (data request)
     * Sends requested blocks to peer
     *
     * @param peer_addr Peer address requesting data
     * @param msg P2P message containing requested hashes
     */
    void OnGetData(const std::string& peer_addr, const P2PMessage& msg);

    // ========================================================================
    // Phase C.3: Headers-First Sync
    // Efficient blockchain synchronization via header announcements
    // ========================================================================

    /**
     * @brief Generate block locator for GETHEADERS request
     *
     * Creates a list of block hashes using exponential backoff algorithm.
     * Used to help peers find common ancestor efficiently.
     *
     * Algorithm (Bitcoin-standard):
     * - Start at best block
     * - Add hashes at heights: best, best-1, best-2, ..., best-9
     * - Then exponential backoff: best-13, best-21, best-37, ...
     * - Always include genesis hash
     *
     * @return Vector of block hashes as uint256 (most recent first, genesis last)
     *         Phase M.0: Returns uint256, convert to hex at P2P/RPC boundary
     */
    std::vector<uint256> GenerateBlockLocator();

    /**
     * @brief Handle GETHEADERS message (header request)
     * Sends header chain to peer based on their block locator
     *
     * @param peer_addr Peer address requesting headers
     * @param msg P2P message containing block locator
     */
    void OnGetHeaders(const std::string& peer_addr, const P2PMessage& msg);

    /**
     * @brief Handle HEADERS message (header announcement)
     * Validates headers and requests missing blocks
     *
     * @param peer_addr Peer address sending headers
     * @param msg P2P message containing headers
     */
    void OnHeaders(const std::string& peer_addr, const P2PMessage& msg);

    // ========================================================================
    // Phase 3D: Wallet notification registry
    // Event-driven wallet updates from blockchain
    // ========================================================================

    /**
     * @brief Register a wallet notifier to receive blockchain events
     *
     * Registered wallets will automatically receive notifications when:
     * - Blocks are connected to the active chain
     * - Blocks are disconnected during reorgs
     * - Transactions enter the mempool
     *
     * @param notifier Pointer to WalletNotifier implementation (must outlive service)
     */
    void registerWalletNotifier(WalletNotifier* notifier);

    /**
     * @brief Unregister a wallet notifier
     * @param notifier Pointer to previously registered notifier
     */
    void unregisterWalletNotifier(WalletNotifier* notifier);

    /**
     * @brief Notify all registered wallets of a connected block
     * @param block The block that was connected
     * @param height The height of the block
     */
    void notifyBlockConnected(const Block& block, uint32_t height);

    /**
     * @brief Notify all registered wallets of a disconnected block
     * @param block The block that was disconnected
     * @param height The height of the block
     */
    void notifyBlockDisconnected(const Block& block, uint32_t height);

    /**
     * @brief Read the stored UndoRecord for a block from archival
     * flatfile storage. Public so BlockAcceptor can reach the
     * archival-mode undo data that ChainDB::getUndo() doesn't
     * see when legacy shadow writes are disabled.
     *
     * @param hash Block hash to read undo for.
     * @return UndoRecord on success; NotFound / IOError on failure.
     */
    StatusOr<UndoRecord> ReadStoredUndoPublic(const uint256& hash) const {
        return ReadStoredUndo(hash);
    }

    // #274: stateless ConnectBlock cannot populate BlockUndo.spent_coins (no UTXO
    // set), so ConnectTip reconstructs the spent list from ChainDB coin rows —
    // the only source carrying full fidelity (height + coinbase flag, which
    // utreexo proofs do not commit to). Must run BEFORE the unified batch stages
    // the deleteCoin calls, while the rows still exist. Same-block spends fall
    // back to the block body (rows not yet written). Any other miss is fatal to
    // the connect: a short undo would make the tip undisconnectable.
    Status ReconstructSpentCoinsFromChainDb(const Block& block,
                                            uint32_t height,
                                            std::vector<dinero::SpentCoin>& out_spent,
                                            std::string& out_error) const;

private:
    struct ShieldedStateSnapshot {
        uint256 root;
        uint64_t tree_size{0};
        uint64_t nullifier_count{0};
    };

    bool LoadShieldedState();
    bool PersistShieldedState() const;
    ShieldedStateSnapshot CurrentShieldedStateSnapshot() const;
    bool PersistShieldedTipMarker(const uint256& tip_hash, uint32_t tip_height) const;

    // #356: Advance the in-memory shielded pool for one stored block during a
    // stateless replay/recovery connect, IFF the shielded pool sits exactly at
    // height-1 (the marker-guard decision — see StatelessReplayShieldedDecision).
    // Shared funnel for the ABC-CSN reorg replay loop and the ConnectTip
    // crash-recovery branch so neither hand-rolls the delta+capture+apply
    // sequence.
    //
    //   marker.height == height-1 -> apply, applied_out=true, undo_out filled
    //   marker.height >= height    -> skip (already applied), applied_out=false,
    //                                 returns true with no mutation
    //   marker.height <  height-1  -> returns false (loud: contiguous-recovery
    //                                 invariant broken)
    //
    // On the apply path, pre_block_shielded_frontier is captured BEFORE the
    // apply (mirroring ConnectBlockInternal); pre_reset_shielded_epoch is filled
    // by ApplyBlockShieldedSection when height == the epoch-reset height.
    // `fallback_spent_outputs` is forwarded to ComputeShieldedDeltasForStoredBlock
    // and consulted ONLY when block.utreexo is absent (CSN replay records carry
    // the spend metadata for hash-only stored blocks).
    bool ApplyStatelessReplayShielded(const Block& block, uint32_t height,
                                      consensus::BlockUndo& undo_out, bool& applied_out,
                                      std::string& error,
                                      const std::vector<consensus::SpentOutputData>*
                                          fallback_spent_outputs = nullptr);
    // Phase 3b step 6: RestoreShieldedFrontierFromUndoBlock,
    // ReplayShieldedBlockForward, and RecoverShieldedStateFromTipMarker
    // were deleted. Their only purpose was to reconcile partial-state
    // mismatches between ShieldedTipMarker and the shielded frontier
    // flat file; option 1 (frontier blob + anchor history blob +
    // marker into the unified WriteBatch) makes those mismatches
    // structurally unreachable through ConnectTip/DisconnectTip.
    // Mismatches encountered now are real corruption and fail loud.
    bool VerifyOrBootstrapShieldedTipMarker(const uint256& tip_hash, uint32_t tip_height);
    bool RewindShieldedStateToActiveTipForStartup(uint32_t stored_tip_height);
    bool RangeHasShieldedActivity(uint32_t start_height, uint32_t end_height) const;

    // Centralized AssumeUTXO state transitions keep in-memory flags and
    // persisted metadata in sync across load/restore/rollback paths.
    // `persist_metadata=true` only for fresh snapshot loads.
    // `persist_metadata=false` restores in-memory state from persisted metadata
    // without rewriting the stored base block/height markers.
    void SetAssumeUTXOState(const uint256& base_block, uint32_t base_height, bool persist_metadata);
    // `clear_persisted_metadata=true` fully exits AssumeUTXO mode by clearing
    // both in-memory state and persisted metadata.
    // `clear_persisted_metadata=false` clears only the in-memory flags.
    void ClearAssumeUTXOState(bool clear_persisted_metadata);
    bool VerifyStrictArchivalStartup(uint32_t tip_height) const;

    // D.2 (Apr 30 2026): walk back from `tip_height` for up to
    // `max_blocks` blocks (0 = all the way to genesis) and verify that
    // every block whose persisted metadata has BLOCK_HAVE_UNDO=true also
    // has a readable undo entry in the flatfile at (undo_file, undo_pos,
    // undo_size). Returns false on the FIRST mismatch and writes a
    // chainstate_recovery.marker pointing to the offending height/hash —
    // operators see "active tip undo unreadable at height N" at startup
    // instead of wedging at the first DisconnectTip.
    //
    // Cheap-mode caller passes max_blocks = e.g. 256 (audit only the
    // tail of the active chain, the part most likely to be reorged).
    // Full-mode caller passes 0 to scan from genesis.
    // Non-const because it may invoke ScheduleChainstateRecovery (writes
    // a chainstate_recovery.marker file) on detected coverage gaps.
    bool VerifyActiveChainUndoCoverage(uint32_t tip_height,
                                       uint32_t max_blocks_back);

    // ATOMIC ACTIVE-TIP PUBLICATION INVARIANT (Apr 30 2026, 2638fa872+).
    //
    //   A block must not become the active tip until all data needed
    //   to disconnect it is durably committed.
    //
    // Pre-fix the unified batch already covered most of this — undo
    // flatfile fsync precedes the rocksdb batch, the batch itself
    // commits {UTXO, txindex, utreexo checkpoint, UD:<hash> sidecar
    // [P1], header metadata with BLOCK_HAVE_UNDO + undo_file/pos/size
    // [D.1], shielded markers, setTip, height index} atomically. But
    // there was no programmatic check pinning the rule: a future
    // refactor could re-introduce a path that publishes a tip without
    // one of those materials, and only a sibling-race wedge would
    // surface the regression.
    //
    // This helper performs a DURABLE READ of the persisted disconnect
    // material for `hash`:
    //
    //   - getHeaderMetadata(hash).status_flags has BLOCK_HAVE_UNDO
    //   - metadata.undo_size > 0
    //   - block_storage_->readUndo({file,pos,size}) succeeds
    //   - if Utreexo is active at `height`: chain_db has UD:<hash> blob
    //
    // It is invoked:
    //
    //   1. Inside ConnectTip immediately before `active_tip_ = X` for
    //      the canonical advancement path. Hard-abort under regtest;
    //      log + safe-mode under mainnet/testnet to refuse further
    //      chain advancement until operator review.
    //
    //   2. By VerifyActiveChainUndoCoverage at startup, per active
    //      chain block in the audit window.
    //
    // Out-of-scope (intentional): non-canonical `active_tip_ = X`
    // sites that move the tip BACKWARD (DisconnectTip rollbacks,
    // snapshot restores, startup loads). Those don't violate the
    // invariant — the tip they set has long been published and its
    // disconnect material was committed at original-publish time.
    struct DisconnectMaterialCheck {
        bool durable;
        std::string failure_reason;  // empty when durable
    };
    // Strict variant. Performs ALL of:
    //   - chaindb header metadata reads (BLOCK_HAVE_UNDO, undo_size > 0)
    //   - flatfile undo bytes read AND UndoRecord::Deserialize parse
    //   - sanity checks on decoded undo (created non-empty, spent count
    //     == sum of non-coinbase tx inputs, pre_block_shielded_frontier
    //     present iff block has shielded txs)
    //   - chaindb UD:<hash> sidecar read (utreexo-active heights)
    //   - UTXO read-back: coinbase output 0 in chaindb post-publish,
    //     but ONLY while that coinbase is still immature. Past
    //     COINBASE_MATURITY confirmations a coinbase output 0 may be
    //     legitimately spent (gettxout == null), which is normal chain
    //     state — not an atomicity failure. `reference_tip_height` is
    //     the chain tip the caller is reasoning from; the read-back is
    //     skipped when (reference_tip_height - height) >= maturity.
    //     The post-commit ConnectTip caller passes the freshly-connected
    //     tip (depth 0, always immature) so its atomicity invariant is
    //     still fully exercised; only the deep startup-audit walk-back
    //     skips, where a spent coinbase used to raise a false
    //     chainstate_recovery.marker.
    //
    // The block must be supplied because the structural sanity checks
    // and shielded-presence check need to inspect tx_count, vin, and
    // shielded txs. Caller can ReadStoredBlock once and pass it.
    // The coinbase output-0 read-back inside this function is maturity-
    // gated via dinero::daemon::CoinbaseReadbackApplies (see
    // include/daemon/coinbase_readback_gate.h) so a legitimately-spent
    // mature coinbase does not raise a false chainstate_recovery.marker.
    DisconnectMaterialCheck CheckBlockDisconnectMaterialDurable(
        const Block& block, const uint256& hash, uint32_t height,
        uint32_t reference_tip_height) const;

    // Reasons a code path may publish (or republish) the in-memory
    // active_tip_ pointer. Each direct-assignment site documents its
    // intent at the call. Only `kAdvancement` is required to satisfy
    // the publication invariant — the others move the tip backward,
    // restore from durable state, or run before the chaindb is fully
    // initialized.
    enum class TipPublishReason : uint8_t {
        kAdvancement,         // ConnectTip's canonical success path
        kRollback,            // DisconnectTip moves tip backward to parent
        kStartupLoad,         // Loading persisted chaindb tip into memory at boot
        kSnapshotRestore,     // AssumeUTXO snapshot restoration
        kEarlyInitGenesis,    // Genesis block during pre-validator init
        kCSNDisconnect,       // Stateless-client lightweight disconnect
        kReorgInvalidate,     // invalidateblock RPC or candidate-graph rejection
        kSelfHealRealign,     // ActivateBestChain self-heal: realign in-memory to durable
    };
    static const char* TipPublishReasonName(TipPublishReason r);

    // The single setter for the in-memory active_tip_ pointer. Logs
    // intent + height + hash. For `kAdvancement` the caller is expected
    // to have already verified the publication invariant via
    // CheckBlockDisconnectMaterialDurable (ConnectTip does this
    // immediately before invoking PublishActiveTip).
    //
    // Pre-fix the in-memory active_tip_ was set by 13 direct
    // assignments scattered across this file. Consolidating them into
    // one setter makes the code structurally enforce "ConnectTip is
    // the only writer of active chain advancement" — every other site
    // documents its non-advancement reason at the call.
    void PublishActiveTip(CBlockIndex* tip, TipPublishReason reason);

    // D.3 (Apr 30 2026): regenerate an UndoRecord from the block body
    // alone, used by DisconnectTip when ReadStoredUndo fails.
    //
    // Currently handles the trivial case (no non-coinbase inputs +
    // no shielded txs): `spent` is empty, `created` = every coinbase
    // output, no shielded frontier required. This unsticks the
    // sibling-race wedge the fleet hit at height 10347 — every
    // coinbase-only sibling is recoverable without any prevout I/O.
    //
    // Returns Ok with the regenerated record on success. Returns
    // Internal/NotFound if the block has non-coinbase inputs or
    // shielded txs (those need full prevout / shielded reverse-apply
    // and are intentionally out of scope for this minimal fallback).
    StatusOr<UndoRecord> RegenerateUndoFromBlockTrivial(const Block& block) const;

    // D.3-full (Apr 30 2026): full prevout-lookup undo regeneration.
    // For each non-coinbase input the block consumes, finds the
    // creating tx via the txindex, reads the parent block from
    // flatfile storage, extracts the spent output's value /
    // scriptPubKey / coinbase flag / height / confidential commitment,
    // and reconstructs the SpentCoin. Combined with the block's own
    // outputs (created), this produces a complete UndoRecord
    // equivalent to the one ConnectTip would have written.
    //
    // Cost: one txindex lookup + one flatfile block read per spent
    // input. For typical blocks (10s of inputs) this is sub-second.
    //
    // Limitations:
    //   - Returns Internal if the block contains shielded txs. Shielded
    //     reverse-apply requires reconstructing pre_block_shielded_frontier
    //     from the live shielded tree and is a separate task.
    //   - Returns NotFound if a prevout's tx is missing from the
    //     txindex (e.g., index pruned). DisconnectTip then falls
    //     through to the recovery-marker path.
    StatusOr<UndoRecord> RegenerateUndoFromBlock(const Block& block) const;

    bool HasFlatfileBlockBody(const uint256& hash) const;
    bool HasStoredBlockBody(const uint256& hash) const;
    StatusOr<Block> ReadStoredBlock(const uint256& hash) const;
    StatusOr<UndoRecord> ReadStoredUndo(const uint256& hash) const;
    void ScheduleChainstateRecovery(const std::string& reason, const std::string& source_tag);

    // ❌ DELETED: std::unique_ptr<ChainManager> chain_manager_ (uses global g_chain_manager instead)
    // ❌ DELETED: std::unique_ptr<ChainDB> chain_db_ (violated ONE DB Definition)
    // ChainDB is owned by global g_chain_manager, constructed by DaemonApp
    std::unique_ptr<UTXOIndex> utxo_index_;                    // UTXO set index (wallet-owned UTXOs only)
    std::unique_ptr<consensus::ConsensusUTXOSet> consensus_utxo_set_; // Phase 2: Pure in-memory UTXO set (owns forest)
    std::unique_ptr<indexing::UTXOPositionIndex> utxo_position_index_; // Phase 11a: UTXO → Utreexo position mapping (indexing layer)
    std::unique_ptr<consensus::BlockValidator> block_validator_; // Production consensus validator (reorg fix)
    std::optional<consensus::ValidationMode> pending_validation_mode_; // Deferred until block_validator_ created
    consensus::shielded::CommitmentTree shielded_tree_;
    consensus::shielded::NullifierSet shielded_nullifiers_;
    consensus::shielded::AnchorHistory shielded_anchor_history_;  // Phase 3 wave 1
    std::filesystem::path shielded_frontier_path_;
    // Dependencies from context
    std::shared_ptr<class LoggerService> logger_;
    std::shared_ptr<class ConfigService> config_;

    // Phase 39: Direct ChainDB access (non-owning pointer, set by DaemonApp)
    ChainDB* chain_db_ = nullptr;
    std::shared_ptr<BlockStorage> block_storage_;

    // Phase 41: BlockIndex graph for fork tracking and reorg logic
    std::unordered_map<uint256, std::unique_ptr<class CBlockIndex>> block_index_;
    std::set<class CBlockIndex*, struct ByWorkThenHash> candidates_;
    std::unordered_map<uint256, std::vector<class CBlockIndex*>> orphan_pool_;
    class CBlockIndex* active_tip_ = nullptr;  // Current active chain tip

    // #439: value-copy of the active tip identity, published by PublishActiveTip
    // (the single setter for active_tip_) under its own mutex. GetSyncSnapshot()
    // reads these instead of dereferencing active_tip_, which would otherwise
    // race with chain advancement. Kept separate from any broader chainstate
    // lock so a sync-status read never contends with block connection.
    mutable std::mutex published_tip_mutex_;
    bool     published_tip_valid_ = false;
    uint256  published_tip_hash_;
    uint32_t published_tip_height_ = 0;
    mutable std::recursive_mutex activation_mutex_;  // Protects ActivateBestChain from concurrent entry

    // Phase 43: Safe mode state (deep reorg protection)
    bool safe_mode_active_ = false;

    // Phase 3b step 3 part 2: journal-row startup verification runs
    // once per process at first canonical activation. Subsequent
    // ActivateBestChain calls (P2P-driven, mining-loop-driven, etc.)
    // skip the check — by then the daemon has been mutating live
    // state, so the journal-row vs DSRH-v2 comparison no longer
    // catches a startup-side partial-commit; it would just race.
    bool journal_verified_at_startup_ = false;
    std::string safe_mode_reason_;
    ReorgLog reorg_log_;
    std::chrono::steady_clock::time_point safe_mode_entered_time_;

    // Crash safety: Incomplete reorg detection
    bool incomplete_reorg_detected_ = false;

    // Header chain stalling detection (Bitcoin Core: demote unavailable chains)
    uint256 stalled_header_tip_{};                                    // Hash of header chain tip we're waiting on
    std::chrono::steady_clock::time_point stalled_header_first_seen_; // When we first noticed blocks missing
    bool stalled_header_tracking_ = false;
    static constexpr int HEADER_STALL_TIMEOUT_SECONDS = 600;         // 10 minutes
    mutable std::mutex header_announcement_mutex_;
    std::unordered_map<std::string, std::vector<std::string>> header_announcing_peers_;
    std::unordered_set<std::string> stalled_missing_block_hashes_;

    // Blocks whose data exists in ChainDB (hasBlock OK) but cannot be parsed.
    // Self-synchronizing: accessed from the scheduler-drain/peer thread
    // (clear) AND from activation_mutex_ holders (mark/contains) — see
    // UnreadableBlockSet for the corruption history that motivated the type.
    UnreadableBlockSet unreadable_blocks_;

    // Forest checkpoint delta campaign phase 0 instrumentation.
    // pending_forest_checkpoint_bytes_ carries the size of the forest blob
    // serialized while connecting the current block (main ConnectTip batch
    // or CommitConnectedBlockBookkeeping) to the RecordBlockConnectStats
    // call at ConnectTip's success returns. ConnectTip is serialized, so a
    // plain member suffices.
    SyncStatsRecorder sync_stats_;
    uint64_t pending_forest_checkpoint_bytes_ = 0;
    void RecordBlockConnectStats(uint32_t height,
                                 std::chrono::steady_clock::time_point connect_start);

    // Forest checkpoint delta campaign phase 2
    // (docs/design/forest-checkpoint-deltas.md): startup restore replays the
    // per-block UD:<blockhash> delta sidecars forward over the checkpoint
    // forest, (checkpoint_height, target_height], verifying the root against
    // every block header on the way. Pure in-memory; no ChainDB writes.
    // Replays into a working copy and swaps it in only on full success, so
    // a failed replay leaves the checkpoint-state forest untouched for the
    // existing body-based catch-up/recovery machinery.
    bool ReplayForestDeltasToTip(uint32_t checkpoint_height,
                                 uint32_t target_height, std::string* error);

    mutable std::atomic<uint64_t> legacy_body_fallback_reads_{0};
    mutable std::atomic<uint64_t> legacy_undo_fallback_reads_{0};
    bool strict_archival_reads_{false};

    // Phase 42: AssumeUTXO state (snapshot-based fast sync)
    bool assumeutxo_active_ = false;           // True if loaded from snapshot
    uint256 assumeutxo_base_block_;            // Block hash where UTXO set was snapshotted
    uint32_t assumeutxo_base_height_ = 0;      // Height where UTXO set was snapshotted
    // Snapshot base height once history was PROMOTED into ChainDB (or restored
    // as fully_validated). NEVER cleared by ClearAssumeUTXOState: the
    // fork-below-base fatal rule (spec: Fatal Mismatch Semantics) is not
    // mode-scoped — undo below the audited tail does not exist even after the
    // assumeutxo markers are gone, so ActivateBestChain must keep refusing
    // (fatally) any reorg whose fork point dips below this height. 0 = unset.
    uint32_t promoted_base_height_ = 0;

    // Forward-connect (mobile profile) support: one-time INFO when the #361
    // tip hold is bypassed, and the durable promotion-completion marker that
    // replaces setTip(base) as the advanced-tip commit point. The marker is
    // keyed by the base block hash, so a future different-base lifecycle can
    // never be satisfied by a stale one.
    bool assumeutxo_forward_connect_logged_ = false;
    // Classic AssumeUTXO deliberately holds the active tip at the snapshot
    // base until history promotion.  Remember the one-time diagnostic emitted
    // when ActivateBestChain defers the post-base header branch; without this
    // guard every arriving (often duplicate) body can produce another log line.
    bool assumeutxo_header_import_deferred_logged_ = false;

    // FIX 2 (issue #186) + rc24.1 single-flight guard: deferred snapshot-bootstrap
    // state machine (peeked at startup; block download is deferred while Pending
    // OR Loading). Only ONE thread may win the Pending -> Loading transition and
    // call LoadSnapshot; all other callers return. Set Pending only on a fresh
    // datadir. Atomic: read by the scheduler's defer predicate (network thread),
    // transitioned from the header-processing + periodic daemon threads.
    //   Inactive --(startup: fresh + snapshot configured)--> Pending
    //   Pending  --(base hash on header chain; CAS winner)---> Loading --> Loaded
    //   Pending  --(load failed | headers passed base height)-> Fallback
    enum class SnapshotBootstrapState { Inactive = 0, Pending, Loading, Loaded, Fallback };
    std::atomic<SnapshotBootstrapState> snapshot_bootstrap_state_{SnapshotBootstrapState::Inactive};
    std::string snapshot_bootstrap_path_;
    uint256 snapshot_bootstrap_base_hash_;
    uint32_t snapshot_bootstrap_base_height_ = 0;
    // Serializes LoadSnapshot across the auto-bootstrap path AND the manual RPC
    // path so the consensus UTXO set is never mutated concurrently (rc24.1 crash).
    std::mutex snapshot_load_mutex_;

    // Phase 44: Background validation state (parallel validation of assumed UTXO)
    BackgroundValidationStatus bg_validation_status_ = BackgroundValidationStatus::NotStarted;
    uint32_t bg_validation_current_height_ = 0;    // Current height being validated
    uint32_t bg_validation_blocks_validated_ = 0; // Total blocks validated
    std::string bg_validation_error_;              // Error message if failed
    std::unique_ptr<std::thread> bg_validation_thread_;  // Background validation worker
    std::atomic<bool> bg_validation_should_stop_{false}; // Signal to stop worker
    mutable std::mutex bg_validation_mutex_;       // Protects background validation state

    // #298 wake-on-store: a newly stored backfill body fires the scheduler's
    // SetOnBackfillBodyStored callback, which flips bg_validation_body_arrived_
    // and notifies bg_validation_cv_ so the worker re-reads immediately instead
    // of polling a fixed 30s. The stop paths set bg_validation_should_stop_
    // under bg_validation_wait_mutex_ then notify, so shutdown never waits the
    // full backstop (no lost-wakeup race). bg_requested_heights_ is touched
    // only by the worker thread (the callback only flips the atomic+cv), so it
    // needs no lock: it records heights reported missing/re-requested so a
    // height that later becomes readable logs "body arrived" exactly once.
    std::condition_variable bg_validation_cv_;
    std::mutex bg_validation_wait_mutex_;
    std::atomic<bool> bg_validation_body_arrived_{false};
    std::map<uint32_t, bool> bg_requested_heights_;
    // #298: armed only while validation is waiting for re-requested gap bodies
    // (backfill stalled). The scheduler's per-store wake callback no-ops unless
    // this is set, so bulk backfill's ~40k stores don't each trigger a re-scan.
    std::atomic<bool> bg_validation_awaiting_bodies_{false};

    // #298 hang-watchdog state. Touched ONLY by CheckHangWatchdog(), which is
    // called from the single p2p scheduler tick thread — so it needs no lock.
    // Tracks the last bg/foreground heights seen making progress and when.
    bool watchdog_initialized_ = false;
    uint32_t watchdog_last_bg_height_ = 0;
    uint32_t watchdog_last_fg_height_ = 0;
    std::chrono::steady_clock::time_point watchdog_last_progress_time_{};

    // AssumeUTXO fatal state machine; lazily constructed once utxo_index_
    // exists (EnsureAssumeUtxoLifecycle()).
    std::unique_ptr<assumeutxo::AssumeUtxoLifecycle> assumeutxo_lifecycle_;
    std::mutex assumeutxo_lifecycle_init_mutex_;
    void EnsureAssumeUtxoLifecycle();

    // Phase 45: IBD state (snapshot-accelerated initial block download)
    IBDStatus ibd_status_ = IBDStatus::NotInIBD;   // Current IBD status
    uint32_t ibd_network_height_ = 0;               // Estimated network height
    std::chrono::steady_clock::time_point ibd_last_update_; // Last time we checked IBD status
    bool services_ready_ = false;                   // True if services can be used
    // IBD_THRESHOLD_BLOCKS: how many blocks behind the estimated network tip
    // the node must be before IsInIBD() returns true.
    //
    // Original value was 1000, which caused a real incident on 2026-04-18 where
    // nodes (LA, Mac) exited IBD with ~700-block gaps because their cached
    // ibd_network_height_ was a stale peer-handshake value. Once IsInIBD()
    // flipped false, the BlockDownloadScheduler stopped aggressive fetching
    // and those nodes wedged mid-catchup.
    //
    // 24 keeps the node in IBD until it's within ~24 blocks of the highest
    // height any peer has ever advertised, which at 5-min block targets is
    // about 2 hours — short enough that normal operation never re-enters
    // IBD from a brief disconnect, but long enough that a node recovering
    // from an outage will stay in aggressive-fetch mode until it's effectively
    // caught up.
    static constexpr int IBD_THRESHOLD_BLOCKS = 24;

    // Phase 46: Pruning state (snapshot-based disk optimization)
    PruningMode pruning_mode_ = PruningMode::Disabled;   // Current pruning mode
    bool pruning_enabled_ = false;                       // True if pruning is enabled
    uint32_t pruned_height_ = 0;                         // Highest block that's been pruned
    uint64_t target_disk_usage_mb_ = 10000;              // Target disk usage in MB (default: 10GB)
    uint64_t blocks_pruned_count_ = 0;                   // Total blocks pruned
    uint64_t bytes_freed_ = 0;                           // Total bytes freed by pruning
    static constexpr int PRUNING_SAFETY_MARGIN = 1000;   // Keep last N blocks for reorg protection

    std::string datadir_;
    bool started_ = false;

    // Phase C.1 v2: P2P service for block broadcasting
    std::shared_ptr<class P2PService> p2p_service_;

    // Phase G.2: Block relay manager for block announcements
    std::shared_ptr<class BlockRelayManager> block_relay_manager_;

    // P2P fix: Header chain selector for header sync
    std::shared_ptr<dinero::consensus::HeaderChainSelector> header_chain_selector_;

    // Phase C.1 v2: Orphan block pool for P2P blocks with missing parents
    std::unique_ptr<p2p::OrphanBlockPool> p2p_orphan_pool_;

    // Phase 3D: Wallet notification registry
    std::vector<WalletNotifier*> wallet_notifiers_;

    // Phase 9.2: Chain oracle for Lightning event forwarding
    std::unique_ptr<class ipc::ChainOracleClient> chain_oracle_client_;

    // Phase 9.2: Time oracle for Lightning block height tracking
    std::unique_ptr<class ipc::TimeOracleClient> time_oracle_client_;

    // Phase 9.2: Transaction oracle for Lightning TX confirmation tracking
    // Phase 9.3: Changed to shared_ptr (shared with WatchRegistrationServer)
    std::shared_ptr<class ipc::TransactionOracleClient> transaction_oracle_client_;

    // Phase P.2: BridgeNode for Utreexo proof pre-caching at connect time
    std::shared_ptr<network::BridgeNode> bridge_node_;
    std::shared_ptr<consensus::ProofGossipManager> proof_gossip_manager_;

    // Ensure a header-validated branch exists in the BlockIndex graph with
    // correct ancestry and cumulative chainwork before using it as active_tip_.
    class CBlockIndex* EnsureHeaderBranchIndexed(const uint256& tip_hash,
                                                 bool mark_chain_valid);

    // CSN reorg support: StatelessNode for forest checkpoint restore during reorg
    std::shared_ptr<network::StatelessNode> stateless_node_;
    std::atomic<uint32_t> csn_reorg_reset_height_{0};
    std::shared_ptr<pool::PoolManager> pool_manager_;

    // Phase C.3: Block download scheduling (Phase 3)
    struct BlockDownloadRequest {
        std::string block_hash;
        std::string peer_addr;
        std::chrono::steady_clock::time_point request_time;
    };

    mutable std::mutex block_request_state_mutex_;
    std::map<std::string, BlockDownloadRequest> in_flight_blocks_;  // hash -> request info
    std::set<std::string> completed_blocks_;  // hashes of blocks we already have
    size_t next_peer_index_ = 0;  // Round-robin peer selection counter
    static constexpr int BLOCK_REQUEST_TIMEOUT_SECONDS = 30;

    // Missing-parent diagnostic rate limiting (per-peer + global)
    struct MissingParentDiagRateLimitState {
        std::chrono::steady_clock::time_point window_start;
        uint32_t emitted_in_window = 0;
        uint32_t suppressed_in_window = 0;
    };
    mutable std::mutex missing_parent_diag_mutex_;
    std::unordered_map<std::string, MissingParentDiagRateLimitState> missing_parent_diag_by_peer_;
    MissingParentDiagRateLimitState missing_parent_diag_global_;
    static constexpr uint32_t MISSING_PARENT_DIAG_WINDOW_SECONDS = 5;
    static constexpr uint32_t MISSING_PARENT_DIAG_BURST_PER_WINDOW = 2;
    static constexpr uint32_t MISSING_PARENT_DIAG_GLOBAL_WINDOW_SECONDS = 1;
    static constexpr uint32_t MISSING_PARENT_DIAG_GLOBAL_BURST_PER_WINDOW = 1;

    // Missing-parent request throttling (per parent hash)
    mutable std::mutex parent_request_mutex_;
    std::unordered_map<uint256, std::chrono::steady_clock::time_point> parent_request_by_hash_;
    static constexpr uint32_t PARENT_REQUEST_COOLDOWN_SECONDS = 2;

    // Helper methods for genesis initialization
    bool initializeGenesisInChainDB();

    // Phase C.3: Header validation helpers
    bool ValidateHeaderChain(const std::vector<BlockHeader>& headers, const std::string& peer_addr);
    bool ValidateProofOfWork(const BlockHeader& header);
    bool ValidateTimestamp(const BlockHeader& header);

    // Phase C.3 Phase 3: Block download scheduling
    void RequestBlocks(const std::vector<std::string>& block_hashes);
    bool AlreadyHaveBlock(const std::string& block_hash);
    bool ResetTrackedStallForBlock(const std::string& block_hash, const std::string& source);

    // Phase 44: Background validation helpers
    void BackgroundValidationWorker();  // Main validation loop (runs in thread)
    bool VerifyUTXOSetMatch();          // Verify UTXO sets match at snapshot height
    void OnBackgroundValidationComplete(bool success, const std::string& error);

    // Promote replay-proven history 1..base into ChainDB so the assumeutxo
    // exit gate (chaindb tip >= base) can fire: height index per block, undo
    // for the audited tail window, bulk coin-CF reconcile from the proven set,
    // tip-anchored markers from the engine state, then a durable tip at base.
    // Idempotent: safe to re-run after a crash (worker re-runs replay first).
    // Returns false (with error populated) on any write failure — caller
    // treats that as OPERATIONAL (retry next pass), never as snapshot-fatal.
    bool PromoteValidatedHistory(const assumeutxo::AssumeUtxoReplayEngine& engine,
                                 const std::vector<uint256>& canonical_hashes,
                                 std::string& error);

    // Forward-connect promotion bookkeeping (see PromoteValidatedHistory):
    // marker key for the current base, and whether it is already durable.
    std::string PromotionMarkerKey() const;
    bool PromotionArtifactsCommitted() const;

    // Phase 46: Pruning helpers
    bool PruneBlocksUpToHeight(uint32_t height);  // Execute pruning operation
    uint64_t GetBlockchainDiskUsage() const;      // Calculate current blockchain disk usage
    uint32_t GetMaxPrunableHeight() const;        // Calculate safe pruning limit
    void AutoPruneIfNeeded();                     // Automatic pruning based on disk usage

    // Phase C.1 v2: Orphan block handling helpers
    bool AddOrphanBlock(const Block& block, const std::string& peer_id);  // Add to orphan pool
    void ProcessOrphans(const std::string& parent_hash);  // Process orphans when parent arrives
    void RequestParentBlock(const uint256& parent_hash, const std::string& peer_id);  // Request missing parent
    bool ShouldEmitMissingParentDiag(const std::string& peer_id,
                                     uint32_t& suppressed_prev_peer_window,
                                     uint32_t& suppressed_prev_global_window);
    void LogMissingParentDiagRateLimited(const std::string& peer_id,
                                         const uint256& parent_hash,
                                         bool parent_received,
                                         bool parent_expected,
                                         bool synced);
    bool ShouldRequestParentNow(const uint256& parent_hash);
};

} // namespace dinero
