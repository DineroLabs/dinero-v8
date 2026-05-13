#pragma once

#include <cstdint>
#include <string>

namespace dinero_daemon {
namespace gui_events {

// Broadcast new transaction event to GUI WebSocket subscribers
void BroadcastNewTransaction(const std::string& txid, uint64_t fee, double feerate, uint32_t size);

// Broadcast network info update to GUI WebSocket subscribers
void BroadcastNetworkInfo(int peer_count, int inbound_count, int outbound_count);

// Broadcast mempool stats update to GUI WebSocket subscribers
void BroadcastMempoolUpdate(uint32_t tx_count, uint64_t total_bytes, double avg_fee);

// Broadcast sync progress update to GUI WebSocket subscribers
// ibd: whether node is in Initial Block Download (true = syncing, false = synced)
// progress: sync progress as fraction 0.0-1.0 (not percentage)
// eta_s: estimated seconds remaining (0 if synced or unknown)
void BroadcastSyncProgress(bool ibd, double progress, int eta_s);

} // namespace gui_events
} // namespace dinero_daemon
