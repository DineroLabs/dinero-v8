#include "storage/storage_interface.h"
#include "storage/storage_config.h"
#ifdef DIN_WITH_ROCKSDB
#include "storage/rocksdb_backend.h"
#endif
#include <memory>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <string_view>

// Forward declaration for enhanced self-test
namespace dinero {
namespace storage {
bool runEnhancedSelfTest(StorageInterface& storage, const std::string& backend_name);
}
}

namespace dinero {
namespace storage {

std::unique_ptr<StorageInterface> g_storage;

// Case-insensitive string comparison
static bool iequals(std::string_view a, std::string_view b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                     [](char a, char b) { return std::tolower(a) == std::tolower(b); });
}

// StorageFactory implementation with explicit fallback control
std::unique_ptr<StorageInterface> StorageFactory::create(const StorageConfig& config) {
    return create(config.backend, config.allow_fallback);
}

std::unique_ptr<StorageInterface> StorageFactory::create(std::string_view name, bool allow_fallback) {
    if (iequals(name, "rocksdb")) {
#ifdef DIN_WITH_ROCKSDB
        return std::make_unique<RocksDBBackend>();
#else
        if (allow_fallback) {
            std::cerr << "WARNING: RocksDB backend requested but not compiled. Falling back to SQLite." << std::endl;
            return create("sqlite", allow_fallback);
        } else {
            std::cerr << "FATAL: RocksDB backend requested but not compiled. Fallback disabled." << std::endl;
            return nullptr;
        }
#endif
    }
    if (iequals(name, "sqlite")) {
        if (allow_fallback) {
            // TODO: Implement SQLite backend factory
            std::cerr << "WARN: SQLite backend factory not yet implemented; falling back to Memory" << std::endl;
            return create("memory", allow_fallback);
        } else {
            std::cerr << "FATAL: SQLite backend not yet implemented. Fallback disabled." << std::endl;
            return nullptr;
        }
    }
    if (iequals(name, "memory")) {
        // TODO: Implement memory backend
        std::cerr << "FATAL: Memory backend not yet implemented" << std::endl;
        return nullptr;
    }
    
    std::cerr << "ERROR: Unknown storage backend: " << name << std::endl;
    return nullptr;
}

// Legacy enum-based create method for compatibility
std::unique_ptr<StorageInterface> StorageFactory::create(BackendType type) {
    switch (type) {
        case BackendType::ROCKSDB: return create("rocksdb");
        case BackendType::SQLITE: return create("sqlite");
        case BackendType::MEMORY: return create("memory");
    }
    return nullptr;
}

std::vector<std::string> StorageFactory::getAvailableBackends() {
    std::vector<std::string> backends;

#ifdef DIN_WITH_ROCKSDB
    backends.push_back("RocksDB");
#endif

    backends.push_back("SQLite");
    backends.push_back("Memory");

    return backends;
}

std::string StorageFactory::getBackendName(BackendType type) {
    switch (type) {
        case BackendType::ROCKSDB: return "RocksDB";
        case BackendType::SQLITE: return "SQLite";
        case BackendType::MEMORY: return "Memory";
    }
    return "Unknown";
}

std::optional<StorageFactory::BackendType> StorageFactory::parseBackendType(const std::string& name) {
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    
    if (lower_name == "rocksdb" || lower_name == "rocks") {
        return BackendType::ROCKSDB;
    } else if (lower_name == "sqlite" || lower_name == "sql") {
        return BackendType::SQLITE;
    } else if (lower_name == "memory" || lower_name == "mem") {
        return BackendType::MEMORY;
    }
    
    return std::nullopt;
}

// StorageConfig implementation
// StorageConfig::toString() implementation
std::string StorageConfig::toString() const {
    std::ostringstream oss;
    oss << "StorageConfig:\n";
    oss << "  Backend: " << backend << "\n";
    oss << "  Data Directory: " << data_dir << "\n";
    oss << "  Allow Fallback: " << (allow_fallback ? "Yes" : "No") << "\n";
    
    if (backend == "rocksdb") {
        oss << "  RocksDB Options:\n";
        oss << "    Block Cache: " << (rocksdb.block_cache_size >> 20) << " MB\n";
        oss << "    Write Buffer: " << (rocksdb.write_buffer_size >> 20) << " MB\n";
        oss << "    Compression: " << (rocksdb.enable_compression ? "Enabled" : "Disabled") << "\n";
        oss << "    Statistics: " << (rocksdb.enable_statistics ? "Enabled" : "Disabled") << "\n";
    }
    
    return oss.str();
}

// Global functions with self-test
StorageResult InitializeStorage(const StorageConfig& config) {
    std::cout << "Initializing storage backend: " << config.backend << std::endl;
    
    g_storage = StorageFactory::create(config);
    if (!g_storage) {
        std::cerr << "FATAL: Failed to create storage backend: " << config.backend << std::endl;
        if (!config.allow_fallback) {
            std::cerr << "FATAL: Fallback disabled, cannot continue" << std::endl;
        }
        return StorageResult::IO_ERROR;
    }
    
    StorageResult result = g_storage->initialize(config.data_dir);
    if (result != StorageResult::SUCCESS) {
        std::cerr << "FATAL: Failed to initialize storage: " << static_cast<int>(result) << std::endl;
        g_storage.reset();
        return result;
    }
    
    // Run enhanced self-test to verify backend is working
    if (!runEnhancedSelfTest(*g_storage, g_storage->name())) {
        std::cerr << "FATAL: Storage backend failed enhanced self-test" << std::endl;
        g_storage->shutdown();
        g_storage.reset();
        return StorageResult::IO_ERROR;
    }
    
    std::cout << "Storage backend initialized successfully: " << g_storage->name() << std::endl;
    return StorageResult::SUCCESS;
}

// Legacy function for backward compatibility
StorageResult InitializeStorage(const std::string& data_dir, const std::string& backend_name) {
    StorageConfig config;
    config.backend = backend_name;
    config.data_dir = data_dir;
    config.allow_fallback = false; // Fail hard on storage init error
    return InitializeStorage(config);
}

StorageResult ShutdownStorage() {
    if (!g_storage) {
        return StorageResult::SUCCESS;
    }
    
    StorageResult result = g_storage->shutdown();
    g_storage.reset();
    
    return result;
}

} // namespace storage
} // namespace dinero
