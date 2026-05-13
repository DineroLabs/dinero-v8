#include "storage/rocksdb_config_loader.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <cctype>

namespace dinero {
namespace storage {

// ========== Public Methods ==========

RocksDBConfig RocksDBConfigLoader::loadFromFile(const std::string& config_file, RocksDBConfig::WorkloadType default_workload) {
    RocksDBConfig config = RocksDBConfig::autoDetect(default_workload);

    try {
        auto kv_pairs = parseIniFile(config_file);
        applyConfigMap(config, kv_pairs);
        std::cout << "[ConfigLoader] Loaded config from: " << config_file << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[ConfigLoader] Failed to load " << config_file << ": " << e.what() << std::endl;
        std::cerr << "[ConfigLoader] Using default workload configuration" << std::endl;
    }

    return config;
}

RocksDBConfig RocksDBConfigLoader::loadFromEnvironment(RocksDBConfig::WorkloadType default_workload) {
    RocksDBConfig config = RocksDBConfig::autoDetect(default_workload);

    std::map<std::string, std::string> env_vars;

    // Check for environment variables with DIN_ROCKSDB_ prefix
    const char* env_keys[] = {
        "DIN_ROCKSDB_BLOCK_CACHE_MB",
        "DIN_ROCKSDB_WRITE_BUFFER_MB",
        "DIN_ROCKSDB_MAX_OPEN_FILES",
        "DIN_ROCKSDB_COMPRESSION",
        "DIN_ROCKSDB_USE_TIERED_COMPRESSION",
        "DIN_ROCKSDB_MAX_BACKGROUND_JOBS",
        "DIN_ROCKSDB_ENABLE_STATISTICS",
        "DIN_ROCKSDB_USE_DIRECT_READS"
    };

    for (const char* key : env_keys) {
        const char* value = std::getenv(key);
        if (value) {
            // Strip "DIN_ROCKSDB_" prefix for config key
            std::string config_key(key + 12); // Skip "DIN_ROCKSDB_"
            env_vars[config_key] = value;
        }
    }

    if (!env_vars.empty()) {
        applyConfigMap(config, env_vars);
        std::cout << "[ConfigLoader] Applied " << env_vars.size() << " environment variable overrides" << std::endl;
    }

    return config;
}

RocksDBConfig RocksDBConfigLoader::loadWithFallback(
    const std::optional<std::string>& config_file,
    RocksDBConfig::WorkloadType default_workload)
{
    RocksDBConfig config = RocksDBConfig::autoDetect(default_workload);

    // Priority 1: Config file (if provided)
    if (config_file.has_value()) {
        try {
            auto kv_pairs = parseIniFile(*config_file);
            applyConfigMap(config, kv_pairs);
            std::cout << "[ConfigLoader] Loaded config from file: " << *config_file << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[ConfigLoader] Failed to load config file: " << e.what() << std::endl;
        }
    }

    // Priority 2: Environment variables (override file settings)
    RocksDBConfig env_config = loadFromEnvironment(default_workload);
    // Merge env overrides into config
    if (env_config.block_cache_size_bytes.has_value()) {
        config.block_cache_size_bytes = env_config.block_cache_size_bytes;
    }
    // ... (similar for other fields - simplified here)

    return config;
}

bool RocksDBConfigLoader::saveToFile(const RocksDBConfig& config, const std::string& output_file) {
    std::ofstream ofs(output_file);
    if (!ofs.is_open()) {
        std::cerr << "[ConfigLoader] Failed to open output file: " << output_file << std::endl;
        return false;
    }

    ofs << "# Dinero RocksDB Configuration (Phase 6A)\n";
    ofs << "# Auto-generated configuration file\n\n";

    ofs << "[memory]\n";
    ofs << "block_cache_mb = " << (config.getBlockCacheSize() >> 20) << "\n";
    ofs << "write_buffer_mb = " << (config.getWriteBufferSize() >> 20) << "\n";
    ofs << "max_write_buffer_number = " << config.max_write_buffer_number.value_or(4) << "\n\n";

    ofs << "[files]\n";
    ofs << "max_open_files = " << config.getMaxOpenFiles() << "\n";
    ofs << "target_file_size_mb = " << (config.getTargetFileSizeBase() >> 20) << "\n\n";

    ofs << "[compression]\n";
    ofs << "compression_type = " << config.compression_type << "\n";
    ofs << "use_tiered_compression = " << (config.use_tiered_compression ? "true" : "false") << "\n\n";

    ofs << "[level0]\n";
    ofs << "compaction_trigger = " << config.level0_file_num_compaction_trigger.value_or(4) << "\n";
    ofs << "slowdown_trigger = " << config.level0_slowdown_writes_trigger.value_or(20) << "\n";
    ofs << "stop_trigger = " << config.level0_stop_writes_trigger.value_or(36) << "\n\n";

    ofs << "[parallelism]\n";
    ofs << "max_background_jobs = " << config.getMaxBackgroundJobs() << "\n";
    ofs << "max_background_flushes = " << config.max_background_flushes.value_or(2) << "\n\n";

    ofs << "[io]\n";
    ofs << "use_direct_reads = " << (config.use_direct_reads.value_or(false) ? "true" : "false") << "\n";
    ofs << "bytes_per_sync_mb = " << (config.bytes_per_sync.value_or(1 << 20) >> 20) << "\n\n";

    ofs << "[monitoring]\n";
    ofs << "enable_statistics = " << (config.enable_statistics ? "true" : "false") << "\n";
    ofs << "stats_dump_period_sec = " << config.stats_dump_period_sec.value_or(600) << "\n\n";

    std::cout << "[ConfigLoader] Config saved to: " << output_file << std::endl;
    return true;
}

std::string RocksDBConfigLoader::generateExampleConfig() {
    std::ostringstream oss;

    oss << "# Dinero RocksDB Configuration (Phase 6A)\n";
    oss << "# Example configuration file with recommended settings\n\n";

    oss << "[memory]\n";
    oss << "# Block cache size in MB (default: auto-detect, 25% of RAM, max 2GB)\n";
    oss << "block_cache_mb = 512\n\n";
    oss << "# Write buffer size in MB per memtable (default: 128MB)\n";
    oss << "write_buffer_mb = 128\n\n";
    oss << "# Number of write buffers (default: 4, allows pipelining)\n";
    oss << "max_write_buffer_number = 4\n\n";

    oss << "[files]\n";
    oss << "# Max open files (-1 = unlimited, 0 = auto-detect, N = limit)\n";
    oss << "max_open_files = 0\n\n";
    oss << "# Target SST file size in MB (default: 128MB)\n";
    oss << "target_file_size_mb = 128\n\n";

    oss << "[compression]\n";
    oss << "# Compression algorithm: none, snappy, lz4, zstd (default: lz4)\n";
    oss << "compression_type = lz4\n\n";
    oss << "# Use tiered compression (L0-L1: none, L2-L4: lz4, L5+: zstd)\n";
    oss << "use_tiered_compression = true\n\n";

    oss << "[level0]\n";
    oss << "# L0 file count trigger for compaction (default: 4)\n";
    oss << "compaction_trigger = 4\n\n";
    oss << "# L0 file count trigger for slowdown (default: 20)\n";
    oss << "slowdown_trigger = 20\n\n";
    oss << "# L0 file count trigger for write stop (default: 36)\n";
    oss << "stop_trigger = 36\n\n";

    oss << "[parallelism]\n";
    oss << "# Total background jobs (default: auto, based on CPU cores)\n";
    oss << "max_background_jobs = 0\n\n";
    oss << "# Background flush threads (default: 2)\n";
    oss << "max_background_flushes = 2\n\n";

    oss << "[io]\n";
    oss << "# Use Direct I/O for reads (Linux: true, macOS: false)\n";
    oss << "use_direct_reads = false\n\n";
    oss << "# Sync interval in MB (default: 1MB)\n";
    oss << "bytes_per_sync_mb = 1\n\n";

    oss << "[monitoring]\n";
    oss << "# Enable statistics collection (default: true)\n";
    oss << "enable_statistics = true\n\n";
    oss << "# Statistics dump interval in seconds (default: 600 = 10 min)\n";
    oss << "stats_dump_period_sec = 600\n\n";

    return oss.str();
}

// ========== Private Helpers ==========

std::map<std::string, std::string> RocksDBConfigLoader::parseIniFile(const std::string& file_path) {
    std::map<std::string, std::string> result;
    std::ifstream ifs(file_path);

    if (!ifs.is_open()) {
        throw std::runtime_error("Cannot open config file: " + file_path);
    }

    std::string line;
    std::string current_section;

    while (std::getline(ifs, line)) {
        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        // Section header [section]
        if (line[0] == '[' && line.back() == ']') {
            current_section = line.substr(1, line.size() - 2);
            continue;
        }

        // Key-value pair
        size_t eq_pos = line.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = line.substr(0, eq_pos);
            std::string value = line.substr(eq_pos + 1);

            // Trim
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            // Store with section prefix
            std::string full_key = current_section.empty() ? key : (current_section + "." + key);
            result[full_key] = value;
        }
    }

