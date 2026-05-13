/**
 * Health Check Test (Phase D.4 of Dinero Core 1.0)
 *
 * Verifies the pure ComputeHealthStatus() function maps inputs to
 * the OK/DEGRADED/FAILING schema and exit codes 0/1/2 specified in
 * docs/specs/dinero-core-1.0.md §10.
 *
 * The RPC handler that gathers real ChecksData from chainstate/p2p
 * is tested separately via end-to-end smoke; this test isolates the
 * decision logic.
 */

#include "daemon/health.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace dinero::health;

namespace {

// Default healthy snapshot: small positive tip-age, no safemode, plenty of
// peers, undo present, no recent FATALs.
ChecksData healthy() {
    ChecksData d;
    d.tip_height         = 12345;
    d.tip_age_seconds    = 60;       // 1 min — well under 30-min DEGRADED edge
    d.safemode_active    = false;
    d.peer_count         = 6;
    d.tip_undo_present   = true;
    d.fatal_in_last_5min = 0;
    return d;
}

// ─────────────────────────────────────────────────────────────────
// #1 — Healthy snapshot → OK / exit_code 0 / empty reason
// ─────────────────────────────────────────────────────────────────
void test01_HealthyIsOk() {
    auto r = ComputeHealthStatus(healthy());
    assert(r.status == Status::Ok);
    assert(r.exit_code == 0);
    assert(r.reason.empty());
}

// ─────────────────────────────────────────────────────────────────
// #2 — StatusToString returns the locked schema strings
// ─────────────────────────────────────────────────────────────────
void test02_StatusStringsLocked() {
    assert(std::string(StatusToString(Status::Ok)) == "OK");
    assert(std::string(StatusToString(Status::Degraded)) == "DEGRADED");
    assert(std::string(StatusToString(Status::Failing)) == "FAILING");
}

// ─────────────────────────────────────────────────────────────────
// #3 — Safemode active → FAILING (most-severe reason wins)
// ─────────────────────────────────────────────────────────────────
void test03_SafemodeActiveIsFailing() {
    auto d = healthy();
    d.safemode_active = true;
    auto r = ComputeHealthStatus(d);
    assert(r.status == Status::Failing);
    assert(r.exit_code == 2);
    assert(r.reason.find("safemode") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// #4 — tip_undo_present=false → FAILING (catches Apr 30 phantom-undo)
// ─────────────────────────────────────────────────────────────────
void test04_MissingTipUndoIsFailing() {
    auto d = healthy();
    d.tip_undo_present = false;
    auto r = ComputeHealthStatus(d);
    assert(r.status == Status::Failing);
    assert(r.exit_code == 2);
    assert(r.reason.find("undo") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// #5 — tip_age > 7200s (2h) → FAILING
// ─────────────────────────────────────────────────────────────────
void test05_VeryStaleTipIsFailing() {
    auto d = healthy();
    d.tip_age_seconds = kTipAgeThresholdFailingSec + 1;
    auto r = ComputeHealthStatus(d);
    assert(r.status == Status::Failing);
    assert(r.exit_code == 2);
    assert(r.reason.find("tip") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// #6 — tip_age > 1800s but <= 7200s → DEGRADED
// ─────────────────────────────────────────────────────────────────
void test06_StaleTipIsDegraded() {
    auto d = healthy();
    d.tip_age_seconds = kTipAgeThresholdDegradedSec + 1;  // 1801s, just over 30 min
    auto r = ComputeHealthStatus(d);
    assert(r.status == Status::Degraded);
    assert(r.exit_code == 1);
    assert(r.reason.find("tip") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// #7 — Boundary: tip_age == 1800s → exactly OK (threshold is exclusive)
// ─────────────────────────────────────────────────────────────────
void test07_TipAgeAtDegradedBoundaryIsOk() {
    auto d = healthy();
    d.tip_age_seconds = kTipAgeThresholdDegradedSec;  // exactly 1800s
    auto r = ComputeHealthStatus(d);
    assert(r.status == Status::Ok);
    assert(r.exit_code == 0);
}

// ─────────────────────────────────────────────────────────────────
// #8 — Boundary: tip_age == 7200s → DEGRADED (not FAILING; threshold exclusive)
// ─────────────────────────────────────────────────────────────────
void test08_TipAgeAtFailingBoundaryIsDegraded() {
    auto d = healthy();
    d.tip_age_seconds = kTipAgeThresholdFailingSec;  // exactly 7200s
    auto r = ComputeHealthStatus(d);
    assert(r.status == Status::Degraded);
    assert(r.exit_code == 1);
}

// ─────────────────────────────────────────────────────────────────
// #9 — peer_count < 3 → DEGRADED
// ─────────────────────────────────────────────────────────────────
void test09_LowPeersIsDegraded() {
    auto d = healthy();
    d.peer_count = 2;
    auto r = ComputeHealthStatus(d);
    assert(r.status == Status::Degraded);
    assert(r.exit_code == 1);
    assert(r.reason.find("peer") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// #10 — Boundary: peer_count == 3 → OK (>=3 is healthy)
// ─────────────────────────────────────────────────────────────────
void test10_PeerCountAtBoundaryIsOk() {
    auto d = healthy();
    d.peer_count = kPeerCountThreshold;
    auto r = ComputeHealthStatus(d);
    assert(r.status == Status::Ok);
    assert(r.exit_code == 0);
}

// ─────────────────────────────────────────────────────────────────
// #11 — fatal_in_last_5min > 0 → DEGRADED
// ─────────────────────────────────────────────────────────────────
void test11_RecentFatalIsDegraded() {
    auto d = healthy();
    d.fatal_in_last_5min = 1;
    auto r = ComputeHealthStatus(d);
    assert(r.status == Status::Degraded);
    assert(r.exit_code == 1);
    assert(r.reason.find("fatal") != std::string::npos ||
           r.reason.find("FATAL") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// #12 — FAILING wins over DEGRADED when both apply
// ─────────────────────────────────────────────────────────────────
void test12_FailingWinsOverDegraded() {
    auto d = healthy();
    d.safemode_active = true;     // FAILING signal
    d.peer_count      = 0;        // DEGRADED signal
    d.fatal_in_last_5min = 5;     // DEGRADED signal
    auto r = ComputeHealthStatus(d);
    assert(r.status == Status::Failing);
    assert(r.exit_code == 2);
    // Reason must reference the failing condition, not just degraded ones.
    assert(r.reason.find("safemode") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// #13 — Negative tip_age (clock skew, tip from future) → OK
//        — defensive: don't false-alarm on minor clock skew. Only
//        positive ages count toward staleness.
// ─────────────────────────────────────────────────────────────────
void test13_NegativeTipAgeIsOk() {
    auto d = healthy();
    d.tip_age_seconds = -10;     // tip's timestamp is 10 sec in the future
    auto r = ComputeHealthStatus(d);
    assert(r.status == Status::Ok);
    assert(r.exit_code == 0);
}

// ─────────────────────────────────────────────────────────────────
// #14 — Multiple DEGRADED signals → status DEGRADED, reason mentions
//        the first listed condition (deterministic order)
// ─────────────────────────────────────────────────────────────────
void test14_DegradedReasonIsDeterministic() {
    auto d = healthy();
    d.tip_age_seconds    = 2000;  // DEGRADED stale tip
    d.peer_count         = 1;     // DEGRADED low peers
    d.fatal_in_last_5min = 3;     // DEGRADED recent fatal
    auto r = ComputeHealthStatus(d);
    assert(r.status == Status::Degraded);
    assert(r.exit_code == 1);
    // Per spec precedence: stale-tip-degraded comes first.
    assert(r.reason.find("tip") != std::string::npos);
}

}  // namespace

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Health Check Test (Phase D.4)" << std::endl;
    std::cout << "========================================" << std::endl;

    test01_HealthyIsOk();
    std::cout << "  [✓] #1 healthy snapshot is OK" << std::endl;
    test02_StatusStringsLocked();
    std::cout << "  [✓] #2 StatusToString returns OK/DEGRADED/FAILING" << std::endl;
    test03_SafemodeActiveIsFailing();
    std::cout << "  [✓] #3 safemode active is FAILING" << std::endl;
    test04_MissingTipUndoIsFailing();
    std::cout << "  [✓] #4 missing tip undo is FAILING (Apr-30 regression)" << std::endl;
    test05_VeryStaleTipIsFailing();
    std::cout << "  [✓] #5 tip > 2h stale is FAILING" << std::endl;
    test06_StaleTipIsDegraded();
    std::cout << "  [✓] #6 tip > 30min stale is DEGRADED" << std::endl;
    test07_TipAgeAtDegradedBoundaryIsOk();
    std::cout << "  [✓] #7 tip age == 1800s is still OK" << std::endl;
    test08_TipAgeAtFailingBoundaryIsDegraded();
    std::cout << "  [✓] #8 tip age == 7200s is DEGRADED, not FAILING" << std::endl;
    test09_LowPeersIsDegraded();
    std::cout << "  [✓] #9 peers < 3 is DEGRADED" << std::endl;
    test10_PeerCountAtBoundaryIsOk();
    std::cout << "  [✓] #10 peers == 3 is OK" << std::endl;
    test11_RecentFatalIsDegraded();
    std::cout << "  [✓] #11 fatal_in_last_5min > 0 is DEGRADED" << std::endl;
    test12_FailingWinsOverDegraded();
    std::cout << "  [✓] #12 FAILING precedence over DEGRADED" << std::endl;
    test13_NegativeTipAgeIsOk();
    std::cout << "  [✓] #13 negative tip age (clock skew) is OK" << std::endl;
    test14_DegradedReasonIsDeterministic();
    std::cout << "  [✓] #14 multiple DEGRADED signals: deterministic reason" << std::endl;

    std::cout << "\n✅ Health check: 14/14 properties hold" << std::endl;
    return 0;
}
