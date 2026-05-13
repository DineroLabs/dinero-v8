/**
 * Phase G.1.4 Step 3: INV/GETDATA/NOTFOUND State Machine Implementation
 *
 * Pure protocol choreography implementation.
 */

#include "../../include/p2p/inventory_handler.h"

namespace dinero {
namespace p2p {

//=============================================================================
// Constructor
//=============================================================================

InventoryHandler::InventoryHandler(
    InFlightManager& inflight,
    IMessageSender& sender,
    ICallbackProvider& callbacks
)
    : inflight_(inflight)
    , sender_(sender)
    , callbacks_(callbacks)
{
}

//=============================================================================
// handleInv: Receive inventory announcement
//=============================================================================

void InventoryHandler::handleInv(const std::string& peer, const InvMessage& msg) {
    GetDataMessage getdata;

    for (const auto& inv : msg.inventory) {
        // Check if already in-flight (duplicate request prevention)
        if (inflight_.exists(inv)) {
            continue;  // Already requesting from someone, skip
        }

        // Ask callback: "Do we want this object?"
        if (!callbacks_.wantObject(inv)) {
            continue;  // Don't want it, skip
        }

        // Add to in-flight tracking
        bool added = inflight_.add(inv, peer);
        if (!added) {
            // Race condition: someone else added it between exists() and add()
            continue;
        }

        // Queue for GETDATA
        getdata.add(inv.type, inv.hash);
    }

    // Send batched GETDATA (if any)
    if (!getdata.empty()) {
        sender_.sendGetData(peer, getdata);
    }
}

//=============================================================================
// handleGetData: Peer requests objects
//=============================================================================

void InventoryHandler::handleGetData(const std::string& peer, const GetDataMessage& msg) {
    NotFoundMessage notfound;

    for (const auto& inv : msg.inventory) {
        // Ask callback: "Can we provide this object?"
        auto obj = callbacks_.provideObject(inv);

        if (obj.has_value()) {
            // We have it, send the object
            if (inv.type == MSG_BLOCK) {
                sender_.sendBlock(peer, obj.value());
            } else if (inv.type == MSG_TX) {
                sender_.sendTx(peer, obj.value());
            }
        } else {
            // We don't have it, queue for NOTFOUND
            notfound.add(inv.type, inv.hash);
        }
    }

    // Send batched NOTFOUND (if any)
    if (!notfound.empty()) {
        sender_.sendNotFound(peer, notfound);
    }
}

//=============================================================================
// handleNotFound: Peer doesn't have requested objects
//=============================================================================

void InventoryHandler::handleNotFound(const std::string& peer, const NotFoundMessage& msg) {
    for (const auto& inv : msg.inventory) {
        // Clear from in-flight tracking
        inflight_.remove(inv);

        // Step 4: Record peer failure
        peer_failures_[peer]++;

        // Clear retry attempts (object not available)
        retry_attempts_.erase(inv);
    }
}

//=============================================================================
// Step 4: Timeout + Retry Policy
//=============================================================================

void InventoryHandler::processTimeouts(
    std::chrono::steady_clock::time_point now,
    std::chrono::seconds timeout,
    ICallbackProvider& callbacks
) {
    // Get expired requests from InFlightManager
    auto expired = inflight_.expired(timeout, now);

    for (const auto& inv : expired) {
        // Increment retry attempts
        size_t attempts = ++retry_attempts_[inv];

        if (attempts > MAX_RETRY_ATTEMPTS) {
            // Give up after max retries
            inflight_.remove(inv);
            retry_attempts_.erase(inv);
            continue;
        }

        // Get the peer we originally requested from (for peer selection)
        auto failed_peer = inflight_.getPeer(inv);
        std::string failed_peer_str = failed_peer.value_or("");

        // Remove from in-flight first
        inflight_.remove(inv);

        // Ask callback for a retry peer (avoid the failed peer)
        auto retry_peer = callbacks.selectPeerForRetry(inv, failed_peer_str);

        if (!retry_peer.has_value()) {
            // No peers available, give up
            retry_attempts_.erase(inv);
            continue;
        }

        // Re-add to in-flight with new timestamp
        bool added = inflight_.add(inv, retry_peer.value(), now);
        if (!added) {
            // Race condition or already requested, skip
            continue;
        }

        // Emit GETDATA for retry
        GetDataMessage getdata;
        getdata.add(inv.type, inv.hash);
        sender_.sendGetData(retry_peer.value(), getdata);
    }
}

//=============================================================================
// Peer Accounting
//=============================================================================

size_t InventoryHandler::getPeerFailureCount(const std::string& peer) const {
    auto it = peer_failures_.find(peer);
    if (it != peer_failures_.end()) {
        return it->second;
    }
    return 0;
}

} // namespace p2p
} // namespace dinero
