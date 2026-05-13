#pragma once

#include <cstdint>

namespace dinero {

// Forward declaration
class CBlockIndex;

/**
 * IChainManager - Minimal interface for HeaderSyncManager
 *
 * This interface defines the minimal contract that HeaderSyncManager
 * requires from a chain manager implementation. It allows for both
 * production (ChainManager) and test (MockChainManager) implementations
 * without undefined behavior from reinterpret_cast.
 *
 * Design: Interface segregation principle - only expose what's needed.
 */
class IChainManager {
public:
    virtual ~IChainManager() = default;

    /**
     * Get the current active chain tip
     *
     * @return Pointer to active tip block index, or nullptr if no blocks
     */
    virtual CBlockIndex* GetTip() const = 0;

    /**
     * Get the current active chain height
     *
     * @return Height of active chain (0 = genesis)
     */
    virtual uint32_t GetHeight() const = 0;
};

} // namespace dinero
