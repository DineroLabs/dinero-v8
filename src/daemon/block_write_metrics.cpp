#include "daemon/block_write_metrics.h"

#include <chrono>

namespace dinero {
namespace daemon {

std::atomic<uint64_t> g_durable_body_writes{0};
std::atomic<uint64_t> g_concurrent_acceptances_suppressed{0};
std::atomic<uint64_t> g_duplicate_logs_suppressed{0};
std::atomic<uint64_t> g_sidechain_logs_suppressed{0};

bool ShouldEmitRateLimitedAt(std::atomic<uint64_t>& state,
                             uint32_t interval_seconds,
                             uint64_t now_s) {
    // The stored value is now_s + 1, never a raw timestamp, so that 0 can mean
    // "never emitted" without colliding with a real one.
    //
    // steady_clock's epoch is unspecified and is boot-relative on Linux, so
    // now_s IS 0 during the first second of uptime -- and a daemon auto-started
    // at boot hits exactly that. Storing a raw 0 then read back as "never
    // emitted", defeating the limiter for that second: every call emitted.
    uint64_t last = state.load(std::memory_order_relaxed);
    if (last != 0 && now_s - (last - 1) < interval_seconds) {
        return false;
    }
    // Only one thread wins the slot; the rest are suppressed for this interval.
    return state.compare_exchange_strong(last, now_s + 1,
                                         std::memory_order_relaxed);
}

bool ShouldEmitRateLimited(std::atomic<uint64_t>& state,
                           uint32_t interval_seconds) {
    const auto now_s = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    return ShouldEmitRateLimitedAt(state, interval_seconds, now_s);
}

void ResetBlockWriteMetricsForTest() {
    g_durable_body_writes.store(0);
    g_concurrent_acceptances_suppressed.store(0);
    g_duplicate_logs_suppressed.store(0);
    g_sidechain_logs_suppressed.store(0);
}

}  // namespace daemon
}  // namespace dinero
