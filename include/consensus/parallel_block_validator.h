#pragma once

/**
 * Phase 6B: Parallel Block Validator
 *
 * ParallelBlockValidator - Thread-safe wrapper around BlockValidator
 *
 * Integrates:
 * - ValidationWorkerPool for parallel script verification
 * - ChainstateGuard for safe UTXO access
 * - Metrics for validation performance tracking
 *
 * Usage:
 *   ParallelBlockValidator validator(utxo_set, &g_chainstate_guard);
 *
 *   // Validate block (read-only, parallelized)
 *   bool valid = validator.validateBlock(block, error);
 *
 * This wrapper is deliberately read-only. Chain connection and undo
 * persistence belong to the canonical ConnectTip/ValidationQueue path, which
 * writes the canonical UndoRecord flat-file format. Do not add a mutating
 * connection API here without an end-to-end persistence contract and test.
 */

#include "consensus/validation_worker_pool.h"
#include "consensus/chainstate_guard.h"
#include "consensus/interfaces/iconsensus_utxo_set.h"  // Phase 2: Direct IConsensusUTXOSet
#include "primitives/block.h"
#include <memory>

namespace dinero {
namespace consensus {

class ParallelBlockValidator {
public:
    struct Config {
        bool enable_parallel = true;         // Use worker pool
        size_t parallel_threshold = 10;      // Min tx count for parallelization
        bool enable_script_cache = true;     // Cache script verification results
        size_t worker_threads = 0;           // 0 = auto

        static Config forIBD();
        static Config forNormalOperation();
        static Config forLowResource();
    };

    struct Metrics {
        std::atomic<uint64_t> blocks_validated{0};
        std::atomic<uint64_t> parallel_validations{0};
        std::atomic<uint64_t> serial_validations{0};

        std::atomic<uint64_t> total_validation_time_us{0};
        void reset();
        std::string toString() const;
    };

    // Phase 2: Takes IConsensusUTXOSet* directly (no adapter chain)
    explicit ParallelBlockValidator(
        IConsensusUTXOSet* consensus_utxo_set,
        ChainstateGuard* chainstate_guard,
        const Config& config = Config::forNormalOperation()
    );

    ~ParallelBlockValidator();

    /**
     * Validate block (read-only, parallelized if >= threshold tx)
     * Acquires: read lock on chainstate
     */
    bool validateBlock(const Block& block, uint64_t height, std::string& error);

    /**
     * Validate scripts only (read-only, for two-phase validation)
     * Does NOT check UTXO existence (caller must do that separately)
     * Parallelizes signature verification and script execution
     * Acquires: NO locks (stateless validation)
     *
     * Use case: BlockAcceptor can call this before RocksDB writes
     */
    bool validateScriptsOnly(const Block& block, std::string& error);

    // Metrics
    const Metrics& getMetrics() const { return metrics_; }
    void resetMetrics() { metrics_.reset(); }

    // Configuration
    const Config& getConfig() const { return config_; }

private:
    // Parallel validation helpers
    bool validateBlockParallel(const Block& block, uint64_t height, std::string& error);
    bool validateBlockSerial(const Block& block, uint64_t height, std::string& error);

    // Script-only validation helpers (two-phase)
    bool validateScriptsOnlyParallel(const Block& block, std::string& error);
    bool validateScriptsOnlySerial(const Block& block, std::string& error);

    Config config_;
    Metrics metrics_;

    // Phase 2: Direct IConsensusUTXOSet (owns forest, no adapter needed)
    IConsensusUTXOSet* consensus_utxo_set_;
    ChainstateGuard* chainstate_guard_;
    // Worker pool (lazy-initialized)
    std::unique_ptr<ValidationWorkerPool> worker_pool_;
};

} // namespace consensus
} // namespace dinero
