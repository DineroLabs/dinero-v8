#pragma once

#include "crypto/bulletproofs.h"
#include <vector>
#include <cstdint>
#include <memory>
#include <thread>
#include <future>

namespace dinero {
namespace crypto {

/**
 * Optimized Bulletproof generation and verification
 *
 * Provides performance optimizations for Bulletproofs:
 * 1. Parallel proof generation for multiple outputs
 * 2. Batch verification (2-3x faster than individual)
 * 3. Proof caching for common amounts
 * 4. Thread pool for concurrent operations
 */
class BulletproofsOptimized {
public:
    /**
     * Generate multiple range proofs in parallel
     *
     * Significantly faster than sequential generation when creating
     * transactions with multiple confidential outputs.
     *
     * @param values Vector of values to prove
     * @param blindings Vector of blinding factors (must match values length)
     * @return Vector of serialized proofs
     * @throws std::runtime_error on failure
     *
     * Performance: ~2x faster for 4+ outputs on multi-core systems
     */
    static std::vector<std::vector<uint8_t>> generateParallel(
        const std::vector<uint64_t>& values,
        const std::vector<std::vector<uint8_t>>& blindings
    );

    /**
     * Verify multiple proofs in batch (optimized)
     *
     * Uses Bulletproofs batch verification which is 2-3x faster than
     * verifying proofs individually. Essential for block validation.
     *
     * @param commitments Vector of Pedersen commitments
     * @param proofs Vector of serialized proofs
     * @return true if all proofs valid, false otherwise
     * @throws std::runtime_error on error
     *
     * Performance: 2-3x faster than individual verification
     */
    static bool verifyBatch(
        const std::vector<std::vector<uint8_t>>& commitments,
        const std::vector<std::vector<uint8_t>>& proofs
    );

    /**
     * Generate proof with caching for common amounts
     *
     * Caches proofs for frequently-used amounts (e.g., round numbers)
     * to avoid regeneration. Useful for testing and benchmarking.
     *
     * @param value Value to prove
     * @param blinding Blinding factor
     * @param use_cache Enable caching (default: false for security)
     * @return Serialized proof
     *
     * WARNING: Only use caching in testing environments!
     * Production should always generate fresh proofs.
     */
    static std::vector<uint8_t> generateCached(
        uint64_t value,
        const std::vector<uint8_t>& blinding,
        bool use_cache = false
    );

    /**
     * Benchmark proof generation performance
     *
     * Measures time to generate N proofs sequentially and in parallel.
     *
     * @param num_proofs Number of proofs to generate
     * @return Pair of <sequential_time_ms, parallel_time_ms>
     */
    static std::pair<double, double> benchmarkGeneration(size_t num_proofs);

    /**
     * Benchmark proof verification performance
     *
     * Measures time to verify N proofs individually vs batch.
     *
     * @param num_proofs Number of proofs to verify
     * @return Pair of <individual_time_ms, batch_time_ms>
     */
    static std::pair<double, double> benchmarkVerification(size_t num_proofs);

    /**
     * Set maximum number of threads for parallel operations
     *
     * @param num_threads Number of threads (0 = auto-detect)
     */
    static void setMaxThreads(size_t num_threads);

    /**
     * Get current thread pool size
     * @return Number of threads
     */
    static size_t getMaxThreads();

private:
    // Thread pool configuration
    static size_t max_threads_;

    // Helper: Get optimal chunk size for parallel processing
    static size_t getOptimalChunkSize(size_t total_items);
};

/**
 * Proof generation statistics
 */
struct ProofStats {
    size_t total_proofs_generated = 0;
    size_t total_proofs_verified = 0;
    double total_generation_time_ms = 0.0;
    double total_verification_time_ms = 0.0;
    double avg_generation_time_ms = 0.0;
    double avg_verification_time_ms = 0.0;
};

/**
 * Performance monitor for Bulletproof operations
 */
class BulletproofsMonitor {
public:
    /**
     * Record proof generation
     * @param duration_ms Time taken in milliseconds
     */
    static void recordGeneration(double duration_ms);

    /**
     * Record proof verification
     * @param duration_ms Time taken in milliseconds
     */
    static void recordVerification(double duration_ms);

    /**
     * Get current statistics
     * @return Proof generation and verification stats
     */
    static ProofStats getStats();

    /**
     * Reset statistics
     */
    static void resetStats();

private:
    static ProofStats stats_;
};

} // namespace crypto
} // namespace dinero
