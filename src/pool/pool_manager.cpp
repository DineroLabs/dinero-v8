/**
 * Pool Manager Implementation
 *
 * Coordinates pool operations and integrates with Stratum server.
 */

#include "pool/pool_manager.h"
#include "common/logger.h"
#include "consensus/subsidy.h"
#include "storage/chain_db.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace dinero {
namespace pool {

namespace {
constexpr int64_t kSubmitWindowSeconds = 60;
constexpr uint32_t kMaxSubmissionsPerWindow = 300;
constexpr uint32_t kMaxInvalidPerWindow = 80;
constexpr uint32_t kMaxDuplicatePerWindow = 140;
constexpr int64_t kBanSeconds = 120;
constexpr int64_t kShareDedupeRetentionSeconds = 6 * 60 * 60;
} // namespace

PoolManager::PoolManager(const std::string& db_path)
    : db_(std::make_unique<PoolDB>(db_path))
    , current_round_id_(0)
    , running_(false)
    , maintenance_running_(false)
{}

PoolManager::~PoolManager() {
    stopMaintenanceThread();
}

bool PoolManager::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_->initialize()) {
        g_logger.error("Failed to initialize pool database");
        return false;
    }

    // Load configuration
    config_ = db_->getConfig();

    // Get or create current round
    auto round = db_->getCurrentRound();
    if (round) {
        current_round_id_ = round->round_id;
    } else {
        current_round_id_ = db_->startNewRound();
    }

    // Create calculator and processor
    calculator_ = std::make_unique<PayoutCalculator>(*db_, config_);

    running_ = true;
    g_logger.info("Pool manager initialized (mode: " + PayoutModeToString(config_.payout_mode) +
                  ", round: " + std::to_string(current_round_id_) + ")");

    return true;
}

// ============================================================================
// STRATUM INTEGRATION
// ============================================================================

bool PoolManager::onWorkerAuthorize(const std::string& worker_id, const std::string& wallet_address) {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        auto worker = db_->getOrCreateWorker(worker_id, wallet_address);
        g_logger.debug("Worker authorized: " + worker_id + " -> " + wallet_address);
        return true;
    } catch (const std::exception& e) {
        g_logger.error("Failed to authorize worker " + worker_id + ": " + e.what());
        return false;
    }
}

