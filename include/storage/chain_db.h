#pragma once

#include "common/status.h"
#include "common/serialization.h"
#include "storage/tip_info.h"
#include "storage/chain_write_token.h"
#include "consensus/undo.h"
#include <rocksdb/db.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/cache.h>
#include <rocksdb/filter_policy.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <functional>
#include <vector>
#include <thread>

namespace dinero {

// Forward declarations
class CBlockIndex;  // From consensus/block_index.h

// Use unified TipInfo from storage/tip_info.h

struct Coin {
    uint64_t amount = 0;
    std::string script_pubkey;
    int height = 0;
    bool coinbase = false;
    bool is_confidential = false;
    std::vector<uint8_t> commitment;
};

// RAII deleter for RocksDB column family handles
struct CfDeleter {
    void operator()(rocksdb::ColumnFamilyHandle* h) const noexcept {
        if (h) delete h;
    }
};
using CfUPtr = std::unique_ptr<rocksdb::ColumnFamilyHandle, CfDeleter>;

/**
 * ═══════════════════════════════════════════════════════════════════════════
 * ChainDB - CANONICAL BLOCKCHAIN STATE (Consensus-Critical)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * ChainDB is NOT a utility database. It is the single source of truth for:
 * - Block headers and full blocks
 * - UTXO set (consensus-critical)
 * - Utreexo accumulator state
 * - Transaction index (optional, but affects reorg safety)
 * - Chain tip and height index
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * INVARIANTS (NON-NEGOTIABLE)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * 🔒 INVARIANT #1 — Single Writer Authority
 *    ONLY BlockAcceptor may write to ChainDB.
 *    All write methods require a ChainWriteToken, which only BlockAcceptor
 *    can construct. This is enforced at compile-time.
 *
 *    ✅ Allowed: BlockAcceptor::ConnectBlock, BlockAcceptor::DisconnectBlock
 *    ❌ Forbidden: Miner, Network, RPC, Wallet, Mempool, Tests (unless explicit)
 *
 * 🔒 INVARIANT #2 — Atomicity
 *    A block is either fully applied or not applied at all.
 *    UTXO mutations, TX index updates, Utreexo state, height index, and tip
 *    advancement MUST be committed in a single atomic WriteBatch.
 *
 *    ❌ NEVER: Partial commits, best-effort writes, catching exceptions and continuing
 *
 * 🔒 INVARIANT #3 — Reorg Symmetry
 *    DisconnectBlock MUST perfectly undo ConnectBlock.
 *    For every addUTXO, there must be a removeUTXO in rollback.
 *    For every TX index write, there must be a TX index delete in rollback.
 *    For every Utreexo leaf add, there must be a Utreexo leaf remove in rollback.
 *
 * 🔒 INVARIANT #4 — Read-Only Everywhere Else
 *    All subsystems except BlockAcceptor are read-only consumers:
 *    - Mempool: reads UTXOs, never writes
 *    - Network: requests data, never mutates state
 *    - RPC: queries only, no side effects
 *    - Wallet: observes state, never modifies consensus data
 *
 * 🔒 INVARIANT #5 — Utreexo Consistency
 *    UTXO mutations and Utreexo mutations are inseparable.
 *    No UTXO add/remove without Utreexo update.
 *    No Utreexo update without UTXO add/remove.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * VIOLATIONS ARE PROTOCOL BUGS, NOT CODE BUGS
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * If you violate these invariants:
 * - Reorgs will corrupt state
 * - Wallets will see phantom balances
 * - Utreexo proofs will fail
 * - Indexes will lie
 * - Chain state becomes non-deterministic
 *
 * This is not theoretical. These bugs killed testnets and delayed mainnet
 * launches for major projects. Do not weaken these guarantees.
 */
class ChainDB {
public:
    ChainDB() = default;
    ~ChainDB() { close(); }

