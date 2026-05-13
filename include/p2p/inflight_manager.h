/**
 * Phase G.1.4 Step 2: In-Flight Request Manager
 *
 * Tracks inventory requests to prevent duplicate requests and detect timeouts.
 *
 * Design Principles:
 * - Thread-safe (multiple P2P threads access concurrently)
 * - Global uniqueness (one object → one in-flight request)
 * - Deterministic (injectable clock for testing)
 * - Pure tracking (no validation, no blockchain, no mempool)
 *
 * Key Rule:
 * - (type, hash) is the identity — not hash alone
 * - MSG_BLOCK and MSG_TX with same hash are different objects
 */

#pragma once

#include "inventory.h"
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>
#include <optional>

namespace dinero {
namespace p2p {

//=============================================================================
// InventoryVector hashing for unordered_map
//=============================================================================

struct InventoryVectorHasher {
    size_t operator()(const InventoryVector& inv) const {
        // Combine type and first 8 bytes of hash
        size_t result = inv.type;
        for (int i = 0; i < 8; i++) {
            result = (result << 8) | inv.hash.data[i];
        }
        return result;
    }
};

//=============================================================================
// InFlightManager: Pure In-Flight Request Tracking
//=============================================================================

class InFlightManager {
public:
    using PeerId = std::string;
    using TimePoint = std::chrono::steady_clock::time_point;

    InFlightManager() = default;

    // Core API
    bool add(const InventoryVector& inv, const PeerId& peer);
    bool add(const InventoryVector& inv, const PeerId& peer, TimePoint timestamp);

    bool exists(const InventoryVector& inv) const;

    void remove(const InventoryVector& inv);

    std::vector<InventoryVector> expired(
        std::chrono::seconds timeout,
        TimePoint now = std::chrono::steady_clock::now()
    ) const;

    // Query peer for in-flight request
    std::optional<PeerId> getPeer(const InventoryVector& inv) const;

    // Statistics (for monitoring, not policy)
    size_t count() const;

private:
    struct RequestInfo {
        PeerId peer;
        TimePoint requested_at;
    };

    // (type, hash) → (peer, timestamp)
    std::unordered_map<InventoryVector, RequestInfo, InventoryVectorHasher> in_flight_;

    // Thread safety
    mutable std::mutex mutex_;
};

} // namespace p2p
} // namespace dinero
