#pragma once

#include "storage/rocksdb_config.h"
#include <string>
#include <fstream>
#include <map>
#include <optional>

namespace dinero {
namespace storage {

/**
 * Phase 6A: Configuration file loader for RocksDB
 *
 * Supports loading configuration from:
 * - INI-style config files
 * - Environment variables (DIN_ROCKSDB_*)
 * - Command-line arguments
 *
 * Priority (highest to lowest):
 * 1. Explicit runtime overrides
 * 2. Config file values
 * 3. Environment variables
 * 4. Workload defaults
 */
class RocksDBConfigLoader {
public:
    /// Load config from file with optional workload hint
    static RocksDBConfig loadFromFile(
        const std::string& config_file,
        RocksDBConfig::WorkloadType default_workload = RocksDBConfig::WorkloadType::NORMAL_OPERATION
    );

    /// Load config from environment variables (DIN_ROCKSDB_*)
    static RocksDBConfig loadFromEnvironment(
        RocksDBConfig::WorkloadType default_workload = RocksDBConfig::WorkloadType::NORMAL_OPERATION
    );

    /// Load config with priority: file > env > defaults
    static RocksDBConfig loadWithFallback(
        const std::optional<std::string>& config_file,
        RocksDBConfig::WorkloadType default_workload = RocksDBConfig::WorkloadType::NORMAL_OPERATION
    );

    /// Save config to INI file
    static bool saveToFile(const RocksDBConfig& config, const std::string& output_file);

    /// Generate example config file
    static std::string generateExampleConfig();

private:
    /// Parse INI-style config file
    static std::map<std::string, std::string> parseIniFile(const std::string& file_path);

    /// Apply key-value pairs to config
    static void applyConfigMap(RocksDBConfig& config, const std::map<std::string, std::string>& kv_pairs);

    /// Parse size with units (e.g., "512MB", "2GB")
    static std::optional<size_t> parseSize(const std::string& value);

    /// Parse boolean (true/false, yes/no, 1/0)
    static std::optional<bool> parseBool(const std::string& value);

    /// Parse integer
    static std::optional<int> parseInt(const std::string& value);
};

} // namespace storage
} // namespace dinero
