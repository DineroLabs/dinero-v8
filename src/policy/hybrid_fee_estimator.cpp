/**
 * Phase 32: Hybrid ML Fee Estimator Implementation
 *
 * Combines EWMA + ML + Mempool analysis for robust fee estimation on small chains.
 */

#include "policy/hybrid_fee_estimator.h"
#include "common/logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <ctime>

namespace dinero {
namespace policy {

// ═══════════════════════════════════════════════════════════════════════════
// ML Trend Predictor Implementation
// ═══════════════════════════════════════════════════════════════════════════

MLTrendPredictor::MLTrendPredictor(size_t window_size)
    : window_size_(window_size)
    , min_observations_(10)  // Need at least 10 observations for meaningful regression
    , slope_(0.0)
    , intercept_(0.0)
    , last_update_(0)
{
    g_logger.info("[MLTrendPredictor] Initialized with window_size=" + std::to_string(window_size));
}

void MLTrendPredictor::addObservation(const FeeDataPoint& data_point) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Add to sliding window
    observations_.push_back(data_point);

    // Keep window size limited
    while (observations_.size() > window_size_) {
        observations_.pop_front();
    }

    // Mark regression as stale
    last_update_ = 0;

    g_logger.debug("[MLTrendPredictor] Added observation: fee_rate=" +
                  std::to_string(data_point.fee_rate) + " sat/vB, height=" +
                  std::to_string(data_point.block_height) + ", observations=" +
                  std::to_string(observations_.size()));
}

double MLTrendPredictor::predictFeeRate(size_t target_blocks, uint64_t current_time) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!hasSufficientData()) {
        return 0.0;  // Insufficient data
    }

    // Recompute regression if stale
    if (last_update_ == 0) {
        computeRegression();
    }

    // Predict future fee rate using linear model
    // fee_rate = intercept + slope * time
    uint64_t target_time = current_time + (target_blocks * 600);  // 600s per block (10 min)
    double predicted = intercept_ + (slope_ * static_cast<double>(target_time));

    // Clamp to reasonable bounds (min 1.0 sat/vB, max 1000 sat/vB)
    predicted = std::max(1.0, std::min(predicted, 1000.0));

    g_logger.debug("[MLTrendPredictor] Prediction for +" + std::to_string(target_blocks) +
                  " blocks: " + std::to_string(predicted) + " sat/vB (slope=" +
                  std::to_string(slope_) + ")");

    return predicted;
}

void MLTrendPredictor::computeRegression() const {
    // Simple linear regression: y = mx + b
    // Where: y = fee_rate, x = timestamp
    //
    // slope (m) = (N * Σ(xy) - Σx * Σy) / (N * Σ(x²) - (Σx)²)
    // intercept (b) = (Σy - m * Σx) / N

    size_t n = observations_.size();
    if (n < min_observations_) {
        slope_ = 0.0;
        intercept_ = 0.0;
        return;
    }

    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xy = 0.0;
    double sum_x2 = 0.0;

    for (const auto& obs : observations_) {
        double x = static_cast<double>(obs.timestamp);
        double y = obs.fee_rate;

        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }

    double n_double = static_cast<double>(n);

    // Calculate slope
    double numerator = (n_double * sum_xy) - (sum_x * sum_y);
    double denominator = (n_double * sum_x2) - (sum_x * sum_x);

    if (std::abs(denominator) < 1e-9) {
        // Degenerate case: all x values are the same
        slope_ = 0.0;
        intercept_ = sum_y / n_double;
    } else {
        slope_ = numerator / denominator;
        intercept_ = (sum_y - (slope_ * sum_x)) / n_double;
    }

    last_update_ = std::time(nullptr);

    g_logger.debug("[MLTrendPredictor] Regression updated: slope=" + std::to_string(slope_) +
                  ", intercept=" + std::to_string(intercept_) + ", n=" + std::to_string(n));
}

void MLTrendPredictor::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    observations_.clear();
    slope_ = 0.0;
    intercept_ = 0.0;
    last_update_ = 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Mempool Analyzer Implementation
// ═══════════════════════════════════════════════════════════════════════════

