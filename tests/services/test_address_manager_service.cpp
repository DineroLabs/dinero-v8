// AddressManagerService boundary smoke test.
//
// Second entry in the services-boundary-smoke bucket. Mirrors the contract
// shape from test_metrics_service.cpp but adapted for AddressManagerService's
// simpler lifecycle (no logger/config wiring required).
//
// Verifies the service-API contract of dinero::daemon::AddressManagerService:
//   - Default-constructed service exposes Name() == "AddressManagerService"
//   - getManager() returns a non-null AddressManager from construction onward
//   - Init/Start/Stop lifecycle runs without throwing
//   - Stop is idempotent (second Stop on stopped service is safe)
//   - getManager() stays non-null across the full lifecycle (addrman is owned
//     by the service, not by DaemonContext)

#include "consensus/cpu_budget_monitor.h"
#include "storage/disk_space_monitor.h"
#include "p2p/network_limits_monitor.h"
#include "mining/block_assembler.h"
#include "daemon/daemon_context.h"
#include "daemon/services/address_manager_service.h"

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
    dinero::daemon::AddressManagerService svc;
    EXPECT(svc.Name() == "AddressManagerService",
           "Name() must be exactly 'AddressManagerService'");
    EXPECT(svc.getManager() != nullptr,
           "getManager() must return non-null AddressManager from construction");
    // const overload returns same object
    const auto& csvc = svc;
    EXPECT(csvc.getManager() == svc.getManager(),
           "const getManager() must return same pointer as non-const overload");
    std::cout << "  PASSED\n";
}

void test_full_lifecycle() {
    std::cout << "Test 2: Init/Start/Stop lifecycle is safe\n";
    DaemonContext ctx;
    dinero::daemon::AddressManagerService svc;
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
    std::cout << "Test 3: getManager() stays non-null across full lifecycle\n";
    DaemonContext ctx;
    dinero::daemon::AddressManagerService svc;
    auto* before_init = svc.getManager();
    svc.Init(ctx);
    EXPECT(svc.getManager() == before_init,
           "Init must not reallocate the underlying AddressManager");
    svc.Start();
    EXPECT(svc.getManager() == before_init,
           "Start must not reallocate the underlying AddressManager");
    svc.Stop();
    EXPECT(svc.getManager() == before_init,
           "Stop must not destroy the underlying AddressManager");
    std::cout << "  PASSED\n";
}

} // namespace

int main() {
    std::cout << "AddressManagerService boundary smoke test\n";
    std::cout << "=========================================\n";
    test_name_and_manager_available_at_construction();
    test_full_lifecycle();
    test_manager_persists_across_lifecycle();
    std::cout << "\nAll assertions passed (" << g_pass << "/" << g_total << ")\n";
    return 0;
}
