#pragma once

#include "daemon/ws_subscriptions.hpp"

/**
 * Global WebSocket Subscriptions Instance (Nullable Pointer Pattern)
 *
 * Provides a singleton pointer to the Subscriptions manager that can be
 * accessed from anywhere in the daemon for publishing events to WebSocket clients.
 *
 * This uses a nullable pointer pattern to ensure safe operation when WebSocket
 * server is disabled or not yet initialized. Always check for nullptr before use.
 *
 * Usage:
 *   #include "daemon/ws_globals.h"
 *
 *   // Publish block notification (with null-safety)
 *   if (g_subscriptions) {
 *       Json::Value block_event;
 *       block_event["channel"] = "newBlocks";
 *       block_event["height"] = 1234;
 *       std::string json_str = Json::writeString(builder, block_event);
 *       g_subscriptions->enqueue("newBlocks", json_str);
 *   }
 */

// Global Subscriptions instance - nullptr until WebSocket server initialized
extern Subscriptions* g_subscriptions;
