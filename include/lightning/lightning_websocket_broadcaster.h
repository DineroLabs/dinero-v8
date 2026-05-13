#pragma once

#include "lightning/lightning_events.h"
#include "lightning/lightning_event_manager.h"
#include <memory>

namespace dinero {

// Forward declaration
class WebSocketServer;

namespace lightning {

/**
 * @class LightningWebSocketBroadcaster
 * @brief Bridges Lightning events to WebSocket subscriptions
 *
 * Phase 14: Live Lightning Event Stream
 *
 * This class subscribes to the LightningEventManager and forwards
 * events to the WebSocket server for real-time streaming to clients.
 *
 * Thread Safety: Yes - relies on LightningEventManager's thread safety
 */
class LightningWebSocketBroadcaster {
public:
    /**
     * @brief Construct broadcaster with event and WebSocket managers
     * @param event_mgr Lightning event manager to subscribe to
     * @param ws_server WebSocket server to broadcast events through
     */
    LightningWebSocketBroadcaster(
        LightningEventManager* event_mgr,
        WebSocketServer* ws_server
    );

    ~LightningWebSocketBroadcaster();

    /**
     * @brief Start broadcasting events
     * @param replay_history If true, replay event history to new subscribers
     */
    void start(bool replay_history = false);

    /**
     * @brief Stop broadcasting events
     */
    void stop();

    /**
     * @brief Check if broadcaster is active
     */
    bool is_running() const { return m_running; }

private:
    LightningEventManager* m_event_mgr;
    WebSocketServer* m_ws_server;
    std::unique_ptr<LightningEventManager::SubscriptionHandle> m_subscription;
    bool m_running;

    /**
     * @brief Event callback - called by LightningEventManager
     * @param event The Lightning event to broadcast
     */
    void onLightningEvent(const LightningEvent& event);
};

} // namespace lightning
} // namespace dinero
