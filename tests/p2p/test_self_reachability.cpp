// SelfReachability decides which of our candidate addresses other nodes have
// proven they can dial. Only verified addresses may be gossiped, so these
// cases pin the rules that keep an unreachable address from ever being
// advertised: distinct verifier networks, a time window, periodic re-checks.

#include "p2p/self_reachability.h"

#include <gtest/gtest.h>

#include <chrono>

using namespace dinero::p2p;
using namespace std::chrono_literals;

namespace {

struct FakeClock {
    std::chrono::steady_clock::time_point now{std::chrono::steady_clock::time_point{} + 1000h};
    SelfReachability::Clock fn() { return [this] { return now; }; }
};

const Candidate kAddr{"203.0.113.5", 20999, Transport::Tcp, CandidateSource::PeerVote};
Verifier V(const char* ip) { return Verifier{std::string(ip) + ":20999", ip}; }

void Ok(SelfReachability& r, const Verifier& v) { r.OnResult(kAddr, v, 0); }
void Fail(SelfReachability& r, const Verifier& v) { r.OnResult(kAddr, v, 2); }

}  // namespace

TEST(SelfReachability, TwoDistinctGroupsVerify) {
    FakeClock c;
    SelfReachability r({}, c.fn());
    r.AddCandidate(kAddr);
    EXPECT_FALSE(r.IsVerified(kAddr));
    Ok(r, V("1.2.3.4"));
    EXPECT_FALSE(r.IsVerified(kAddr));
    Ok(r, V("5.6.7.8"));
    EXPECT_TRUE(r.IsVerified(kAddr));
    EXPECT_TRUE(r.AnyVerified(Transport::Tcp));
    EXPECT_FALSE(r.AnyVerified(Transport::Quic));
}

TEST(SelfReachability, SameGroupVerifiersDoNotVerify) {
    FakeClock c;
    SelfReachability r({}, c.fn());
    r.AddCandidate(kAddr);
    Ok(r, V("1.2.3.4"));
    Ok(r, V("1.2.9.9"));  // same /16
    EXPECT_FALSE(r.IsVerified(kAddr));
}

TEST(SelfReachability, OkOutsideWindowDoesNotCount) {
    FakeClock c;
    SelfReachability r({}, c.fn());
    r.AddCandidate(kAddr);
    Ok(r, V("1.2.3.4"));
    c.now += 11min;
    Ok(r, V("5.6.7.8"));
    EXPECT_FALSE(r.IsVerified(kAddr));
}

TEST(SelfReachability, RecheckAfter30MinAndDropAfterTwoFailedRounds) {
    FakeClock c;
    SelfReachability r({}, c.fn());
    r.AddCandidate(kAddr);
    Ok(r, V("1.2.3.4"));
    Ok(r, V("5.6.7.8"));
    ASSERT_TRUE(r.IsVerified(kAddr));
    const std::vector<Verifier> vs{V("1.2.3.4"), V("5.6.7.8"), V("9.9.1.1")};
    EXPECT_TRUE(r.PlanChecks(vs).empty()) << "re-checked before 30 min";
    for (int round = 0; round < 2; ++round) {
        c.now += 31min;
        const auto plan = r.PlanChecks(vs);
        ASSERT_FALSE(plan.empty()) << "no re-check scheduled in round " << round;
        EXPECT_TRUE(r.IsVerified(kAddr)) << "dropped before two failed rounds";
        for (const auto& [cand, v] : plan) Fail(r, v);
    }
    EXPECT_FALSE(r.IsVerified(kAddr)) << "still verified after two failed rounds";
}

TEST(SelfReachability, PlanChecksNeverPairsTwoVerifiersFromOneGroup) {
    FakeClock c;
    SelfReachability r({}, c.fn());
    r.AddCandidate(kAddr);
    const auto plan = r.PlanChecks({V("1.2.3.4"), V("1.2.5.6"), V("1.2.7.8"), V("5.6.7.8")});
    ASSERT_EQ(plan.size(), 2u);
    EXPECT_NE(plan[0].second.ip.substr(0, 3), plan[1].second.ip.substr(0, 3));
}

