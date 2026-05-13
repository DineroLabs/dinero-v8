#include "consensus/cpu_budget_monitor.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/processor_info.h>
#include <mach/mach_host.h>
#elif defined(_WIN32)
#include <windows.h>
#include <pdh.h>
#undef ERROR  // windows.h defines ERROR as 0, conflicts with CPUBudgetStatus::ERROR
#else
// Linux
#include <fstream>
#endif

namespace dinero {
namespace consensus {

//==============================================================================
// Utility Functions
//==============================================================================

const char* CPUBudgetStatusToString(CPUBudgetStatus status) {
    switch (status) {
        case CPUBudgetStatus::OK:       return "OK";
        case CPUBudgetStatus::WARNING:  return "WARNING";
        case CPUBudgetStatus::CRITICAL: return "CRITICAL";
        case CPUBudgetStatus::EXHAUSTED: return "EXHAUSTED";
        case CPUBudgetStatus::ERROR:    return "ERROR";
    }
    return "UNKNOWN";
}

//==============================================================================
// ScopedCPUBudget Implementation
//==============================================================================

ScopedCPUBudget::ScopedCPUBudget(CPUBudgetMonitor* monitor, Operation op)
    : monitor_(monitor)
    , operation_(op)
    , start_time_(std::chrono::steady_clock::now())
{
}

ScopedCPUBudget::~ScopedCPUBudget() {
    if (!monitor_) return;

    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time_
    ).count();

    bool timed_out = isTimedOut();
    monitor_->recordValidationTime(operation_, static_cast<uint64_t>(elapsed_ms), timed_out);
}

bool ScopedCPUBudget::isTimedOut() const {
    if (!monitor_) return false;

    uint64_t elapsed_ms = getElapsedMs();
    const auto& config = monitor_->getConfig();

    switch (operation_) {
        case Operation::SCRIPT_VALIDATION:
            return config.enable_script_timeout &&
                   elapsed_ms > config.max_script_validation_ms;

        case Operation::BLOCK_VALIDATION:
            return config.enable_block_timeout &&
                   elapsed_ms > config.max_block_validation_ms;

        case Operation::SIGNATURE_VERIFICATION:
            return config.enable_signature_timeout &&
                   elapsed_ms > config.max_signature_verification_ms;
    }
    return false;
}

uint64_t ScopedCPUBudget::getElapsedMs() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - start_time_
    );
    return static_cast<uint64_t>(elapsed.count());
}

//==============================================================================
// CPUBudgetMonitor Implementation
//==============================================================================

CPUBudgetMonitor::CPUBudgetMonitor(const CPUBudgetConfig& config)
    : config_(config)
{
}

CPUUsageInfo CPUBudgetMonitor::getCPUUsage() const {
    CPUUsageInfo info;

    // Gather statistics
    info.total_validation_time_ms = total_validation_time_ms_.load();
    info.script_validation_time_ms = script_validation_time_ms_.load();
    info.block_validation_time_ms = block_validation_time_ms_.load();
    info.signature_validation_time_ms = signature_validation_time_ms_.load();

    info.scripts_validated = scripts_validated_.load();
    info.blocks_validated = blocks_validated_.load();
    info.signatures_verified = signatures_verified_.load();

    info.script_timeouts = script_timeouts_.load();
    info.block_timeouts = block_timeouts_.load();

    // Get current CPU load (if available)
    info.cpu_load_percent = getCurrentCPULoad();

    // Determine overall status
    info.status = calculateBudgetStatus();

    // Build status details string
    std::ostringstream details;
    details << "Scripts: " << info.scripts_validated
            << " (" << info.script_timeouts << " timeouts)";

    if (info.blocks_validated > 0) {
        details << ", Blocks: " << info.blocks_validated
                << " (" << info.block_timeouts << " timeouts)";
    }

    if (info.status == CPUBudgetStatus::EXHAUSTED) {
        details << " - EXHAUSTED";
    } else if (info.status == CPUBudgetStatus::CRITICAL) {
        details << " - CRITICAL";
    } else if (info.status == CPUBudgetStatus::WARNING) {
        details << " - WARNING";
    }

    info.details = details.str();

    return info;
}