    // Disable copy, enable move
    ChainDB(const ChainDB&) = delete;
    ChainDB& operator=(const ChainDB&) = delete;
    ChainDB(ChainDB&& other) noexcept { moveFrom(std::move(other)); }
    ChainDB& operator=(ChainDB&& other) noexcept {
        if (this != &other) {
            close();
            moveFrom(std::move(other));
        }
        return *this;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Initialization (no write token needed)
    // ═══════════════════════════════════════════════════════════════════════
    Status init(const std::filesystem::path& dir);
    void close();

    // Test-only seam (#371): inject a rocksdb::Env (e.g. FaultInjectionTestEnv)
    // BEFORE init() so tests can force and heal storage-level IO failures.
    // Never used in production; nullptr means rocksdb's default Env.
    void setEnvForTesting(rocksdb::Env* env) { test_env_ = env; }

    // Test-only (#371): force a memtable flush so tests can latch a rocksdb
    // background error through an injected Env failure — mirrors the EU1
    // incident (EBADF during an sst flush → "regular background error").
    Status flushForTesting();

    // #371 loud-failure escalation: after this many CONSECUTIVE writeBatch
    // failures (Resume unable to recover), the node must fail loudly rather
    // than zombie — default action logs FATAL and exits non-zero so systemd /
    // the fleet watchdog restarts it (a restart is a proven full recovery for
    // the latched-background-error class).
    static constexpr int kMaxConsecutiveWriteFailures = 25;
    void setFatalWriteFailureHookForTesting(std::function<void(const std::string&)> hook) {
        fatal_write_failure_hook_ = std::move(hook);
    }

private:
    Status initAttempt(const std::filesystem::path& dir, bool allow_lock_recovery);
    std::filesystem::path dir_;  // Remembered for lock recovery

public:

    // ═══════════════════════════════════════════════════════════════════════
    // Write Operations (REQUIRE ChainWriteToken - BlockAcceptor only)
    // ═══════════════════════════════════════════════════════════════════════

    // Block operations
    Status putBlock(const ChainWriteToken& token, const uint256& hash, const Block& block, rocksdb::WriteBatch* wb = nullptr);
    Status deleteBlock(const ChainWriteToken& token, const uint256& hash, rocksdb::WriteBatch* wb = nullptr);

    // Header operations
    Status putHeader(const ChainWriteToken& token, const uint256& hash, const BlockHeader& header, int height, arith_uint256 work, rocksdb::WriteBatch* wb = nullptr);

    // Phase H.3: Minimal header metadata persistence (header-first sync restart recovery)
    // Stores only topology data: parent_hash, height, chainwork, status_flags
    // NO full header serialization - minimal footprint for restart safety
    //
    // Schema version 1 (locked):
    // - parent_hash: topology (parent-first invariant)
    // - height: ordering (monotonic)
    // - chainwork: fork choice (deterministic)
    // - status_flags: progress (separate batch domain)
    //
    // Schema version 2 (Phase P.2 - added):
    // - file_number, data_pos, data_size: block disk positions
    // - undo_file, undo_pos, undo_size: undo disk positions
    struct PersistedHeaderMetadata {
        static constexpr uint8_t SCHEMA_VERSION = 2;

        uint256 parent_hash;
        int32_t height{0};
        arith_uint256 chainwork;
        uint32_t status_flags{0};  // BLOCK_VALID_HEADER | BLOCK_HAVE_DATA | BLOCK_FAILED

        // Phase P.2: Disk storage positions (schema v2)
        uint32_t file_number{0};  // blk00000.dat file number (0 = not stored)
        uint32_t data_pos{0};     // Offset of block data in file
        uint32_t data_size{0};    // Size of block data
        uint32_t undo_file{0};    // rev00000.dat file number (0 = no undo)
        uint32_t undo_pos{0};     // Offset of undo data in undo file
        uint32_t undo_size{0};    // Size of undo data
    };

    Status putHeaderMetadata(const ChainWriteToken& token, const uint256& hash,
                            const PersistedHeaderMetadata& metadata, rocksdb::WriteBatch* wb = nullptr);
    // Writes header metadata while preserving any already-durable undo
    // reference for the same block. This is for block-body/side-chain/relay
    // writers that know where the block body lives but do NOT own canonical
    // undo state. A duplicate relay of an already-connected block must never
    // clobber BLOCK_HAVE_UNDO or undo_file/pos/size that ConnectTip just
    // committed. New full-overwrite callers should use putHeaderMetadata only
    // when they own every field in the row.
    Status putHeaderMetadataPreservingExistingUndo(const ChainWriteToken& token,
                                                   const uint256& hash,
                                                   const PersistedHeaderMetadata& metadata,
                                                   rocksdb::WriteBatch* wb = nullptr);
    // OVERWRITES status_flags with the value passed. This is intentionally
    // dangerous: callers that only know about a subset of bits MUST use
    // setHeaderStatusBits or clearHeaderStatusBits instead. Passing a partial
    // status to updateHeaderStatus silently strips any bits the caller didn't
    // include. This was the May 2026 fleet-wide HAVE_UNDO regression:
    // HeaderSyncManager::MarkBlockReceived passed node->status (which only
    // tracks HAVE_DATA / FAILED bits) and overwrote chaindb metadata that
    // ConnectTip had set HAVE_UNDO on.
    //
    // Production code must not call this for partial mutations. Keep new uses
    // behind an explicit regression-test update so reviewers are forced to
    // decide whether a full overwrite is truly intended.
    Status updateHeaderStatus(const ChainWriteToken& token, const uint256& hash,
                             uint32_t status_flags, rocksdb::WriteBatch* wb = nullptr);
    // OR-merges `bits_to_set` into the on-disk status_flags. Reads current
    // metadata, ORs the bits, writes back. Never strips bits the caller
    // didn't specify. This is the safe API for any caller that knows about
    // only a subset of consensus-layer status bits — every HeaderSyncManager
    // path, IBD-style block-received notifications, etc.
    //
    // Returns Status::Ok if the metadata was updated. Returns the underlying
    // read error if the existing metadata can't be loaded (caller decides
    // whether to fall back to putHeaderMetadata with a full status).
    Status setHeaderStatusBits(const ChainWriteToken& token, const uint256& hash,
                               uint32_t bits_to_set, rocksdb::WriteBatch* wb = nullptr);

    // AND-NOT-merges `bits_to_clear` out of the on-disk status_flags. Reads
    // current metadata, clears only the requested bits, writes back. Never
    // strips unrelated bits such as BLOCK_HAVE_UNDO, block-validity bits, or
    // disk-position metadata. This is the safe API for reconsider/reorg paths
    // that only need to clear invalidity bits.
    Status clearHeaderStatusBits(const ChainWriteToken& token, const uint256& hash,
                                 uint32_t bits_to_clear, rocksdb::WriteBatch* wb = nullptr);

    // Updates only the canonical undo locator fields and ORs BLOCK_HAVE_UNDO
    // when `undo_size` is non-zero. This is the safe ConnectTip primitive:
    // it preserves topology, chainwork, block-body positions, and unrelated
    // status bits while atomically stamping "this block has durable undo."
    //
    // Keep this as a single read/merge/write helper instead of composing
    // setHeaderStatusBits() + another locator update in the same WriteBatch:
    // RocksDB WriteBatch does not make separate helper reads see prior staged
    // writes.
    Status updateUndoLocator(const ChainWriteToken& token,
                             const uint256& hash,
                             uint32_t undo_file,
                             uint32_t undo_pos,
                             uint32_t undo_size,
                             rocksdb::WriteBatch* wb = nullptr);

    // Phase P.2: Update CBlockIndex disk positions and status flags after pruning
    Status updateBlockIndex(const ChainWriteToken& token, const CBlockIndex* pindex, rocksdb::WriteBatch* wb = nullptr);

    // Height index operations
    Status putHeightIndex(const ChainWriteToken& token, int height, const uint256& hash, rocksdb::WriteBatch* wb = nullptr);
    Status deleteHeightIndex(const ChainWriteToken& token, int height, rocksdb::WriteBatch* wb = nullptr);

    // Tip management (storage tip — tracks what BlockAcceptor has stored)
    Status setTip(const ChainWriteToken& token, const uint256& hash, int height, arith_uint256 work, rocksdb::WriteBatch* wb = nullptr);

    // Validated tip marker (tracks last ConnectTip completion — UTXO + forest consistent)
    // On startup, active_tip_ is restored from this marker to prevent unnecessary replay.
    Status setValidatedTip(const ChainWriteToken& token, const uint256& hash, int height, rocksdb::WriteBatch* wb = nullptr);
    StatusOr<TipInfo> getValidatedTip() const;

    // Phase P.2: Prune height management
    // The prune height is the lowest block height that still has BLOCK_HAVE_DATA.
    // All blocks below this height have been pruned (block data deleted, headers kept).
    Status setPruneHeight(const ChainWriteToken& token, uint32_t height, rocksdb::WriteBatch* wb = nullptr);

    // Phase P.2: Prune mode persistence (immutable after first set)
    // mode: 0 = archival (keep all blocks), 1 = pruned (delete old blocks)
    // Once set, the mode cannot be changed without wiping the datadir.
    Status setPruneMode(const ChainWriteToken& token, bool enabled, rocksdb::WriteBatch* wb = nullptr);
    StatusOr<bool> getPruneMode() const;  // Returns nullopt if never set

    // UTXO operations
    Status putCoin(const ChainWriteToken& token, const uint256& txid, uint32_t vout, const Coin& coin, rocksdb::WriteBatch* wb = nullptr);
    Status deleteCoin(const ChainWriteToken& token, const uint256& txid, uint32_t vout, rocksdb::WriteBatch* wb = nullptr);

    // Transaction index (optional but affects reorg safety)
    Status putTxIndex(const ChainWriteToken& token, const uint256& txid, const uint256& block_hash, uint32_t offset, rocksdb::WriteBatch* wb = nullptr);
    Status deleteTxIndex(const ChainWriteToken& token, const uint256& txid, rocksdb::WriteBatch* wb = nullptr);

    // ═══════════════════════════════════════════════════════════════════════
    // Legacy undo operations still used by the active reorg path.
    // Keep these until snapshot-only reorg handling fully replaces them.
    // ═══════════════════════════════════════════════════════════════════════
    Status putUndo(const ChainWriteToken& token, const uint256& hash, const UndoRecord& undo, rocksdb::WriteBatch* wb = nullptr);

    // Utreexo accumulator persistence (Phase 2.1: Disk Persistence)
    // Checkpoints saved every N blocks for crash recovery
    Status putUtreexoCheckpoint(const ChainWriteToken& token, int height,
                               const std::vector<uint8_t>& serialized_forest,
                               rocksdb::WriteBatch* wb = nullptr);
    Status deleteUtreexoCheckpoint(const ChainWriteToken& token, int height,
                                   rocksdb::WriteBatch* wb = nullptr);

    // Atomic checkpoint + SHA256 checksum (power-loss safe)
    Status putUtreexoCheckpointWithChecksum(const ChainWriteToken& token, int height,
                                            const std::vector<uint8_t>& serialized_forest,
                                            rocksdb::WriteBatch* wb = nullptr);
    Status deleteUtreexoCheckpointWithChecksum(const ChainWriteToken& token, int height);
    StatusOr<std::vector<uint8_t>> getUtreexoChecksum(int height) const;

    // Utreexo metadata (version flags for upgrade tracking).
    // Phase 3b step 3 part 3: optional `wb` parameter routes the
    // write through a caller-owned WriteBatch instead of committing
    // standalone. Used by ConsensusWriteBatch to fold the
    // consensus_journal row into ConnectTip's existing UTXO/txindex
    // WriteBatch so they commit atomically.
    Status putUtreexoMeta(const ChainWriteToken& token, const std::string& key,
                          const std::string& value,
                          rocksdb::WriteBatch* wb = nullptr);
    StatusOr<std::string> getUtreexoMeta(const std::string& key) const;

    // CSN reorg: Spend targets stored in utreexo CF for forest replay
    Status putCSNSpendTargets(const ChainWriteToken& token, const uint256& block_hash,
                              const std::string& serialized_targets,
                              rocksdb::WriteBatch* wb = nullptr);
    StatusOr<std::string> getCSNSpendTargets(const uint256& block_hash) const;

    // ═══════════════════════════════════════════════════════════════════════
    // Utreexo Forest Recovery (crash-resilient startup)
    // ═══════════════════════════════════════════════════════════════════════

    // Forest tip marker: persisted alongside checkpoint for startup verification.
    // Stores height + block hash + forest root commitment so startup can verify
    // forest state matches the block header without deserializing the full forest.
    struct ForestTipMarker {
        int32_t height{0};
        uint256 block_hash;
        uint256 forest_root;  // getCommitment() at this height
    };

    // Phase 3b step 3 part 3 slice 2: optional `wb` parameter routes
    // the write through a caller-owned WriteBatch instead of
    // committing standalone. Used by ConnectTip to fold the forest
    // tip marker into the same atomic batch as UTXO/txindex/journal
    // row — no crash window between the forest checkpoint and the
    // marker that confirms it.
    Status putForestTipMarker(const ChainWriteToken& token, const ForestTipMarker& marker,
                              rocksdb::WriteBatch* wb = nullptr);
    StatusOr<ForestTipMarker> getForestTipMarker() const;

    // Shielded tip marker: persisted alongside shielded frontier/nullifier state
    // so startup can verify that the loaded shielded state belongs to the
    // active tip. This is the shielded analogue of ForestTipMarker, but the
    // commitment lives only in local state today, not in the block header.
    struct ShieldedTipMarker {
        int32_t height{0};
        uint256 block_hash;
        uint256 shielded_root;
        uint64_t tree_size{0};
        uint64_t nullifier_count{0};
    };

    // Phase 3b step 3 part 3 slice 4: optional `wb` parameter routes
    // the write through a caller-owned WriteBatch instead of
    // committing standalone. Used by ConnectTip to fold the
    // shielded tip marker into the same atomic batch as
    // UTXO/txindex/forest checkpoint/block-undo metadata/journal
    // row — closes the window where the marker could outpace or
    // lag the canonical tip.
    Status putShieldedTipMarker(const ChainWriteToken& token, const ShieldedTipMarker& marker,
                                rocksdb::WriteBatch* wb = nullptr);
    StatusOr<ShieldedTipMarker> getShieldedTipMarker() const;
    Status deleteShieldedTipMarker(const ChainWriteToken& token);

    // ── Phase 3b nullifier fold-in: shielded nullifier rows ─────────────
    //
    // Stored under the utreexo column family with key shape
    //   [PREFIX_SHIELDED_NULLIFIER]['<height_be_4>']['<nullifier_32>']
    // value is empty (existence-only). Sorted ascending by (height,
    // nullifier) under big-endian height encoding so prefix iteration
    // gives the same order NullifierSet::SerializeContent currently
    // produces from sqlite ORDER BY.
    //
    // All write methods accept an optional rocksdb::WriteBatch* so
    // ConnectTip / DisconnectTip can stage them inside the same
    // atomic batch as UTXO + forest + frontier + anchor history +
    // ShieldedTipMarker. With the nullifier rows in the unified
    // batch, sqlite is no longer the source of truth and the
    // residual orphan-nullifier inline rollback in
    // VerifyOrBootstrapShieldedTipMarker becomes unreachable.

    // Insert one nullifier row. Idempotent (Put on an existing key
    // overwrites with the same empty value). Caller must serialize
    // the 32-byte nullifier hash — the bytes are stored verbatim
    // for byte-exact prefix iteration.
    Status putShieldedNullifier(const ChainWriteToken& token,
                                uint32_t block_height,
                                const uint8_t nullifier[32],
                                rocksdb::WriteBatch* wb = nullptr);

    // Delete every nullifier row whose height is strictly greater
    // than `height`. Used by DisconnectTip to roll back disconnected
    // blocks. Implemented as a range scan + per-key Delete because
    // rocksdb::WriteBatch::DeleteRange is not enabled on this CF;
    // returns the count of rows deleted.
    StatusOr<uint64_t> deleteShieldedNullifiersAboveHeight(
        const ChainWriteToken& token,
        uint32_t height,
        rocksdb::WriteBatch* wb = nullptr);

    // Delete EVERY nullifier row (all heights). Used by the shielded epoch
    // reset (hard-fork cutover) to purge the authoritative ChainDB nullifier
    // set so a restart's rehydration cannot resurrect the pre-cutover pool.
    // Stage into the same WriteBatch as the cutover block's setTip so the
    // purge is atomic with the tip advance. Returns the count of rows deleted.
    StatusOr<uint64_t> deleteAllShieldedNullifiers(
        const ChainWriteToken& token,
        rocksdb::WriteBatch* wb = nullptr);

    // Iterate every nullifier row in ascending (height, nullifier)
    // order. Used at startup to populate NullifierSet's in-memory
    // map and by SerializeContent to produce the DSRH preimage.
    // Returning false from the callback aborts the scan early.
    using ShieldedNullifierVisitor =
        std::function<bool(uint32_t height, const uint8_t* nullifier_32)>;
    Status forEachShieldedNullifier(const ShieldedNullifierVisitor& visit) const;

    // Total count of nullifier rows. O(N) scan.
    StatusOr<uint64_t> countShieldedNullifiers() const;

    // Wipe all Utreexo checkpoints + checksums + tip marker (for auto-recovery).
    // Does NOT require ChainWriteToken — called during startup before writer exists.
    Status wipeAllUtreexoCheckpoints();

    // Recovery marker: prevents rebuild loops. Records last recovery event.
    struct RecoveryMarker {
        int32_t height{0};
        uint256 tip_hash;
        uint64_t timestamp{0};  // unix epoch seconds
    };

    Status putRecoveryMarker(const RecoveryMarker& marker);
    StatusOr<RecoveryMarker> getRecoveryMarker() const;

    // Utreexo transition proof persistence (Phase 3: Transition Proof Verification)
    Status putTransitionProof(const ChainWriteToken& token, int height,
                              const std::vector<uint8_t>& serialized_proof,
                              rocksdb::WriteBatch* wb = nullptr);
    StatusOr<std::vector<uint8_t>> getTransitionProof(int height) const;

    // Batch operations (atomic commit - all or nothing)
    Status writeBatch(const ChainWriteToken& token, rocksdb::WriteBatch&& batch, bool sync = false);

    // Schema version (initialization only)
    Status setSchemaVersion(const ChainWriteToken& token, int version, rocksdb::WriteBatch* wb = nullptr);

    // ═══════════════════════════════════════════════════════════════════════
    // Read Operations (No token needed - anyone can read)
    // ═══════════════════════════════════════════════════════════════════════

    StatusOr<Block> getBlock(const uint256& hash) const;
    Status hasBlock(const uint256& hash) const;

    // Legacy undo operations still used by the active reorg path.
    StatusOr<UndoRecord> getUndo(const uint256& hash) const;

    StatusOr<BlockHeader> getHeader(const uint256& hash) const;
    StatusOr<int> getBlockHeight(const uint256& hash) const;
    StatusOr<arith_uint256> getBlockWork(const uint256& hash) const;

    // Phase H.3: Read minimal header metadata (restart recovery)
    StatusOr<PersistedHeaderMetadata> getHeaderMetadata(const uint256& hash) const;

    StatusOr<uint256> getBlockHashByHeight(int height) const;

    StatusOr<TipInfo> getTip() const;

    // Phase P.2: Get persisted prune height (lowest height with block data)
    // Returns 0 if pruning has never been performed (all blocks available).
    StatusOr<uint32_t> getPruneHeight() const;

    StatusOr<Coin> getCoin(const uint256& txid, uint32_t vout) const;
    StatusOr<Coin> getCoinWithConfidentialFallback(const uint256& txid, uint32_t vout) const;

    StatusOr<std::pair<uint256, uint32_t>> getTxLocation(const uint256& txid) const;

    /**
     * Phase 7: Lightning transaction queries (read-only)
     *
     * Get confirmed transaction by txid (main chain only).
     * Returns nullopt if:
     * - Transaction not found
     * - Transaction was reorged out
     * - Transaction only in mempool
     * - Transaction in orphan block
     *
     * Lightning only cares about confirmed facts.
     */
    StatusOr<Transaction> getTransaction(const uint256& txid) const;

    /**
     * Phase 7: Get block height where transaction was confirmed.
     * Returns nullopt if not in main chain.
     */
    StatusOr<uint64_t> getTransactionHeight(const uint256& txid) const;

    // Raw key-value access (for undo data storage)
    Status getRaw(const std::string& key, std::string& value) const;

    // BIP158 GCS block filter storage
    Status putBlockFilter(const ChainWriteToken& token, const uint256& hash,
                          const std::vector<uint8_t>& filter_data, uint32_t element_count,
                          rocksdb::WriteBatch* wb = nullptr);
    // Direct-write overload for backfill (no write token required)
    Status putBlockFilter(const uint256& hash,
                          const std::vector<uint8_t>& filter_data, uint32_t element_count);
    struct StoredFilter {
        std::vector<uint8_t> data;
        uint32_t element_count;
    };
    StatusOr<StoredFilter> getBlockFilter(const uint256& hash) const;

    // Utreexo accumulator persistence (Phase 2.1: Read operations)
    StatusOr<std::vector<uint8_t>> getUtreexoCheckpoint(int height) const;
    StatusOr<std::pair<int, std::vector<uint8_t>>> getLatestUtreexoCheckpoint() const;
    StatusOr<std::vector<int>> listUtreexoCheckpoints() const;

    // Iteration helpers
    Status forEachHeaderHeight(std::function<bool(int height, const uint256& hash)> callback) const;
    Status forEachBlock(std::function<bool(const uint256& hash, const Block& block)> callback) const;
    Status forEachUTXO(std::function<bool(const uint256& txid, uint32_t vout, const Coin& coin)> callback) const;

    // Phase H.3: Iterate all header metadata (restart recovery - rebuild header tree)
    Status forEachHeaderMetadata(std::function<bool(const uint256& hash, const PersistedHeaderMetadata& metadata)> callback) const;

    // Schema and metadata
    Status getSchemaVersion(int& version) const;

    // Statistics
    StatusOr<std::string> getStats() const;

    // ═══════════════════════════════════════════════════════════════════════
    // Block Index Operations (G.3.4/G.3.5 Adapter Support)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Check if a block is connected (BLOCK_CONNECTED flag set)
     *
     * Returns true if the block has been fully validated and applied to UTXO set.
     * This is a pure storage query - no logic, no validation.
     */
    bool isBlockConnected(const uint256& block_hash) const;

    /**
     * Mark block as connected/disconnected
     *
     * Sets or clears the BLOCK_CONNECTED flag for a block.
     * This is a pure storage write - no logic, no side effects.
     *
     * @param block_hash  Block to mark
     * @param connected   true = BLOCK_CONNECTED, false = not connected
     */
    void markBlockConnected(const uint256& block_hash, bool connected);

    /**
     * Retrieve block index metadata
     *
     * Returns block metadata (height, chainwork, status flags).
     * This is a pure storage query - no logic, no traversal.
     *
     * @param block_hash  Block to look up
     * @return Block metadata, or nullptr if not found
     *
     * NOTE: Returns internal storage pointer. Do not cache or store.
     */
    CBlockIndex* getBlockIndex(const uint256& block_hash);

    /**
     * Commit pending writes
     *
     * Flushes any buffered writes to disk.
     * This is a pure storage operation - no logic, no validation.
     *
     * @return true if commit succeeded, false otherwise
     */
    bool commitBatch();

private:
    void moveFrom(ChainDB&& other) {
        db_ = std::move(other.db_);
        cf_ = std::move(other.cf_);
        idx_meta_ = other.idx_meta_;
        idx_blocks_ = other.idx_blocks_;
        idx_headers_ = other.idx_headers_;
        idx_height_ = other.idx_height_;
        idx_txindex_ = other.idx_txindex_;
        idx_utxo_ = other.idx_utxo_;
        idx_utreexo_ = other.idx_utreexo_;
    }

    // ⚠️ ORDER MATTERS: declare DB FIRST so it is destroyed LAST
    std::unique_ptr<rocksdb::DB> db_;
    std::vector<CfUPtr> cf_;

    rocksdb::Env* test_env_ = nullptr;  // test-only (#371); see setEnvForTesting

    // #371: consecutive writeBatch failure streak (reset on any success) and
    // the loud-failure action taken when the streak reaches
    // kMaxConsecutiveWriteFailures. Empty hook = default FATAL log + exit(1).
    std::atomic<int> consecutive_write_failures_{0};
    std::function<void(const std::string&)> fatal_write_failure_hook_;

    // Column family indices
    int idx_meta_ = 1;
    int idx_blocks_ = 2;
    int idx_headers_ = 3;
    int idx_height_ = 4;
    int idx_txindex_ = 5;
    int idx_utxo_ = 6;
    int idx_utreexo_ = 7;  // Phase 2.1: Utreexo accumulator checkpoints

    // Key prefixes (1-byte tags)
    static constexpr uint8_t PREFIX_BLOCK = 'b';
    static constexpr uint8_t PREFIX_HEADER = 'h';
    static constexpr uint8_t PREFIX_HEADER_META = 'm';
    static constexpr uint8_t PREFIX_HEIGHT = 'H';
    static constexpr uint8_t PREFIX_TXINDEX = 't';
    static constexpr uint8_t PREFIX_UTXO = 'u';
    static constexpr uint8_t PREFIX_UTREEXO_CHECKPOINT = 'U';  // Phase 2.1: Utreexo checkpoints
    static constexpr uint8_t PREFIX_UTREEXO_CHECKSUM = 'C';   // Integrity checksum for forest checkpoints
    static constexpr uint8_t PREFIX_UTREEXO_META = 'M';       // Utreexo metadata (version flags)
    static constexpr uint8_t PREFIX_CSN_SPEND_TARGETS = 'S';  // CSN reorg: spend targets per block
    static constexpr uint8_t PREFIX_TRANSITION_PROOF = 'T';   // Phase 3: Transition proof persistence
    static constexpr uint8_t PREFIX_BLOCK_FILTER = 'F';      // BIP158 GCS block filter
    static constexpr uint8_t PREFIX_SHIELDED_NULLIFIER = 'N'; // Phase 3b nullifier fold-in

    // Metadata keys
    static constexpr const char* KEY_TIP = "tip";
    static constexpr const char* KEY_HEIGHT = "height";
    static constexpr const char* KEY_SCHEMA_VERSION = "schema_version";
    static constexpr const char* KEY_BEST_WORK = "best_work";
    static constexpr const char* KEY_PRUNE_HEIGHT = "prune_height";  // Phase P.2: Lowest height with block data
    static constexpr const char* KEY_PRUNE_MODE = "prune_mode";      // Phase P.2: Prune mode (0=archival, 1=pruned)
    static constexpr const char* KEY_VALIDATED_TIP = "validated_tip"; // Last ConnectTip-validated block (hash + height)
    static constexpr const char* KEY_FOREST_TIP = "forest_tip";      // Forest tip marker (height/hash/root)
    static constexpr const char* KEY_SHIELDED_TIP = "shielded_tip";  // Shielded tip marker (height/hash/root/size/count)
    static constexpr const char* KEY_RECOVERY = "forest_recovery";   // Last recovery event marker

    // Helper methods
    std::string makeBlockKey(const uint256& hash) const;
    std::string makeHeaderKey(const uint256& hash) const;
    std::string makeHeaderMetadataKey(const uint256& hash) const;
    std::string makeHeightKey(int height) const;
    std::string makeTxIndexKey(const uint256& txid) const;
    std::string makeUtxoKey(const uint256& txid, uint32_t vout) const;
    std::string makeBlockFilterKey(const uint256& hash) const;

    Status convertRocksDBStatus(const rocksdb::Status& status) const;
    rocksdb::Options getDefaultOptions() const;
    rocksdb::ReadOptions getReadOptions() const;
    std::vector<rocksdb::ColumnFamilyDescriptor> getColumnFamilyDescriptors() const;
};

} // namespace dinero
