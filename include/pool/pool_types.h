#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <map>

namespace dinero {
namespace pool {

/**
 * Mining Pool Payout Accounting System
 *
 * Supports multiple payout modes:
 * - PROP (Proportional): Rewards split by share count per round
 * - PPLNS (Pay Per Last N Shares): Rolling window of N shares
 * - PPS (Pay Per Share): Fixed reward per share submitted
 * - SOLO: Full block reward to finder (pool takes fee only)
 */

// ============================================================================
// ENUMS
// ============================================================================

enum class PayoutMode {
    PROP,   // Proportional - split reward by shares in round
    PPLNS,  // Pay Per Last N Shares - rolling window
    PPS,    // Pay Per Share - fixed rate per share
    SOLO    // Solo mining - block finder gets full reward
};

enum class ShareStatus {
    VALID,      // Valid share, meets difficulty
    STALE,      // Valid but for old job
    DUPLICATE,  // Already submitted
    INVALID,    // Does not meet difficulty
    BLOCK       // Found a block!
};

enum class PayoutStatus {
    PENDING,    // Calculated, waiting for confirmation
    CONFIRMED,  // Block confirmed, ready to pay
    PAID,       // Payment sent
    FAILED      // Payment failed
};

// ============================================================================
// SHARE RECORD
// ============================================================================

/**
 * Single share submission from a miner
 */
struct Share {
    uint64_t share_id;          // Auto-increment ID
    std::string worker_id;      // Worker identifier (username.worker)
    std::string wallet_address; // Payout address

    // Share details
    std::string job_id;         // Job this share is for
    uint32_t difficulty;        // Share difficulty
    double difficulty_real;     // Actual difficulty (for vardiff)
    ShareStatus status;         // Valid/stale/invalid/block

    // Block info (if status == BLOCK)
    std::string block_hash;     // Block hash if found
    uint32_t block_height;      // Block height if found
    uint64_t block_reward;      // Block reward in una

    // Timestamps
    int64_t submitted_at;       // Unix timestamp

    Share() : share_id(0), difficulty(1), difficulty_real(1.0),
              status(ShareStatus::VALID), block_height(0), block_reward(0),
              submitted_at(0) {}
};

// ============================================================================
// WORKER STATS
// ============================================================================

/**
 * Aggregated stats for a worker
 */
struct WorkerStats {
    std::string worker_id;
    std::string wallet_address;

    // Share counts
    uint64_t shares_valid;
    uint64_t shares_stale;
    uint64_t shares_invalid;
    uint64_t blocks_found;

    // Difficulty
    double current_difficulty;
    double total_difficulty;    // Sum of all share difficulties

    // Hashrate (calculated from shares)
    double hashrate_1m;         // Last minute
    double hashrate_15m;        // Last 15 minutes
    double hashrate_1h;         // Last hour
    double hashrate_24h;        // Last 24 hours

    // Earnings
    uint64_t total_earned;      // Total earned (una)
    uint64_t pending_payout;    // Pending payout (una)
    uint64_t total_paid;        // Total paid out (una)

    // Timestamps
    int64_t first_seen;
    int64_t last_seen;
    int64_t last_share;

    WorkerStats() : shares_valid(0), shares_stale(0), shares_invalid(0),
                    blocks_found(0), current_difficulty(1.0), total_difficulty(0.0),
                    hashrate_1m(0.0), hashrate_15m(0.0), hashrate_1h(0.0),
                    hashrate_24h(0.0), total_earned(0), pending_payout(0),
                    total_paid(0), first_seen(0), last_seen(0), last_share(0) {}
};

// ============================================================================
// BLOCK RECORD
// ============================================================================

/**
 * Block found by the pool
 */
struct PoolBlock {
    uint64_t block_id;          // Auto-increment ID
    std::string block_hash;
    uint32_t height;

    // Finder info
    std::string finder_worker;
    std::string finder_address;

    // Reward info
    uint64_t reward;            // Block reward in una
    uint64_t fees;              // Transaction fees in una
    uint64_t total_reward;      // reward + fees

    // Pool fee
    double pool_fee_percent;    // e.g., 1.0 = 1%
    uint64_t pool_fee_amount;   // Calculated fee in una
    uint64_t distributable;     // total_reward - pool_fee_amount

    // Round info (for PROP mode)
    uint64_t round_shares;      // Total shares in this round
    double round_difficulty;    // Total difficulty in round

    // Status
    uint32_t confirmations;     // Current confirmations
    uint32_t required_confirmations; // Required before payout (e.g., 100)
    bool orphaned;              // True if block was orphaned
    bool payouts_calculated;    // True if payouts have been calculated
    bool payouts_sent;          // True if all payouts sent

    // Timestamps
    int64_t found_at;
    int64_t confirmed_at;       // When it reached required confirmations

    PoolBlock() : block_id(0), height(0), reward(0), fees(0), total_reward(0),
                  pool_fee_percent(1.0), pool_fee_amount(0), distributable(0),
                  round_shares(0), round_difficulty(0.0), confirmations(0),
                  required_confirmations(100), orphaned(false),
                  payouts_calculated(false), payouts_sent(false),
                  found_at(0), confirmed_at(0) {}
};

// ============================================================================
// PAYOUT RECORD
// ============================================================================

/**
 * Individual payout to a worker
 */
struct Payout {
    uint64_t payout_id;         // Auto-increment ID
    uint64_t block_id;          // Which block this payout is for (0 for PPS)