TEST(SelfReachability, NetworkChangeRequeuesVerified) {
    FakeClock c;
    SelfReachability r({}, c.fn());
    r.AddCandidate(kAddr);
    Ok(r, V("1.2.3.4"));
    Ok(r, V("5.6.7.8"));
    ASSERT_TRUE(r.IsVerified(kAddr));
    r.OnNetworkChange();
    EXPECT_FALSE(r.IsVerified(kAddr));
    EXPECT_FALSE(r.PlanChecks({V("1.2.3.4"), V("5.6.7.8")}).empty());
}

TEST(SelfReachability, MinGroupsOneForRegtest) {
    FakeClock c;
    SelfReachability::Params p;
    p.min_groups = 1;
    SelfReachability r(p, c.fn());
    r.AddCandidate(kAddr);
    Ok(r, V("127.0.0.1"));
    EXPECT_TRUE(r.IsVerified(kAddr));
}

TEST(SelfReachability, PlanChecksOnlyForRequestedTransport) {
    // STUN's reflexive address is a QUIC candidate; until verifiers can dial
    // QUIC back, planning it would only burn their rate budget.
    FakeClock c;
    SelfReachability r({}, c.fn());
    const Candidate quic{"203.0.113.5", 20999, Transport::Quic, CandidateSource::Stun};
    r.AddCandidate(kAddr);
    r.AddCandidate(quic);
    const std::vector<Verifier> vs = {V("1.2.3.4"), V("5.6.7.8")};
    auto plan = r.PlanChecks(vs, Transport::Tcp);
    ASSERT_EQ(plan.size(), 2u);
    for (const auto& [cand, v] : plan) EXPECT_EQ(cand.transport, Transport::Tcp);
    // The skipped candidate has no round open: it is still plannable later.
    auto later = r.PlanChecks(vs, Transport::Quic);
    ASSERT_EQ(later.size(), 2u);
    EXPECT_EQ(later[0].first.transport, Transport::Quic);
}

TEST(SelfReachability, UnverifiedCandidateBacksOff) {
    // A NAT'd node's gateway can never verify; asking the same seeds every
    // 30 s forever would spend their whole dial-back budget.
    FakeClock c;
    SelfReachability r({}, c.fn());
    r.AddCandidate(kAddr);
    const std::vector<Verifier> vs{V("1.2.3.4"), V("5.6.7.8")};
    std::chrono::seconds gap{30};
    for (int round = 0; round < 4; ++round) {
        auto plan = r.PlanChecks(vs);
        ASSERT_FALSE(plan.empty()) << "round " << round;
        for (const auto& [cand, v] : plan) Fail(r, v);
        c.now += gap - 1s;
        EXPECT_TRUE(r.PlanChecks(vs).empty()) << "retried before backoff " << gap.count() << "s";
        c.now += 1s;
        gap *= 2;
    }
}

TEST(SelfReachability, BackoffIsCappedAt30Minutes) {
    FakeClock c;
    SelfReachability r({}, c.fn());
    r.AddCandidate(kAddr);
    const std::vector<Verifier> vs{V("1.2.3.4"), V("5.6.7.8")};
    for (int round = 0; round < 12; ++round) {
        auto plan = r.PlanChecks(vs);
        for (const auto& [cand, v] : plan) Fail(r, v);
        c.now += 31min;
    }
    EXPECT_FALSE(r.PlanChecks(vs).empty());
}

TEST(SelfReachability, RefusalsAreNotFailures) {
    // "Busy" (1) and "unsupported" (4) say nothing about our address: two such
    // rounds at recheck must not un-verify a reachable node.
    FakeClock c;
    SelfReachability r({}, c.fn());
    r.AddCandidate(kAddr);
    Ok(r, V("1.2.3.4"));
    Ok(r, V("5.6.7.8"));
    ASSERT_TRUE(r.IsVerified(kAddr));
    const std::vector<Verifier> vs{V("1.2.3.4"), V("5.6.7.8")};
    for (int round = 0; round < 3; ++round) {
        c.now += 31min;
        auto plan = r.PlanChecks(vs);
        ASSERT_FALSE(plan.empty());
        for (const auto& [cand, v] : plan) r.OnResult(kAddr, v, round % 2 ? 4 : 1);
    }
    EXPECT_TRUE(r.IsVerified(kAddr));
}

