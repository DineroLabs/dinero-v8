/**
 * InMemoryChainDB - In-Memory ChainDB for Integration Testing
 *
 * Purpose:
 * - Test Layer 1-2 semantics without RocksDB dependency
 * - Fast test execution (no disk I/O)
 * - Simple implementation (std::map)
 *
 * Use Case:
 * - Day 1-2: Test DisconnectBlock, RollbackToFork, ReorgGuard
 * - Verify UTXO restoration logic
 * - Verify value conservation
 *
 * NOT Tested:
 * - RocksDB crash recovery
 * - Disk persistence
 * - WriteBatch atomicity (in-memory operations are inherently atomic)
 *
 * This is a TEST ONLY implementation.
 * Production code uses real ChainDB with RocksDB backend.
 */

#pragma once

#include "test_chain_db_interface.h"
#include <map>
#include <vector>
#include <cstdint>
#include <optional>
#include <string>

namespace dinero {
namespace test {

/**
 * In-memory ChainDB implementation using std::map
 *
 * Thread-safety: NOT thread-safe (tests are single-threaded)
 * Persistence: None (data lost on destruction)
 * Atomicity: All operations are atomic (in-memory)
 */
class InMemoryChainDB : public ITestChainDB {
public:
    InMemoryChainDB() : closed_(false) {}

    ~InMemoryChainDB() override {
        Close();
    }

    // Disable copy/move
    InMemoryChainDB(const InMemoryChainDB&) = delete;
    InMemoryChainDB& operator=(const InMemoryChainDB&) = delete;

    bool Put(const std::vector<uint8_t>& key,
             const std::vector<uint8_t>& value) override {
        if (closed_) return false;

        // Use vector as key (std::map can compare vectors lexicographically)
        data_[key] = value;
        return true;
    }

    std::optional<std::vector<uint8_t>> Get(const std::vector<uint8_t>& key) const override {
        if (closed_) return std::nullopt;

        auto it = data_.find(key);
        if (it != data_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    bool Delete(const std::vector<uint8_t>& key) override {
        if (closed_) return false;

        data_.erase(key);  // Safe even if key doesn't exist
        return true;
    }

    bool Has(const std::vector<uint8_t>& key) const override {
        if (closed_) return false;

        return data_.count(key) > 0;
    }

    size_t Size() const override {
        return data_.size();
    }

    void Clear() override {
        data_.clear();
    }

    void Close() override {
        closed_ = true;
        // In-memory data persists until destruction
        // (allows reopening to simulate crash recovery)
    }

    bool Flush() override {
        // No-op for in-memory implementation
        // (all writes are already "flushed")
        return true;
    }

    /**
     * Reopen database (for crash recovery simulation)
     *
     * In-memory implementation: Just clear the closed flag.
     * Data is still in memory (simulates successful recovery).
     */
    void Reopen() {
        closed_ = false;
    }

    /**
     * Get raw data map (for debugging/verification)
     *
     * WARNING: Test-only method. Do not use in production.
     */
    const std::map<std::vector<uint8_t>, std::vector<uint8_t>>& GetData() const {
        return data_;
    }

private:
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> data_;
    bool closed_;
};

} // namespace test
} // namespace dinero