PoolManager::ShareSubmitResult PoolManager::onShareSubmit(const std::string& worker_id,
                                                          const std::string& job_id,
                                                          double difficulty,
                                                          bool is_valid,
                                                          bool is_stale,
                                                          bool is_block,
                                                          const std::string& block_hash,
                                                          uint32_t block_height,
                                                          uint64_t block_reward,
                                                          const std::string& share_uid) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t now = std::time(nullptr);
    auto& submit_state = getSubmitStateForWorker(worker_id, now);

    std::string rate_limit_reason;
    if (shouldRateLimitSubmission(submit_state, now, rate_limit_reason)) {
        return ShareSubmitResult{
            ShareSubmitCode::RATE_LIMITED,
            ShareStatus::INVALID,
            rate_limit_reason
        };
    }
    submit_state.submissions++;

    auto worker = db_->getWorker(worker_id);
    if (!worker) {
        registerInvalidSubmission(submit_state, now, "unknown-worker");
        return ShareSubmitResult{
            ShareSubmitCode::UNKNOWN_WORKER,
            ShareStatus::INVALID,
            "Worker is not authorized"
        };
    }

    if (difficulty <= 0.0 || !std::isfinite(difficulty)) {
        registerInvalidSubmission(submit_state, now, "invalid-difficulty");
        return ShareSubmitResult{
            ShareSubmitCode::REJECTED,
            ShareStatus::INVALID,
            "Invalid difficulty"
        };
    }

    if (!is_valid && is_block) {
        registerInvalidSubmission(submit_state, now, "invalid-block-flag");
        return ShareSubmitResult{
            ShareSubmitCode::REJECTED,
            ShareStatus::INVALID,
            "Invalid share cannot be a block"
        };
    }

    const std::string dedupe_key = buildShareDedupeKey(
        worker_id,
        job_id,
        difficulty,
        is_valid,
        is_stale,
        is_block,
        block_hash,
        block_height,
        block_reward,
        share_uid);

    // Create share record
    Share share;
    share.worker_id = worker_id;
    share.job_id = job_id;
    share.difficulty = static_cast<uint32_t>(difficulty);
    share.difficulty_real = difficulty;
    share.submitted_at = now;
    share.wallet_address = worker->wallet_address;

    // Determine status
    if (!is_valid) {
        share.status = ShareStatus::INVALID;
    } else if (is_stale) {
        share.status = ShareStatus::STALE;
    } else if (is_block) {
        share.status = ShareStatus::BLOCK;
        share.block_hash = block_hash;
        share.block_height = block_height;
        share.block_reward = block_reward;
    } else {
        share.status = ShareStatus::VALID;
    }

    WorkerStats stats = *worker;
    stats.last_share = share.submitted_at;
    stats.last_seen = share.submitted_at;
    stats.current_difficulty = difficulty;

    switch (share.status) {
        case ShareStatus::VALID:
        case ShareStatus::BLOCK:
            stats.shares_valid++;
            stats.total_difficulty += difficulty;
            break;
        case ShareStatus::STALE:
            stats.shares_stale++;
            break;
        case ShareStatus::INVALID:
            stats.shares_invalid++;
            break;
        default:
            break;
    }

    if (share.status == ShareStatus::INVALID) {
        registerInvalidSubmission(submit_state, now, "invalid-share");
    }

    if (share.status == ShareStatus::BLOCK) {
        stats.blocks_found++;
    }

    const uint64_t original_round_id = current_round_id_;
    uint64_t next_round_id = current_round_id_;
    bool duplicate_submission = false;
    bool dedupe_reservation_failed = false;
    bool block_recorded = false;
    PoolBlock committed_block;

    const bool committed = db_->runInTransaction([&]() -> bool {
        const auto reservation = db_->reserveShareSubmissionKey(dedupe_key, worker_id, now);
        if (reservation != PoolDB::ShareSubmissionReservationResult::Reserved) {
            duplicate_submission = reservation == PoolDB::ShareSubmissionReservationResult::Duplicate;
            dedupe_reservation_failed = reservation == PoolDB::ShareSubmissionReservationResult::Error;
            return false;
        }

        if (!db_->insertShare(share)) {
            g_logger.error("Failed to insert share for worker " + worker_id);
            return false;
        }

        if (!db_->updateWorkerStats(stats)) {
            g_logger.error("Failed to update worker stats for " + worker_id);
            return false;
        }

        if (share.status == ShareStatus::VALID || share.status == ShareStatus::BLOCK) {
            if (!db_->addWorkerDifficultyToRound(current_round_id_, worker_id, difficulty)) {
                g_logger.error("Failed to update round difficulty for worker " + worker_id);
                return false;
            }
        }

        if (share.status == ShareStatus::BLOCK) {
            const uint64_t consensus_subsidy = ConsensusSubsidy::GetBlockSubsidy(block_height).GetUna();
            uint64_t block_reward_component = block_reward;
            uint64_t block_fee_component = 0;
            if (block_reward >= consensus_subsidy) {
                block_reward_component = consensus_subsidy;
                block_fee_component = block_reward - consensus_subsidy;
            } else {
                g_logger.warning("[Pool] Block reward below consensus subsidy at height " +
                                 std::to_string(block_height) + " reward=" + std::to_string(block_reward) +
                                 " subsidy=" + std::to_string(consensus_subsidy));
            }

            PoolBlock block;
            block.block_hash = block_hash;
            block.height = block_height;
            block.finder_worker = worker_id;
            block.finder_address = share.wallet_address;
            block.reward = block_reward_component;
            block.fees = block_fee_component;
            block.total_reward = block_reward;
            block.pool_fee_percent = config_.pool_fee_percent;
            block.pool_fee_amount = static_cast<uint64_t>(block_reward * config_.pool_fee_percent / 100.0);
            block.distributable = block_reward - block.pool_fee_amount;
            block.required_confirmations = config_.required_confirmations;
            block.found_at = share.submitted_at;

            auto round = db_->getRound(current_round_id_);
            if (round) {
                block.round_shares = round->total_shares;
                block.round_difficulty = round->total_difficulty;
            }

            if (!db_->insertBlock(block)) {
                g_logger.error("Failed to record block " + block_hash);
                return false;
            }

            if (config_.new_round_on_block) {
                if (!db_->endRound(current_round_id_, block.block_id)) {
                    g_logger.error("Failed to end round " + std::to_string(current_round_id_));
                    return false;
                }
                next_round_id = db_->startNewRound();
                if (next_round_id == 0) {
                    g_logger.error("Failed to start a new round after block " + block_hash);
                    return false;
                }
            }

            committed_block = block;
            block_recorded = true;
        }

        return true;
    });

    if (!committed) {
        if (duplicate_submission) {
            registerDuplicateSubmission(submit_state, now);
            return ShareSubmitResult{
                ShareSubmitCode::DUPLICATE,
                ShareStatus::DUPLICATE,
                "Duplicate share submission"
            };
        }

        if (dedupe_reservation_failed) {
            g_logger.error("Failed to reserve share dedupe key for worker " + worker_id);
        }
        g_logger.error("Failed to commit share accounting for worker " + worker_id);
        return ShareSubmitResult{
            ShareSubmitCode::REJECTED,
            ShareStatus::INVALID,
            "Failed to record share"
        };
    }

    current_round_id_ = next_round_id;

    if (block_recorded) {
        g_logger.info("BLOCK FOUND! height=" + std::to_string(committed_block.height) +
                      " hash=" + committed_block.block_hash.substr(0, 16) +
                      " finder=" + committed_block.finder_worker +
                      " reward=" + std::to_string(committed_block.total_reward / 100000000.0));
        if (config_.new_round_on_block && current_round_id_ != original_round_id) {
            g_logger.info("New round started: " + std::to_string(current_round_id_));
        }
    }

    std::string status_str = share.status == ShareStatus::VALID ? "valid" :
                             share.status == ShareStatus::STALE ? "stale" :
                             share.status == ShareStatus::INVALID ? "invalid" :
                             share.status == ShareStatus::BLOCK ? "BLOCK!" : "unknown";
    g_logger.debug("Share recorded: worker=" + worker_id + " diff=" + std::to_string(difficulty) +
                   " status=" + status_str);

    if (share.status == ShareStatus::VALID || share.status == ShareStatus::BLOCK) {
        updateWorkerHashrate(worker_id);
    }

    return ShareSubmitResult{
        ShareSubmitCode::ACCEPTED,
        share.status,
        "Share accepted"
    };
}

