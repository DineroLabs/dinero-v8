#pragma once

#include "pool/pool_types.h"
#include "pool/pool_db.h"
#include <memory>
#include <vector>
#include <map>
#include <functional>

namespace dinero {
namespace pool {

/**
 * Payout Calculator - Implements PROP, PPLNS, PPS, and SOLO payout algorithms
 *
 * Usage:
 *   PayoutCalculator calc(db, config);
 *   auto payouts = calc.calculatePayouts(block);
 *   for (auto& payout : payouts) {
 *       db.insertPayout(payout);
 *   }
 */
class PayoutCalculator {
public:
    explicit PayoutCalculator(PoolDB& db, const PoolConfig& config);

    // ========================================================================
    // MAIN CALCULATION ENTRY POINT
    // ========================================================================

    /**
     * Calculate payouts for a found block
     * Routes to appropriate algorithm based on config.payout_mode
     */
    std::vector<Payout> calculatePayouts(const PoolBlock& block);

    // ========================================================================
    // PAYOUT ALGORITHMS
    // ========================================================================

    /**
     * PROP (Proportional) Payout
     * - Split block reward proportionally by shares in the round
     * - Each worker gets: (worker_shares / total_shares) * distributable
     * - Simple and fair, but variance depends on luck
     */
    std::vector<Payout> calculatePROP(const PoolBlock& block);

    /**
     * PPLNS (Pay Per Last N Shares) Payout
     * - Consider only the last N shares (configurable window)
     * - More stable earnings for consistent miners
     * - Discourages pool hopping
     */
    std::vector<Payout> calculatePPLNS(const PoolBlock& block);

    /**
     * PPS (Pay Per Share) Payout
     * - Fixed payment per share submitted
     * - Pool takes the variance risk
     * - Most predictable for miners, risky for pool operator
     */
    std::vector<Payout> calculatePPS(const PoolBlock& block);

    /**
     * SOLO Payout
     * - Block finder gets full reward minus pool fee
     * - Other workers get nothing
     * - For pools that offer solo mining option
     */
    std::vector<Payout> calculateSOLO(const PoolBlock& block);

    // ========================================================================
    // HELPER METHODS
    // ========================================================================

    /**
     * Calculate pool fee from total reward
     */
    uint64_t calculatePoolFee(uint64_t total_reward) const;

    /**
     * Get distributable amount after pool fee
     */
    uint64_t getDistributable(uint64_t total_reward) const;

    /**
     * Calculate share percentage for a worker
     */
    double calculateSharePercent(double worker_difficulty, double total_difficulty) const;

    /**
     * Calculate PPS rate based on network difficulty and block reward
     * Rate per difficulty unit = expected_block_reward / network_difficulty
     */
    double calculatePPSRate(double network_difficulty, uint64_t block_reward) const;

    // ========================================================================
    // VALIDATION
    // ========================================================================

    /**
     * Validate calculated payouts
     * - Total payouts should not exceed distributable
     * - All amounts should be positive
     * - All workers should have valid addresses
     */
    bool validatePayouts(const std::vector<Payout>& payouts, uint64_t distributable) const;

    // ========================================================================
    // BATCH PAYOUT PROCESSING
    // ========================================================================

    /**
     * Process confirmed blocks and create payout records
     * Returns number of blocks processed
     */
    uint32_t processConfirmedBlocks();

    /**
     * Get all payouts ready to send (above minimum threshold)
     * Groups payouts by wallet address for batch sending
     */
    std::map<std::string, std::vector<Payout>> getPayoutsReadyToSend();

    /**
     * Aggregate pending payouts by wallet address
     * Returns map of address -> total pending amount
     */
    std::map<std::string, uint64_t> aggregatePendingByAddress();

private:
    PoolDB& db_;
    PoolConfig config_;

    // Helper to create base payout object
    Payout createBasePayout(const PoolBlock& block, const std::string& worker_id,
                           const std::string& wallet_address) const;

    // Aggregate shares by worker for a round
    std::map<std::string, double> aggregateSharesByWorker(uint64_t round_id) const;

    // Get worker wallet address
    std::string getWorkerWallet(const std::string& worker_id) const;
};

/**
 * Payout Processor - Handles actual payment transactions
 *
 * Separate from PayoutCalculator to allow different payment backends:
 * - Direct wallet integration
 * - External payment service
 * - Manual approval workflow
 */
class PayoutProcessor {
public:
    using PaymentCallback = std::function<bool(const std::string& address, uint64_t amount, std::string& txid)>;

    PayoutProcessor(PoolDB& db, PaymentCallback payment_fn);

    /**
     * Process pending payouts
     * - Groups by address
     * - Sends payments via callback
     * - Updates payout records with txid
     */
    uint32_t processPendingPayouts();

    /**
     * Process a single payout
     */
    bool processPayout(Payout& payout);

    /**
     * Retry failed payouts
     */
    uint32_t retryFailedPayouts(uint32_t max_retries = 3);

private:
    PoolDB& db_;
    PaymentCallback payment_fn_;
};

} // namespace pool
} // namespace dinero
