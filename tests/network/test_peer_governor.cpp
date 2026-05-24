#include "p2p/peer_governor.h"

#include <cstdio>
#include <string>
#include <vector>

using dinero::p2p::PeerGovernor;
using dinero::p2p::PeerGovernorCandidate;
using dinero::p2p::PeerGovernorConfig;
using dinero::p2p::PeerQualitySnapshot;

static int g_fails = 0;

static void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fails;
}

static PeerQualitySnapshot q(int score,
                             bool hot = false,
                             bool relay = false,
                             uint32_t latency = 100) {
    PeerQualitySnapshot s;
    s.score = score;
    s.hot_peer_candidate = hot;
    s.relay_candidate = relay;
    s.latency_ms = latency;
    return s;
}

static PeerGovernorCandidate candidate(const char* endpoint,
                                       int score,
                                       bool connected,
                                       bool outbound,
                                       bool configured_seed,
                                       bool relay_capable,
                                       bool hot = false,
                                       bool relay = false,
                                       uint32_t latency = 100) {
    PeerGovernorCandidate c;
    c.endpoint = endpoint;
    c.quality = q(score, hot, relay, latency);
    c.connected = connected;
    c.outbound = outbound;
    c.configured_seed = configured_seed;
    c.relay_capable = relay_capable;
    return c;
}

static bool has(const std::vector<std::string>& values, const char* needle) {
    for (const auto& value : values) {
        if (value == needle) return true;
    }
    return false;
}

int main() {
    {
        PeerGovernorConfig config;
        config.target_hot_outbound = 4;
        config.max_configured_seed_hot = 1;
        PeerGovernor governor(config);

        std::vector<PeerGovernorCandidate> peers = {
            candidate("va:20999", 95, true, true, true, true),
            candidate("la:20999", 90, true, true, true, true),
            candidate("home-a:20999", 80, true, true, false, false),
            candidate("home-b:20999", 70, true, true, false, false),
            candidate("home-c:20999", 65, true, true, false, false),
        };

        auto decision = governor.Evaluate(peers);
        check(decision.hot_peers.size() == 4, "keeps target hot outbound count");
        check(decision.configured_seed_hot == 1, "caps configured seed hot peers");
        check(has(decision.hot_peers, "va:20999"), "keeps best configured seed");
        check(!has(decision.hot_peers, "la:20999"), "skips second configured seed when cap reached");
        check(has(decision.hot_peers, "home-a:20999"), "keeps discovered peer A");
    }

    {
        PeerGovernorConfig config;
        config.target_warm_standby = 2;
        PeerGovernor governor(config);

        std::vector<PeerGovernorCandidate> peers = {
            candidate("warm-a:20999", 77, false, false, false, false, true),
            candidate("warm-b:20999", 82, false, false, false, false, true),
            candidate("warm-c:20999", 75, false, false, false, false, true),
            candidate("cold-a:20999", 50, false, false, false, false, false),
        };

        auto decision = governor.Evaluate(peers);
        check(decision.warm_candidates.size() == 2, "limits warm candidates");
        check(decision.warm_candidates[0] == "warm-b:20999", "sorts warm candidates by quality");
        check(!has(decision.warm_candidates, "cold-a:20999"), "does not warm unqualified cold peer");
    }

    {
        PeerGovernorConfig config;
        config.target_relay_registrations = 2;
        PeerGovernor governor(config);

        std::vector<PeerGovernorCandidate> peers = {
            candidate("relay-a:20999", 75, true, true, false, true, true, true, 220),
            candidate("relay-b:20999", 85, true, true, false, true, true, true, 140),
            candidate("relay-c:20999", 70, true, true, false, true, true, false),
        };

        auto decision = governor.Evaluate(peers);
        check(decision.relay_capable_seen == 3, "counts relay-capable peers");
        check(decision.relay_registration_candidates.size() == 2, "limits relay registrations");
        check(decision.relay_registration_candidates[0] == "relay-b:20999",
              "prefers best relay candidate");
        check(!has(decision.relay_registration_candidates, "relay-c:20999"),
              "requires relay quality candidate flag");
    }

    {
        PeerGovernorConfig config;
        config.demote_score_threshold = 35;
        PeerGovernor governor(config);

        std::vector<PeerGovernorCandidate> peers = {
            candidate("weak-a:20999", 20, true, true, false, false),
            candidate("weak-b:20999", 35, true, true, false, false),
            candidate("ok-a:20999", 36, true, true, false, false),
        };

        auto decision = governor.Evaluate(peers);
        check(has(decision.demote_candidates, "weak-a:20999"), "demotes low score peer");
        check(has(decision.demote_candidates, "weak-b:20999"), "demotes threshold score peer");
        check(!has(decision.demote_candidates, "ok-a:20999"), "keeps peer above threshold");
    }

    if (g_fails) {
        std::printf("\n%d CHECK(S) FAILED\n", g_fails);
        return 1;
    }
    std::printf("\nALL PEER GOVERNOR CHECKS PASSED\n");
    return 0;
}

