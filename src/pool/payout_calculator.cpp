/**
 * Payout Calculator Implementation
 *
 * Implements PROP, PPLNS, PPS, and SOLO payout algorithms for mining pools.
 */

#include "pool/payout_calculator.h"
#include <ctime>
#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace dinero {
namespace pool {

// ============================================================================
// PAYOUT CALCULATOR
// ============================================================================

PayoutCalculator::PayoutCalculator(PoolDB& db, const PoolConfig& config)
    : db_(db), config_(config) {}

std::vector<Payout> PayoutCalculator::calculatePayouts(const PoolBlock& block) {
    switch (config_.payout_mode) {
        case PayoutMode::PROP:
            return calculatePROP(block);
        case PayoutMode::PPLNS:
            return calculatePPLNS(block);
        case PayoutMode::PPS:
            return calculatePPS(block);
        case PayoutMode::SOLO:
            return calculateSOLO(block);
        default:
            return calculatePPLNS(block);  // Default to PPLNS
    }
}

// ============================================================================
// PROP (Proportional) Algorithm
// ============================================================================

std::vector<Payout> PayoutCalculator::calculatePROP(const PoolBlock& block) {
    std::vector<Payout> payouts;

    // Get the round associated with this block
    auto round = db_.getRound(block.block_id);
    if (!round) {
        return payouts;  // No round data
    }

    // Get distributable amount (after pool fee)
    uint64_t distributable = getDistributable(block.total_reward);
    if (distributable == 0 || round->total_difficulty == 0) {
        return payouts;
    }

    // Calculate payout for each worker based on their difficulty contribution
    for (const auto& [worker_id, worker_diff] : round->worker_difficulty) {
        if (worker_diff <= 0) continue;

        // Calculate share percentage
        double share_percent = calculateSharePercent(worker_diff, round->total_difficulty);

        // Calculate amount (rounding down to ensure we don't overpay)
        uint64_t amount = static_cast<uint64_t>(distributable * share_percent);
        if (amount == 0) continue;

        // Get worker's wallet address
        std::string wallet = getWorkerWallet(worker_id);
        if (wallet.empty()) continue;

        // Create payout record
        Payout payout = createBasePayout(block, worker_id, wallet);
        payout.amount = amount;
        payout.share_percent = share_percent * 100.0;  // Store as percentage
        payout.difficulty_sum = worker_diff;

        payouts.push_back(payout);
    }

    // Validate total doesn't exceed distributable
    if (!validatePayouts(payouts, distributable)) {
        // Scale down if needed (shouldn't happen with proper rounding)
        uint64_t total = 0;
        for (const auto& p : payouts) total += p.amount;

        if (total > distributable) {
            double scale = static_cast<double>(distributable) / total;
            for (auto& p : payouts) {
                p.amount = static_cast<uint64_t>(p.amount * scale);
            }
        }
    }

    return payouts;
}

// ============================================================================
// PPLNS (Pay Per Last N Shares) Algorithm
// ============================================================================

std::vector<Payout> PayoutCalculator::calculatePPLNS(const PoolBlock& block) {
    std::vector<Payout> payouts;

    // Get the last N shares
    auto shares = db_.getLastNShares(config_.pplns_window);
    if (shares.empty()) {
        return payouts;
    }

    // Get distributable amount
    uint64_t distributable = getDistributable(block.total_reward);
    if (distributable == 0) {
        return payouts;
    }

    // Aggregate difficulty by worker
    std::map<std::string, double> worker_difficulty;
    std::map<std::string, uint64_t> worker_share_count;
    std::map<std::string, std::string> worker_wallet;  // Cache wallet addresses
    double total_difficulty = 0;

    for (const auto& share : shares) {
        if (share.status != ShareStatus::VALID && share.status != ShareStatus::BLOCK) {
            continue;  // Only count valid shares
        }

        worker_difficulty[share.worker_id] += share.difficulty_real;
        worker_share_count[share.worker_id]++;
        worker_wallet[share.worker_id] = share.wallet_address;
        total_difficulty += share.difficulty_real;
    }

    if (total_difficulty == 0) {
        return payouts;
    }

    // Calculate payout for each worker
    for (const auto& [worker_id, worker_diff] : worker_difficulty) {
        if (worker_diff <= 0) continue;

        // Calculate share percentage
        double share_percent = calculateSharePercent(worker_diff, total_difficulty);

        // Calculate amount
        uint64_t amount = static_cast<uint64_t>(distributable * share_percent);
        if (amount == 0) continue;

        // Get wallet address
        std::string wallet = worker_wallet[worker_id];
        if (wallet.empty()) {
            wallet = getWorkerWallet(worker_id);
        }
        if (wallet.empty()) continue;

        // Create payout record
        Payout payout = createBasePayout(block, worker_id, wallet);
        payout.amount = amount;
        payout.share_percent = share_percent * 100.0;
        payout.share_count = worker_share_count[worker_id];
        payout.difficulty_sum = worker_diff;

        payouts.push_back(payout);
    }

    // Validate
    validatePayouts(payouts, distributable);

    return payouts;
}

// ============================================================================
// PPS (Pay Per Share) Algorithm
// ============================================================================

std::vector<Payout> PayoutCalculator::calculatePPS(const PoolBlock& block) {
    std::vector<Payout> payouts;

    // For PPS, payouts are calculated per-share as they're submitted
    // This function handles the case when a block is found
    // We need to credit workers for shares submitted since last block

    // Get shares since the last block was found
    int64_t last_block_time = 0;
    auto recent_blocks = db_.getRecentBlocks(2);  // Get this block and previous
    if (recent_blocks.size() > 1) {
        last_block_time = recent_blocks[1].found_at;
    }

    // Get shares in this time range
    auto shares = db_.getSharesInRange(last_block_time, block.found_at);
    if (shares.empty()) {
        return payouts;
    }

    // Calculate PPS rate
    // Rate = expected_reward / network_difficulty
    // For simplicity, use the block's actual reward
    double pps_rate = config_.pps_rate;
    if (pps_rate <= 0) {
        // Auto-calculate based on block reward and pool-estimated difficulty
        // This is a simplified calculation - real pools use network difficulty
        pps_rate = static_cast<double>(block.total_reward) /
                   (block.round_difficulty > 0 ? block.round_difficulty : 1.0);
    }

    // Apply pool fee to PPS rate
    pps_rate *= (1.0 - config_.pool_fee_percent / 100.0);

    // Aggregate by worker
    std::map<std::string, double> worker_difficulty;
    std::map<std::string, uint64_t> worker_share_count;
    std::map<std::string, std::string> worker_wallet;

    for (const auto& share : shares) {
        if (share.status != ShareStatus::VALID && share.status != ShareStatus::BLOCK) {
            continue;
        }

        worker_difficulty[share.worker_id] += share.difficulty_real;
        worker_share_count[share.worker_id]++;
        worker_wallet[share.worker_id] = share.wallet_address;
    }

    // Calculate payouts
    for (const auto& [worker_id, worker_diff] : worker_difficulty) {
        if (worker_diff <= 0) continue;

        // PPS amount = difficulty * rate
        uint64_t amount = static_cast<uint64_t>(worker_diff * pps_rate);
        if (amount == 0) continue;

        std::string wallet = worker_wallet[worker_id];
        if (wallet.empty()) {
            wallet = getWorkerWallet(worker_id);
        }
        if (wallet.empty()) continue;

        Payout payout = createBasePayout(block, worker_id, wallet);
        payout.amount = amount;
        payout.share_count = worker_share_count[worker_id];
        payout.difficulty_sum = worker_diff;

        // For PPS, share_percent represents rate used
        payout.share_percent = pps_rate;

        payouts.push_back(payout);
    }

    return payouts;
}

// ============================================================================
// SOLO Algorithm
// ============================================================================

std::vector<Payout> PayoutCalculator::calculateSOLO(const PoolBlock& block) {
    std::vector<Payout> payouts;

    // Solo mining: block finder gets everything minus pool fee
    if (block.finder_worker.empty() || block.finder_address.empty()) {
        return payouts;
    }

    uint64_t distributable = getDistributable(block.total_reward);
    if (distributable == 0) {
        return payouts;
    }

    Payout payout = createBasePayout(block, block.finder_worker, block.finder_address);
    payout.amount = distributable;
    payout.share_percent = 100.0;
    payout.share_count = 1;

    payouts.push_back(payout);

    return payouts;
}

// ============================================================================
// HELPER METHODS
// ============================================================================

uint64_t PayoutCalculator::calculatePoolFee(uint64_t total_reward) const {
    return static_cast<uint64_t>(total_reward * config_.pool_fee_percent / 100.0);
}

uint64_t PayoutCalculator::getDistributable(uint64_t total_reward) const {
    uint64_t fee = calculatePoolFee(total_reward);
    return total_reward > fee ? total_reward - fee : 0;
}

double PayoutCalculator::calculateSharePercent(double worker_difficulty, double total_difficulty) const {
    if (total_difficulty <= 0) return 0.0;
    return worker_difficulty / total_difficulty;
}

double PayoutCalculator::calculatePPSRate(double network_difficulty, uint64_t block_reward) const {
    if (network_difficulty <= 0) return 0.0;
    return static_cast<double>(block_reward) / network_difficulty;
}

bool PayoutCalculator::validatePayouts(const std::vector<Payout>& payouts, uint64_t distributable) const {
    uint64_t total = 0;
    for (const auto& p : payouts) {
        if (p.amount == 0) return false;
        if (p.wallet_address.empty()) return false;
        total += p.amount;
    }
    return total <= distributable;
}

Payout PayoutCalculator::createBasePayout(const PoolBlock& block, const std::string& worker_id,
                                          const std::string& wallet_address) const {
    Payout payout;
    payout.payout_id = 0;  // Will be set by DB on insert
    payout.block_id = block.block_id;
    payout.worker_id = worker_id;
    payout.wallet_address = wallet_address;
    payout.status = PayoutStatus::PENDING;
    payout.calculated_at = std::time(nullptr);
    return payout;
}

std::map<std::string, double> PayoutCalculator::aggregateSharesByWorker(uint64_t round_id) const {
    std::map<std::string, double> result;

    // This would typically query the round_shares table
    auto round = db_.getRound(round_id);
    if (round) {
        result = round->worker_difficulty;
    }

    return result;
}

std::string PayoutCalculator::getWorkerWallet(const std::string& worker_id) const {
    auto worker = db_.getWorker(worker_id);
    if (worker) {
        return worker->wallet_address;
    }
    return "";
}

// ============================================================================
// BATCH PROCESSING
// ============================================================================

uint32_t PayoutCalculator::processConfirmedBlocks() {
    uint32_t processed = 0;

    // Get blocks ready for payout (confirmed but payouts not calculated)
    auto blocks = db_.getBlocksReadyForPayout();

    for (auto& block : blocks) {
        if (block.orphaned) continue;
        if (block.payouts_calculated) continue;

        // Calculate payouts
        auto payouts = calculatePayouts(block);

        // Insert payout records
        for (auto& payout : payouts) {
            payout.status = PayoutStatus::CONFIRMED;
            db_.insertPayout(payout);

            // Add to worker's pending balance
            db_.addWorkerPending(payout.worker_id, payout.amount);
        }

        // Mark block as payouts calculated
        block.payouts_calculated = true;
        db_.updateBlock(block);

        processed++;
    }

    return processed;
}

std::map<std::string, std::vector<Payout>> PayoutCalculator::getPayoutsReadyToSend() {
    std::map<std::string, std::vector<Payout>> by_address;

    auto payouts = db_.getPayoutsReadyToSend();
    for (auto& payout : payouts) {
        by_address[payout.wallet_address].push_back(payout);
    }

    return by_address;
}

std::map<std::string, uint64_t> PayoutCalculator::aggregatePendingByAddress() {
    std::map<std::string, uint64_t> result;

    auto workers = db_.getWorkersWithPendingBalance(config_.min_payout);
    for (const auto& worker : workers) {
        result[worker.wallet_address] += worker.pending_payout;
    }

    return result;
}

// ============================================================================
// PAYOUT PROCESSOR
// ============================================================================

PayoutProcessor::PayoutProcessor(PoolDB& db, PaymentCallback payment_fn)
    : db_(db), payment_fn_(payment_fn) {}

uint32_t PayoutProcessor::processPendingPayouts() {
    uint32_t processed = 0;

    auto payouts = db_.getPayoutsReadyToSend();

    // Group by address for batch processing
    std::map<std::string, std::vector<Payout*>> by_address;
    for (auto& payout : payouts) {
        by_address[payout.wallet_address].push_back(&payout);
    }

    // Process each address
    for (auto& [address, payout_ptrs] : by_address) {
        // Calculate total for this address
        uint64_t total = 0;
        for (auto* p : payout_ptrs) {
            total += p->amount;
        }

        // Send payment
        std::string txid;
        bool success = payment_fn_(address, total, txid);

        // Update payout records
        for (auto* p : payout_ptrs) {
            if (success) {
                p->status = PayoutStatus::PAID;
                p->txid = txid;
                p->paid_at = std::time(nullptr);

                // Update worker balance
                db_.subtractWorkerPending(p->worker_id, p->amount);
                db_.addWorkerPaid(p->worker_id, p->amount);
            } else {
                p->status = PayoutStatus::FAILED;
                p->error_message = "Payment failed";
            }

            db_.updatePayoutStatus(p->payout_id, p->status, p->txid, p->error_message);
            processed++;
        }
    }

    return processed;
}

bool PayoutProcessor::processPayout(Payout& payout) {
    std::string txid;
    bool success = payment_fn_(payout.wallet_address, payout.amount, txid);

    if (success) {
        payout.status = PayoutStatus::PAID;
        payout.txid = txid;
        payout.paid_at = std::time(nullptr);

        db_.subtractWorkerPending(payout.worker_id, payout.amount);
        db_.addWorkerPaid(payout.worker_id, payout.amount);
    } else {
        payout.status = PayoutStatus::FAILED;
        payout.error_message = "Payment failed";
    }

    db_.updatePayoutStatus(payout.payout_id, payout.status, payout.txid, payout.error_message);
    return success;
}

uint32_t PayoutProcessor::retryFailedPayouts(uint32_t max_retries) {
    uint32_t processed = 0;

    auto pending = db_.getPendingPayouts();

    for (auto& payout : pending) {
        if (payout.status == PayoutStatus::FAILED) {
            if (payout.retry_count >= max_retries) {
                continue;
            }

            const uint32_t attempt = payout.retry_count + 1;
            db_.incrementPayoutRetry(
                payout.payout_id,
                std::time(nullptr),
                "Retry attempt " + std::to_string(attempt));

            if (processPayout(payout)) {
                processed++;
            } else if (attempt >= max_retries) {
                db_.updatePayoutStatus(
                    payout.payout_id,
                    PayoutStatus::FAILED,
                    "",
                    "Payment failed (retry limit reached)");
            }
        }
    }

    return processed;
}

} // namespace pool
} // namespace dinero
