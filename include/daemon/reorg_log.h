// include/daemon/reorg_log.h
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DINERO_DAEMON_REORG_LOG_H
#define DINERO_DAEMON_REORG_LOG_H

#include <chrono>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace dinero {

struct ReorgEvent {
    uint64_t seq = 0;
    std::string timestamp;   // ISO 8601, UTC
    uint32_t disconnected = 0;
    uint32_t connected = 0;
};

/**
 * In-memory record of chain reorganisations.
 *
 * Deliberately self-contained and header-only: the call site is inside
 * chainstate_service.cpp, the riskiest file in the daemon, and keeping the
 * logic here means it can be tested without building the daemon at all.
 *
 * Record() is called on the chain-activation path. It must never throw and
 * never block on anything slow — it takes a mutex, pushes four integers and a
 * short string, and returns. It observes a decision already made and can never
 * change one.
 *
 * Total() is the process-lifetime count and is NOT the ring length. A consumer
 * that sees Total() advance further than the events it can account for knows
 * the ring overflowed. That is the only overflow signal, which is why the
 * counter is exposed separately.
 */
class ReorgLog {
public:
    static constexpr size_t kCapacity = 64;

    ReorgLog() : boot_id_(MakeBootId()) {}

    void Record(uint32_t disconnected, uint32_t connected) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            ReorgEvent event;
            event.seq = ++total_;
            event.timestamp = NowIso8601();
            event.disconnected = disconnected;
            event.connected = connected;
            events_.push_back(std::move(event));
            while (events_.size() > kCapacity) events_.pop_front();
        } catch (...) {
            // A failure to record an observation must never disturb chain
            // activation. Dropping the event is the correct outcome; Total()
            // then outruns the ring, which is exactly how a consumer learns
            // something was lost.
        }
    }

    uint64_t Total() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_;
    }

    std::vector<ReorgEvent> Events() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::vector<ReorgEvent>(events_.begin(), events_.end());
    }

    const std::string& BootId() const { return boot_id_; }

private:
    static std::string NowIso8601() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        std::ostringstream out;
        out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return out.str();
    }

    static std::string MakeBootId() {
        std::random_device rd;
        std::ostringstream out;
        for (int i = 0; i < 4; ++i) {
            out << std::hex << std::setw(8) << std::setfill('0') << rd();
        }
        return out.str();
    }

    mutable std::mutex mutex_;
    std::deque<ReorgEvent> events_;
    uint64_t total_ = 0;
    const std::string boot_id_;
};

}  // namespace dinero

#endif  // DINERO_DAEMON_REORG_LOG_H