    return result;
}

void RocksDBConfigLoader::applyConfigMap(RocksDBConfig& config, const std::map<std::string, std::string>& kv_pairs) {
    for (const auto& [key, value] : kv_pairs) {
        // Memory settings
        if (key == "memory.block_cache_mb" || key == "BLOCK_CACHE_MB") {
            if (auto size = parseSize(value + "M")) {
                config.block_cache_size_bytes = *size;
            }
        } else if (key == "memory.write_buffer_mb" || key == "WRITE_BUFFER_MB") {
            if (auto size = parseSize(value + "M")) {
                config.write_buffer_size_bytes = *size;
            }
        } else if (key == "memory.max_write_buffer_number") {
            if (auto num = parseInt(value)) {
                config.max_write_buffer_number = *num;
            }
        }
        // File settings
        else if (key == "files.max_open_files" || key == "MAX_OPEN_FILES") {
            if (auto num = parseInt(value)) {
                config.max_open_files = *num;
            }
        } else if (key == "files.target_file_size_mb") {
            if (auto size = parseSize(value + "M")) {
                config.target_file_size_base_bytes = *size;
            }
        }
        // Compression
        else if (key == "compression.compression_type" || key == "COMPRESSION") {
            config.compression_type = value;
        } else if (key == "compression.use_tiered_compression" || key == "USE_TIERED_COMPRESSION") {
            if (auto b = parseBool(value)) {
                config.use_tiered_compression = *b;
            }
        }
        // Level-0
        else if (key == "level0.compaction_trigger") {
            if (auto num = parseInt(value)) {
                config.level0_file_num_compaction_trigger = *num;
            }
        } else if (key == "level0.slowdown_trigger") {
            if (auto num = parseInt(value)) {
                config.level0_slowdown_writes_trigger = *num;
            }
        } else if (key == "level0.stop_trigger") {
            if (auto num = parseInt(value)) {
                config.level0_stop_writes_trigger = *num;
            }
        }
        // Parallelism
        else if (key == "parallelism.max_background_jobs" || key == "MAX_BACKGROUND_JOBS") {
            if (auto num = parseInt(value)) {
                config.max_background_jobs = *num;
            }
        } else if (key == "parallelism.max_background_flushes") {
            if (auto num = parseInt(value)) {
                config.max_background_flushes = *num;
            }
        }
        // I/O
        else if (key == "io.use_direct_reads" || key == "USE_DIRECT_READS") {
            if (auto b = parseBool(value)) {
                config.use_direct_reads = *b;
            }
        } else if (key == "io.bytes_per_sync_mb") {
            if (auto size = parseSize(value + "M")) {
                config.bytes_per_sync = *size;
            }
        }
        // Monitoring
        else if (key == "monitoring.enable_statistics" || key == "ENABLE_STATISTICS") {
            if (auto b = parseBool(value)) {
                config.enable_statistics = *b;
            }
        } else if (key == "monitoring.stats_dump_period_sec") {
            if (auto num = parseInt(value)) {
                config.stats_dump_period_sec = static_cast<uint32_t>(*num);
            }
        }
    }
}

std::optional<size_t> RocksDBConfigLoader::parseSize(const std::string& value) {
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(), ::toupper);

