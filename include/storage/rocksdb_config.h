#pragma once

#include <cstdint>
#include <string>
#include <optional>
#include <thread>
#include <sys/sysctl.h> // For macOS memory detection

namespace dinero {
namespace storage {

/**
 * Phase 6A: Advanced RocksDB Configuration
 *
 * Optimized for high-throughput blockchain workloads with:
 * - Dynamic memory allocation based on system resources
 * - Tiered compression strategies
 * - Level-0 flush heuristics for write bursts
 * - Production-ready defaults with override capabilities
 */
struct RocksDBConfig {
    // ========== Memory & Cache ==========

    /// Block cache size for hot data (default: auto-detect 25% of system RAM, max 2GB)
    std::optional<size_t> block_cache_size_bytes;

    /// Write buffer size per memtable (default: 128MB for mainnet, 64MB for regtest)
    std::optional<size_t> write_buffer_size_bytes;

    /// Number of write buffers (default: 4, allows pipelining)
    std::optional<int> max_write_buffer_number;

    /// Min write buffers to merge before flush (default: 2)
    std::optional<int> min_write_buffer_number_to_merge;

    // ========== File Management ==========

    /// Max open files (-1 = unlimited, 0 = auto-detect based on ulimit)
    std::optional<int> max_open_files;

    /// Target SST file size (default: 128MB)
    std::optional<size_t> target_file_size_base_bytes;

    /// SST file size multiplier per level (default: 1, keep uniform)
    std::optional<int> target_file_size_multiplier;

    /// Max bytes for L1 (default: 512MB)
    std::optional<size_t> max_bytes_for_level_base;

    // ========== Compression Strategy ==========

    /// Compression algorithm (none, snappy, lz4, zstd)
    std::string compression_type = "lz4";

    /// Per-level compression (L0-L1: none, L2+: lz4, L5+: zstd)
    bool use_tiered_compression = true;

    /// Compression level for zstd (default: 3, balance of speed/ratio)
    std::optional<int> zstd_compression_level;

    // ========== Level-0 Flush & Compaction ==========

    /// L0 file trigger for slowdown (default: 20)
    std::optional<int> level0_slowdown_writes_trigger;

    /// L0 file trigger for full stop (default: 36)
    std::optional<int> level0_stop_writes_trigger;

    /// L0 compaction trigger (default: 4)
    std::optional<int> level0_file_num_compaction_trigger;

    /// Enable dynamic level size adjustment
    bool level_compaction_dynamic_level_bytes = true;

    // ========== Parallelism ==========

    /// Background flush threads (default: 2)
    std::optional<int> max_background_flushes;

    /// Background compaction threads (default: CPU cores / 2, max 8)
    std::optional<int> max_background_compactions;

    /// Total background jobs (default: auto = flushes + compactions)
    std::optional<int> max_background_jobs;

    /// Compaction readahead size for sequential I/O (default: 2MB)
    std::optional<size_t> compaction_readahead_size_bytes;

    // ========== I/O Optimization ==========

    /// Use Direct I/O for reads (default: true on Linux, false on macOS)
    std::optional<bool> use_direct_reads;

    /// Use Direct I/O for writes (default: false, can hurt latency)
    std::optional<bool> use_direct_io_for_flush_and_compaction;

    /// Bytes to sync for data writes (default: 1MB)
    std::optional<size_t> bytes_per_sync;

    /// Bytes to sync for WAL writes (default: 1MB)
    std::optional<size_t> wal_bytes_per_sync;

    // ========== Block Table Options ==========

    /// Block size in block cache (default: 16KB for blockchain)
    std::optional<size_t> block_size_bytes;

    /// Cache index and filter blocks (default: true)
    bool cache_index_and_filter_blocks = true;

    /// Pin L0 index/filter blocks in cache (default: true for fast tip access)
    bool pin_l0_filter_and_index_blocks_in_cache = true;

    /// Bloom filter bits per key (default: 10, ~1% false positive rate)
    std::optional<int> bloom_filter_bits_per_key;

    /// Use block-based bloom filter (default: false, use full filter)
    bool block_based_bloom_filter = false;

    // ========== Statistics & Monitoring ==========

    /// Enable statistics collection (default: true)
    bool enable_statistics = true;

    /// Statistics dump period in seconds (default: 600 = 10 min)
    std::optional<uint32_t> stats_dump_period_sec;

    // ========== Workload Hints ==========

    enum class WorkloadType {
        INITIAL_BLOCK_DOWNLOAD,  // High write, sequential
        NORMAL_OPERATION,        // Balanced read/write
        UTXO_HEAVY,             // Heavy UTXO lookups
        MEMORY_CONSTRAINED      // Low-memory environment
    };

    WorkloadType workload_hint = WorkloadType::NORMAL_OPERATION;

    // ========== Factory Methods ==========

