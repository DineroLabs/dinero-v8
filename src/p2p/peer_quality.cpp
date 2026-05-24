#include "p2p/peer_quality.h"

#include <algorithm>

namespace dinero {
namespace p2p {

int PeerQuality::ClampScore(int score) {
    return std::clamp(score, 0, 100);
}

void PeerQuality::Apply(PeerQualityEvent event) {
    switch (event) {
    case PeerQualityEvent::ConnectionSuccess:
        ++connection_successes_;
        score_ = ClampScore(score_ + 6);
        break;
    case PeerQualityEvent::ConnectionFailure:
        ++connection_failures_;
        score_ = ClampScore(score_ - 10);
        break;
    case PeerQualityEvent::HandshakeSuccess:
        ++handshake_successes_;
        score_ = ClampScore(score_ + 8);
        break;
    case PeerQualityEvent::HandshakeFailure:
        ++handshake_failures_;
        score_ = ClampScore(score_ - 15);
        break;
    case PeerQualityEvent::UsefulHeader:
        ++useful_headers_;
        score_ = ClampScore(score_ + 3);
        break;
    case PeerQualityEvent::UsefulBlock:
        ++useful_blocks_;
        score_ = ClampScore(score_ + 5);
        break;
    case PeerQualityEvent::StaleHeight:
        ++stale_height_events_;
        score_ = ClampScore(score_ - 6);
        break;
    case PeerQualityEvent::RelaySuccess:
        ++relay_successes_;
        score_ = ClampScore(score_ + 7);
        break;
    case PeerQualityEvent::RelayFailure:
        ++relay_failures_;
        score_ = ClampScore(score_ - 9);
        break;
    }
}

void PeerQuality::RecordLatency(uint32_t latency_ms) {
    latency_ms_ = latency_ms;
    if (latency_ms == 0) {
        return;
    }
    if (latency_ms <= 150) {
        score_ = ClampScore(score_ + 2);
    } else if (latency_ms >= 1500) {
        score_ = ClampScore(score_ - 4);
    }
}

void PeerQuality::DecayTowardNeutral() {
    if (score_ > 50) {
        --score_;
    } else if (score_ < 50) {
        ++score_;
    }
}

PeerQualitySnapshot PeerQuality::Snapshot() const {
    PeerQualitySnapshot snapshot;
    snapshot.score = score_;
    snapshot.connection_successes = connection_successes_;
    snapshot.connection_failures = connection_failures_;
    snapshot.handshake_successes = handshake_successes_;
    snapshot.handshake_failures = handshake_failures_;
    snapshot.useful_headers = useful_headers_;
    snapshot.useful_blocks = useful_blocks_;
    snapshot.stale_height_events = stale_height_events_;
    snapshot.relay_successes = relay_successes_;
    snapshot.relay_failures = relay_failures_;
    snapshot.latency_ms = latency_ms_;
    snapshot.hot_peer_candidate =
        score_ >= 60 && handshake_successes_ > 0 && stale_height_events_ <= useful_headers_ + useful_blocks_;
    snapshot.relay_candidate =
        score_ >= 55 && relay_successes_ > 0 && relay_failures_ <= relay_successes_;
    return snapshot;
}

} // namespace p2p
} // namespace dinero

