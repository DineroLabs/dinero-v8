// HeadersSyncService boundary smoke test.
//
// Fifth entry in the services-boundary-smoke bucket. Same shape as the
// AddressManagerService / RBFPolicyService / PeerScoringService tests —
// the service owns a unique_ptr to its underlying manager and exposes a
// stable Init/Start/Stop lifecycle.
//
// Verifies the service-API contract of dinero::daemon::HeadersSyncService:
//   - Default-constructed service exposes Name() == "HeadersSyncService"
//   - getManager() returns a non-null HeadersFirstSync from construction
//   - Init/Start/Stop lifecycle runs without throwing
//   - Stop is idempotent
//   - Manager identity preserved across the full lifecycle
//   - Fresh sync state defaults wire through: isSyncing() == false on a
//     newly-constructed service.

#include "consensus/cpu_budget_monitor.h"
#include "storage/disk_space_monitor.h"
#include "p2p/network_limits_monitor.h"
#include "mining/block_assembler.h"
#include "daemon/daemon_context.h"
#include "daemon/services/headers_sync_service.h"

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
    dinero::daemon::HeadersSyncService svc;
    EXPECT(svc.Name() == "HeadersSyncService",
           "Name() must be exactly 'HeadersSyncService'");
    EXPECT(svc.getManager() != nullptr,
           "getManager() must return non-null HeadersFirstSync from construction");
    const auto& csvc = svc;
    EXPECT(csvc.getManager() == svc.getManager(),
           "const getManager() must return same pointer as non-const overload");
    std::cout << "  PASSED\n";
}

void test_full_lifecycle() {
    std::cout << "Test 2: Init/Start/Stop lifecycle is safe\n";
    DaemonContext ctx;
    dinero::daemon::HeadersSyncService svc;
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
    dinero::daemon::HeadersSyncService svc;
    auto* before_init = svc.getManager();
    svc.Init(ctx);
    EXPECT(svc.getManager() == before_init,
           "Init must not reallocate the underlying HeadersFirstSync");
    svc.Start();
    EXPECT(svc.getManager() == before_init,
           "Start must not reallocate the underlying HeadersFirstSync");
    svc.Stop();
    EXPECT(svc.getManager() == before_init,
           "Stop must not destroy the underlying HeadersFirstSync");
    std::cout << "  PASSED\n";
}

void test_fresh_sync_state_default() {
    std::cout << "Test 4: Fresh service reports isSyncing()==false\n";
    DaemonContext ctx;
    dinero::daemon::HeadersSyncService svc;
    svc.Init(ctx);
    svc.Start();
    EXPECT(!svc.isSyncing(),
           "Newly-started service must report isSyncing() == false");
    svc.Stop();
    std::cout << "  PASSED\n";
}

} // namespace

int main() {
    std::cout << "HeadersSyncService boundary smoke test\n";
    std::cout << "======================================\n";
    test_name_and_manager_available_at_construction();
    test_full_lifecycle();
    test_manager_persists_across_lifecycle();
    test_fresh_sync_state_default();
    std::cout << "\nAll assertions passed (" << g_pass << "/" << g_total << ")\n";
    return 0;
}
