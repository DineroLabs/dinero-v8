#include "crypto/bulletproofs_optimized.h"
#include "dinero/core/logging/logger.h"
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <mutex>

namespace dinero {
namespace crypto {

// Static member initialization
size_t BulletproofsOptimized::max_threads_ = std::thread::hardware_concurrency();
ProofStats BulletproofsMonitor::stats_;

// ============================================================================
// Parallel Proof Generation
// ============================================================================

std::vector<std::vector<uint8_t>> BulletproofsOptimized::generateParallel(
    const std::vector<uint64_t>& values,
    const std::vector<std::vector<uint8_t>>& blindings
) {
    if (values.size() != blindings.size()) {
        throw std::invalid_argument("Values and blindings must have same size");
    }

    size_t num_proofs = values.size();

    // For small batches, sequential is faster due to thread overhead
    if (num_proofs <= 2) {
        std::vector<std::vector<uint8_t>> proofs;
        proofs.reserve(num_proofs);

        for (size_t i = 0; i < num_proofs; ++i) {
            proofs.push_back(
                BulletproofRangeProof::generate(values[i], blindings[i])
            );
        }

        return proofs;
    }

    // Parallel generation for larger batches
    size_t num_threads = std::min(max_threads_, num_proofs);
    std::vector<std::future<std::vector<uint8_t>>> futures;
    futures.reserve(num_proofs);

    dinero::g_logger.debug("Generating " + std::to_string(num_proofs) +
                          " Bulletproofs in parallel using " +
                          std::to_string(num_threads) + " threads");

    auto start = std::chrono::high_resolution_clock::now();

    // Launch parallel tasks
    for (size_t i = 0; i < num_proofs; ++i) {
        futures.push_back(
            std::async(std::launch::async,
                [&values, &blindings, i]() {
                    return BulletproofRangeProof::generate(values[i], blindings[i]);
                }
            )
        );
    }

    // Collect results
    std::vector<std::vector<uint8_t>> proofs;
    proofs.reserve(num_proofs);

    for (auto& future : futures) {
        proofs.push_back(future.get());
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double, std::milli>(end - start).count();

    dinero::g_logger.info("Generated " + std::to_string(num_proofs) +
                         " Bulletproofs in " + std::to_string(duration) + " ms (" +
                         std::to_string(duration / num_proofs) + " ms per proof)");

    BulletproofsMonitor::recordGeneration(duration);

    return proofs;
}

// ============================================================================
// Batch Verification
// ============================================================================

bool BulletproofsOptimized::verifyBatch(
    const std::vector<std::vector<uint8_t>>& commitments,
    const std::vector<std::vector<uint8_t>>& proofs
) {
    if (commitments.size() != proofs.size()) {
        throw std::invalid_argument("Commitments and proofs must have same size");
    }

    if (commitments.empty()) {
        return true;
    }

    auto start = std::chrono::high_resolution_clock::now();

    // Use Bulletproofs batch verification (2-3x faster)
    bool result = BulletproofRangeProof::verifyBatch(commitments, proofs);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double, std::milli>(end - start).count();

    dinero::g_logger.debug("Batch verified " + std::to_string(proofs.size()) +
                          " Bulletproofs in " + std::to_string(duration) + " ms (" +
                          std::to_string(duration / proofs.size()) + " ms per proof)");

    BulletproofsMonitor::recordVerification(duration);

    return result;
}

// ============================================================================
// Cached Generation (Testing Only)
// ============================================================================

std::vector<uint8_t> BulletproofsOptimized::generateCached(
    uint64_t value,
    const std::vector<uint8_t>& blinding,
    bool use_cache
) {
    // WARNING: Caching is disabled by default for security
    // Only use in testing environments!
    if (!use_cache) {
        auto start = std::chrono::high_resolution_clock::now();
        auto proof = BulletproofRangeProof::generate(value, blinding);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start).count();

        BulletproofsMonitor::recordGeneration(duration);
        return proof;
    }

    // Cache implementation (not secure for production)
    static std::mutex cache_mutex;
    static std::map<std::pair<uint64_t, std::vector<uint8_t>>, std::vector<uint8_t>> proof_cache;

    std::lock_guard<std::mutex> lock(cache_mutex);

    auto key = std::make_pair(value, blinding);
    auto it = proof_cache.find(key);

    if (it != proof_cache.end()) {
        dinero::g_logger.warn("Using cached Bulletproof (TESTING ONLY)");
        return it->second;
    }

    auto proof = BulletproofRangeProof::generate(value, blinding);
    proof_cache[key] = proof;

    return proof;
}

// ============================================================================
// Benchmarking
// ============================================================================

std::pair<double, double> BulletproofsOptimized::benchmarkGeneration(size_t num_proofs) {
    dinero::g_logger.info("Benchmarking Bulletproof generation: " +
                         std::to_string(num_proofs) + " proofs");

    // Prepare test data
    std::vector<uint64_t> values;
    std::vector<std::vector<uint8_t>> blindings;

    for (size_t i = 0; i < num_proofs; ++i) {
        values.push_back(100000000 * (i + 1)); // 1 DIN, 2 DIN, ...

        std::vector<uint8_t> blinding(32);
        for (size_t j = 0; j < 32; ++j) {
            blinding[j] = static_cast<uint8_t>((i * 31 + j) % 256);
        }
        blindings.push_back(blinding);
    }

    // Sequential benchmark
    auto seq_start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < num_proofs; ++i) {
        BulletproofRangeProof::generate(values[i], blindings[i]);
    }

