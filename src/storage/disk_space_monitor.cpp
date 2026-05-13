#include "storage/disk_space_monitor.h"
#include <iostream>
#include <iomanip>
#include <sstream>

// Platform-specific includes
#ifdef _WIN32
    #include <windows.h>
    #undef ERROR  // windows.h defines ERROR as 0, conflicts with DiskSpaceStatus::ERROR
#else
    #include <sys/statvfs.h>
#endif

namespace dinero {
namespace storage {

//==============================================================================
// Utility Functions
//==============================================================================

const char* DiskSpaceStatusToString(DiskSpaceStatus status) {
    switch (status) {
        case DiskSpaceStatus::OK:       return "OK";
        case DiskSpaceStatus::LOW:      return "LOW";
        case DiskSpaceStatus::CRITICAL: return "CRITICAL";
        case DiskSpaceStatus::FULL:     return "FULL";
        case DiskSpaceStatus::ERROR:    return "ERROR";
    }
    return "UNKNOWN";
}

//==============================================================================
// DiskSpaceMonitor Implementation
//==============================================================================

DiskSpaceMonitor::DiskSpaceMonitor(
    const std::filesystem::path& datadir,
    const DiskLimitsConfig& config
)
    : datadir_(datadir)
    , config_(config)
{
}

DiskSpaceInfo DiskSpaceMonitor::getFilesystemInfo(const std::filesystem::path& path) const {
    DiskSpaceInfo info;
    info.path = path.string();
    info.status = DiskSpaceStatus::ERROR;

#ifdef _WIN32
    // Windows: Use GetDiskFreeSpaceEx
    ULARGE_INTEGER free_bytes_available;
    ULARGE_INTEGER total_bytes;
    ULARGE_INTEGER total_free_bytes;

    std::wstring wide_path = path.wstring();
    if (GetDiskFreeSpaceExW(
        wide_path.c_str(),
        &free_bytes_available,
        &total_bytes,
        &total_free_bytes
    )) {
        info.total_bytes = total_bytes.QuadPart;
        info.available_bytes = free_bytes_available.QuadPart;
        info.free_bytes = total_free_bytes.QuadPart;
        info.used_bytes = info.total_bytes - info.free_bytes;
    } else {
        std::cerr << "[DiskSpaceMonitor] ERROR: GetDiskFreeSpaceEx failed for " << path << "\n";
        return info;
    }
#else
    // POSIX: Use statvfs
    struct statvfs stat;
    if (statvfs(path.c_str(), &stat) != 0) {
        std::cerr << "[DiskSpaceMonitor] ERROR: statvfs failed for " << path << "\n";
        return info;
    }

    info.total_bytes = stat.f_blocks * stat.f_frsize;
    info.available_bytes = stat.f_bavail * stat.f_frsize;  // Available to non-root
    info.free_bytes = stat.f_bfree * stat.f_frsize;        // Total free (including reserved)
    info.used_bytes = info.total_bytes - info.free_bytes;
#endif

    // Calculate percentages
    if (info.total_bytes > 0) {
        info.usage_percent = (static_cast<double>(info.used_bytes) / info.total_bytes) * 100.0;
        info.available_percent = (static_cast<double>(info.available_bytes) / info.total_bytes) * 100.0;
    } else {
        info.usage_percent = 0.0;
        info.available_percent = 0.0;
    }

    // Determine status based on config
    if (info.available_bytes < config_.min_free_bytes ||
        info.available_percent < config_.min_free_percent) {
        info.status = DiskSpaceStatus::FULL;
    } else if (info.available_bytes < config_.min_free_bytes * 5 ||
               info.available_percent < config_.min_free_percent * 2) {
        info.status = DiskSpaceStatus::CRITICAL;
    } else if (info.available_bytes < config_.low_space_threshold_bytes ||
               info.available_percent < config_.low_space_threshold_percent) {
        info.status = DiskSpaceStatus::LOW;
    } else {
        info.status = DiskSpaceStatus::OK;
    }

    return info;
}

DiskSpaceInfo DiskSpaceMonitor::checkDiskSpace() const {
    // Check the datadir filesystem
    return getFilesystemInfo(datadir_);
}

bool DiskSpaceMonitor::canWrite(uint64_t bytes) const {
    auto info = checkDiskSpace();

    // Cannot write if:
    // 1. Filesystem check failed
    if (info.status == DiskSpaceStatus::ERROR) {
        return false;
    }

    // 2. Disk is full
    if (info.status == DiskSpaceStatus::FULL) {
        return false;
    }

    // 3. Writing would violate hard limits
    if (info.available_bytes < bytes + config_.min_free_bytes) {
        return false;
    }

    return true;
}

uint64_t DiskSpaceMonitor::getBlockStorageSize() const {
    std::filesystem::path blocks_dir = datadir_ / "blocks";

    if (!std::filesystem::exists(blocks_dir)) {
        return 0;
    }

    uint64_t total_size = 0;

    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(blocks_dir)) {
            if (entry.is_regular_file()) {
                total_size += entry.file_size();
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "[DiskSpaceMonitor] ERROR scanning blocks directory: " << e.what() << "\n";
    }

    return total_size;
}

bool DiskSpaceMonitor::blockStorageExceedsLimit() const {
    // If no limit configured, never exceeds
    if (config_.max_block_storage_bytes == 0) {
        return false;
    }

    uint64_t current_size = getBlockStorageSize();
    return current_size > config_.max_block_storage_bytes;
}

std::string DiskSpaceMonitor::getDiskUsageReport() const {
    auto info = checkDiskSpace();

    std::ostringstream report;
    report << "========================================\n";
    report << "Disk Usage Report\n";
    report << "========================================\n\n";

    // Filesystem info
    report << "Filesystem: " << info.path << "\n";
    report << "Status:     " << DiskSpaceStatusToString(info.status) << "\n";
    report << "\n";

    // Space breakdown
    auto to_gb = [](uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0); };

