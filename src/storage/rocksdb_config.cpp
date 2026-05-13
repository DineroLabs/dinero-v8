#include "storage/rocksdb_config.h"
#include <sstream>
#include <algorithm>
#include <sys/resource.h> // For getrlimit

namespace dinero {
namespace storage {

// ========== Factory Methods ==========

RocksDBConfig RocksDBConfig::autoDetect(WorkloadType workload) {
    RocksDBConfig config;
    config.workload_hint = workload;

    // All values will use getters with auto-detection
    // No need to set optional values here - they'll auto-detect on first access

    return config;
}

RocksDBConfig RocksDBConfig::forDevelopment() {
    RocksDBConfig config;
    config.workload_hint = WorkloadType::NORMAL_OPERATION;

    // Smaller cache for development
    config.block_cache_size_bytes = 128 << 20; // 128MB

    // Smaller write buffers
    config.write_buffer_size_bytes = 32 << 20; // 32MB
    config.max_write_buffer_number = 2;

    // Faster compaction for quick iteration
    config.level0_file_num_compaction_trigger = 2;
    config.target_file_size_base_bytes = 32 << 20; // 32MB files

    // More logging
    config.enable_statistics = true;
    config.stats_dump_period_sec = 300; // 5 minutes

    // No compression for faster writes during dev
    config.compression_type = "none";
    config.use_tiered_compression = false;

    // Fewer background threads
    config.max_background_jobs = 2;

    return config;
}

RocksDBConfig RocksDBConfig::forProduction() {
    RocksDBConfig config;
    config.workload_hint = WorkloadType::NORMAL_OPERATION;

    // Large cache for hot data
    size_t system_ram = detectSystemMemory();
    config.block_cache_size_bytes = std::min(2ULL << 30, system_ram / 4); // 2GB or 25% RAM

    // Large write buffers for batching
    config.write_buffer_size_bytes = 128 << 20; // 128MB
    config.max_write_buffer_number = 4;
    config.min_write_buffer_number_to_merge = 2;

    // Large SST files for efficiency
    config.target_file_size_base_bytes = 128 << 20; // 128MB
    config.max_bytes_for_level_base = 512 << 20;    // 512MB for L1

    // Tiered compression for space savings
    config.compression_type = "lz4";
    config.use_tiered_compression = true;
    config.zstd_compression_level = 3;

    // Aggressive Level-0 management
    config.level0_file_num_compaction_trigger = 4;
    config.level0_slowdown_writes_trigger = 20;
    config.level0_stop_writes_trigger = 36;

    // Use all available cores
    int cores = detectCpuCores();
    config.max_background_flushes = 2;
    config.max_background_compactions = std::min(8, std::max(2, cores / 2));
    config.max_background_jobs = std::min(10, std::max(4, cores));

    // I/O optimizations
    config.compaction_readahead_size_bytes = 2 << 20; // 2MB
    config.bytes_per_sync = 1 << 20;                  // 1MB
    config.wal_bytes_per_sync = 1 << 20;              // 1MB

    // Direct I/O (platform dependent)
#ifdef __linux__
    config.use_direct_reads = true;
#else
    config.use_direct_reads = false; // macOS has issues with direct I/O
#endif
    config.use_direct_io_for_flush_and_compaction = false; // Can hurt latency

    // Block table optimizations
    config.block_size_bytes = 16 << 10; // 16KB
    config.cache_index_and_filter_blocks = true;
    config.pin_l0_filter_and_index_blocks_in_cache = true;
    config.bloom_filter_bits_per_key = 10;

    // Statistics
    config.enable_statistics = true;
    config.stats_dump_period_sec = 600; // 10 minutes

    // Dynamic level sizing
    config.level_compaction_dynamic_level_bytes = true;

    // File limits
    config.max_open_files = 0; // Auto-detect

    return config;
}

RocksDBConfig RocksDBConfig::forRegtest() {
    RocksDBConfig config;
    config.workload_hint = WorkloadType::NORMAL_OPERATION;

    // Small cache for regtest
    config.block_cache_size_bytes = 64 << 20; // 64MB

    // Small write buffers
    config.write_buffer_size_bytes = 16 << 20; // 16MB
    config.max_write_buffer_number = 2;

    // Small SST files for quick cleanup
    config.target_file_size_base_bytes = 16 << 20; // 16MB

    // No compression for fast writes
    config.compression_type = "none";
    config.use_tiered_compression = false;

    // Fast compaction
    config.level0_file_num_compaction_trigger = 2;
    config.level0_slowdown_writes_trigger = 8;
    config.level0_stop_writes_trigger = 12;

    // Minimal background threads
    config.max_background_jobs = 2;

    // Statistics for debugging
    config.enable_statistics = true;
    config.stats_dump_period_sec = 60; // 1 minute

    // Fewer open files
    config.max_open_files = 256;

    return config;
}

RocksDBConfig RocksDBConfig::forLowMemory() {
    RocksDBConfig config;
    config.workload_hint = WorkloadType::MEMORY_CONSTRAINED;

    // Minimal cache
    config.block_cache_size_bytes = 32 << 20; // 32MB

    // Small write buffers
    config.write_buffer_size_bytes = 16 << 20; // 16MB
    config.max_write_buffer_number = 2;
    config.min_write_buffer_number_to_merge = 1;

    // Small SST files
    config.target_file_size_base_bytes = 32 << 20; // 32MB
    config.max_bytes_for_level_base = 128 << 20;   // 128MB for L1

    // Aggressive compression to save space
    config.compression_type = "lz4";
    config.use_tiered_compression = true;

    // Trigger compaction early to avoid L0 buildup
    config.level0_file_num_compaction_trigger = 2;
    config.level0_slowdown_writes_trigger = 8;
    config.level0_stop_writes_trigger = 12;

    // Minimal background threads
    config.max_background_flushes = 1;
    config.max_background_compactions = 1;
    config.max_background_jobs = 2;

    // No direct I/O (can require more memory)
    config.use_direct_reads = false;
    config.use_direct_io_for_flush_and_compaction = false;

    // Smaller block size
    config.block_size_bytes = 8 << 10; // 8KB

    // Don't cache index/filter aggressively
    config.cache_index_and_filter_blocks = false;
    config.pin_l0_filter_and_index_blocks_in_cache = false;

    // Smaller bloom filter
    config.bloom_filter_bits_per_key = 8;

    // Disable statistics to save memory
    config.enable_statistics = false;

    // Fewer open files
    config.max_open_files = 128;

    return config;
}

RocksDBConfig RocksDBConfig::forInitialBlockDownload() {
    RocksDBConfig config;
    config.workload_hint = WorkloadType::INITIAL_BLOCK_DOWNLOAD;

    // Large write buffers for high throughput
    config.write_buffer_size_bytes = 256 << 20; // 256MB
    config.max_write_buffer_number = 6; // More buffers for pipelining
    config.min_write_buffer_number_to_merge = 2;

    // Moderate block cache (writes dominate)
    config.block_cache_size_bytes = 512 << 20; // 512MB

    // Large SST files
    config.target_file_size_base_bytes = 256 << 20; // 256MB
    config.max_bytes_for_level_base = 1024 << 20;   // 1GB for L1

    // Light compression for write speed
    config.compression_type = "lz4";
    config.use_tiered_compression = true;

    // Delay compaction to favor write throughput
    config.level0_file_num_compaction_trigger = 8;
    config.level0_slowdown_writes_trigger = 30;
    config.level0_stop_writes_trigger = 48;

    // Max background threads for compaction
    int cores = detectCpuCores();
    config.max_background_flushes = 2;
    config.max_background_compactions = std::max(4, cores - 2); // Leave 2 cores for validation
    config.max_background_jobs = std::max(6, cores);

    // Large readahead for sequential writes
    config.compaction_readahead_size_bytes = 4 << 20; // 4MB

    // Aggressive syncing to avoid WAL buildup
    config.bytes_per_sync = 2 << 20;     // 2MB
    config.wal_bytes_per_sync = 2 << 20; // 2MB

    // Direct I/O on Linux
#ifdef __linux__
    config.use_direct_reads = true;
    config.use_direct_io_for_flush_and_compaction = true;
#else
    config.use_direct_reads = false;
    config.use_direct_io_for_flush_and_compaction = false;
#endif

    // Larger block size for sequential access
    config.block_size_bytes = 32 << 10; // 32KB

    // Cache filters for faster validation
    config.cache_index_and_filter_blocks = true;
    config.pin_l0_filter_and_index_blocks_in_cache = true;
    config.bloom_filter_bits_per_key = 10;

    // Statistics
    config.enable_statistics = true;
    config.stats_dump_period_sec = 600;

    // Dynamic level sizing
    config.level_compaction_dynamic_level_bytes = true;

    // More open files for parallel I/O
    config.max_open_files = 0; // Auto-detect

    return config;
}

// ========== Validation ==========

bool RocksDBConfig::validate(std::string* error) const {
    // Validate memory settings
    if (block_cache_size_bytes.has_value() && *block_cache_size_bytes < (1 << 20)) {
        if (error) *error = "block_cache_size_bytes must be at least 1MB";
        return false;
    }

    if (write_buffer_size_bytes.has_value() && *write_buffer_size_bytes < (1 << 20)) {
        if (error) *error = "write_buffer_size_bytes must be at least 1MB";
        return false;
    }

    // Validate parallelism
    if (max_background_jobs.has_value() && *max_background_jobs < 1) {
        if (error) *error = "max_background_jobs must be at least 1";
        return false;
    }

    // Validate Level-0 triggers
    if (level0_file_num_compaction_trigger.has_value() &&
        level0_slowdown_writes_trigger.has_value() &&
        *level0_file_num_compaction_trigger >= *level0_slowdown_writes_trigger) {
        if (error) *error = "level0_file_num_compaction_trigger must be < level0_slowdown_writes_trigger";
        return false;
    }

    if (level0_slowdown_writes_trigger.has_value() &&
        level0_stop_writes_trigger.has_value() &&
        *level0_slowdown_writes_trigger >= *level0_stop_writes_trigger) {
        if (error) *error = "level0_slowdown_writes_trigger must be < level0_stop_writes_trigger";
        return false;
    }

    // Validate compression
    if (compression_type != "none" && compression_type != "snappy" &&
        compression_type != "lz4" && compression_type != "zstd") {
        if (error) *error = "compression_type must be one of: none, snappy, lz4, zstd";
        return false;
    }

    return true;
}

// ========== String Representation ==========

std::string RocksDBConfig::toString() const {
    std::ostringstream oss;

    oss << "RocksDBConfig {\n";
    oss << "  Workload: ";
    switch (workload_hint) {
        case WorkloadType::INITIAL_BLOCK_DOWNLOAD: oss << "IBD\n"; break;
        case WorkloadType::NORMAL_OPERATION: oss << "Normal\n"; break;
        case WorkloadType::UTXO_HEAVY: oss << "UTXO-Heavy\n"; break;
        case WorkloadType::MEMORY_CONSTRAINED: oss << "Low-Memory\n"; break;
    }

    oss << "  Memory:\n";
    oss << "    Block Cache: " << (getBlockCacheSize() >> 20) << " MB\n";
    oss << "    Write Buffer: " << (getWriteBufferSize() >> 20) << " MB\n";
    oss << "    Write Buffers: " << max_write_buffer_number.value_or(4) << "\n";

    oss << "  Files:\n";
    oss << "    Max Open: " << getMaxOpenFiles() << "\n";
    oss << "    Target SST Size: " << (getTargetFileSizeBase() >> 20) << " MB\n";

    oss << "  Compression:\n";
    oss << "    Type: " << compression_type << "\n";
    oss << "    Tiered: " << (use_tiered_compression ? "Yes" : "No") << "\n";

    oss << "  Level-0:\n";
    oss << "    Compaction Trigger: " << level0_file_num_compaction_trigger.value_or(4) << "\n";
    oss << "    Slowdown Trigger: " << level0_slowdown_writes_trigger.value_or(20) << "\n";
    oss << "    Stop Trigger: " << level0_stop_writes_trigger.value_or(36) << "\n";

    oss << "  Parallelism:\n";
    oss << "    Background Jobs: " << getMaxBackgroundJobs() << "\n";
    oss << "    Flushes: " << max_background_flushes.value_or(2) << "\n";
    oss << "    Compactions: " << max_background_compactions.value_or(getMaxBackgroundJobs() - 2) << "\n";

    oss << "  I/O:\n";
    oss << "    Direct Reads: " << (use_direct_reads.value_or(false) ? "Yes" : "No") << "\n";
    oss << "    Sync Interval: " << (bytes_per_sync.value_or(1 << 20) >> 20) << " MB\n";

    oss << "  Statistics: " << (enable_statistics ? "Enabled" : "Disabled") << "\n";

    oss << "}";

    return oss.str();
}

} // namespace storage
} // namespace dinero
