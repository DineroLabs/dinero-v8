/**
 * TestChainDBFactory - Create Isolated Test ChainDB Instances
 *
 * V2: Uses ITestChainDB interface (no RocksDB dependency)
 *
 * Purpose:
 * - Create isolated test databases (no shared state)
 * - Support in-memory (InMemoryChainDB) and persistent (future RocksChainDB)
 * - Deterministic cleanup
 * - Crash/restart simulation
 *
 * Usage:
 *   auto db = TestChainDBFactory::CreateInMemory("test_name");
 *   // ... use db ...
 *   TestChainDBFactory::Cleanup("test_name");
 */

#pragma once

#include "test_chain_db_interface.h"
#include "in_memory_chain_db.h"
#include <map>
#include <string>
#include <memory>
#include <stdexcept>

namespace dinero {
namespace test {

class TestChainDBFactory {
public:
    /**
     * Create in-memory ChainDB for testing
     *
     * @param test_name Unique test identifier
     * @return In-memory ChainDB instance
     *
     * No filesystem access, no RocksDB dependency.
     * Perfect for Day 1-2 semantic testing.
     */
    static std::unique_ptr<ITestChainDB> CreateInMemory(const std::string& test_name) {
        auto db = std::make_unique<InMemoryChainDB>();

        // Register for cleanup
        RegisterDB(test_name, db.get());

        return db;
    }

    /**
     * Reopen existing in-memory database (for crash recovery testing)
     *
     * @param test_name Test name (must match previous Create call)
     * @return Pointer to existing database
     *
     * NOTE: Caller must have kept the unique_ptr alive!
     * This returns a raw pointer to the existing database.
     */
    static InMemoryChainDB* ReopenInMemory(const std::string& test_name) {
        auto it = db_registry_.find(test_name);
        if (it == db_registry_.end()) {
            throw std::runtime_error("No ChainDB registered for test: " + test_name);
        }

        // Cast to InMemoryChainDB and reopen
        InMemoryChainDB* in_mem_db = dynamic_cast<InMemoryChainDB*>(it->second);
        if (!in_mem_db) {
            throw std::runtime_error("Database is not an InMemoryChainDB");
        }

        in_mem_db->Reopen();
        return in_mem_db;
    }

    /**
     * Simulate dirty shutdown (close without flush)
     *
     * @param db Database to close
     *
     * For in-memory DB, this just sets closed flag.
     * Data remains in memory (can be reopened).
     */
    static void DirtyShutdown(ITestChainDB& db) {
        db.Close();
    }

    /**
     * Cleanup database (unregister from tracking)
     *
     * @param test_name Test name
     *
     * For in-memory DB, this just removes from registry.
     * Actual cleanup happens when unique_ptr is destroyed.
     */
    static void Cleanup(const std::string& test_name) {
        db_registry_.erase(test_name);
    }

    /**
     * Cleanup all registered databases
     */
    static void CleanupAll() {
        db_registry_.clear();
    }

private:
    // Registry: test_name → database pointer
    // We store raw pointers because unique_ptr ownership stays with caller
    static inline std::map<std::string, ITestChainDB*> db_registry_;

    static void RegisterDB(const std::string& test_name, ITestChainDB* db) {
        db_registry_[test_name] = db;
    }
};

/**
 * RAII helper for automatic cleanup
 *
 * Usage:
 *   TestDBGuard guard("my_test");
 *   auto db = TestChainDBFactory::CreateInMemory("my_test");
 *   // ... use db ...
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
