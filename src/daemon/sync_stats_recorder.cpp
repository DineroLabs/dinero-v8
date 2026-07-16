#include "daemon/sync_stats_recorder.h"

#include <cstdio>

namespace dinero {

SyncStatsRecorder::SyncStatsRecorder(std::chrono::steady_clock::time_point start)
    : window_anchor_(start) {}

std::string SyncStatsRecorder::RecordBlockConnect(
    uint32_t height, uint64_t connect_ms, uint64_t checkpoint_bytes,
    std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);

    snap_.blocks_connected++;
    snap_.total_connect_ms += connect_ms;
    snap_.total_checkpoint_bytes += checkpoint_bytes;
    snap_.last_height = height;
    snap_.last_connect_ms = connect_ms;
    snap_.last_checkpoint_bytes = checkpoint_bytes;

    window_blocks_++;
    window_connect_ms_ += connect_ms;
    window_checkpoint_bytes_ += checkpoint_bytes;

    if (window_blocks_ < kSummaryInterval) {
        return "";
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - window_anchor_);
    const double elapsed_min =
        static_cast<double>(elapsed.count()) / 60000.0;
    const double blk_min =
        elapsed_min > 0.0 ? static_cast<double>(window_blocks_) / elapsed_min : 0.0;
    const double avg_connect_ms =
        static_cast<double>(window_connect_ms_) / window_blocks_;
    const uint64_t avg_ckpt_bytes = window_checkpoint_bytes_ / window_blocks_;
    const double window_ckpt_mb =
        static_cast<double>(window_checkpoint_bytes_) / (1024.0 * 1024.0);

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "[SyncStats] height=%u window_blocks=%u avg_connect_ms=%.1f "
                  "avg_ckpt_bytes=%llu window_ckpt_mb=%.2f blk_min=%.1f "
                  "total_blocks=%llu",
                  height, window_blocks_, avg_connect_ms,
                  static_cast<unsigned long long>(avg_ckpt_bytes),
                  window_ckpt_mb, blk_min,
                  static_cast<unsigned long long>(snap_.blocks_connected));

    window_blocks_ = 0;
    window_connect_ms_ = 0;
    window_checkpoint_bytes_ = 0;
    window_anchor_ = now;

    return buf;
}

void SyncStatsRecorder::RecordCsnCheckpoint(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.csn_checkpoint_writes++;
    snap_.csn_checkpoint_bytes += bytes;
}

SyncStatsRecorder::Snapshot SyncStatsRecorder::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_;
}

}  // namespace dinero