void PoolManager::onWorkerDisconnect(const std::string& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto worker = db_->getWorker(worker_id);
    if (worker) {
        WorkerStats stats = *worker;
        stats.last_seen = std::time(nullptr);
        db_->updateWorkerStats(stats);
    }
}

// ============================================================================
// BLOCK MANAGEMENT
// ============================================================================

uint64_t PoolManager::recordBlock(const PoolBlock& block) {
    // Note: mutex already held by onShareSubmit

    PoolBlock b = block;
    const uint64_t original_round_id = current_round_id_;
    uint64_t next_round_id = current_round_id_;

    const bool committed = db_->runInTransaction([&]() -> bool {
        if (!db_->insertBlock(b)) {
            g_logger.error("Failed to record block " + block.block_hash);
            return false;
        }

        if (config_.new_round_on_block) {
            if (!db_->endRound(current_round_id_, b.block_id)) {
                g_logger.error("Failed to end round " + std::to_string(current_round_id_));
                return false;
            }

            next_round_id = db_->startNewRound();
            if (next_round_id == 0) {
                g_logger.error("Failed to start a new round after block " + block.block_hash);
                return false;
            }
        }

        return true;
    });

    if (!committed) {
        return 0;
    }

    current_round_id_ = next_round_id;

    g_logger.info("BLOCK FOUND! height=" + std::to_string(b.height) +
                  " hash=" + b.block_hash.substr(0, 16) +
                  " finder=" + b.finder_worker +
                  " reward=" + std::to_string(b.total_reward / 100000000.0));

    if (config_.new_round_on_block && current_round_id_ != original_round_id) {
        g_logger.info("New round started: " + std::to_string(current_round_id_));
    }

    return b.block_id;
}