bool CPUBudgetMonitor::canValidate(ScopedCPUBudget::Operation operation) const {
    // Phase E.2.d: For now, always allow validation
    // In the future, could refuse validation if CPU is exhausted
    // (e.g., too many timeouts, CPU load too high)
    (void)operation;  // Unused parameter
    return true;
}

std::string CPUBudgetMonitor::getCPUUsageReport() const {
    auto info = getCPUUsage();

    std::ostringstream report;
    report << "========================================\n";
    report << "CPU Budget Report\n";
    report << "========================================\n\n";

    // Overall status
    report << "Status: " << CPUBudgetStatusToString(info.status) << "\n";
    report << "Details: " << info.details << "\n\n";

    // Validation timing breakdown
    report << "Validation Time:\n";
    report << "  Total:      " << (info.total_validation_time_ms / 1000.0) << " s\n";
    report << "  Scripts:    " << (info.script_validation_time_ms / 1000.0) << " s\n";
    report << "  Blocks:     " << (info.block_validation_time_ms / 1000.0) << " s\n";
    report << "  Signatures: " << (info.signature_validation_time_ms / 1000.0) << " s\n";
    report << "\n";

    // Validation counts
    report << "Validation Counts:\n";
    report << "  Scripts validated:     " << info.scripts_validated << "\n";
    report << "  Blocks validated:      " << info.blocks_validated << "\n";
    report << "  Signatures verified:   " << info.signatures_verified << "\n";
    report << "\n";

    // Timeout tracking
    if (info.script_timeouts > 0 || info.block_timeouts > 0) {
        report << "Timeouts:\n";
        report << "  Script timeouts:  " << info.script_timeouts << "\n";
        report << "  Block timeouts:   " << info.block_timeouts << "\n";
        report << "\n";
    }

    // CPU load (if available)
    if (info.cpu_load_percent > 0.0) {
        report << std::fixed << std::setprecision(1);
        report << "CPU Load: " << info.cpu_load_percent << "%\n";
        report << "\n";
    }

    // Budget limits
    report << "Budget Limits:\n";
    report << "  Max script validation:     " << config_.max_script_validation_ms << " ms\n";
    report << "  Max block validation:      " << config_.max_block_validation_ms << " ms\n";
    report << "  Max signature verification: " << config_.max_signature_verification_ms << " ms\n";
    report << "\n";

    // Warnings
    if (info.status == CPUBudgetStatus::EXHAUSTED) {
        report << "⚠️  WARNING: CPU budget EXHAUSTED - validation may be failing\n";
    } else if (info.status == CPUBudgetStatus::CRITICAL) {
        report << "⚠️  WARNING: CPU budget CRITICAL - many validations timing out\n";
    } else if (info.status == CPUBudgetStatus::WARNING) {
        report << "⚠️  WARNING: CPU budget approaching limits\n";
    }

    report << "========================================\n";

    return report.str();
}

void CPUBudgetMonitor::resetStats() {
    total_validation_time_ms_.store(0);
    script_validation_time_ms_.store(0);
    block_validation_time_ms_.store(0);
    signature_validation_time_ms_.store(0);

    scripts_validated_.store(0);
    blocks_validated_.store(0);
    signatures_verified_.store(0);

    script_timeouts_.store(0);
    block_timeouts_.store(0);
}

//==============================================================================
// Private Helper Methods
//==============================================================================

