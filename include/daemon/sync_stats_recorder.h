#ifndef DINERO_DAEMON_SYNC_STATS_RECORDER_H
#define DINERO_DAEMON_SYNC_STATS_RECORDER_H

// Forest checkpoint delta campaign — phase 0 instrumentation
// (docs/design/forest-checkpoint-deltas.md).
//
// Accumulates per-block connect latency and forest-checkpoint write volume,
// emits one aggregated summary line per kSummaryInterval connected blocks,
// and exposes a snapshot for getsynchealth / getsnapshotbootstrapstatus.
// Pure bookkeeping: no persistence or consensus behavior changes.

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace dinero {

class SyncStatsRecorder {
public:
    static constexpr uint32_t kSummaryInterval = 100;

    struct Snapshot {
        uint64_t blocks_connected = 0;
        uint64_t total_connect_ms = 0;
        uint64_t total_checkpoint_bytes = 0;
        uint32_t last_height = 0;
        uint64_t last_connect_ms = 0;
        uint64_t last_checkpoint_bytes = 0;
        uint64_t csn_checkpoint_writes = 0;
        uint64_t csn_checkpoint_bytes = 0;
    };

    explicit SyncStatsRecorder(
        std::chrono::steady_clock::time_point start =
            std::chrono::steady_clock::now());

    // Records one connected block. Returns a non-empty "[SyncStats] ..."
    // summary line once per kSummaryInterval blocks (window-local averages
    // plus blocks/min since the previous summary); otherwise "".
    std::string RecordBlockConnect(
        uint32_t height, uint64_t connect_ms, uint64_t checkpoint_bytes,
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now());

    // Records the CSN validation worker's extra full-forest checkpoint
    // write (daemon_app CSN reorg-support path) — tracked separately from
    // the ConnectTip batch write.
    void RecordCsnCheckpoint(uint64_t bytes);

    Snapshot GetSnapshot() const;

private:
    mutable std::mutex mutex_;

    Snapshot snap_;

    // Current summary window.
    uint32_t window_blocks_ = 0;
    uint64_t window_connect_ms_ = 0;
    uint64_t window_checkpoint_bytes_ = 0;
    std::chrono::steady_clock::time_point window_anchor_;
};

}  // namespace dinero

#endif  // DINERO_DAEMON_SYNC_STATS_RECORDER_H