MempoolAnalyzer::MempoolAnalyzer()
    : capacity_mb_(300)  // Default mempool capacity (300 MB)
{
    g_logger.info("[MempoolAnalyzer] Initialized with capacity=" + std::to_string(capacity_mb_) + " MB");
}

double MempoolAnalyzer::analyzeMempoolFees(const mempool::Mempool& mempool, size_t target_blocks) const {
    // Get mempool congestion
    double congestion = calculateCongestion(mempool);

    // Get fee at appropriate percentile based on target
    double percentile;
    if (target_blocks == 1) {
        percentile = 0.90;  // IMMEDIATE: 90th percentile (high priority)
    } else if (target_blocks <= 3) {
        percentile = 0.75;  // FAST: 75th percentile
    } else if (target_blocks <= 6) {
        percentile = 0.50;  // NORMAL: median
    } else {
        percentile = 0.25;  // SLOW/ECONOMY: 25th percentile
    }

    double base_fee = getFeeAtPercentile(mempool, percentile);

    // Adjust for congestion
    // If mempool is >70% full, increase fees
    if (congestion > 0.7) {
        double multiplier = 1.0 + ((congestion - 0.7) * 2.0);  // Up to 2.6x at 100% full
        base_fee *= multiplier;

        g_logger.info("[MempoolAnalyzer] Congestion adjustment: " + std::to_string(congestion * 100) +
                     "% full → " + std::to_string(multiplier) + "x multiplier");
    }

    g_logger.debug("[MempoolAnalyzer] target=" + std::to_string(target_blocks) +
                  " blocks, p" + std::to_string(static_cast<int>(percentile * 100)) +
                  "=" + std::to_string(base_fee) + " sat/vB");

    return base_fee;
}

double MempoolAnalyzer::calculateCongestion(const mempool::Mempool& mempool) const {
    size_t mempool_size_bytes = mempool.getSize();
    size_t capacity_bytes = capacity_mb_ * 1024 * 1024;

    double congestion = static_cast<double>(mempool_size_bytes) / static_cast<double>(capacity_bytes);

    return congestion;
}

double MempoolAnalyzer::getFeeAtPercentile(const mempool::Mempool& mempool, double percentile) const {
    auto entries = mempool.getEntriesByFeeRate();  // Sorted by fee rate (descending)

    if (entries.empty()) {
        return 1.0;  // Default minimum fee
    }

    // Reverse to get ascending order
    std::reverse(entries.begin(), entries.end());

    size_t index = static_cast<size_t>(percentile * entries.size());
    if (index >= entries.size()) {
        index = entries.size() - 1;
    }

    return entries[index]->fee_rate;
}

// ═══════════════════════════════════════════════════════════════════════════
// Historical Data Persistence Implementation
// ═══════════════════════════════════════════════════════════════════════════

FeeHistoryPersistence::FeeHistoryPersistence(const std::string& data_dir)
    : data_dir_(data_dir)
    , history_file_(data_dir + "/fee_history.csv")
{
    g_logger.info("[FeeHistoryPersistence] Using history file: " + history_file_);
}

std::vector<FeeDataPoint> FeeHistoryPersistence::loadHistory() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<FeeDataPoint> history;
    std::ifstream file(history_file_);

    if (!file.is_open()) {
        g_logger.warning("[FeeHistoryPersistence] No history file found, starting fresh");
        return history;
    }

    std::string line;
    size_t line_num = 0;

    while (std::getline(file, line)) {
        line_num++;

        // Skip header
        if (line_num == 1 && line.find("timestamp") != std::string::npos) {
            continue;
        }

        // Parse CSV: timestamp,height,fee_rate,target_blocks,confidence
        std::istringstream ss(line);
        std::string field;
        std::vector<std::string> fields;

        while (std::getline(ss, field, ',')) {
            fields.push_back(field);
        }

        if (fields.size() != 5) {
            g_logger.warning("[FeeHistoryPersistence] Invalid line " + std::to_string(line_num) +
                           ": expected 5 fields, got " + std::to_string(fields.size()));
            continue;
        }

        try {
            FeeDataPoint dp;
            dp.timestamp = std::stoull(fields[0]);
            dp.block_height = std::stoul(fields[1]);
            dp.fee_rate = std::stod(fields[2]);
            dp.confirmation_blocks = std::stoull(fields[3]);
            dp.confidence = std::stod(fields[4]);

            history.push_back(dp);
        } catch (const std::exception& e) {
            g_logger.warning("[FeeHistoryPersistence] Failed to parse line " +
                           std::to_string(line_num) + ": " + e.what());
            continue;
        }
    }

    file.close();

    g_logger.info("[FeeHistoryPersistence] Loaded " + std::to_string(history.size()) +
                 " historical data points");

    return history;
}

