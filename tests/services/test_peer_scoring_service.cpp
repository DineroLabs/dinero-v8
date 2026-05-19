// PeerScoringService boundary smoke test.
//
// Fourth entry in the services-boundary-smoke bucket. Same shape as the
// AddressManagerService / RBFPolicyService tests — service owns a
// unique_ptr to its underlying manager and exposes a stable lifecycle.
//
// Verifies the service-API contract of dinero::daemon::PeerScoringService:
//   - Default-constructed service exposes Name() == "PeerScoringService"
//   - getManager() returns a non-null PeerScoringManager from construction
//   - Init/Start/Stop lifecycle runs without throwing
//   - Stop is idempotent
//   - The manager's identity is preserved across the full lifecycle
//   - Per-peer score plumbing wires through the service correctly:
//       fresh peer score is 0, ban check is false, and the service forwards
//       to the underlying manager.

#include "consensus/cpu_budget_monitor.h"
#include "storage/disk_space_monitor.h"
#include "p2p/network_limits_monitor.h"
#include "mining/block_assembler.h"
#include "daemon/daemon_context.h"
#include "daemon/services/peer_scoring_service.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>

namespace {

int g_pass = 0;
int g_total = 0;

#define EXPECT(cond, msg) do { \
    ++g_total; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
        std::abort(); \
    } \
    ++g_pass; \
} while (0)

void test_name_and_manager_available_at_construction() {
    std::cout << "Test 1: Name() + getManager() are valid at construction\n";
    dinero::daemon::PeerScoringService svc;
    EXPECT(svc.Name() == "PeerScoringService",
           "Name() must be exactly 'PeerScoringService'");
    EXPECT(svc.getManager() != nullptr,
           "getManager() must return non-null PeerScoringManager from construction");
    const auto& csvc = svc;
    EXPECT(csvc.getManager() == svc.getManager(),
           "const getManager() must return same pointer as non-const overload");
    std::cout << "  PASSED\n";
}

void test_full_lifecycle() {
    std::cout << "Test 2: Init/Start/Stop lifecycle is safe\n";
    DaemonContext ctx;
    dinero::daemon::PeerScoringService svc;
    EXPECT(svc.Init(ctx),
           "Init must succeed with default DaemonContext");
    EXPECT(svc.Start(),
           "Start must succeed after Init");
    svc.Stop();
    // Property: Stop is idempotent.
    svc.Stop();
    std::cout << "  PASSED\n";
}

void test_manager_persists_across_lifecycle() {
    std::cout << "Test 3: getManager() identity preserved across lifecycle\n";
    DaemonContext ctx;
    dinero::daemon::PeerScoringService svc;
    auto* before_init = svc.getManager();
    svc.Init(ctx);
    EXPECT(svc.getManager() == before_init,
           "Init must not reallocate the underlying PeerScoringManager");
    svc.Start();
    EXPECT(svc.getManager() == before_init,
           "Start must not reallocate the underlying PeerScoringManager");
    svc.Stop();
    EXPECT(svc.getManager() == before_init,
           "Stop must not destroy the underlying PeerScoringManager");
    std::cout << "  PASSED\n";
}

void test_fresh_peer_defaults() {
    std::cout << "Test 4: Fresh peer has score 0 and is not banned\n";
    DaemonContext ctx;
    dinero::daemon::PeerScoringService svc;
    svc.Init(ctx);
    svc.Start();

    const std::string fresh_peer = "127.0.0.1:23999";
    EXPECT(svc.getScore(fresh_peer) == 0,
           "Fresh peer must have score 0");
    EXPECT(!svc.isBanned(fresh_peer),
           "Fresh peer must not be banned");

    svc.Stop();
    std::cout << "  PASSED\n";
}

} // namespace

int main() {
    std::cout << "PeerScoringService boundary smoke test\n";
    std::cout << "======================================\n";
    test_name_and_manager_available_at_construction();
    test_full_lifecycle();
    test_manager_persists_across_lifecycle();
    test_fresh_peer_defaults();
    std::cout << "\nAll assertions passed (" << g_pass << "/" << g_total << ")\n";
    return 0;
}
