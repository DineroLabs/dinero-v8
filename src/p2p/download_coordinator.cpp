/**
 * Phase G.2: Block & TX Download Coordination Implementation
 *
 * Pure download orchestration with structural checks only.
 */

#include "../../include/p2p/download_coordinator.h"
#include <iostream>

namespace dinero {
namespace p2p {

//=============================================================================
// Constructor
//=============================================================================

DownloadCoordinator::DownloadCoordinator(InFlightManager& inflight, IDownloadSink& sink)
    : inflight_(inflight)
    , sink_(sink)
{
}

//=============================================================================
// handleBlock: Receive block payload
//=============================================================================

void DownloadCoordinator::handleBlock(
    const std::string& peer,
    const Hash256& hash,
    const std::vector<uint8_t>& data
) {
    std::lock_guard<std::mutex> lock(mutex_);

    InventoryVector inv(MSG_BLOCK, hash);

    // Check if we requested this
    if (!wasRequested(inv)) {
        // Unsolicited block, reject
        return;
    }

    // Check if already received
    if (alreadyReceived(inv)) {
        // Duplicate, ignore
        return;
    }

    // Structural check: size limit
    if (data.size() > MAX_BLOCK_SIZE) {
        // Oversized block, reject and clear in-flight
        inflight_.remove(inv);
        return;
    }

    // Success path:
    // 1. Mark as received
    markReceived(inv);

    // 2. Clear from in-flight
    inflight_.remove(inv);

    // 3. Hand off to sink
    sink_.onBlock(inv, data);
}

//=============================================================================
// handleTx: Receive transaction payload
//=============================================================================

void DownloadCoordinator::handleTx(
    const std::string& peer,
    const Hash256& hash,
    const std::vector<uint8_t>& data
) {
    std::lock_guard<std::mutex> lock(mutex_);

    InventoryVector inv(MSG_TX, hash);

    // Check if we requested this
    if (!wasRequested(inv)) {
        // Unsolicited tx, reject
        return;
    }

    // Check if already received
    if (alreadyReceived(inv)) {
        // Duplicate, ignore
        return;
    }

    // Structural check: size limit
    if (data.size() > MAX_TX_SIZE) {
        // Oversized tx, reject and clear in-flight
        inflight_.remove(inv);
        return;
    }

    // Success path:
    // 1. Mark as received
    markReceived(inv);

    // 2. Clear from in-flight
    inflight_.remove(inv);

    // 3. Hand off to sink
    sink_.onTx(inv, data);
}

//=============================================================================
// Per-Peer Tracking
//=============================================================================

size_t DownloadCoordinator::getPeerInFlightCount(const std::string& peer) const {
    // This is a simplified version - in production, InFlightManager
    // would track per-peer counts directly
    // For now, just return total count (good enough for Phase G.2)
    return inflight_.count();
}

//=============================================================================
// Helpers
//=============================================================================

bool DownloadCoordinator::wasRequested(const InventoryVector& inv) const {
    return inflight_.exists(inv);
}

bool DownloadCoordinator::alreadyReceived(const InventoryVector& inv) const {
    return received_objects_.count(inv) > 0;
}

void DownloadCoordinator::markReceived(const InventoryVector& inv) {
    received_objects_.insert(inv);
}

} // namespace p2p
} // namespace dinero