bool FeeHistoryPersistence::saveDataPoint(const FeeDataPoint& data_point) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ofstream file(history_file_, std::ios::app);

    if (!file.is_open()) {
        g_logger.error("[FeeHistoryPersistence] Failed to open history file for writing");
        return false;
    }

    // Check if file is empty (need header)
    file.seekp(0, std::ios::end);
    if (file.tellp() == 0) {
        file << "timestamp,height,fee_rate,target_blocks,confidence\n";
    }

    // Write data point
    file << data_point.timestamp << ","
         << data_point.block_height << ","
         << data_point.fee_rate << ","
         << data_point.confirmation_blocks << ","
         << data_point.confidence << "\n";

    file.close();

    return true;
}

size_t FeeHistoryPersistence::pruneOldData(uint32_t max_age_days) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto history = loadHistory();
    if (history.empty()) {
        return 0;
    }

    uint64_t cutoff_time = std::time(nullptr) - (max_age_days * 86400);
    size_t initial_count = history.size();

    // Filter out old data
    auto new_end = std::remove_if(history.begin(), history.end(),
        [cutoff_time](const FeeDataPoint& dp) {
            return dp.timestamp < cutoff_time;
        });

    history.erase(new_end, history.end());

    size_t pruned = initial_count - history.size();

    if (pruned == 0) {
        return 0;
    }

    // Rewrite file with pruned data
    std::ofstream file(history_file_, std::ios::trunc);
    if (!file.is_open()) {
        g_logger.error("[FeeHistoryPersistence] Failed to rewrite history file");
        return 0;
    }

    file << "timestamp,height,fee_rate,target_blocks,confidence\n";

    for (const auto& dp : history) {
        file << dp.timestamp << ","
             << dp.block_height << ","
             << dp.fee_rate << ","
             << dp.confirmation_blocks << ","
             << dp.confidence << "\n";
    }

    file.close();

    g_logger.info("[FeeHistoryPersistence] Pruned " + std::to_string(pruned) +
                 " old data points (older than " + std::to_string(max_age_days) + " days)");

    return pruned;
}

// ═══════════════════════════════════════════════════════════════════════════
// Adaptive Fallback Rates Implementation
// ═══════════════════════════════════════════════════════════════════════════

AdaptiveFallbackRates::AdaptiveFallbackRates()
    : decay_factor_(0.95)  // EWMA decay for adaptive updates
{
    // Initialize with static fallback rates (sat/KB)
    fallback_rates_[FeeTarget::IMMEDIATE] = 100000.0;  // 100 sat/vB
    fallback_rates_[FeeTarget::FAST] = 50000.0;        // 50 sat/vB
    fallback_rates_[FeeTarget::NORMAL] = 20000.0;      // 20 sat/vB
    fallback_rates_[FeeTarget::SLOW] = 10000.0;        // 10 sat/vB
    fallback_rates_[FeeTarget::ECONOMY] = 5000.0;      // 5 sat/vB

    g_logger.info("[AdaptiveFallbackRates] Initialized with static fallbacks");
}