void PoolManager::updateBlockConfirmations(const std::string& block_hash, uint32_t confirmations) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto block = db_->getBlockByHash(block_hash);
    if (!block) return;

    PoolBlock b = *block;
    b.confirmations = confirmations;

    if (confirmations >= b.required_confirmations && b.confirmed_at == 0) {
        b.confirmed_at = std::time(nullptr);
        g_logger.info("Block " + block_hash.substr(0, 16) + " confirmed (" +
                      std::to_string(confirmations) + " confirmations)");
    }

    db_->updateBlock(b);
}

void PoolManager::markBlockOrphaned(const std::string& block_hash) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto block = db_->getBlockByHash(block_hash);
    if (!block) return;

    auto payouts = db_->getPayoutsForBlock(block->block_id);
    uint32_t pending_reversed = 0;
    uint32_t already_paid = 0;
    for (const auto& payout : payouts) {
        if (payout.status == PayoutStatus::PAID) {
            ++already_paid;
            continue;
        }
        if (payout.status == PayoutStatus::FAILED) {
            continue;
        }

        db_->subtractWorkerPending(payout.worker_id, payout.amount);
        db_->updatePayoutStatus(
            payout.payout_id,
            PayoutStatus::FAILED,
            "",
            "orphaned block");
        ++pending_reversed;
    }

    db_->markBlockOrphaned(block->block_id);
    g_logger.warning("Block " + block_hash.substr(0, 16) + " orphaned!");
    if (pending_reversed > 0 || already_paid > 0) {
        g_logger.warning("Pool payout rollback for orphan block " + block_hash.substr(0, 16) +
                         ": reversed_pending=" + std::to_string(pending_reversed) +
                         ", already_paid=" + std::to_string(already_paid));
    }
}

// ============================================================================
// PAYOUT PROCESSING
// ============================================================================

uint32_t PoolManager::processConfirmedBlocks() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!calculator_) return 0;
    return calculator_->processConfirmedBlocks();
}

uint32_t PoolManager::sendPendingPayouts() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!processor_) return 0;
    return processor_->processPendingPayouts();
}

uint32_t PoolManager::retryFailedPayouts(uint32_t max_retries) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!processor_) return 0;
    return processor_->retryFailedPayouts(max_retries);
}

void PoolManager::setPaymentCallback(PaymentCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);

    payment_callback_ = callback;
    processor_ = std::make_unique<PayoutProcessor>(*db_, callback);
}

// ============================================================================
// ROUND MANAGEMENT
// ============================================================================

uint64_t PoolManager::startNewRound() {
    std::lock_guard<std::mutex> lock(mutex_);

    current_round_id_ = db_->startNewRound();
    return current_round_id_;
}

uint64_t PoolManager::getCurrentRoundId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_round_id_;
}

// ============================================================================
// STATS & CONFIG
// ============================================================================

PoolStats PoolManager::getStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    return db_->getPoolStats();
}

