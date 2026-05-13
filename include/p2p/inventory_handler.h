/**
 * Phase G.1.4 Step 3: INV/GETDATA/NOTFOUND State Machine
 *
 * Pure protocol choreography: "Given message X, emit message Y."
 *
 * Design Principles:
 * - Decoupled via callbacks (no validation, chainstate, mempool coupling)
 * - Stateless (state managed by InFlightManager)
 * - Pure choreography (message in → message out)
 * - Testable without networking
 *
 * What This IS:
 * - Protocol message handling
 * - Traffic control (via InFlightManager)
 * - Message emission
 *
 * What This Is NOT:
 * - Validation logic
 * - Block downloading
 * - Mempool integration
 * - Consensus hooks
 * - Policy enforcement
 */

#pragma once

#include "inventory.h"
#include "inflight_manager.h"
#include <functional>
#include <optional>
#include <vector>
#include <map>

namespace dinero {
namespace p2p {

//=============================================================================
// Callback Type Definitions
//=============================================================================

// "Do we want this object?"
using WantObjectFn = std::function<bool(const InventoryVector&)>;

// "Can we provide this object?"
using ProvideObjectFn = std::function<std::optional<std::vector<uint8_t>>(const InventoryVector&)>;

//=============================================================================
// Message Sender Interface (Decoupled from Networking)
//=============================================================================

struct IMessageSender {
    virtual ~IMessageSender() = default;

    virtual void sendGetData(const std::string& peer, const GetDataMessage& msg) = 0;
    virtual void sendNotFound(const std::string& peer, const NotFoundMessage& msg) = 0;
    virtual void sendBlock(const std::string& peer, const std::vector<uint8_t>& data) = 0;
    virtual void sendTx(const std::string& peer, const std::vector<uint8_t>& data) = 0;
};

//=============================================================================
// Callback Provider Interface
//=============================================================================

struct ICallbackProvider {
    virtual ~ICallbackProvider() = default;

    virtual bool wantObject(const InventoryVector& inv) = 0;
    virtual std::optional<std::vector<uint8_t>> provideObject(const InventoryVector& inv) = 0;

    // Step 4: Peer selection for retries
    virtual std::optional<std::string> selectPeerForRetry(
        const InventoryVector& inv,
        const std::string& failed_peer
    ) = 0;
};

//=============================================================================
// InventoryHandler: Pure Protocol Choreography
//=============================================================================

class InventoryHandler {
public:
    InventoryHandler(
        InFlightManager& inflight,
        IMessageSender& sender,
        ICallbackProvider& callbacks
    );

    // Message handlers (pure choreography)
    void handleInv(const std::string& peer, const InvMessage& msg);
    void handleGetData(const std::string& peer, const GetDataMessage& msg);
    void handleNotFound(const std::string& peer, const NotFoundMessage& msg);

    // Step 4: Timeout + retry policy
    void processTimeouts(
        std::chrono::steady_clock::time_point now,
        std::chrono::seconds timeout,
        ICallbackProvider& callbacks
    );

    // Peer accounting
    size_t getPeerFailureCount(const std::string& peer) const;

private:
    InFlightManager& inflight_;
    IMessageSender& sender_;
    ICallbackProvider& callbacks_;

    // Step 4: Peer failure tracking
    std::map<std::string, size_t> peer_failures_;

    // Step 4: Retry attempt tracking (InventoryVector → attempt count)
    std::map<InventoryVector, size_t> retry_attempts_;

    // Configuration
    static constexpr size_t MAX_RETRY_ATTEMPTS = 3;
};

} // namespace p2p
} // namespace dinero