void AdaptiveFallbackRates::updateFromRecentActivity(const std::vector<double>& recent_fees) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (recent_fees.empty()) {
        return;
    }

    // Calculate percentiles from recent fees
    std::vector<double> sorted_fees = recent_fees;
    std::sort(sorted_fees.begin(), sorted_fees.end());

    auto get_percentile = [&sorted_fees](double p) -> double {
        size_t idx = static_cast<size_t>(p * sorted_fees.size());
        if (idx >= sorted_fees.size()) idx = sorted_fees.size() - 1;
        return sorted_fees[idx];
    };

    // Update fallbacks using EWMA
    // New_fallback = decay * old_fallback + (1 - decay) * observed
    double p90 = get_percentile(0.90) * 1000;  // Convert to sat/KB
    double p75 = get_percentile(0.75) * 1000;
    double p50 = get_percentile(0.50) * 1000;
    double p25 = get_percentile(0.25) * 1000;
    double p10 = get_percentile(0.10) * 1000;

    fallback_rates_[FeeTarget::IMMEDIATE] = decay_factor_ * fallback_rates_[FeeTarget::IMMEDIATE] +
                                            (1.0 - decay_factor_) * p90;
    fallback_rates_[FeeTarget::FAST] = decay_factor_ * fallback_rates_[FeeTarget::FAST] +
                                       (1.0 - decay_factor_) * p75;
    fallback_rates_[FeeTarget::NORMAL] = decay_factor_ * fallback_rates_[FeeTarget::NORMAL] +
                                         (1.0 - decay_factor_) * p50;
    fallback_rates_[FeeTarget::SLOW] = decay_factor_ * fallback_rates_[FeeTarget::SLOW] +
                                       (1.0 - decay_factor_) * p25;
    fallback_rates_[FeeTarget::ECONOMY] = decay_factor_ * fallback_rates_[FeeTarget::ECONOMY] +
                                          (1.0 - decay_factor_) * p10;

    g_logger.debug("[AdaptiveFallbackRates] Updated: IMMEDIATE=" +
                  std::to_string(fallback_rates_[FeeTarget::IMMEDIATE] / 1000) + " sat/vB");
}

double AdaptiveFallbackRates::getFallbackRate(FeeTarget target) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = fallback_rates_.find(target);
    if (it == fallback_rates_.end()) {
        return 10000.0;  // Default 10 sat/vB (as sat/KB)
    }

    return it->second;
}

void AdaptiveFallbackRates::reset() {
    std::lock_guard<std::mutex> lock(mutex_);

    fallback_rates_[FeeTarget::IMMEDIATE] = 100000.0;
    fallback_rates_[FeeTarget::FAST] = 50000.0;
    fallback_rates_[FeeTarget::NORMAL] = 20000.0;
    fallback_rates_[FeeTarget::SLOW] = 10000.0;
    fallback_rates_[FeeTarget::ECONOMY] = 5000.0;

    g_logger.info("[AdaptiveFallbackRates] Reset to static fallbacks");
}

// ═══════════════════════════════════════════════════════════════════════════
// Hybrid Fee Estimator Implementation
// ═══════════════════════════════════════════════════════════════════════════

HybridFeeEstimator::HybridFeeEstimator(const std::string& data_dir)
    : data_dir_(data_dir)
{
    g_logger.info("[HybridFeeEstimator] Phase 32: Initializing Hybrid ML Fee Estimator");
}

HybridFeeEstimator::~HybridFeeEstimator() {
    g_logger.info("[HybridFeeEstimator] Shutdown");
}

bool HybridFeeEstimator::initialize(std::shared_ptr<FeeEstimator> base_estimator) {
    std::lock_guard<std::mutex> lock(mutex_);

    g_logger.info("[HybridFeeEstimator] Initializing components...");

    // Store base EWMA estimator (optional)
    base_estimator_ = base_estimator;

    // Initialize sub-components
    ml_predictor_ = std::make_unique<MLTrendPredictor>(100);  // 100 observation window
    mempool_analyzer_ = std::make_unique<MempoolAnalyzer>();
    persistence_ = std::make_unique<FeeHistoryPersistence>(data_dir_);
    adaptive_fallbacks_ = std::make_unique<AdaptiveFallbackRates>();

    // Load historical data
    auto history = persistence_->loadHistory();
    g_logger.info("[HybridFeeEstimator] Loaded " + std::to_string(history.size()) +
                 " historical observations");

    // Feed historical data to ML predictor
    for (const auto& dp : history) {
        ml_predictor_->addObservation(dp);
    }

    // Update adaptive fallbacks from historical data
    if (!history.empty()) {
        std::vector<double> recent_fees;
        for (const auto& dp : history) {
            recent_fees.push_back(dp.fee_rate);
        }
        adaptive_fallbacks_->updateFromRecentActivity(recent_fees);
    }

    g_logger.info("[HybridFeeEstimator] ✅ Initialization complete");

    return true;
}

