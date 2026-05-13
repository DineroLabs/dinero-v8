// Per-peer reachability tracking. The seeder probes each known peer on
// a loop and tracks success ratio, last-success, last-failure, etc.
// This state is persisted to a JSON-ish flat text file (one peer per
// line) so the seeder can resume across restarts.
//
// Scoring shape (kept simple for v8.0.0 — Bitcoin's seeder has 16
// statistical buckets and an exponential-decay reliability score; we
// can iterate to that if observed-network warrants):
//
//   - "healthy" = handshook successfully within `healthy_window`
//     (default: 24h) AND total success ratio over the lifetime of the
//     state file is >= `min_success_ratio` (default: 0.6).
//
// The seeder's output step (output.h) reads this state and emits the
// healthy subset to seeds_observed.txt.

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dinero {
namespace seeder {

struct PeerStats {
    std::string ip;
    uint16_t port = 0;

    int64_t first_seen_unix = 0;     // when we first heard about this peer
    int64_t last_attempt_unix = 0;   // when we last tried to probe
    int64_t last_success_unix = 0;   // when we last successfully handshook
    uint64_t attempts = 0;
    uint64_t successes = 0;
    std::string last_error;          // for diagnostics; cleared on success
    std::string last_user_agent;     // most recent UA observed
    uint32_t last_protocol_version = 0;
    uint32_t last_best_height = 0;

    double success_ratio() const {
        return attempts == 0
            ? 0.0
            : static_cast<double>(successes) / static_cast<double>(attempts);
    }

    std::string key() const { return ip + ":" + std::to_string(port); }
};

// Thread-safe state container. Single-threaded today, but the mutex
// makes it trivial to add parallel probe workers later without
// rethinking the data structure.
class PeerStore {
 public:
    void seed(const std::string& ip, uint16_t port);
    void record_attempt(const std::string& ip, uint16_t port);
    void record_success(const std::string& ip,
                        uint16_t port,
                        const std::string& user_agent,
                        uint32_t protocol_version,
                        uint32_t best_height);
    void record_failure(const std::string& ip,
                        uint16_t port,
                        const std::string& error_detail);

    // Snapshot the entire store for output emission or persistence.
    std::vector<PeerStats> snapshot() const;

    // Pick the next N peers due for re-probe, oldest-attempt first.
    // Newly-seeded peers (attempts == 0) sort to the front. Used by
    // the crawler's main loop to decide what to probe next.
    std::vector<PeerStats> select_for_probe(size_t n) const;

    // Persist + load. Flat text format, one peer per line:
    //   <ip> <port> first_seen last_attempt last_success attempts
    //   successes proto height ua_var_len <ua> err_var_len <err>
    // Header line "# DINERO_SEEDER_PEERS_V1".
    bool save(const std::string& path) const;
    bool load(const std::string& path);

    size_t size() const;

 private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, PeerStats> peers_;  // keyed by "ip:port"
};

}  // namespace seeder
}  // namespace dinero