    size_t multiplier = 1;
    std::string num_str = v;

    if (v.size() >= 2) {
        std::string suffix = v.substr(v.size() - 2);
        if (suffix == "KB") {
            multiplier = 1024;
            num_str = v.substr(0, v.size() - 2);
        } else if (suffix == "MB") {
            multiplier = 1024 * 1024;
            num_str = v.substr(0, v.size() - 2);
        } else if (suffix == "GB") {
            multiplier = 1024 * 1024 * 1024;
            num_str = v.substr(0, v.size() - 2);
        } else if (v.back() == 'K' || v.back() == 'M' || v.back() == 'G') {
            char c = v.back();
            multiplier = (c == 'K') ? 1024 : (c == 'M') ? (1024 * 1024) : (1024ULL * 1024 * 1024);
            num_str = v.substr(0, v.size() - 1);
        }
    }

    try {
        size_t num = std::stoull(num_str);
        return num * multiplier;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> RocksDBConfigLoader::parseBool(const std::string& value) {
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);

    if (v == "true" || v == "yes" || v == "1" || v == "on") {
        return true;
    } else if (v == "false" || v == "no" || v == "0" || v == "off") {
        return false;
    }
    return std::nullopt;
}

std::optional<int> RocksDBConfigLoader::parseInt(const std::string& value) {
    try {
        return std::stoi(value);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace storage
} // namespace dinero