TEST(SelfReachability, FailingVerifierIsRotatedOut) {
    FakeClock c;
    SelfReachability::Params p;
    p.min_groups = 1;
    SelfReachability r(p, c.fn());
    r.AddCandidate(kAddr);
    const std::vector<Verifier> vs{V("1.2.3.4"), V("5.6.7.8"), V("9.9.9.9")};
    auto first = r.PlanChecks(vs);
    ASSERT_EQ(first.size(), 1u);
    Fail(r, first[0].second);
    c.now += 1h;
    auto second = r.PlanChecks(vs);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_NE(second[0].second.ip, first[0].second.ip) << "the verifier that just failed was asked again";
}

TEST(SelfReachability, VerifierMustShareCandidateAddressFamily) {
    // A verifier connected over IPv6 dials our v6 address, never the v4 one.
    FakeClock c;
    SelfReachability r({}, c.fn());
    r.AddCandidate(kAddr);
    EXPECT_TRUE(r.PlanChecks({Verifier{"[2600::1]:20999", "2600::1"},
                              Verifier{"[2a00::1]:20999", "2a00::1"}}).empty());
    EXPECT_EQ(r.PlanChecks({Verifier{"[2600::1]:20999", "2600::1"}, V("1.2.3.4"), V("5.6.7.8")}).size(), 2u);
}

TEST(SelfReachability, OneNodeOverTwoNetworksCountsOnce) {
    // One dual-stack host is one witness, however many groups its
    // connections sit in.
    FakeClock c;
    SelfReachability r({}, c.fn());
    r.AddCandidate(kAddr);
    r.OnResult(kAddr, Verifier{"1.2.3.4:20999", "1.2.3.4", "node-x"}, 0);
    r.OnResult(kAddr, Verifier{"5.6.7.8:20999", "5.6.7.8", "node-x"}, 0);
    EXPECT_FALSE(r.IsVerified(kAddr));
    r.OnResult(kAddr, Verifier{"9.9.9.9:20999", "9.9.9.9", "node-y"}, 0);
    EXPECT_TRUE(r.IsVerified(kAddr));
}

TEST(SelfReachability, RepeatedFailedRoundsShowUnverified) {
    // A candidate nobody can reach reports "unverified", not "verifying"
    // forever (the status line reads this).
    FakeClock c;
    SelfReachability r({}, c.fn());
    r.AddCandidate(kAddr);
    const std::vector<Verifier> vs{V("1.2.3.4"), V("5.6.7.8")};
    for (int round = 0; round < 2; ++round) {
        for (const auto& [cand, v] : r.PlanChecks(vs)) Fail(r, v);
        c.now += 1h;
    }
    ASSERT_EQ(r.Status().size(), 1u);
    EXPECT_EQ(r.Status()[0].state, VerifyState::Unverified);
}

TEST(SelfReachability, CandidatesAreCapped) {
    // Each candidate costs verifier budget; voted-in addresses cannot pile up.
    FakeClock c;
    SelfReachability r({}, c.fn());
    for (int i = 0; i < 100; ++i) {
        r.AddCandidate(Candidate{"44.0.0." + std::to_string(i), 20999, Transport::Tcp, CandidateSource::PeerVote});
    }
    EXPECT_EQ(r.Status().size(), SelfReachability::Params{}.max_candidates);
    // An existing candidate is still accepted (idempotent), a new one is not.
    r.AddCandidate(Candidate{"44.0.0.0", 20999, Transport::Tcp, CandidateSource::PeerVote});
    EXPECT_EQ(r.Status().size(), SelfReachability::Params{}.max_candidates);
}
