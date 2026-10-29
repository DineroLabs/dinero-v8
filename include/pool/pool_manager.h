#pragma once

#include "pool/pool_db.h"
#include "pool/pool_types.h"
#include "pool/payout_calculator.h"
#include <memory>
#include <string>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <unordered_map>

namespace dinero {
class ChainstateService;
namespace pool {

/**
 * Pool Manager - Coordinates pool operations and integrates with Stratum
 *
 * This is the main entry point for pool accounting. It:
 * - Receives share submissions from Stratum server
 * - Tracks workers and their stats
 * - Records blocks found
 * - Manages mining rounds (PROP mode)
 * - Triggers payout calculations
 *
 * Thread Safety: All public methods are thread-safe
 */
class PoolManager {
    struct MaintenanceWakeState;
public:
    // Ephemeral pass diagnostics only. Neither a pass nor a wake acknowledges
    // durable consumer delivery, canonical finality or all-consumer readiness.
    struct MaintenanceReport {
        uint64_t passes=0;
        bool running=false,accounting_pass_returned=false,maintenance_pass_returned=false;
    };
    class MaintenanceWakeHandle final {
    public:
        void RequestReplay() const noexcept;
        [[nodiscard]] bool Running() const noexcept;
    private:
        friend class PoolManager;
        explicit MaintenanceWakeHandle(std::shared_ptr<MaintenanceWakeState> state):state_(std::move(state)){}
        std::shared_ptr<MaintenanceWakeState> state_;
    };
    [[nodiscard]] MaintenanceWakeHandle CaptureMaintenanceWakeHandle() const noexcept {return MaintenanceWakeHandle(maintenance_state_);}
    [[nodiscard]] MaintenanceReport MaintenanceSnapshot() const;

    enum class ShareSubmitCode {
        ACCEPTED,
        DUPLICATE,
        RATE_LIMITED,
        UNKNOWN_WORKER,
        REJECTED
    };

    struct ShareSubmitResult {
        ShareSubmitCode code = ShareSubmitCode::REJECTED;
        ShareStatus status = ShareStatus::INVALID;
        std::string message;

        bool accepted() const { return code == ShareSubmitCode::ACCEPTED; }
    };

    // Callback types
    using BlockSubmitCallback = std::function<bool(const std::string& block_hex)>;
    using PaymentCallback = std::function<bool(const std::string& address, uint64_t amount, std::string& txid)>;

    /**
     * Constructor
     * @param db_path Path to SQLite database file
     */
    explicit PoolManager(const std::string& db_path);
    ~PoolManager();

    /**
     * Initialize the pool manager
     * Creates database tables and loads configuration
     */
    bool initialize();

    // Provide active chain view for confirmation/orphan tracking.
    void setChainstateSource(const std::shared_ptr<ChainstateService>& source);

    /**
     * Check if pool manager is running
     */
    bool isRunning() const { return running_.load(); }

    // ========================================================================
    // STRATUM INTEGRATION
    // ========================================================================

    /**
     * Called when a miner authorizes
     * Creates or updates worker record
     *
     * @param worker_id Worker identifier (e.g., "username.worker")
     * @param wallet_address Payout address
     * @return true if authorization successful
     */
    bool onWorkerAuthorize(const std::string& worker_id, const std::string& wallet_address);

    /**
     * Called when a share is submitted
     * Records share and updates worker stats
     *
     * @param worker_id Worker identifier
     * @param job_id Job ID the share is for
     * @param difficulty Share difficulty
     * @param is_valid Whether share meets difficulty target
     * @param is_stale Whether share is for old job
     * @param is_block Whether share found a block
     * @param block_hash Block hash if found
     * @param block_height Block height if found
     * @param block_reward Block reward if found (in una)
     */
    ShareSubmitResult onShareSubmit(const std::string& worker_id,
                                    const std::string& job_id,
                                    double difficulty,
                                    bool is_valid,
                                    bool is_stale,
                                    bool is_block,
                                    const std::string& block_hash = "",
                                    uint32_t block_height = 0,
                                    uint64_t block_reward = 0,
                                    const std::string& share_uid = "");

    /**
     * Called when a miner disconnects
     * Updates worker last_seen timestamp
     */
    void onWorkerDisconnect(const std::string& worker_id);

    // ========================================================================
    // BLOCK MANAGEMENT
    // ========================================================================

    /**
     * Record a block found by the pool
     * Starts payout calculation process
     *
     * @param block Block details
     * @return Block ID assigned by database
     */
    uint64_t recordBlock(const PoolBlock& block);

    /**
     * Update block confirmations
     * Called periodically to check block maturity
     *
     * @param block_hash Block hash
     * @param confirmations Current confirmation count
     */
    void updateBlockConfirmations(const std::string& block_hash, uint32_t confirmations);

    /**
     * Mark block as orphaned
     * Reverses any pending payouts
     */
    bool markBlockOrphaned(const std::string& block_hash);

    // ========================================================================
    // PAYOUT PROCESSING
    // ========================================================================

    /**
     * Process confirmed blocks and calculate payouts
     * Should be called periodically (e.g., every minute)
     *
     * @return Number of blocks processed
     */
    uint32_t processConfirmedBlocks();