    auto seq_end = std::chrono::high_resolution_clock::now();
    double sequential_time = std::chrono::duration<double, std::milli>(seq_end - seq_start).count();

    // Parallel benchmark
    auto par_start = std::chrono::high_resolution_clock::now();

    generateParallel(values, blindings);

    auto par_end = std::chrono::high_resolution_clock::now();
    double parallel_time = std::chrono::duration<double, std::milli>(par_end - par_start).count();

    dinero::g_logger.info("Sequential: " + std::to_string(sequential_time) + " ms (" +
                         std::to_string(sequential_time / num_proofs) + " ms/proof)");
    dinero::g_logger.info("Parallel: " + std::to_string(parallel_time) + " ms (" +
                         std::to_string(parallel_time / num_proofs) + " ms/proof)");
    dinero::g_logger.info("Speedup: " + std::to_string(sequential_time / parallel_time) + "x");

    return {sequential_time, parallel_time};
}

std::pair<double, double> BulletproofsOptimized::benchmarkVerification(size_t num_proofs) {
    dinero::g_logger.info("Benchmarking Bulletproof verification: " +
                         std::to_string(num_proofs) + " proofs");

    // Generate test proofs
    std::vector<uint64_t> values;
    std::vector<std::vector<uint8_t>> blindings;
    std::vector<std::vector<uint8_t>> commitments;

    for (size_t i = 0; i < num_proofs; ++i) {
        values.push_back(100000000 * (i + 1));

        std::vector<uint8_t> blinding(32);
        for (size_t j = 0; j < 32; ++j) {
            blinding[j] = static_cast<uint8_t>((i * 31 + j) % 256);
        }
        blindings.push_back(blinding);

        // Create dummy commitment (in production, use secp256k1-zkp)
        std::vector<uint8_t> commitment(32);
        for (size_t j = 0; j < 32; ++j) {
            commitment[j] = static_cast<uint8_t>((i * 17 + j) % 256);
        }
        commitments.push_back(commitment);
    }

    auto proofs = generateParallel(values, blindings);

    // Individual verification benchmark
    auto ind_start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < num_proofs; ++i) {
        BulletproofRangeProof::verify(commitments[i], proofs[i]);
    }

    auto ind_end = std::chrono::high_resolution_clock::now();
    double individual_time = std::chrono::duration<double, std::milli>(ind_end - ind_start).count();

    // Batch verification benchmark
    auto batch_start = std::chrono::high_resolution_clock::now();

    verifyBatch(commitments, proofs);

    auto batch_end = std::chrono::high_resolution_clock::now();
    double batch_time = std::chrono::duration<double, std::milli>(batch_end - batch_start).count();

    dinero::g_logger.info("Individual: " + std::to_string(individual_time) + " ms (" +
                         std::to_string(individual_time / num_proofs) + " ms/proof)");
    dinero::g_logger.info("Batch: " + std::to_string(batch_time) + " ms (" +
                         std::to_string(batch_time / num_proofs) + " ms/proof)");
    dinero::g_logger.info("Speedup: " + std::to_string(individual_time / batch_time) + "x");

    return {individual_time, batch_time};
}

// ============================================================================
// Thread Configuration
// ============================================================================

void BulletproofsOptimized::setMaxThreads(size_t num_threads) {
    if (num_threads == 0) {
        max_threads_ = std::thread::hardware_concurrency();
    } else {
        max_threads_ = num_threads;
    }

    dinero::g_logger.info("Bulletproofs thread pool size set to: " +
                         std::to_string(max_threads_));
}

size_t BulletproofsOptimized::getMaxThreads() {
    return max_threads_;
}

size_t BulletproofsOptimized::getOptimalChunkSize(size_t total_items) {
    if (total_items == 0) return 0;

    size_t chunk_size = (total_items + max_threads_ - 1) / max_threads_;
    return std::max(chunk_size, size_t(1));
}

// ============================================================================
// Performance Monitoring
// ============================================================================

void BulletproofsMonitor::recordGeneration(double duration_ms) {
    static std::mutex stats_mutex;
    std::lock_guard<std::mutex> lock(stats_mutex);

    stats_.total_proofs_generated++;
    stats_.total_generation_time_ms += duration_ms;
    stats_.avg_generation_time_ms = stats_.total_generation_time_ms / stats_.total_proofs_generated;
}

void BulletproofsMonitor::recordVerification(double duration_ms) {
    static std::mutex stats_mutex;
    std::lock_guard<std::mutex> lock(stats_mutex);

    stats_.total_proofs_verified++;
    stats_.total_verification_time_ms += duration_ms;
    stats_.avg_verification_time_ms = stats_.total_verification_time_ms / stats_.total_proofs_verified;
}

ProofStats BulletproofsMonitor::getStats() {
    static std::mutex stats_mutex;
    std::lock_guard<std::mutex> lock(stats_mutex);
    return stats_;
}

void BulletproofsMonitor::resetStats() {
    static std::mutex stats_mutex;
    std::lock_guard<std::mutex> lock(stats_mutex);

    stats_ = ProofStats{};
    dinero::g_logger.info("Bulletproof statistics reset");
}

} // namespace crypto
} // namespace dinero
