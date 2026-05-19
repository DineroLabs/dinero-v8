// CompactBlockService boundary smoke test.
//
// Sixth entry in the services-boundary-smoke bucket. CompactBlockService is
// the thinnest service in the original 9 — it owns no internal state,
// delegating all stats to BlockRelayManager via DaemonContext. So the
// contract verified here is narrow:
//
//   - Default-constructed service exposes Name() == "CompactBlockService"
//   - Init/Start/Stop lifecycle is safe with a default-constructed
//     DaemonContext (no block_relay wired). The service must early-return
//     from the stat accessors when ctx_->block_relay is null instead of
//     deref-crashing.
//   - Stop is idempotent
//   - Stat accessors (getBlocksProcessed, getReconstructionRate,
//     getBandwidthSaved) return zero when block_relay is absent — this
//     is the "null guard" property the .cpp documents.
//
// Build note: CompactBlockService.cpp references
// BlockRelayManager::GetStats() and BlockRelayManager::GetCompactBlockSuccessRate().
// Those symbols live in src/daemon/block_relay_manager.cpp, which is
// dinero_core-heavy. The test target instead links a tiny test-only stub
// (block_relay_manager_test_stub.cpp) that provides empty implementations
// returning defaults. Since the test never sets ctx_->block_relay, those
// stubs are never invoked at runtime — they only exist to satisfy the
// linker. This is exactly the "minimal BlockRelayManager test stub"
// pathway noted in the earlier deferral comment.

#include "consensus/cpu_budget_monitor.h"
#include "storage/disk_space_monitor.h"
#include "p2p/network_limits_monitor.h"
#include "mining/block_assembler.h"
#include "daemon/daemon_context.h"
#include "daemon/services/compact_block_service.h"

#include <cassert>
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

void test_name_is_stable() {
    std::cout << "Test 1: Name() returns 'CompactBlockService' without setup\n";
    dinero::daemon::CompactBlockService svc;
    EXPECT(svc.Name() == "CompactBlockService",
           "Name() must be exactly 'CompactBlockService'");
    std::cout << "  PASSED\n";
}

void test_full_lifecycle_with_null_block_relay() {
    std::cout << "Test 2: Init/Start/Stop lifecycle is safe with null block_relay\n";
    DaemonContext ctx;
    // Property: ctx.block_relay stays null. Service must not dereference it.
    dinero::daemon::CompactBlockService svc;
    EXPECT(svc.Init(ctx),
           "Init must succeed with default DaemonContext (no block_relay wired)");
    EXPECT(svc.Start(),
           "Start must succeed after Init");
    svc.Stop();
    // Property: Stop is idempotent.
    svc.Stop();
    std::cout << "  PASSED\n";
}

void test_stat_accessors_null_guard() {
    std::cout << "Test 3: Stat accessors return zero when block_relay is absent\n";
    DaemonContext ctx;
    dinero::daemon::CompactBlockService svc;
    svc.Init(ctx);
    svc.Start();
    // The null-guard property: with ctx_->block_relay == nullptr, accessors
    // must early-return zero rather than dereference a null pointer.
    EXPECT(svc.getBlocksProcessed() == 0ULL,
           "getBlocksProcessed must return 0 when block_relay is null");
    EXPECT(svc.getReconstructionRate() == 0.0,
           "getReconstructionRate must return 0.0 when block_relay is null");
    EXPECT(svc.getBandwidthSaved() == 0ULL,
           "getBandwidthSaved must return 0 when block_relay is null");
    svc.Stop();
    std::cout << "  PASSED\n";
}

} // namespace

int main() {
    std::cout << "CompactBlockService boundary smoke test\n";
    std::cout << "=======================================\n";
    test_name_is_stable();
    test_full_lifecycle_with_null_block_relay();
    test_stat_accessors_null_guard();
    std::cout << "\nAll assertions passed (" << g_pass << "/" << g_total << ")\n";
    return 0;
}
