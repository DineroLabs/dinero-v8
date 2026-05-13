#pragma once

#include <string>
#include <optional>

namespace dinero {
namespace storage {

/**
 * Storage configuration with explicit fallback control
 */
struct StorageConfig {
    std::string backend = "rocksdb";           // Preferred backend
    std::string data_dir = "data/chain";       // Storage directory
    bool allow_fallback = false;               // Explicit fallback control

    // Backend-specific options
    struct RocksDBOptions {
        size_t block_cache_size = 512 << 20;   // 512 MiB
        size_t write_buffer_size = 64 << 20;   // 64 MiB
        int max_background_jobs = 0;           // 0 = auto-detect
        bool use_direct_reads = true;
        bool enable_compression = false;       // Start without compression
        bool enable_statistics = true;
    } rocksdb;
    
    // Validation
    bool validate() const {
        if (backend.empty() || data_dir.empty()) {
            return false;
        }
        return true;
    }
    
    // String representation
    std::string toString() const;
    
    // Factory method for different environments
    static StorageConfig forDevelopment() {
        StorageConfig config;
        config.backend = "rocksdb";
        config.allow_fallback = true;  // Allow fallback in dev
        config.data_dir = "data/dev";
        return config;
    }
    
    static StorageConfig forTesting() {
        StorageConfig config;
        config.backend = "memory";
        config.allow_fallback = false;  // Strict in tests
        config.data_dir = "data/test";
        return config;
    }
    
    static StorageConfig forProduction() {
        StorageConfig config;
        config.backend = "rocksdb";
        config.allow_fallback = false;  // No fallback in production
        config.data_dir = "data/mainnet";
        config.rocksdb.enable_statistics = true;
        return config;
    }
};

} // namespace storage
} // namespace dinero
