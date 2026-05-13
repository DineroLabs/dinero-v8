#pragma once

/**
 * Phase E.2.d: CPU Budget Monitor
 *
 * PRODUCTION HARDENING: Prevent CPU exhaustion from validation DoS attacks.
 *
 * Philosophy:
 * - The node may refuse validation, but must never exhaust CPU resources
 * - CPU budgets are HARD, enforced via timeouts
 * - Fail early and loudly, not silently
 *
 * What this prevents:
 * - Script validation bombs (complex scripts that take forever)
 * - Block validation DoS (attacker sends huge blocks to slow node)
 * - Signature verification floods (many invalid signatures)
 * - CPU exhaustion (100% CPU → node unresponsive)
 *
 * Note: This complements Phase D consensus limits (MAX_SCRIPT_OPCODES, MAX_BLOCK_SIGOPS_COST).
 * Phase D limits prevent pathological inputs. Phase E.2.d limits prevent CPU exhaustion.
 *
 * SPDX-License-Identifier: MIT
 */

#include <chrono>
#include <cstdint>
#include <string>
#include <atomic>

namespace dinero {
namespace consensus {

/**
 * CPU budget status
 */
enum class CPUBudgetStatus {
    OK = 0,              // CPU usage within safe limits
    WARNING,             // Approaching budget limits (>80% utilization)
    CRITICAL,            // Near budget limits (>95% utilization)
    EXHAUSTED,           // Budget exceeded (100% utilization)
    ERROR                // Monitoring error (cannot determine status)
};

const char* CPUBudgetStatusToString(CPUBudgetStatus status);

/**
 * CPU usage information
 */
struct CPUUsageInfo {
    // Validation timing
    uint64_t total_validation_time_ms{0};    // Total time spent on validation
    uint64_t script_validation_time_ms{0};   // Time spent on script validation
    uint64_t block_validation_time_ms{0};    // Time spent on block validation
    uint64_t signature_validation_time_ms{0}; // Time spent on signature verification

    // Validation counts
    uint64_t scripts_validated{0};
    uint64_t blocks_validated{0};
    uint64_t signatures_verified{0};

    // Timeout tracking
    uint64_t script_timeouts{0};             // Scripts that exceeded timeout
    uint64_t block_timeouts{0};              // Blocks that exceeded timeout

    // CPU load (if available)
    double cpu_load_percent{0.0};            // Current CPU load (0-100%)

    CPUBudgetStatus status{CPUBudgetStatus::ERROR};
    std::string details;                     // Human-readable status details
};

/**
 * CPU budget configuration
 */
struct CPUBudgetConfig {
    // Script validation limits
    uint64_t max_script_validation_ms{100};        // Max time per script (100ms default)
    uint64_t max_block_validation_ms{30000};       // Max time per block (30s default)
    uint64_t max_signature_verification_ms{50};    // Max time per signature (50ms default)

    // Warning thresholds (percentage of budget)
    double warning_threshold_percent{80.0};        // Warn at 80% of budget
    double critical_threshold_percent{95.0};       // Critical at 95% of budget

    // Enable/disable enforcement
    bool enable_script_timeout{true};
    bool enable_block_timeout{true};
    bool enable_signature_timeout{true};

    CPUBudgetConfig() = default;
};

/**
 * Scoped CPU budget tracker
 *
 * RAII wrapper for timing validation operations.
 * Usage:
 *   {
 *       ScopedCPUBudget budget(monitor, CPUBudgetMonitor::Operation::SCRIPT_VALIDATION);
 *       // ... perform validation ...
 *       if (budget.isTimedOut()) {
 *           return false;  // Abort validation
 *       }
 *   }  // Automatically records time on destruction
 */
class ScopedCPUBudget {
public:
    enum class Operation {
        SCRIPT_VALIDATION,
        BLOCK_VALIDATION,
        SIGNATURE_VERIFICATION
    };

    ScopedCPUBudget(class CPUBudgetMonitor* monitor, Operation op);
    ~ScopedCPUBudget();

    /**
     * Check if operation has exceeded timeout
     * @return true if timed out, false otherwise
     */
    bool isTimedOut() const;

    /**
     * Get elapsed time in milliseconds
     * @return Elapsed time since construction
     */
    uint64_t getElapsedMs() const;

    // Prevent copying
    ScopedCPUBudget(const ScopedCPUBudget&) = delete;
    ScopedCPUBudget& operator=(const ScopedCPUBudget&) = delete;

private:
    class CPUBudgetMonitor* monitor_;
    Operation operation_;
    std::chrono::steady_clock::time_point start_time_;
};

/**
 * CPU Budget Monitor
 *
 * Tracks and enforces CPU time budgets for validation operations.
 * Prevents CPU exhaustion attacks via timeouts and budget tracking.
 *
 * Usage:
 *   CPUBudgetMonitor monitor(config);
 *   auto info = monitor.getCPUUsage();
 *
 *   if (info.status == CPUBudgetStatus::EXHAUSTED) {
 *       std::cerr << "CRITICAL: CPU budget exhausted\n";
 *       // Refuse new validation work
 *   }
 *
 *   // Use scoped budget tracker for validation:
 *   {
 *       ScopedCPUBudget budget(&monitor, ScopedCPUBudget::Operation::SCRIPT_VALIDATION);
 *       // ... validate script ...
 *       if (budget.isTimedOut()) {
 *           return false;  // Script took too long
 *       }
 *   }
 */
class CPUBudgetMonitor {
public:
    /**
     * Constructor
     * @param config CPU budget configuration (optional)
     */
    explicit CPUBudgetMonitor(const CPUBudgetConfig& config = CPUBudgetConfig());

    /**
     * Get current CPU usage statistics
     * @return CPUUsageInfo with current stats
     */
    CPUUsageInfo getCPUUsage() const;

    /**
     * Check if safe to perform validation operation
     * @param operation Type of operation to perform
     * @return true if safe to proceed, false if budget exhausted
     */
    bool canValidate(ScopedCPUBudget::Operation operation) const;

    /**
     * Get detailed CPU usage report (for logging/RPC)
     * @return Human-readable report
     */
    std::string getCPUUsageReport() const;

    /**
     * Get configuration
     */
    const CPUBudgetConfig& getConfig() const { return config_; }

    /**
     * Reset statistics (for testing)
     */
    void resetStats();

private:
    friend class ScopedCPUBudget;

    /**
     * Record validation time (called by ScopedCPUBudget destructor)
     */
    void recordValidationTime(ScopedCPUBudget::Operation op, uint64_t elapsed_ms, bool timed_out);

    /**
     * Helper: Determine budget status
     */
    CPUBudgetStatus calculateBudgetStatus() const;

    /**
     * Helper: Get current CPU load (if available)
     * Returns 0.0 if not available
     */
    double getCurrentCPULoad() const;

    // Configuration
    CPUBudgetConfig config_;

    // Statistics (atomic for thread-safety)
    std::atomic<uint64_t> total_validation_time_ms_{0};
    std::atomic<uint64_t> script_validation_time_ms_{0};
    std::atomic<uint64_t> block_validation_time_ms_{0};
    std::atomic<uint64_t> signature_validation_time_ms_{0};

    std::atomic<uint64_t> scripts_validated_{0};
    std::atomic<uint64_t> blocks_validated_{0};
    std::atomic<uint64_t> signatures_verified_{0};

    std::atomic<uint64_t> script_timeouts_{0};
    std::atomic<uint64_t> block_timeouts_{0};
};

} // namespace consensus
} // namespace dinero
