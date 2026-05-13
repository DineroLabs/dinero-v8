#pragma once

#include "compat/jsoncpp_compat.h"
#include <string>

namespace dinero_daemon {

/**
 * Activity Bus - Publishes structured events to WebSocket clients
 * 
 * Provides real-time visibility into daemon operations including:
 * - WebSocket connections/disconnections
 * - RPC requests and authentication
 * - Mining events
 * - Blockchain operations
 * - System events
 */
class ActivityBus {
public:
    /**
     * Publish an activity event
     * @param j JSON object containing event data (will have timestamp added)
     */
    static void publish(Json::Value j);
    
    /**
     * Get recent activity backlog via RPC
     * @param limit Maximum number of events to return (default: 200)
     * @return JSON array of recent events
     */
    static Json::Value getActivity(int limit = 200);
    
    /**
     * Clear activity history
     */
    static void clear();
    
    /**
     * Get current activity count
     * @return Number of events in history
     */
    static size_t getCount();

private:
    static constexpr size_t MAX_EVENTS = 500;
};

} // namespace dinero_daemon