void HybridFeeEstimator::recordConfirmation(double fee_rate, size_t confirmation_blocks, uint32_t height) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t timestamp = std::time(nullptr);

    // Create data point
    FeeDataPoint dp(timestamp, height, fee_rate, confirmation_blocks, 1.0);

    // Add to ML predictor
    ml_predictor_->addObservation(dp);

    // Persist to disk
    persistence_->saveDataPoint(dp);

    g_logger.debug("[HybridFeeEstimator] Recorded confirmation: fee_rate=" +
                  std::to_string(fee_rate) + " sat/vB, conf_blocks=" +
                  std::to_string(confirmation_blocks) + ", height=" + std::to_string(height));
}

FeeEstimate HybridFeeEstimator::estimateFee(FeeTarget target) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t target_blocks = static_cast<size_t>(target);
    FeeEstimate result;

    // Get estimates from each component
    double ewma_estimate = 0.0;
    double ml_estimate = 0.0;
    double mempool_estimate = 0.0;

    // 1. EWMA Base Layer
    if (base_estimator_) {
        auto ewma_result = base_estimator_->estimateFee(target);
        ewma_estimate = ewma_result.fee_rate;
    }

    // 2. ML Trend Prediction
    if (ml_predictor_->hasSufficientData()) {
        ml_estimate = ml_predictor_->predictFeeRate(target_blocks, std::time(nullptr));
    }

    // 3. Mempool Analysis
    if (mempool_) {
        mempool_estimate = mempool_analyzer_->analyzeMempoolFees(*mempool_, target_blocks);
    }

    // Calculate weights for hybrid decision
    Weights weights = calculateWeights(target);

    // Combine estimates
    double hybrid_fee = combineEstimates(ewma_estimate, ml_estimate, mempool_estimate, weights);

    // If all sources failed, use adaptive fallback
    if (hybrid_fee < 1.0) {
        hybrid_fee = adaptive_fallbacks_->getFallbackRate(target) / 1000.0;  // Convert sat/KB to sat/vB
        result.is_sufficient_data = false;
        result.confidence = 0.3;  // Low confidence

        g_logger.warning("[HybridFeeEstimator] Using adaptive fallback: " +
                        std::to_string(hybrid_fee) + " sat/vB");
    } else {
        result.is_sufficient_data = true;
        result.confidence = std::min(weights.ewma_weight + weights.ml_weight + weights.mempool_weight, 1.0);
    }

    result.fee_rate = hybrid_fee;
    result.sample_size = ml_predictor_->hasSufficientData() ? 100 : 0;  // Approximate

    g_logger.info("[HybridFeeEstimator] Hybrid estimate for " + std::to_string(target_blocks) +
                 " blocks: " + std::to_string(result.fee_rate) + " sat/vB (confidence=" +
                 std::to_string(result.confidence) + ")");

    return result;
}

double HybridFeeEstimator::estimateFeeRate(size_t target_blocks) {
    FeeTarget target;
    if (target_blocks == 1) target = FeeTarget::IMMEDIATE;
    else if (target_blocks <= 3) target = FeeTarget::FAST;
    else if (target_blocks <= 6) target = FeeTarget::NORMAL;
    else if (target_blocks <= 12) target = FeeTarget::SLOW;
    else target = FeeTarget::ECONOMY;

    auto estimate = estimateFee(target);
    return estimate.fee_rate;
}

