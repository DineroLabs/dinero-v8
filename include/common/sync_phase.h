// SyncPhase (moved verbatim from include/p2p/block_download_scheduler.h,
// Task 6.1): the phase is shared vocabulary between the node's download
// scheduler and the sync diagnostics, so it lives in common/ where both
// sides can name it without a p2p header.
#pragma once

namespace dinero {

/**
 * Sync phase (Phase G.12)
 * Determines scheduling behavior based on sync state
 */
enum class SyncPhase {
    IBD,           // Initial Block Download - aggressive parallelism
    CATCHING_UP,   // Behind but syncing - balanced approach
    STEADY_STATE   // Caught up - conservative, network-friendly
};

} // namespace dinero
