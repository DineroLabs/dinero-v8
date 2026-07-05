#pragma once

#include <cstdint>

namespace dinero {
namespace consensus {

// #371: tracks consecutive drain TEMPORARY_FAIL results at the same height so
// the BlockDownloadScheduler can escalate a persistent storage-layer failure
// instead of silently retrying every tick forever (the EU1 2026-07-04 zombie:
// 17,979+ "will retry next tick" iterations against a latched rocksdb error).
//
// Pure state machine — no I/O, no clock — so it is unit-testable and the
// scheduler wiring stays one call per site.
struct DrainFailureStreak {
    // Drain ticks at the same height before escalation. Ticks run ~1/s, so
    // ~50 ≈ a minute of zero progress — far beyond any transient (mempool
    // contention, brief compaction stall), far below the 15-min watchdog.
    static constexpr int kEscalationThreshold = 50;

    uint32_t height = 0;
    int count = 0;

    // Record a TEMPORARY_FAIL at height `h`. Returns true exactly once per
    // streak: when the count at an unchanged height crosses the threshold.
    bool RecordFailure(uint32_t h) {
        if (h != height) {
            height = h;
            count = 0;
        }
        ++count;
        return count == kEscalationThreshold;
    }

    // Any successful drain progress clears the streak.
    void RecordProgress() {
        height = 0;
        count = 0;
    }
};

}  // namespace consensus
}  // namespace dinero
