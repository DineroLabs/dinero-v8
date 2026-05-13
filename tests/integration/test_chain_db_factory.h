/**
 * TestChainDBFactory - Isolated RocksDB Instances for Integration Testing
 *
 * Purpose:
 * - Create isolated ChainDB instances (no shared state)
 * - Deterministic cleanup (no test pollution)
 * - Crash/restart simulation hooks
 *
 * Capabilities:
 * - Clean start (fresh database)
 * - Dirty shutdown (simulate crash)
 * - Reopen + verify state (crash recovery testing)
 */

#pragma once

#include "../../include/storage/chain_db.h"
#include "../../include/storage/chain_write_token.h"
#include <filesystem>
#include <string>
#include <memory>

namespace dinero {
namespace test {

class TestChainDBFactory {
public:
    /**
     * Create a fresh ChainDB instance for testing
     *
     * @param test_name Unique name for this test (used for directory)
     * @return Initialized ChainDB instance
     *
     * Directory: /tmp/dinero_test_{test_name}_{random}
     */
    static std::unique_ptr<ChainDB> Create(const std::string& test_name) {
        // Generate unique temp directory
        std::filesystem::path temp_dir = GenerateTempDir(test_name);

        // Create directory
        std::filesystem::create_directories(temp_dir);

        // Create and initialize ChainDB
        auto chain_db = std::make_unique<ChainDB>();
        auto status = chain_db->init(temp_dir);

        if (!status.ok()) {
            throw std::runtime_error("Failed to initialize ChainDB: " + status.message());
        }

        // Store path for cleanup
        RegisterPath(test_name, temp_dir);

        return chain_db;
    }

    /**
     * Reopen an existing ChainDB (for crash recovery testing)
     *
     * @param test_name Test name (must match previous Create call)
     * @return Reopened ChainDB instance
     *
     * Use case: Test crash recovery by closing DB without flush,
     * then reopening and verifying state.
     */
    static std::unique_ptr<ChainDB> Reopen(const std::string& test_name) {
        auto it = path_registry_.find(test_name);
        if (it == path_registry_.end()) {
            throw std::runtime_error("No ChainDB registered for test: " + test_name);
        }

        std::filesystem::path temp_dir = it->second;

        if (!std::filesystem::exists(temp_dir)) {
            throw std::runtime_error("ChainDB directory does not exist: " + temp_dir.string());
        }

        auto chain_db = std::make_unique<ChainDB>();
        auto status = chain_db->init(temp_dir);

        if (!status.ok()) {
            throw std::runtime_error("Failed to reopen ChainDB: " + status.message());
        }

        return chain_db;
    }

    /**
     * Cleanup ChainDB directory
     *
     * @param test_name Test name
     *
     * Call this at end of test to remove database.
     * Safe to call even if database was never created.
     */
    static void Cleanup(const std::string& test_name) {
        auto it = path_registry_.find(test_name);
        if (it != path_registry_.end()) {
            std::filesystem::path temp_dir = it->second;

            if (std::filesystem::exists(temp_dir)) {
                std::filesystem::remove_all(temp_dir);
            }

            path_registry_.erase(it);
        }
    }

    /**
     * Cleanup all test databases
     *
     * Call at end of test suite to cleanup everything.
     */
    static void CleanupAll() {
        for (const auto& [test_name, temp_dir] : path_registry_) {
            if (std::filesystem::exists(temp_dir)) {
                std::filesystem::remove_all(temp_dir);
            }
        }
        path_registry_.clear();
    }

    /**
     * Get path for a test database (for debugging)
     *
     * @param test_name Test name
     * @return Path to database directory
     */
    static std::filesystem::path GetPath(const std::string& test_name) {
        auto it = path_registry_.find(test_name);
        if (it != path_registry_.end()) {
            return it->second;
        }
        return {};
    }

    /**
     * Simulate dirty shutdown (close without flush)
     *
     * @param chain_db ChainDB to close
     *
     * Closes the database immediately without flushing.
     * Use this to test crash recovery.
     */
    static void DirtyShutdown(std::unique_ptr<ChainDB>& chain_db) {
        // Just close - no flush
        chain_db->close();
        chain_db.reset();
    }

    /**
     * Create a write token for testing
     *
     * @return ChainWriteToken for test use
     *
     * WARNING: This bypasses all invariant protection!
     * Only use in integration tests.
     */
    static ChainWriteToken CreateWriteToken() {
        return ChainWriteToken::CreateForTesting();
    }

private:
    // Registry of test name → database path
    static inline std::map<std::string, std::filesystem::path> path_registry_;

    // Counter for unique directories
    static inline std::atomic<uint64_t> counter_{0};

    /**
     * Generate unique temp directory for test
     *
     * @param test_name Test name
     * @return Unique temp directory path
     */
    static std::filesystem::path GenerateTempDir(const std::string& test_name) {
        uint64_t id = counter_++;
        std::string dir_name = "dinero_test_" + test_name + "_" + std::to_string(id);
        return std::filesystem::temp_directory_path() / dir_name;
    }

    /**
     * Register path for cleanup
     *
     * @param test_name Test name
     * @param path Database path
     */
    static void RegisterPath(const std::string& test_name, const std::filesystem::path& path) {
        path_registry_[test_name] = path;
    }
};

/**
 * RAII helper for automatic cleanup
 *
 * Usage:
 *   TestDBGuard guard("my_test");
 *   auto chain_db = TestChainDBFactory::Create("my_test");
 *   // ... use chain_db ...
 *   // Automatic cleanup when guard goes out of scope
 */
class TestDBGuard {
public:
    explicit TestDBGuard(const std::string& test_name)
        : test_name_(test_name) {}

    ~TestDBGuard() {
        TestChainDBFactory::Cleanup(test_name_);
    }

    // Non-copyable, non-movable
    TestDBGuard(const TestDBGuard&) = delete;
    TestDBGuard& operator=(const TestDBGuard&) = delete;

private:
    std::string test_name_;
};

} // namespace test
} // namespace dinero
