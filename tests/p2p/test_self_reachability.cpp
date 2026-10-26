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
