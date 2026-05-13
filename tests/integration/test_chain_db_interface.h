/**
 * ITestChainDB - Minimal ChainDB Interface for Integration Testing
 *
 * Purpose:
 * - Isolate RocksDB from integration tests
 * - Enable in-memory testing (no build dependencies)
 * - Support future swap to real RocksDB (same interface)
 *
 * Design:
 * - Minimal operations needed for Layer 1-2 tests
 * - No RocksDB types exposed (uses std::vector<uint8_t> for keys/values)
 * - Two implementations:
 *   1. InMemoryChainDB (std::map, for Day 1-2 testing)
 *   2. RocksChainDB (real RocksDB, for persistence testing later)
 *
 * This is exactly how Bitcoin Core isolates chainstate tests.
 */

#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <optional>

namespace dinero {
namespace test {

/**
 * Minimal ChainDB interface for testing
 *
 * Only the operations needed for Layer 1-2 integration tests.
 * No RocksDB types leak through this interface.
 */
class ITestChainDB {
public:
    virtual ~ITestChainDB() = default;

    /**
     * Store key-value pair
     *
     * @param key Raw key bytes
     * @param value Raw value bytes
     * @return true on success, false on failure
     */
    virtual bool Put(const std::vector<uint8_t>& key,
                     const std::vector<uint8_t>& value) = 0;

    /**
     * Retrieve value for key
     *
     * @param key Raw key bytes
     * @return Value if found, std::nullopt otherwise
     */
    virtual std::optional<std::vector<uint8_t>> Get(const std::vector<uint8_t>& key) const = 0;

    /**
     * Delete key-value pair
     *
     * @param key Raw key bytes
     * @return true on success (even if key didn't exist)
     */
    virtual bool Delete(const std::vector<uint8_t>& key) = 0;

    /**
     * Check if key exists
     *
     * @param key Raw key bytes
     * @return true if key exists
     */
    virtual bool Has(const std::vector<uint8_t>& key) const = 0;

    /**
     * Get number of key-value pairs
     *
     * @return Size of database (for testing/metrics)
     */
    virtual size_t Size() const = 0;

    /**
     * Clear all data (for testing)
     *
     * WARNING: Destructive operation. Only for tests.
     */
    virtual void Clear() = 0;

    /**
     * Close database (for crash simulation)
     */
    virtual void Close() = 0;

    /**
     * Flush pending writes to storage (if applicable)
     *
     * For in-memory implementation, this is a no-op.
     * For RocksDB implementation, this calls Flush().
     *
     * @return true on success
     */
    virtual bool Flush() = 0;
};

} // namespace test
} // namespace dinero
