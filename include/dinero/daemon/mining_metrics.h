#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <chrono>

namespace dinero {

class MiningMetrics {
public:
    MiningMetrics();

    // --- called by your miner threads ---
    // Add N hash attempts (call once per inner loop or per batch).
    void recordHashAttempts(uint64_t n = 1);

    // Call when a block is found.
    void onBlockFound(int height, int64_t unixTimeSeconds);

    // Update the compact target bits currently being mined.
    void setCurrentBits(uint32_t bits);

    // Update running state.
    void setRunning(bool r);

    // --- queried by RPC/GUI ---
    bool isRunning() const { return running_.load(std::memory_order_relaxed); }
    uint32_t currentBits() const { return bits_.load(std::memory_order_relaxed); }
    uint64_t blocksFound() const { return blocksFound_.load(std::memory_order_relaxed); }

    // Rolling hashrate over the last WINDOW_SEC seconds (default 10s).
    uint64_t hashrateHps() const;

    // ISO8601 string of the last found block time, or "" if none.
    std::string lastBlockTimeISO8601() const;

private:
    static constexpr int WINDOW_SEC = 10;
    struct Bucket {
        int64_t sec = 0;      // wall-clock second
        uint64_t count = 0;   // hash attempts recorded during 'sec'
    };

    // ring of per-second buckets
    mutable std::mutex mtx_;
    std::array<Bucket, WINDOW_SEC> buckets_{};

    std::atomic<bool> running_{false};
    std::atomic<uint32_t> bits_{0};
    std::atomic<uint64_t> blocksFound_{0};
    std::atomic<int> lastFoundHeight_{-1};
    std::atomic<int64_t> lastFoundUnix_{0};

    static std::string iso8601FromUnix(int64_t t);
};

} // namespace dinero
