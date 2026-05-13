#pragma once
#include <cstdint>

// Register sync state RPC methods (vNext RpcRegistry):
// - wallet.getsyncstate: Get current wallet sync state and progress
// - wallet.getstatus: Get overall wallet status (loaded, encrypted, balance, sync state)
void registerSyncRPC();

// Sync state helpers for wallet operations
namespace din {
namespace sync {
    // Update sync progress (called during wallet rescan)
    void updateProgress(uint32_t height, uint32_t target);

    // Set sync active state
    void setSyncActive(bool active);

    // Record discovered UTXO
    void addDiscoveredUTXO(uint64_t amount);
}
}
