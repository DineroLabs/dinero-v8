#include "p2p/peer_quality.h"

#include <cstdio>

using dinero::p2p::PeerQuality;
using dinero::p2p::PeerQualityEvent;

static int g_fails = 0;

static void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fails;
}

int main() {
    {
        PeerQuality quality;
        auto snap = quality.Snapshot();
        check(snap.score == 50, "starts neutral");
        check(!snap.hot_peer_candidate, "neutral peer is not hot yet");
        check(!snap.relay_candidate, "neutral peer is not a relay candidate");
    }

    {
        PeerQuality quality;
        quality.Apply(PeerQualityEvent::ConnectionSuccess);
        quality.Apply(PeerQualityEvent::HandshakeSuccess);
        quality.Apply(PeerQualityEvent::UsefulHeader);
        quality.Apply(PeerQualityEvent::UsefulBlock);
        quality.RecordLatency(90);
        auto snap = quality.Snapshot();
        check(snap.score > 60, "useful peer rises above hot threshold");
        check(snap.hot_peer_candidate, "useful peer becomes hot candidate");
        check(snap.handshake_successes == 1, "handshake success counted");
        check(snap.useful_blocks == 1, "useful block counted");
    }

    {
        PeerQuality quality;
        quality.Apply(PeerQualityEvent::ConnectionSuccess);
        quality.Apply(PeerQualityEvent::HandshakeSuccess);
        quality.Apply(PeerQualityEvent::RelaySuccess);
        auto snap = quality.Snapshot();
        check(snap.relay_candidate, "relay success makes peer a relay candidate");

        quality.Apply(PeerQualityEvent::RelayFailure);
        quality.Apply(PeerQualityEvent::RelayFailure);
        snap = quality.Snapshot();
        check(!snap.relay_candidate, "relay failures can demote relay candidate");
    }

    {
        PeerQuality quality;
        for (int i = 0; i < 20; ++i) {
            quality.Apply(PeerQualityEvent::HandshakeFailure);
        }
        auto snap = quality.Snapshot();
        check(snap.score == 0, "score clamps at zero");
        for (int i = 0; i < 20; ++i) {
            quality.DecayTowardNeutral();
        }
        snap = quality.Snapshot();
        check(snap.score == 20, "decay moves bad score toward neutral gradually");
    }

    if (g_fails) {
        std::printf("\n%d CHECK(S) FAILED\n", g_fails);
        return 1;
    }
    std::printf("\nALL PEER QUALITY CHECKS PASSED\n");
    return 0;
}

