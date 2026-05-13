#pragma once

// Forward declarations
namespace dinero {
class WebSocketServer;

namespace lightning {
class LightningEventManager;
}
}

/**
 * Phase 14: Lightning Network WebSocket RPC Handlers
 *
 * Provides RPC methods for Lightning event subscription management:
 * - lightning.subscribe: Subscribe to Lightning event channels
 * - lightning.unsubscribe: Unsubscribe from Lightning event channels
 * - lightning.getevents: Get recent Lightning events (with optional filtering)
 * - lightning.geteventstats: Get Lightning event statistics
 * - lightning.replayevents: Replay historical Lightning events
 */

// Set dependencies for RPC handlers (called during daemon initialization)
void setLightningRpcDependencies(dinero::lightning::LightningEventManager* event_mgr, dinero::WebSocketServer* ws_server);

// Register RPC methods in vNext RpcRegistry
void registerLightningWebSocketRPC();
