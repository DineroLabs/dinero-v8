#pragma once

// WebSocket Management RPC Method Registration (vNext RpcRegistry)
// Registers WebSocket subscription management RPC methods in global RpcRegistry:
// - ws.subscribe: Subscribe to WebSocket topics
// - ws.replay: Replay historical events
// - ws.getconnections: List active WebSocket connections (admin)
// - ws.gettopicstats: Get per-topic statistics
// - ws.getstatus: Get overall WebSocket system status
void registerWebSocketManagementRPC();