    /**
     * Send pending payouts
     * Requires payment callback to be set
     *
     * @return Number of payouts sent
     */
    uint32_t sendPendingPayouts();

    /**
     * Retry failed payouts that are below retry cap
     * @return Number of payouts successfully retried
     */
    uint32_t retryFailedPayouts(uint32_t max_retries);

    /**
     * Set payment callback
     * Called when payouts need to be sent
     */
    // Install once for this manager lifetime; replacing a live context refuses.
    void setPaymentCallback(PaymentCallback callback);
    // Production payments use this exclusive owned backend. ClosePayments drains it.
    void setPaymentBackend(std::unique_ptr<PoolPaymentBackend>);
    // Permanent payment shutdown. Call outside wallet/chain/SQLite owners.
    // Existing synchronous operation drains without holding the manager mutex.
    void ClosePayments();

    // ========================================================================
    // ROUND MANAGEMENT (PROP MODE)
    // ========================================================================

    /**
     * Start a new mining round
     * Called automatically when block is found (if new_round_on_block enabled)
     */
    uint64_t startNewRound();

    /**
     * Get current round ID
     */
    uint64_t getCurrentRoundId() const;

    // ========================================================================
    // STATS & CONFIG
    // ========================================================================

    /**
     * Get pool statistics
     */
    PoolStats getStats();

    /**
     * Get pool configuration
     */
    PoolConfig getConfig();

    /**
     * Update pool configuration
     */
    bool setConfig(const PoolConfig& config);
    // A partial RPC update is prepared outside this owner. Refuse a stale base
    // rather than overwrite settings changed since the caller captured it.
    bool compareAndSetConfig(const PoolConfig& expected, const PoolConfig& config);

    /**
     * Get worker statistics
     */
    std::optional<WorkerStats> getWorkerStats(const std::string& worker_id);

    /**
     * Get database handle (for RPC methods)
     */
    PoolDB& getDatabase() { return *db_; }

    // ========================================================================
    // MAINTENANCE
    // ========================================================================

    /**
     * Start background maintenance thread
     * - Prunes old shares
     * - Updates block confirmations
     * - Processes payouts
     */
    void startMaintenanceThread();

    /**
     * Stop maintenance thread
     */
    void stopMaintenanceThread();

    /**
     * Run maintenance tasks once
     */
    void runMaintenance();

private:
    friend class CanonicalPoolMaintenance;
    friend struct PoolMaintenanceSourceTestAccess;
    std::shared_ptr<PoolDB> db_;
    std::unique_ptr<PayoutCalculator> calculator_;
    std::shared_ptr<PayoutProcessor> processor_;
    bool payments_closed_{false};

    PoolConfig config_;
    uint64_t current_round_id_;

    mutable std::mutex mutex_;
    std::atomic<bool> running_;

    // Only PoolManager owns/joins this thread. Prepared notification tokens
    // retain a mailbox, never this manager, the thread or the selected source.
    std::mutex maintenance_lifecycle_;
    struct MaintenanceWakeState {
        mutable std::mutex mutex;
        std::condition_variable wake;
        bool stopping=true,requested=false;
        MaintenanceReport report;
    };
    const std::shared_ptr<MaintenanceWakeState> maintenance_state_=std::make_shared<MaintenanceWakeState>();
    std::thread maintenance_thread_;
    void RunMaintenanceWorker() noexcept;


    // Internal helpers
    bool setConfigOwned(const PoolConfig& config); // mutex_ held by caller
    std::string buildShareDedupeKey(const std::string& worker_id,
                                    const std::string& job_id,
                                    double difficulty,
                                    bool is_valid,
                                    bool is_stale,
                                    bool is_block,
                                    const std::string& block_hash,
                                    uint32_t block_height,
                                    uint64_t block_reward,
                                    const std::string& share_uid) const;

    struct WorkerSubmitState {
        int64_t window_start = 0;
        uint32_t submissions = 0;
        uint32_t invalid = 0;
        uint32_t duplicates = 0;
        int64_t banned_until = 0;
        std::string ban_reason;
    };

    WorkerSubmitState& getSubmitStateForWorker(const std::string& worker_id, int64_t now);
    bool shouldRateLimitSubmission(WorkerSubmitState& state, int64_t now, std::string& reason);
    void registerInvalidSubmission(WorkerSubmitState& state, int64_t now, const std::string& reason);
    void registerDuplicateSubmission(WorkerSubmitState& state, int64_t now);

    void updateWorkerHashrate(const std::string& worker_id);
    void checkBlockConfirmations();

    std::unordered_map<std::string, WorkerSubmitState> submit_state_;
    std::weak_ptr<ChainstateService> chain_source_; // Captured under mutex_, pinned before use.
    // Bound only by setChainstateSource's concrete core adapter. Keeping this
    // call edge indirect lets the standalone accounting archive remain free
    // of daemon implementation dependencies; callers cannot install a bypass.
    using CanonicalMaintenance = void (*)(const std::shared_ptr<ChainstateService>&, PoolManager&);
    CanonicalMaintenance canonical_maintenance_{nullptr};
};

} // namespace pool
} // namespace dinero
