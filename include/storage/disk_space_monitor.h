#pragma once

/**
 * Phase E.2.b: Disk Space Monitor
 *
 * PRODUCTION HARDENING: Prevent disk exhaustion from killing the node.
 *
 * Philosophy:
 * - The node may refuse to write, but must never fill the disk
 * - Disk limits are HARD, not heuristic
 * - Fail early and loudly, not silently
 *
 * What this prevents:
 * - Disk fill → crash (OS runs out of space)
 * - Disk fill → database corruption (incomplete writes)
 * - Disk fill → system-wide failure (other processes starve)
 *
 * SPDX-License-Identifier: MIT
 */

#include "common/status.h"
#include <filesystem>
#include <cstdint>
#include <string>

namespace dinero {
namespace storage {

/**
 * Disk space check result
 */
enum class DiskSpaceStatus {
    OK = 0,              // Sufficient space available
    LOW,                 // Warning level (< 10% free or < 5 GB)
    CRITICAL,            // Critical level (< 5% free or < 1 GB)
    FULL,                // Cannot write (< 1% free or < 100 MB)
    ERROR                // Filesystem error (cannot stat)
};

const char* DiskSpaceStatusToString(DiskSpaceStatus status);

/**
 * Disk space information
 */
struct DiskSpaceInfo {
    uint64_t total_bytes;        // Total disk capacity
    uint64_t available_bytes;    // Bytes available for unprivileged users
    uint64_t free_bytes;         // Total free bytes (including reserved)
    uint64_t used_bytes;         // Bytes used
    double usage_percent;        // Percentage used
    double available_percent;    // Percentage available

    DiskSpaceStatus status;      // Overall status
    std::string path;            // Path that was checked
};

/**
 * Disk usage limits configuration
 */
struct DiskLimitsConfig {
    // Hard limits (node refuses to write if exceeded)
    uint64_t min_free_bytes;         // Minimum free bytes (default: 1 GB)
    double min_free_percent;         // Minimum free percentage (default: 5%)

    // Soft limits (warnings, may trigger pruning)
    uint64_t low_space_threshold_bytes;    // Low space warning (default: 5 GB)
    double low_space_threshold_percent;    // Low space warning (default: 10%)

    // Block storage limits
    uint64_t max_block_storage_bytes;      // Max block data size (default: unlimited)
    bool enable_auto_prune;                // Auto-prune when low on space (default: false)
    uint64_t target_prune_bytes;           // Target size after pruning (default: 50% of max)

    // UTXO cache limits
    uint64_t max_utxo_cache_bytes;         // Max UTXO cache size (default: 1 GB)

    // Log rotation limits
    uint64_t max_log_size_bytes;           // Max single log file size (default: 100 MB)
    uint32_t max_log_files;                // Max number of rotated logs (default: 10)

    DiskLimitsConfig()
        : min_free_bytes(1024ULL * 1024 * 1024)  // 1 GB
        , min_free_percent(5.0)
        , low_space_threshold_bytes(5ULL * 1024 * 1024 * 1024)  // 5 GB
        , low_space_threshold_percent(10.0)
        , max_block_storage_bytes(0)  // 0 = unlimited
        , enable_auto_prune(false)
        , target_prune_bytes(0)
        , max_utxo_cache_bytes(1024ULL * 1024 * 1024)  // 1 GB
        , max_log_size_bytes(100ULL * 1024 * 1024)  // 100 MB
        , max_log_files(10)
    {}
};

/**
 * Disk Space Monitor
 *
 * Provides disk space checking and limit enforcement for production hardening.
 *
 * Usage:
 *   DiskSpaceMonitor monitor(datadir, config);
 *   auto info = monitor.checkDiskSpace();
 *
 *   if (info.status == DiskSpaceStatus::FULL) {
 *       std::cerr << "CRITICAL: Disk full, cannot write\n";
 *       return false;
 *   }
 */
class DiskSpaceMonitor {
public:
    explicit DiskSpaceMonitor(
        const std::filesystem::path& datadir,
        const DiskLimitsConfig& config = DiskLimitsConfig()
    );

    /**
     * Check available disk space
     *
     * Uses platform-specific syscalls:
     * - POSIX: statvfs()
     * - Windows: GetDiskFreeSpaceEx()
     *
     * @return  Disk space information
     */
    DiskSpaceInfo checkDiskSpace() const;

    /**
     * Check if safe to write N bytes
     *
     * Returns false if writing would violate disk limits.
     *
     * @param bytes  Number of bytes to write
     * @return       true if safe to write, false otherwise
     */
    bool canWrite(uint64_t bytes) const;

    /**
     * Get current block storage size
     *
     * Scans blocks/ directory and sums file sizes.
     *
     * @return  Total block storage bytes
     */
    uint64_t getBlockStorageSize() const;

    /**
     * Check if block storage exceeds limit
     *
     * Returns true if max_block_storage_bytes is configured and exceeded.
     *
     * @return  true if limit exceeded, false otherwise
     */
    bool blockStorageExceedsLimit() const;

    /**
     * Get detailed disk usage report (for logging/RPC)
     *
     * Returns multi-line report with:
     * - Total/used/available disk space
     * - Block storage size
     * - UTXO cache size
     * - Log file sizes
     *
     * @return  Human-readable report
     */
    std::string getDiskUsageReport() const;

private:
    std::filesystem::path datadir_;
    DiskLimitsConfig config_;

    // Helper: Get filesystem stats
    DiskSpaceInfo getFilesystemInfo(const std::filesystem::path& path) const;
};

} // namespace storage
} // namespace dinero
