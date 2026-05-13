#pragma once
#include <cstdint>
#include <string>

// Forward declaration for Json (defined in din_json.h, in global namespace)
namespace Json {
    class Value;
}

namespace dinero {

/**
 * @brief Abstract interface for chain state access
 *
 * This interface shields wallet and consensus code from RocksDB header pollution.
 * Provides read-only access to chain tip information without exposing ChainDB internals.
 *
 * Pattern: Interface Segregation Principle (ISP) from SOLID
 *
 * Extended in Phase 2 to support full blockchain RPC queries:
 * - Best block hash
 * - Network difficulty
 * - Block headers
 */
class ChainHeightProvider {
public:
    virtual ~ChainHeightProvider() = default;

    /**
     * @brief Get current blockchain tip height
     * @return Current height (0 if chain empty or unavailable)
     */
    virtual uint32_t GetBestHeight() const = 0;

    /**
     * @brief Get hash of best (tip) block
     * @return 64-character hex string (genesis hash if chain empty)
     */
    virtual std::string GetBestHash() const = 0;

    /**
     * @brief Get current network difficulty
     * @return Difficulty as double (1.0 minimum)
     */
    virtual double GetDifficulty() const = 0;

    /**
     * @brief Get block header information by hash
     * @param hash Block hash (64-character hex string)
     * @return Json object with header fields, or null if not found
     */
    virtual ::Json::Value GetBlockHeader(const std::string& hash) const = 0;

    /**
     * @brief Check if chain data is available
     * @return true if chain DB is initialized and accessible
     */
    virtual bool IsAvailable() const = 0;
};

/**
 * @brief Global accessor for production chain height provider
 *
 * Returns the singleton ChainDB-backed provider.
 * Safe to call from any thread after initialization.
 *
 * @return Pointer to global provider (never null after daemon initialization)
 */
ChainHeightProvider* GetGlobalChainHeightProvider();

/**
 * @brief Set the global chain height provider (called once at daemon startup)
 *
 * This is typically called from main.cpp after ChainDB initialization.
 *
 * @param provider Pointer to ChainDB-backed provider instance
 */
void SetGlobalChainHeightProvider(ChainHeightProvider* provider);

} // namespace dinero
