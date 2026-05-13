#pragma once

#include <atomic>
#include <chrono>
#include <random>
#include "mining/work_template.h"

namespace dinero {

/**
 * @brief Fake hasher for deterministic testing
 * 
 * This replaces real SHA-256 hashing with predictable behavior
 * for testing mining logic without actual cryptographic work.
 */
class FakeHasher {
public:
    struct Config {
        uint32_t baseHashesPerSecond = 1000;  // Base hashrate
        double variance = 0.1;                 // ±10% variance
        bool shouldFindSolution = false;       // Whether to "find" a solution
        uint32_t solutionAfterHashes = 10000; // Hash count before solution
        std::string solutionHash = "0000000000000000000000000000000000000000000000000000000000000000";
    };
    
    explicit FakeHasher(const Config& config = {}) 
        : config_(config), hashesComputed_(0), solutionFound_(false) {
        rng_.seed(12345); // Fixed seed for reproducibility
    }
    
    /**
     * @brief Simulate hashing work for a given duration
     * @param duration Duration to simulate
     * @param stopToken Atomic flag to check for early termination
     * @return Number of hashes computed
     */
    uint64_t hashForDuration(std::chrono::milliseconds duration, 
                           const std::atomic<bool>& stopToken) {
        auto start = std::chrono::steady_clock::now();
        uint64_t hashes = 0;
        
        while (!stopToken.load(std::memory_order_relaxed)) {
            auto now = std::chrono::steady_clock::now();
            if (now - start >= duration) {
                break;
            }
            
            // Simulate hash computation
            uint32_t hashesThisBatch = computeHashesBatch();
            hashes += hashesThisBatch;
            hashesComputed_ += hashesThisBatch;
            
            // Check if we should find a solution
            if (config_.shouldFindSolution && 
                hashesComputed_ >= config_.solutionAfterHashes && 
                !solutionFound_) {
                solutionFound_ = true;
                break;
            }
            
            // Small sleep to prevent busy waiting
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        
        return hashes;
    }
    
    /**
     * @brief Check if a solution was found
     */
    bool hasSolution() const {
        return solutionFound_;
    }
    
    /**
     * @brief Get the solution hash
     */
    std::string getSolutionHash() const {
        return config_.solutionHash;
    }
    
    /**
     * @brief Get total hashes computed
     */
    uint64_t getTotalHashes() const {
        return hashesComputed_;
    }
    
    /**
     * @brief Reset the hasher state
     */
    void reset() {
        hashesComputed_ = 0;
        solutionFound_ = false;
        rng_.seed(12345); // Reset to fixed seed
    }
    
    /**
     * @brief Configure the hasher
     */
    void configure(const Config& config) {
        config_ = config;
    }
    
private:
    Config config_;
    std::atomic<uint64_t> hashesComputed_{0};
    std::atomic<bool> solutionFound_{false};
    std::mt19937 rng_;
    
    uint32_t computeHashesBatch() {
        // Generate deterministic hash count with variance
        double variance = (rng_() % 2000 - 1000) / 10000.0; // ±10%
        double multiplier = 1.0 + variance;
        return static_cast<uint32_t>(config_.baseHashesPerSecond * multiplier);
    }
};

/**
 * @brief Test harness for mining components
 */
class MiningTestHarness {
public:
    struct TestConfig {
        std::chrono::milliseconds testDuration{1000};
        uint32_t expectedMinHashrate{500};
        uint32_t expectedMaxHashrate{1500};
        bool shouldFindSolution{false};
    };
    
    static bool runHashrateTest(const TestConfig& config) {
        FakeHasher::Config hasherConfig;
        hasherConfig.baseHashesPerSecond = 1000;
        hasherConfig.shouldFindSolution = config.shouldFindSolution;
        hasherConfig.solutionAfterHashes = 5000;
        
        FakeHasher hasher(hasherConfig);
        std::atomic<bool> stopToken{false};
        
        auto start = std::chrono::steady_clock::now();
        uint64_t hashes = hasher.hashForDuration(config.testDuration, stopToken);
        auto end = std::chrono::steady_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        double hashrate = (hashes * 1000.0) / duration.count();
        
        // Check if hashrate is within expected range
        return hashrate >= config.expectedMinHashrate && 
               hashrate <= config.expectedMaxHashrate;
    }
    
    static bool runSolutionTest() {
        FakeHasher::Config hasherConfig;
        hasherConfig.shouldFindSolution = true;
        hasherConfig.solutionAfterHashes = 1000;
        
        FakeHasher hasher(hasherConfig);
        std::atomic<bool> stopToken{false};
        
        // Run until solution is found or timeout
        auto start = std::chrono::steady_clock::now();
        while (!hasher.hasSolution() && 
               std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
            hasher.hashForDuration(std::chrono::milliseconds(100), stopToken);
        }
        
        return hasher.hasSolution();
    }
};

} // namespace dinero