    report << std::fixed << std::setprecision(2);
    report << "Total:      " << to_gb(info.total_bytes) << " GB\n";
    report << "Used:       " << to_gb(info.used_bytes) << " GB (" << info.usage_percent << "%)\n";
    report << "Available:  " << to_gb(info.available_bytes) << " GB (" << info.available_percent << "%)\n";
    report << "Free:       " << to_gb(info.free_bytes) << " GB (including reserved)\n";
    report << "\n";

    // Configured limits
    report << "Configured Limits:\n";
    report << "  Min free:        " << to_gb(config_.min_free_bytes) << " GB (" << config_.min_free_percent << "%)\n";
    report << "  Low threshold:   " << to_gb(config_.low_space_threshold_bytes) << " GB (" << config_.low_space_threshold_percent << "%)\n";
    if (config_.max_block_storage_bytes > 0) {
        report << "  Max block data:  " << to_gb(config_.max_block_storage_bytes) << " GB\n";
    } else {
        report << "  Max block data:  unlimited\n";
    }
    report << "\n";

    // Block storage usage
    uint64_t block_storage = getBlockStorageSize();
    report << "Block Storage:\n";
    report << "  Current size:    " << to_gb(block_storage) << " GB\n";
    if (config_.max_block_storage_bytes > 0) {
        double block_usage = (static_cast<double>(block_storage) / config_.max_block_storage_bytes) * 100.0;
        report << "  Usage:           " << block_usage << "%\n";
        report << "  Limit exceeded:  " << (blockStorageExceedsLimit() ? "YES" : "NO") << "\n";
    }
    report << "\n";

    // Warnings
    if (info.status == DiskSpaceStatus::FULL) {
        report << "⚠️  WARNING: Disk is FULL - node will refuse writes\n";
    } else if (info.status == DiskSpaceStatus::CRITICAL) {
        report << "⚠️  WARNING: Disk space CRITICAL - consider pruning\n";
    } else if (info.status == DiskSpaceStatus::LOW) {
        report << "⚠️  WARNING: Disk space LOW - monitor closely\n";
    }

    report << "========================================\n";

    return report.str();
}

} // namespace storage
} // namespace dinero
