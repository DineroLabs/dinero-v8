// include/daemon/reorg_log.h
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DINERO_DAEMON_REORG_LOG_H
#define DINERO_DAEMON_REORG_LOG_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define DINERO_GETPID _getpid
#else
#include <unistd.h>
#define DINERO_GETPID getpid
#endif

namespace dinero {

// Named ReorgLogEvent (not ReorgEvent) to avoid an ODR collision with the
// unrelated dinero::ReorgEvent in include/wallet/reorg_event.h (wallet-side
// reorg detection). Both are in namespace dinero; chainstate_service.h now
// pulls this header in, and several translation units already include both.
struct ReorgLogEvent {
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
            // Format the timestamp BEFORE taking the lock. put_time does
            // locale/facet work through an ostringstream, which is by far the
            // slowest thing here, and this mutex is held against the chain-
            // activation path. Nothing inside the critical section may be slow.
            std::string stamp = NowIso8601();

            std::lock_guard<std::mutex> lock(mutex_);
            ReorgLogEvent event;
            event.seq = ++total_;
            event.timestamp = std::move(stamp);
            event.disconnected = disconnected;
            event.connected = connected;
            events_.push_back(std::move(event));
            while (events_.size() > kCapacity) {
                events_.pop_front();
            }
        } catch (...) {
            // A failure to record an observation must never disturb chain
            // activation. Dropping the event is the correct outcome; Total()
            // then outruns the ring, which is exactly how a consumer learns
            // something was lost.
        }
    }

    /// The counter and the ring read together, under ONE lock.
    ///
    /// This is the accessor callers should use. Reading Total() and Events()
    /// separately allows a Record() to land between them, so the total would
    /// exceed the events a consumer can account for with no overflow having
    /// occurred — and that comparison is the design's ONLY overflow signal, so
    /// the false positive lands exactly on the thing it exists to detect.
    struct Snapshot {
        std::string boot_id;
        uint64_t total = 0;
        std::vector<ReorgLogEvent> events;
    };

    Snapshot Take() const {
        Snapshot snapshot;
        snapshot.boot_id = boot_id_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot.total = total_;
            snapshot.events.assign(events_.begin(), events_.end());
        }
        return snapshot;
    }

    // Total() and Events() were deliberately removed (they read the counter
    // and ring under SEPARATE locks, which is the exact false-overflow-signal
    // hazard documented on Take() above). Take() is the only accessor now;
    // every caller — the RPC handler and the unit tests — reads through it.

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
        // random_device alone is not enough. [rand.device] explicitly permits
        // an implementation to substitute a deterministic engine when it cannot
        // produce non-deterministic values — and the property actually required
        // here is distinctness ACROSS PROCESSES, so that a consumer can tell a
        // restart from data loss. Mixing in the pid and a high-resolution clock
        // read makes that hold even where random_device does not.
        const auto pid = static_cast<uint64_t>(DINERO_GETPID());
        const auto tick = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::ostringstream out;
        out << std::hex << std::setw(16) << std::setfill('0') << (pid ^ tick);

        // random_device's constructor and operator() are both permitted to
        // throw (the entropy source can be unavailable — a sandboxed or
        // /dev/urandom-less environment). ReorgLog's constructor is NOT
        // noexcept, and ChainstateService now owns a ReorgLog member, so an
        // uncaught throw here would abort daemon startup over what should be
        // a best-effort entropy mix-in. On failure, fall back to wall-clock
        // time (system_clock — a different clock from the steady_clock tick
        // already mixed in above) combined with the pid: this still gives
        // CROSS-PROCESS distinctness, not just non-emptiness, without a fixed
        // literal that would collide between any two processes that hit this
        // fallback at the same time.
        try {
            std::random_device rd;
            for (int i = 0; i < 2; ++i) {
                out << std::hex << std::setw(8) << std::setfill('0') << rd();
            }
        } catch (...) {
            const auto wall = static_cast<uint64_t>(
                std::chrono::system_clock::now().time_since_epoch().count());
            out << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<uint32_t>(wall & 0xffffffffu);
            out << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<uint32_t>((wall >> 32) ^ pid);
        }
        return out.str();
    }

    mutable std::mutex mutex_;
    std::deque<ReorgLogEvent> events_;
    uint64_t total_ = 0;
    const std::string boot_id_;
};

}  // namespace dinero

// Leaked no further than necessary: DINERO_GETPID's last use is MakeBootId()
// above. Left defined, it would leak into every translation unit that
// includes chainstate_service.h (which pulls this header in) via one of the
// riskiest headers in the daemon.
#undef DINERO_GETPID

#endif  // DINERO_DAEMON_REORG_LOG_H
