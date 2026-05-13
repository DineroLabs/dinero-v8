/**
 * Phase G.2: Block & TX Download Coordination
 *
 * Orchestrates actual payload transfer: "we requested an object" → "we receive bytes".
 *
 * Design Principles:
 * - Pure coordination (no validation, no chainstate, no mempool)
 * - Structural checks only (length, basic decode)
 * - Hand-off via interface (IDownloadSink)
 * - Clear in-flight on receipt
 * - Reject unsolicited payloads
 * - Bounded resources (size limits, per-peer caps)
 *
 * What This IS:
 * - Download orchestration
 * - Payload correlation with in-flight requests
 * - Structural validation (size limits)
 * - Hand-off to upper layers
 *
 * What This Is NOT:
 * - Consensus validation
 * - Script execution
 * - UTXO access
 * - Disk writes
 * - Mempool admission
 * - Peer banning
 */

#pragma once

#include "inventory.h"
#include "inflight_manager.h"
#include <functional>
#include <optional>
#include <vector>
#include <map>
#include <set>
#include <mutex>

namespace dinero {
namespace p2p {

//=============================================================================
// Download Sink Interface (Hand-off to Upper Layers)
//=============================================================================

struct IDownloadSink {
    virtual ~IDownloadSink() = default;

    // Called when a block is successfully downloaded
    virtual void onBlock(const InventoryVector& inv, const std::vector<uint8_t>& data) = 0;

    // Called when a transaction is successfully downloaded
    virtual void onTx(const InventoryVector& inv, const std::vector<uint8_t>& data) = 0;
};

//=============================================================================
// DownloadCoordinator: Pure Download Orchestration
//=============================================================================

class DownloadCoordinator {
public:
    DownloadCoordinator(InFlightManager& inflight, IDownloadSink& sink);

    // Payload handlers (called when block/tx messages arrive)
    void handleBlock(const std::string& peer, const Hash256& hash, const std::vector<uint8_t>& data);
    void handleTx(const std::string& peer, const Hash256& hash, const std::vector<uint8_t>& data);

    // Per-peer tracking
    size_t getPeerInFlightCount(const std::string& peer) const;

private:
    InFlightManager& inflight_;
    IDownloadSink& sink_;

    // Already-received tracking (prevent duplicates)
    std::set<InventoryVector> received_objects_;

    mutable std::mutex mutex_;

    // Size limits (Bitcoin-compatible)
    static constexpr size_t MAX_BLOCK_SIZE = 4 * 1024 * 1024;  // 4MB
    static constexpr size_t MAX_TX_SIZE = 100000;               // 100KB (consensus/limits.h)

    // Helper: Check if we requested this object
    bool wasRequested(const InventoryVector& inv) const;

    // Helper: Check if we already received this object
    bool alreadyReceived(const InventoryVector& inv) const;

    // Helper: Mark as received
    void markReceived(const InventoryVector& inv);
};

} // namespace p2p
} // namespace dinero
