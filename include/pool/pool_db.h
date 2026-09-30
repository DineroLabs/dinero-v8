#pragma once

#include "pool/pool_types.h"
#include "pool/payment_attempt.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <functional>
#include <mutex>

struct sqlite3;
struct sqlite3_stmt;

namespace dinero {
namespace pool {
class PayoutCalculator;

/**
 * Pool Database - SQLite-backed storage for mining pool accounting
 *
 * Tables:
 * - shares: Individual share submissions
 * - workers: Worker stats and balances
 * - blocks: Blocks found by pool
 * - payouts: Payout records
 * - rounds: Mining rounds (PROP mode)
 * - config: Pool configuration
 */
class PoolDB {
public:
    enum class ShareSubmissionReservationResult {
        Reserved,
        Duplicate,
        Error
    };

    explicit PoolDB(const std::string& db_path);
    ~PoolDB();

    // Initialize database and create tables
    bool initialize();
    bool isOpen() const { std::lock_guard<std::recursive_mutex> owner(connection_mutex_); return db_ != nullptr; }
    PoolDB(const PoolDB&) = delete;
    PoolDB& operator=(const PoolDB&) = delete;

    // ========================================================================
    // SHARE OPERATIONS
    // ========================================================================

    // Record a new share
    bool insertShare(const Share& share);

    // Execute DB-only operations atomically. Callers acquire any PoolManager
    // owner first; callbacks must not acquire manager/selected-chain owners.
    // The connection owner also serializes direct public PoolDB callers.
    bool runInTransaction(const std::function<bool()>& fn);

    // Reserve a dedupe key for a share submission.
    ShareSubmissionReservationResult reserveShareSubmissionKey(const std::string& dedupe_key,
                                                              const std::string& worker_id,
                                                              int64_t submitted_at);

    // Prune stale dedupe keys older than cutoff timestamp
    uint64_t pruneShareSubmissionKeysOlderThan(int64_t cutoff_timestamp);

    // Get shares for a worker (with limit)
    std::vector<Share> getWorkerShares(const std::string& worker_id,
                                        uint32_t limit = 100);

    // Complete typed inclusive time-range capture. Read/row/EOF errors throw;
    // borrowed transactions remain caller-owned. No historical completeness claim.
    // Get shares in time range
    std::vector<Share> getSharesInRange(int64_t start_time, int64_t end_time);

    // Complete typed selected VALID-only window, preserving the existing filter.
    // Read/row/EOF errors and unrepresentable limits throw, never return a prefix.
    // Get last N shares (for PPLNS)
    std::vector<Share> getLastNShares(uint64_t n);

    // Count shares by worker in current round
    uint64_t countWorkerSharesInRound(const std::string& worker_id,
                                       uint64_t round_id);

    // Get total difficulty by worker in range
    double getWorkerDifficultyInRange(const std::string& worker_id,
                                       int64_t start_time, int64_t end_time);

    // ========================================================================
    // WORKER OPERATIONS
    // ========================================================================

    // Get or create worker
    WorkerStats getOrCreateWorker(const std::string& worker_id,
                                   const std::string& wallet_address);

    // Update worker stats
    bool updateWorkerStats(const WorkerStats& stats);

    // Get worker by ID
    std::optional<WorkerStats> getWorker(const std::string& worker_id);

    // Get worker by wallet address
    std::vector<WorkerStats> getWorkersByAddress(const std::string& wallet_address);

    // Get all workers with pending balance
    std::vector<WorkerStats> getWorkersWithPendingBalance(uint64_t min_balance = 0);

    // Get active workers (shares in last N seconds)
    std::vector<WorkerStats> getActiveWorkers(int64_t seconds = 900);

    // Update worker balance
    bool addWorkerPending(const std::string& worker_id, uint64_t amount);
    bool subtractWorkerPending(const std::string& worker_id, uint64_t amount);
    bool addWorkerPaid(const std::string& worker_id, uint64_t amount);

    // ========================================================================
    // BLOCK OPERATIONS
    // ========================================================================

    // Record a new block
    bool insertBlock(PoolBlock& block);

    // Update block (confirmations, orphan status)
    bool updateBlock(const PoolBlock& block);

    // Get block by ID
    std::optional<PoolBlock> getBlock(uint64_t block_id);

    // Get block by hash
    std::optional<PoolBlock> getBlockByHash(const std::string& block_hash);

    // Get blocks pending confirmation
    std::vector<PoolBlock> getPendingBlocks();

    // Get blocks ready for payout (confirmed but not paid)
    std::vector<PoolBlock> getBlocksReadyForPayout();

    // Complete typed selected recent-block window; failures throw.
    // Get recent blocks
    std::vector<PoolBlock> getRecentBlocks(uint32_t limit = 50);

    struct OrphanResult {
        bool found = false;
        uint64_t pending_reversed = 0;
        uint64_t already_paid = 0;
    };
    // Checked FULL transaction for the existing unpaid-payout reversal policy.
    // False means unavailable/invalid/failed, never an empty successful read.
    // The result is assigned only after COMMIT. Missing block is found=false.
    bool reconcileOrphanedBlock(const std::string& block_hash, OrphanResult& result);

