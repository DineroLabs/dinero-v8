#include "daemon/block_write_metrics.h"

#include <chrono>

namespace dinero {
namespace daemon {

std::atomic<uint64_t> g_durable_body_writes{0};
std::atomic<uint64_t> g_duplicate_body_writes_avoided{0};
std::atomic<uint64_t> g_duplicate_logs_suppressed{0};

bool ShouldEmitRateLimited(std::atomic<uint64_t>& state,
                           uint32_t interval_seconds) {
    const auto now_s = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    uint64_t last = state.load(std::memory_order_relaxed);
    // 0 means "never emitted", so the first call always emits.
    if (last != 0 && now_s - last < interval_seconds) {
        return false;
    }
    // Only one thread wins the slot; the rest are suppressed for this interval.
    return state.compare_exchange_strong(last, now_s,
                                         std::memory_order_relaxed);
}

void ResetBlockWriteMetricsForTest() {
    g_durable_body_writes.store(0);
    g_duplicate_body_writes_avoided.store(0);
    g_duplicate_logs_suppressed.store(0);
}

}  // namespace daemon
}  // namespace dinero