HybridFeeEstimator::Weights HybridFeeEstimator::calculateWeights(FeeTarget target) const {
    Weights w;

    // Base weights
    w.ewma_weight = 0.4;     // EWMA is baseline
    w.ml_weight = 0.3;       // ML adds trend awareness
    w.mempool_weight = 0.3;  // Mempool adds real-time awareness

    // Adjust based on mempool congestion
    if (mempool_) {
        double congestion = mempool_analyzer_->calculateCongestion(*mempool_);

        if (congestion > 0.7) {
            // High congestion: trust mempool more
            w.mempool_weight = 0.6;
            w.ewma_weight = 0.2;
            w.ml_weight = 0.2;
        } else if (congestion < 0.1) {
            // Low congestion: trust EWMA and ML more
            w.mempool_weight = 0.1;
            w.ewma_weight = 0.5;
            w.ml_weight = 0.4;
        }
    }

    // Adjust based on ML confidence
    if (ml_predictor_->hasSufficientData()) {
        double trend_slope = ml_predictor_->getTrendSlope();

        if (std::abs(trend_slope) > 0.001) {
            // Strong trend: increase ML weight
            w.ml_weight += 0.1;
            w.ewma_weight -= 0.05;
            w.mempool_weight -= 0.05;
        }
    } else {
        // Insufficient ML data: redistribute weight
        w.ewma_weight += w.ml_weight / 2.0;
        w.mempool_weight += w.ml_weight / 2.0;
        w.ml_weight = 0.0;
    }

    // Normalize weights to sum to 1.0
    double total = w.ewma_weight + w.ml_weight + w.mempool_weight;
    if (total > 0.0) {
        w.ewma_weight /= total;
        w.ml_weight /= total;
        w.mempool_weight /= total;
    }

    return w;
}

double HybridFeeEstimator::combineEstimates(double ewma, double ml, double mempool, const Weights& weights) const {
    double total_weight = 0.0;
    double weighted_sum = 0.0;

    if (ewma > 0.0) {
        weighted_sum += ewma * weights.ewma_weight;
        total_weight += weights.ewma_weight;
    }

    if (ml > 0.0) {
        weighted_sum += ml * weights.ml_weight;
        total_weight += weights.ml_weight;
    }

    if (mempool > 0.0) {
        weighted_sum += mempool * weights.mempool_weight;
        total_weight += weights.mempool_weight;
    }

    if (total_weight < 0.01) {
        return 0.0;  // No valid estimates
    }

    return weighted_sum / total_weight;
}

HybridFeeEstimator::EstimateBreakdown HybridFeeEstimator::getBreakdown(FeeTarget target) const {
    std::lock_guard<std::mutex> lock(mutex_);

    EstimateBreakdown breakdown;

    size_t target_blocks = static_cast<size_t>(target);

    // EWMA estimate
    if (base_estimator_) {
        auto ewma_result = base_estimator_->estimateFee(target);
        breakdown.ewma_estimate = ewma_result.fee_rate;
    }

    // ML prediction
    if (ml_predictor_->hasSufficientData()) {
        breakdown.ml_prediction = ml_predictor_->predictFeeRate(target_blocks, std::time(nullptr));
        breakdown.trend_slope = ml_predictor_->getTrendSlope();
    }

    // Mempool analysis
    if (mempool_) {
        breakdown.mempool_estimate = mempool_analyzer_->analyzeMempoolFees(*mempool_, target_blocks);
        breakdown.congestion_ratio = mempool_analyzer_->calculateCongestion(*mempool_);
    }

    // Calculate final hybrid estimate
    Weights weights = calculateWeights(target);
    breakdown.hybrid_final = combineEstimates(breakdown.ewma_estimate,
                                              breakdown.ml_prediction,
                                              breakdown.mempool_estimate,
                                              weights);

    // Decision reason
    if (breakdown.congestion_ratio > 0.7) {
        breakdown.decision_reason = "High mempool congestion (mempool-weighted)";
    } else if (ml_predictor_->hasSufficientData() && std::abs(breakdown.trend_slope) > 0.001) {
        breakdown.decision_reason = "Strong fee trend detected (ML-weighted)";
    } else if (breakdown.ewma_estimate > 0.0) {
        breakdown.decision_reason = "Normal operation (EWMA-weighted)";
    } else {
        breakdown.decision_reason = "Insufficient data (adaptive fallback)";
    }

    return breakdown;
}

void HybridFeeEstimator::performMaintenance() {
    std::lock_guard<std::mutex> lock(mutex_);

    g_logger.info("[HybridFeeEstimator] Performing maintenance...");

    // Prune old historical data (keep last 30 days)
    size_t pruned = persistence_->pruneOldData(30);
    if (pruned > 0) {
        g_logger.info("[HybridFeeEstimator] Pruned " + std::to_string(pruned) + " old data points");
    }

    g_logger.info("[HybridFeeEstimator] Maintenance complete");
}

} // namespace policy
} // namespace dinero