    // Mark block as orphaned
    bool markBlockOrphaned(uint64_t block_id);

    // ========================================================================
    // PAYOUT OPERATIONS
    // ========================================================================

    // Insert payout record
    bool insertPayout(const Payout& payout);

    // Update payout status
    bool updatePayoutStatus(uint64_t payout_id, PayoutStatus status,
                            const std::string& txid = "",
                            const std::string& error = "");
    bool incrementPayoutRetry(uint64_t payout_id, int64_t retry_time, const std::string& error = "");

    // Get payouts for block
    // All four payout readers below capture explicit typed columns through
    // SQLITE_DONE or throw without a partial result. Borrowed reads remain
    // caller-owned. Present records alone do not certify payment eligibility.
    std::vector<Payout> getPayoutsForBlock(uint64_t block_id);

    // Get payouts for worker
    std::vector<Payout> getWorkerPayouts(const std::string& worker_id,
                                          uint32_t limit = 100);

    // Get pending payouts
    std::vector<Payout> getPendingPayouts();

    // Complete present attempt/member capture in one checked FULL transaction.
    // Missing/corrupt storage refuses. Durable metadata is not a signed catalog
    // or proof against deletion, backup rollback or canonical eligibility.
    std::vector<PoolPaymentAttempt> getPaymentAttempts();

    // Get payouts ready to send (confirmed blocks)
    std::vector<Payout> getPayoutsReadyToSend();

    // ========================================================================
    // ROUND OPERATIONS (PROP MODE)
    // ========================================================================

    // Start new round
    uint64_t startNewRound();

    // End current round (when block found)
    bool endRound(uint64_t round_id, uint64_t block_id);

    // Get current round
    std::optional<MiningRound> getCurrentRound();

    // Capture the round and all contribution rows in one checked read snapshot.
    // Missing returns nullopt; malformed/incomplete reads throw. A caller-owned
    // transaction is read without being committed or rolled back.
    std::optional<MiningRound> getRound(uint64_t round_id);

    // Resolve the existing rounds.block_id association and capture its complete
    // round in that same read snapshot. Multiple associated rounds refuse.
    std::optional<MiningRound> getRoundForBlock(uint64_t block_id);

    // Add difficulty to worker in round
    bool addWorkerDifficultyToRound(uint64_t round_id,
                                     const std::string& worker_id,
                                     double difficulty);

    // ========================================================================
    // POOL STATS
    // ========================================================================

    // Get overall pool stats
    PoolStats getPoolStats();

    // Get total shares in time period
    uint64_t getTotalSharesInPeriod(int64_t start_time, int64_t end_time);

    // Get total difficulty in time period
    double getTotalDifficultyInPeriod(int64_t start_time, int64_t end_time);

    // Calculate pool luck (actual blocks / expected blocks)
    double calculateLuck(int64_t period_seconds);

    // ========================================================================
    // CONFIG
    // ========================================================================

    // Get pool config
    // Complete typed settings read or throw; missing keys retain documented defaults.
    PoolConfig getConfig();

    // Update pool config
    // Validate and persist all settings in one checked FULL transaction.
    // Refuse borrowed transactions and leave prior settings intact on failure.
    bool updateConfig(const PoolConfig& config);

    // ========================================================================
    // MAINTENANCE
    // ========================================================================

    // Prune old shares (keep last N days)
    uint64_t pruneOldShares(int64_t days = 30);

    // Vacuum database
    bool vacuum();

    // Get database size
    uint64_t getDatabaseSize();

private:
    friend class PayoutProcessor;
    friend struct PoolPaymentAttemptTestAccess;
    friend class PoolPaymentCanonicalOwner;
    friend struct PoolPaymentSettlementTestAccess;
    bool reconcilePaymentSettlement(const PoolPaymentAttempt&,const std::optional<PoolPaymentSettlement>&);
    PoolPaymentAttempt beginPaymentAttempt(const PoolPaymentWalletBinding&,
                                           const std::vector<uint64_t>& payout_ids);
    void retainPaymentAttempt(const PoolPaymentAttempt&,const PoolPaymentRetained&);
    std::vector<PoolPaymentAttempt> readPaymentAttemptsOwned();

    friend class PayoutCalculator;
    // Actual DB-only calculator dispatch, never an external payment callback.
    // Owns the full pending allocation batch; returns only after checked COMMIT.
    uint32_t allocateConfirmedBlockPayouts(PayoutCalculator& calculator);
    std::string getAllocationWorkerWallet(const std::string& worker_id);
    friend struct PoolOrphanAccountingTestAccess;
    // All use of the SQLite handle is serialized, including whole transactions.
    // Recursive only for existing DB-to-DB calls and DB-only transaction bodies.
    // This does not extend the lifetime of PoolDB beyond its owning manager.
    mutable std::recursive_mutex connection_mutex_;
    std::string db_path_;
    sqlite3* db_;

    // Schema creation
    bool createTables();
    bool createIndexes();

    // Helper for SQL execution
    bool executeSQL(const std::string& sql);
    bool executeSQL(const std::string& sql, std::function<void(sqlite3_stmt*)> bind_fn);
};

} // namespace pool
} // namespace dinero
