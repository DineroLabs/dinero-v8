// RBFPolicyService boundary smoke test.
//
// Third entry in the services-boundary-smoke bucket. Mirrors the
// AddressManagerService contract — the service owns a unique_ptr to its
// underlying policy, exposes Init/Start/Stop, and the policy must stay alive
// across the full lifecycle.
//
// Verifies the service-API contract of dinero::daemon::RBFPolicyService:
//   - Default-constructed service exposes Name() == "RBFPolicyService"
//   - getPolicy() returns a non-null MempoolPolicy from construction onward
//   - Init/Start/Stop lifecycle runs without throwing
//   - Stop is idempotent
//   - getPolicy() identity is preserved across the lifecycle (no reallocation)

#include "consensus/cpu_budget_monitor.h"
#include "storage/disk_space_monitor.h"
#include "p2p/network_limits_monitor.h"
#include "mining/block_assembler.h"
#include "daemon/daemon_context.h"
#include "daemon/services/rbf_policy_service.h"

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

void test_name_and_policy_available_at_construction() {
    std::cout << "Test 1: Name() + getPolicy() are valid at construction\n";
    dinero::daemon::RBFPolicyService svc;
    EXPECT(svc.Name() == "RBFPolicyService",
           "Name() must be exactly 'RBFPolicyService'");
    EXPECT(svc.getPolicy() != nullptr,
           "getPolicy() must return non-null MempoolPolicy from construction");
    const auto& csvc = svc;
    EXPECT(csvc.getPolicy() == svc.getPolicy(),
           "const getPolicy() must return same pointer as non-const overload");
    std::cout << "  PASSED\n";
}

void test_full_lifecycle() {
    std::cout << "Test 2: Init/Start/Stop lifecycle is safe\n";
    DaemonContext ctx;
    dinero::daemon::RBFPolicyService svc;
    EXPECT(svc.Init(ctx),
           "Init must succeed with default DaemonContext");
    EXPECT(svc.Start(),
           "Start must succeed after Init");
    svc.Stop();
    // Property: Stop is idempotent.
    svc.Stop();
    std::cout << "  PASSED\n";
}

void test_policy_persists_across_lifecycle() {
    std::cout << "Test 3: getPolicy() stays non-null across full lifecycle\n";
    DaemonContext ctx;
    dinero::daemon::RBFPolicyService svc;
    auto* before_init = svc.getPolicy();
    svc.Init(ctx);
    EXPECT(svc.getPolicy() == before_init,
           "Init must not reallocate the underlying MempoolPolicy");
    svc.Start();
    EXPECT(svc.getPolicy() == before_init,
           "Start must not reallocate the underlying MempoolPolicy");
    svc.Stop();
    EXPECT(svc.getPolicy() == before_init,
           "Stop must not destroy the underlying MempoolPolicy");
    std::cout << "  PASSED\n";
}

} // namespace

int main() {
    std::cout << "RBFPolicyService boundary smoke test\n";
    std::cout << "====================================\n";
    test_name_and_policy_available_at_construction();
    test_full_lifecycle();
    test_policy_persists_across_lifecycle();
    std::cout << "\nAll assertions passed (" << g_pass << "/" << g_total << ")\n";
    return 0;
}