PoolConfig PoolManager::getConfig() {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

bool PoolManager::setConfig(const PoolConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (db_->updateConfig(config)) {
        config_ = config;

        // Recreate calculator with new config
        calculator_ = std::make_unique<PayoutCalculator>(*db_, config_);

        return true;
    }
    return false;
}

std::optional<WorkerStats> PoolManager::getWorkerStats(const std::string& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return db_->getWorker(worker_id);
}

// ============================================================================
// MAINTENANCE
// ============================================================================

void PoolManager::startMaintenanceThread() {
    if (maintenance_running_.load()) return;

    maintenance_running_ = true;
    maintenance_thread_ = std::thread([this]() {
        while (maintenance_running_.load()) {
            runMaintenance();

            // Sleep for 60 seconds between maintenance runs
            for (int i = 0; i < 60 && maintenance_running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });

    g_logger.info("Pool maintenance thread started");
}

void PoolManager::stopMaintenanceThread() {
    maintenance_running_ = false;
    if (maintenance_thread_.joinable()) {
        maintenance_thread_.join();
    }
}

void PoolManager::runMaintenance() {
    try {
        // Process confirmed blocks
        uint32_t blocks_processed = processConfirmedBlocks();
        if (blocks_processed > 0) {
            g_logger.info("Processed " + std::to_string(blocks_processed) + " confirmed blocks");
        }

        // Check block confirmations
        checkBlockConfirmations();

        // Send pending payouts (if payment callback is set)
        if (payment_callback_) {
            uint32_t payouts_sent = sendPendingPayouts();
            if (payouts_sent > 0) {
                g_logger.info("Sent " + std::to_string(payouts_sent) + " payouts");
            }

            uint32_t retries_sent = retryFailedPayouts(config_.max_payout_retries);
            if (retries_sent > 0) {
                g_logger.info("Retried " + std::to_string(retries_sent) + " failed payouts");
            }
        }

        // Refresh hashrates and prune old data.
        std::lock_guard<std::mutex> lock(mutex_);
        auto active_workers = db_->getActiveWorkers(24 * 60 * 60);
        for (const auto& worker : active_workers) {
            updateWorkerHashrate(worker.worker_id);
        }

        uint64_t pruned = db_->pruneOldShares(30);
        if (pruned > 0) {
            g_logger.debug("Pruned " + std::to_string(pruned) + " old shares");
        }

        const int64_t dedupe_cutoff = std::time(nullptr) - kShareDedupeRetentionSeconds;
        uint64_t pruned_dedupe = db_->pruneShareSubmissionKeysOlderThan(dedupe_cutoff);
        if (pruned_dedupe > 0) {
            g_logger.debug("Pruned " + std::to_string(pruned_dedupe) + " share dedupe keys");
        }

    } catch (const std::exception& e) {
        g_logger.error("Maintenance error: " + std::string(e.what()));
    }
}

void PoolManager::checkBlockConfirmations() {
    if (!chain_db_) {
        g_logger.debug("[Pool] checkBlockConfirmations skipped: ChainDB not wired");
        return;
    }

    auto tip_result = chain_db_->getTip();
    if (tip_result.status() != Status::Ok) {
        return;
    }

    const uint32_t tip_height = tip_result.value().height;

    std::vector<PoolBlock> pending_blocks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_blocks = db_->getPendingBlocks();
    }

    for (const auto& pending : pending_blocks) {
        if (pending.block_hash.size() != 64 ||
            !std::all_of(pending.block_hash.begin(), pending.block_hash.end(), [](unsigned char c) {
                return std::isxdigit(c) != 0;
            })) {
            markBlockOrphaned(pending.block_hash);
            continue;
        }

        if (pending.height > tip_height) {
            continue;
        }

        auto active_hash = chain_db_->getBlockHashByHeight(static_cast<int>(pending.height));
        if (active_hash.status() != Status::Ok) {
            markBlockOrphaned(pending.block_hash);
            continue;
        }

        uint256 pending_hash;
        if (!uint256::FromHex(pending.block_hash, pending_hash)) {
            markBlockOrphaned(pending.block_hash);
            continue;
        }
        if (active_hash.value() != pending_hash) {
            markBlockOrphaned(pending.block_hash);
            continue;
        }

        const uint32_t confirmations = tip_height - pending.height + 1;
        if (pending.confirmations != confirmations) {
            updateBlockConfirmations(pending.block_hash, confirmations);
        }
    }
}

void PoolManager::updateWorkerHashrate(const std::string& worker_id) {
    auto worker = db_->getWorker(worker_id);
    if (!worker) {
        return;
    }

    const int64_t now = std::time(nullptr);
    constexpr double kHashesPerDifficulty = 4294967296.0;  // 2^32

    auto calc_hashrate = [&](int64_t window_seconds) -> double {
        if (window_seconds <= 0) {
            return 0.0;
        }
        const double difficulty_sum =
            db_->getWorkerDifficultyInRange(worker_id, now - window_seconds, now);
        if (difficulty_sum <= 0.0) {
            return 0.0;
        }
        return (difficulty_sum * kHashesPerDifficulty) / static_cast<double>(window_seconds);
    };

    WorkerStats updated = *worker;
    updated.hashrate_1m = calc_hashrate(60);
    updated.hashrate_15m = calc_hashrate(15 * 60);
    updated.hashrate_1h = calc_hashrate(60 * 60);
    updated.hashrate_24h = calc_hashrate(24 * 60 * 60);
    db_->updateWorkerStats(updated);
}

std::string PoolManager::buildShareDedupeKey(const std::string& worker_id,
                                             const std::string& job_id,
                                             double difficulty,
                                             bool is_valid,
                                             bool is_stale,
                                             bool is_block,
                                             const std::string& block_hash,
                                             uint32_t block_height,
                                             uint64_t block_reward,
                                             const std::string& share_uid) const {
    if (!share_uid.empty()) {
        return worker_id + "|uid|" + share_uid;
    }

    std::ostringstream oss;
    oss << worker_id << '|'
        << job_id << '|'
        << std::fixed << std::setprecision(8) << difficulty << '|'
        << (is_valid ? 1 : 0) << '|'
        << (is_stale ? 1 : 0) << '|'
        << (is_block ? 1 : 0) << '|'
        << block_hash << '|'
        << block_height << '|'
        << block_reward;
    return oss.str();
}

PoolManager::WorkerSubmitState& PoolManager::getSubmitStateForWorker(const std::string& worker_id, int64_t now) {
    auto& state = submit_state_[worker_id];
    if (state.window_start == 0 || now - state.window_start >= kSubmitWindowSeconds) {
        state.window_start = now;
        state.submissions = 0;
        state.invalid = 0;
        state.duplicates = 0;
    }
    return state;
}

bool PoolManager::shouldRateLimitSubmission(WorkerSubmitState& state, int64_t now, std::string& reason) {
    if (state.banned_until > now) {
        reason = "Worker temporarily rate-limited";
        return true;
    }

    if (state.submissions >= kMaxSubmissionsPerWindow) {
        state.banned_until = now + kBanSeconds;
        state.ban_reason = "submission-flood";
        reason = "Submission rate exceeded";
        return true;
    }
    return false;
}

void PoolManager::registerInvalidSubmission(WorkerSubmitState& state, int64_t now, const std::string& reason) {
    (void)reason;
    state.invalid++;
    if (state.invalid >= kMaxInvalidPerWindow) {
        state.banned_until = now + kBanSeconds;
        state.ban_reason = "invalid-share-flood";
    }
}

void PoolManager::registerDuplicateSubmission(WorkerSubmitState& state, int64_t now) {
    state.duplicates++;
    if (state.duplicates >= kMaxDuplicatePerWindow) {
        state.banned_until = now + kBanSeconds;
        state.ban_reason = "duplicate-share-flood";
    }
}

} // namespace pool
} // namespace dinero
