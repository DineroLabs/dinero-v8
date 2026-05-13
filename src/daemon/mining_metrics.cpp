#include "daemon/mining_metrics.h"
#include <ctime>
#include <iomanip>
#include <sstream>

namespace dinero {

MiningMetrics::MiningMetrics() {
    // initialize buckets to "empty"
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (auto& b : buckets_) { 
        b.sec = now - WINDOW_SEC - 1; 
        b.count = 0; 
    }
}

void MiningMetrics::recordHashAttempts(uint64_t n) {
    const int64_t nowSec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const int idx = static_cast<int>(nowSec % WINDOW_SEC);
    std::lock_guard<std::mutex> lk(mtx_);
    Bucket& b = buckets_[idx];
    if (b.sec != nowSec) {  // new second ⇒ reset bucket
        b.sec = nowSec;
        b.count = n;
    } else {
        b.count += n;
    }
}

void MiningMetrics::onBlockFound(int height, int64_t unixTimeSeconds) {
    blocksFound_.fetch_add(1, std::memory_order_relaxed);
    lastFoundHeight_.store(height, std::memory_order_relaxed);
    lastFoundUnix_.store(unixTimeSeconds, std::memory_order_relaxed);
}

void MiningMetrics::setCurrentBits(uint32_t bits) {
    bits_.store(bits, std::memory_order_relaxed);
}

void MiningMetrics::setRunning(bool r) {
    running_.store(r, std::memory_order_relaxed);
}

uint64_t MiningMetrics::hashrateHps() const {
    const int64_t nowSec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    uint64_t total = 0;
    int64_t minSec = nowSec - WINDOW_SEC + 1;

    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& b : buckets_) {
        if (b.sec >= minSec && b.sec <= nowSec) {
            total += b.count;
        }
    }
    // average over observed window (avoid div by zero)
    const int span = WINDOW_SEC;
    return span > 0 ? (total / static_cast<uint64_t>(span)) : total;
}

std::string MiningMetrics::iso8601FromUnix(int64_t t) {
    if (t <= 0) return {};
    std::tm tm{};
    time_t time_val = static_cast<time_t>(t);
#if defined(_WIN32)
    gmtime_s(&tm, &time_val);
#else
    gmtime_r(&time_val, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string MiningMetrics::lastBlockTimeISO8601() const {
    return iso8601FromUnix(lastFoundUnix_.load(std::memory_order_relaxed));
}

} // namespace dinero