    /// Auto-detect optimal settings based on system resources
    static RocksDBConfig autoDetect(WorkloadType workload = WorkloadType::NORMAL_OPERATION);

    /// Development settings (smaller cache, more logging)
    static RocksDBConfig forDevelopment();

    /// Production mainnet settings (optimized for throughput)
    static RocksDBConfig forProduction();

    /// Regtest/testnet settings (smaller files, faster compaction)
    static RocksDBConfig forRegtest();

    /// Memory-constrained settings (embedded devices, <2GB RAM)
    static RocksDBConfig forLowMemory();

    /// IBD-optimized settings (high write throughput, minimal read cache)
    static RocksDBConfig forInitialBlockDownload();

    // ========== Utility Methods ==========

    /// Get effective value with fallback to auto-detected default
    size_t getBlockCacheSize() const;
    size_t getWriteBufferSize() const;
    int getMaxOpenFiles() const;
    int getMaxBackgroundJobs() const;
    size_t getTargetFileSizeBase() const;

    /// Validate configuration
    bool validate(std::string* error = nullptr) const;

    /// Get human-readable summary
    std::string toString() const;

    /// Detect system resources
    static size_t detectSystemMemory();
    static int detectCpuCores();
    static int detectOpenFileLimit();

private:
    // Platform-specific detection helpers
    static size_t detectSystemMemoryMacOS();
    static size_t detectSystemMemoryLinux();
};

// ========== Implementation of Inline Methods ==========

inline size_t RocksDBConfig::detectSystemMemory() {
#ifdef __APPLE__
    return detectSystemMemoryMacOS();
#elif defined(__linux__)
    return detectSystemMemoryLinux();
#else
    return 4ULL << 30; // Default 4GB fallback
#endif
}

inline size_t RocksDBConfig::detectSystemMemoryMacOS() {
#ifdef __APPLE__
    int64_t memsize;
    size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0) {
        return static_cast<size_t>(memsize);
    }
#endif
    return 4ULL << 30; // 4GB fallback
}

inline size_t RocksDBConfig::detectSystemMemoryLinux() {
#ifdef __linux__
    // Read /proc/meminfo
    FILE* f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            unsigned long kb;
            if (sscanf(line, "MemTotal: %lu kB", &kb) == 1) {
                fclose(f);
                return kb * 1024ULL;
            }
        }
        fclose(f);
    }
#endif
    return 4ULL << 30; // 4GB fallback
}

inline int RocksDBConfig::detectCpuCores() {
    unsigned int cores = std::thread::hardware_concurrency();
    return cores > 0 ? static_cast<int>(cores) : 4; // Default to 4 if detection fails
}

inline int RocksDBConfig::detectOpenFileLimit() {
#ifdef __unix__
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        // Use 75% of hard limit to leave headroom
        return static_cast<int>(limit.rlim_cur * 0.75);
    }
#endif
    return 1024; // Conservative default
}

inline size_t RocksDBConfig::getBlockCacheSize() const {
    if (block_cache_size_bytes.has_value()) {
        return *block_cache_size_bytes;
    }

    // Auto-detect: 25% of system RAM, capped at 2GB
    size_t system_ram = detectSystemMemory();
    size_t cache = system_ram / 4;
    size_t max_cache = 2ULL << 30; // 2GB max
    return std::min(cache, max_cache);
}

inline size_t RocksDBConfig::getWriteBufferSize() const {
    if (write_buffer_size_bytes.has_value()) {
        return *write_buffer_size_bytes;
    }

    // Default: 128MB for mainnet, scaled down for low memory
    if (workload_hint == WorkloadType::MEMORY_CONSTRAINED) {
        return 32 << 20; // 32MB
    } else if (workload_hint == WorkloadType::INITIAL_BLOCK_DOWNLOAD) {
        return 256 << 20; // 256MB for IBD
    }
    return 128 << 20; // 128MB default
}

inline int RocksDBConfig::getMaxOpenFiles() const {
    if (max_open_files.has_value()) {
        return *max_open_files;
    }

    // Auto-detect based on ulimit
    return detectOpenFileLimit();
}

inline int RocksDBConfig::getMaxBackgroundJobs() const {
    if (max_background_jobs.has_value()) {
        return *max_background_jobs;
    }

    int cores = detectCpuCores();

    // IBD: Use more background threads for compaction
    if (workload_hint == WorkloadType::INITIAL_BLOCK_DOWNLOAD) {
        return std::max(4, cores); // Min 4, up to all cores
    }

    // Normal: Use half of cores, max 8
    return std::min(8, std::max(2, cores / 2));
}

inline size_t RocksDBConfig::getTargetFileSizeBase() const {
    if (target_file_size_base_bytes.has_value()) {
        return *target_file_size_base_bytes;
    }

    // Default: 128MB
    return 128 << 20;
}

} // namespace storage
} // namespace dinero