void CPUBudgetMonitor::recordValidationTime(
    ScopedCPUBudget::Operation op,
    uint64_t elapsed_ms,
    bool timed_out
) {
    // Update total time
    total_validation_time_ms_.fetch_add(elapsed_ms);

    // Update operation-specific metrics
    switch (op) {
        case ScopedCPUBudget::Operation::SCRIPT_VALIDATION:
            script_validation_time_ms_.fetch_add(elapsed_ms);
            scripts_validated_.fetch_add(1);
            if (timed_out) {
                script_timeouts_.fetch_add(1);
            }
            break;

        case ScopedCPUBudget::Operation::BLOCK_VALIDATION:
            block_validation_time_ms_.fetch_add(elapsed_ms);
            blocks_validated_.fetch_add(1);
            if (timed_out) {
                block_timeouts_.fetch_add(1);
            }
            break;

        case ScopedCPUBudget::Operation::SIGNATURE_VERIFICATION:
            signature_validation_time_ms_.fetch_add(elapsed_ms);
            signatures_verified_.fetch_add(1);
            // Note: No separate timeout counter for signatures
            break;
    }
}

CPUBudgetStatus CPUBudgetMonitor::calculateBudgetStatus() const {
    // Calculate timeout rate
    uint64_t total_scripts = scripts_validated_.load();
    uint64_t total_blocks = blocks_validated_.load();
    uint64_t total_script_timeouts = script_timeouts_.load();
    uint64_t total_block_timeouts = block_timeouts_.load();

    // If no validations yet, status is OK
    if (total_scripts == 0 && total_blocks == 0) {
        return CPUBudgetStatus::OK;
    }

    // Calculate timeout percentage
    double script_timeout_rate = 0.0;
    if (total_scripts > 0) {
        script_timeout_rate = (static_cast<double>(total_script_timeouts) / total_scripts) * 100.0;
    }

    double block_timeout_rate = 0.0;
    if (total_blocks > 0) {
        block_timeout_rate = (static_cast<double>(total_block_timeouts) / total_blocks) * 100.0;
    }

    // Determine status based on timeout rates
    // EXHAUSTED: >20% of validations timing out
    // CRITICAL: >10% timing out
    // WARNING: >5% timing out
    double max_timeout_rate = std::max(script_timeout_rate, block_timeout_rate);

    if (max_timeout_rate >= 20.0) {
        return CPUBudgetStatus::EXHAUSTED;
    } else if (max_timeout_rate >= 10.0) {
        return CPUBudgetStatus::CRITICAL;
    } else if (max_timeout_rate >= 5.0) {
        return CPUBudgetStatus::WARNING;
    }

    return CPUBudgetStatus::OK;
}

double CPUBudgetMonitor::getCurrentCPULoad() const {
    // Platform-specific CPU load detection
    // Returns 0.0 if not available

#ifdef __APPLE__
    // macOS: Use Mach APIs
    host_cpu_load_info_data_t cpu_info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;

    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                       (host_info_t)&cpu_info, &count) == KERN_SUCCESS) {
        unsigned long long total_ticks = 0;
        for (int i = 0; i < CPU_STATE_MAX; i++) {
            total_ticks += cpu_info.cpu_ticks[i];
        }

        unsigned long long idle_ticks = cpu_info.cpu_ticks[CPU_STATE_IDLE];

        if (total_ticks > 0) {
            double cpu_usage = 100.0 * (1.0 - static_cast<double>(idle_ticks) / total_ticks);
            return cpu_usage;
        }
    }
    return 0.0;

#elif defined(_WIN32)
    // Windows: Would need PDH library
    // For now, return 0.0 (not implemented)
    return 0.0;

#else
    // Linux: Read /proc/stat
    std::ifstream stat_file("/proc/stat");
    if (stat_file.is_open()) {
        std::string cpu_label;
        unsigned long long user, nice, system, idle;

        stat_file >> cpu_label >> user >> nice >> system >> idle;

        unsigned long long total = user + nice + system + idle;
        if (total > 0) {
            double cpu_usage = 100.0 * (1.0 - static_cast<double>(idle) / total);
            return cpu_usage;
        }
    }
    return 0.0;
#endif
}

} // namespace consensus
} // namespace dinero
