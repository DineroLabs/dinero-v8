#pragma once

#include <cstdint>

namespace dinero {
namespace p2p {

// Positive peer usefulness model for Dynamic P2P. This is intentionally
// separate from PeerScoringManager, which handles misbehavior and bans.
enum class PeerQualityEvent {
    ConnectionSuccess,
    ConnectionFailure,
    HandshakeSuccess,
    HandshakeFailure,
    UsefulHeader,
    UsefulBlock,
    StaleHeight,
    RelaySuccess,
    RelayFailure,
};

struct PeerQualitySnapshot {
    int score{50};
    uint32_t connection_successes{0};
    uint32_t connection_failures{0};
    uint32_t handshake_successes{0};
    uint32_t handshake_failures{0};
    uint32_t useful_headers{0};
    uint32_t useful_blocks{0};
    uint32_t stale_height_events{0};
    uint32_t relay_successes{0};
    uint32_t relay_failures{0};
    uint32_t latency_ms{0};
    bool hot_peer_candidate{false};
    bool relay_candidate{false};
};

class PeerQuality {
public:
    void Apply(PeerQualityEvent event);
    void RecordLatency(uint32_t latency_ms);
    void DecayTowardNeutral();

    PeerQualitySnapshot Snapshot() const;

private:
    static int ClampScore(int score);

    int score_{50};
    uint32_t connection_successes_{0};
    uint32_t connection_failures_{0};
    uint32_t handshake_successes_{0};
    uint32_t handshake_failures_{0};
    uint32_t useful_headers_{0};
    uint32_t useful_blocks_{0};
    uint32_t stale_height_events_{0};
    uint32_t relay_successes_{0};
    uint32_t relay_failures_{0};
    uint32_t latency_ms_{0};
};

} // namespace p2p
} // namespace dinero

