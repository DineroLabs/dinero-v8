/**
 * Phase G.1.4 Step 2: In-Flight Request Manager Implementation
 *
 * Pure tracking logic with deterministic behavior.
 */

#include "../../include/p2p/inflight_manager.h"

namespace dinero {
namespace p2p {

//=============================================================================
// Core API
//=============================================================================

bool InFlightManager::add(const InventoryVector& inv, const PeerId& peer) {
    return add(inv, peer, std::chrono::steady_clock::now());
}

bool InFlightManager::add(const InventoryVector& inv, const PeerId& peer, TimePoint timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if already in-flight (global uniqueness)
    auto it = in_flight_.find(inv);
    if (it != in_flight_.end()) {
        // Already requesting from someone, reject
        return false;
    }

    // Add request
    RequestInfo info;
    info.peer = peer;
    info.requested_at = timestamp;

    in_flight_[inv] = info;

    return true;
}

bool InFlightManager::exists(const InventoryVector& inv) const {
    std::lock_guard<std::mutex> lock(mutex_);

    return in_flight_.find(inv) != in_flight_.end();
}

void InFlightManager::remove(const InventoryVector& inv) {
    std::lock_guard<std::mutex> lock(mutex_);

    in_flight_.erase(inv);
}

std::vector<InventoryVector> InFlightManager::expired(
    std::chrono::seconds timeout,
    TimePoint now
) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<InventoryVector> result;

    for (const auto& [inv, info] : in_flight_) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - info.requested_at);

        if (age >= timeout) {
            result.push_back(inv);
        }
    }

    return result;
}

std::optional<InFlightManager::PeerId> InFlightManager::getPeer(const InventoryVector& inv) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = in_flight_.find(inv);
    if (it != in_flight_.end()) {
        return it->second.peer;
    }

    return std::nullopt;
}

//=============================================================================
// Statistics
//=============================================================================

size_t InFlightManager::count() const {
    std::lock_guard<std::mutex> lock(mutex_);

    return in_flight_.size();
}

} // namespace p2p
} // namespace dinero