    // Worker info
    std::string worker_id;
    std::string wallet_address;

    // Amount
    uint64_t amount;            // Payout amount in una
    double share_percent;       // % of pool reward
    uint64_t share_count;       // Number of shares contributed
    double difficulty_sum;      // Sum of share difficulties

    // Status
    PayoutStatus status;
    std::string txid;           // Transaction ID when paid
    std::string error_message;  // If status == FAILED

    // Timestamps
    int64_t calculated_at;
    int64_t paid_at;
    uint32_t retry_count;       // Number of payment retry attempts
    int64_t last_retry_at;      // Last retry timestamp

    Payout() : payout_id(0), block_id(0), amount(0), share_percent(0.0),
               share_count(0), difficulty_sum(0.0), status(PayoutStatus::PENDING),
               calculated_at(0), paid_at(0), retry_count(0), last_retry_at(0) {}
};

// ============================================================================
// POOL CONFIG
// ============================================================================

/**
 * Pool configuration
 */
struct PoolConfig {
    // Payout mode
    PayoutMode payout_mode;

    // PPLNS specific
    uint64_t pplns_window;      // N shares to consider (e.g., 100000)

    // PPS specific
    double pps_rate;            // Rate per difficulty unit (in una)

    // Pool fee
    double pool_fee_percent;    // e.g., 1.0 = 1%
    std::string pool_fee_address; // Where pool fees go

    // Minimum payout
    uint64_t min_payout;        // Minimum payout threshold (una)
    uint64_t min_auto_payout;   // Auto-pay threshold (una)
    uint32_t max_payout_retries; // Retry attempts for failed payouts

    // Confirmations
    uint32_t required_confirmations; // Blocks before payout (e.g., 100)

    // Round boundaries
    bool new_round_on_block;    // Start new round when block found (PROP)

    PoolConfig() : payout_mode(PayoutMode::PPLNS), pplns_window(100000),
                   pps_rate(0), pool_fee_percent(1.0), min_payout(100000000),
                   min_auto_payout(1000000000), max_payout_retries(3),
                   required_confirmations(100),
                   new_round_on_block(true) {}
};

// ============================================================================
// POOL STATS
// ============================================================================

/**
 * Overall pool statistics
 */
struct PoolStats {
    // Workers
    uint32_t active_workers;    // Workers with shares in last 15 min
    uint32_t total_workers;     // Total unique workers ever

    // Hashrate
    double pool_hashrate;       // Combined pool hashrate

    // Shares
    uint64_t total_shares;
    uint64_t shares_per_second;

    // Blocks
    uint64_t blocks_found;
    uint64_t blocks_orphaned;
    uint64_t blocks_pending;    // Waiting for confirmations

    // Payouts
    uint64_t total_paid;        // Total DIN paid out
    uint64_t pending_payouts;   // Pending payout amount

    // Current round (PROP mode)
    uint64_t round_shares;
    int64_t round_start;

    // Luck
    double luck_1d;             // Last 24 hours
    double luck_7d;             // Last 7 days
    double luck_30d;            // Last 30 days

    // Network
    double network_difficulty;
    double network_hashrate;

    PoolStats() : active_workers(0), total_workers(0), pool_hashrate(0.0),
                  total_shares(0), shares_per_second(0), blocks_found(0),
                  blocks_orphaned(0), blocks_pending(0), total_paid(0),
                  pending_payouts(0), round_shares(0), round_start(0),
                  luck_1d(0.0), luck_7d(0.0), luck_30d(0.0),
                  network_difficulty(1.0), network_hashrate(0.0) {}
};

// ============================================================================
// ROUND (for PROP mode)
// ============================================================================

/**
 * Mining round (PROP mode)
 */
struct MiningRound {
    uint64_t round_id;
    uint64_t block_id;          // Block that ended this round (0 if ongoing)

    // Share tracking
    uint64_t total_shares;
    double total_difficulty;
    std::map<std::string, double> worker_difficulty; // worker -> difficulty sum

    // Timestamps
    int64_t started_at;
    int64_t ended_at;           // 0 if ongoing

    MiningRound() : round_id(0), block_id(0), total_shares(0),
                    total_difficulty(0.0), started_at(0), ended_at(0) {}
};

// Helper: Convert PayoutMode to string
inline std::string PayoutModeToString(PayoutMode mode) {
    switch (mode) {
        case PayoutMode::PROP: return "PROP";
        case PayoutMode::PPLNS: return "PPLNS";
        case PayoutMode::PPS: return "PPS";
        case PayoutMode::SOLO: return "SOLO";
        default: return "UNKNOWN";
    }
}

// Helper: Convert string to PayoutMode
inline PayoutMode StringToPayoutMode(const std::string& str) {
    if (str == "PROP") return PayoutMode::PROP;
    if (str == "PPLNS") return PayoutMode::PPLNS;
    if (str == "PPS") return PayoutMode::PPS;
    if (str == "SOLO") return PayoutMode::SOLO;
    return PayoutMode::PPLNS; // Default
}

} // namespace pool
} // namespace dinero
