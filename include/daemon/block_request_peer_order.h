#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dinero::daemon {

struct BlockRequestPeerCandidate {
    std::string key;
    double average_latency_ms{0.0};
};

struct BlockBodyPeerAvailability {
    std::string key;
    uint32_t advertised_height{0};
    bool archival_bridge{false};
};

// Identify archival bridge peers worth probing when every peer advertising the
// requested height has already failed to deliver its body. Ordinarily the
// height filter remains authoritative; this is only a liveness escape for
// stale peer height announcements.
inline std::vector<std::string> LaggingArchivalBridgeFallback(
        const std::vector<BlockBodyPeerAvailability>& peers,
        uint32_t target_height,
        const std::unordered_set<std::string>& unavailable) {
    if (target_height == 0) return {};
    bool advertised_capable = false;
    for (const auto& peer : peers) {
        if (peer.advertised_height < target_height) continue;
        advertised_capable = true;
        if (unavailable.count(peer.key) == 0) return {};
    }
    if (!advertised_capable) return {};

    std::vector<std::string> fallback;
    for (const auto& peer : peers) {
        if (peer.advertised_height < target_height && peer.archival_bridge &&
                unavailable.count(peer.key) == 0) {
            fallback.push_back(peer.key);
        }
    }
    return fallback;
}

// Deterministic peer ordering for one logical block request. Normal tip-sync
// requests retain identity order. AssumeUTXO backfill prefers measured low
// latency so slow remote peers do not occupy the entire fixed in-flight window
// (#299). Unknown/zero latency sorts last; identity breaks ties.
inline std::vector<std::string> OrderBlockRequestPeers(
        const std::vector<BlockRequestPeerCandidate>& candidates,
        bool prefer_low_latency) {
    std::unordered_map<std::string, double> best_latency;
    for (const auto& candidate : candidates) {
        const double latency = candidate.average_latency_ms > 0.0
            ? candidate.average_latency_ms
            : std::numeric_limits<double>::max();
        auto [it, inserted] = best_latency.emplace(candidate.key, latency);
        if (!inserted && latency < it->second) {
            it->second = latency;
        }
    }

    std::vector<std::string> ordered;
    ordered.reserve(best_latency.size());
    for (const auto& [key, _] : best_latency) {
        ordered.push_back(key);
    }
    std::sort(ordered.begin(), ordered.end(),
        [&best_latency, prefer_low_latency](const std::string& a,
                                            const std::string& b) {
            if (prefer_low_latency && best_latency.at(a) != best_latency.at(b)) {
                return best_latency.at(a) < best_latency.at(b);
            }
            return a < b;
        });
    return ordered;
}

}  // namespace dinero::daemon
