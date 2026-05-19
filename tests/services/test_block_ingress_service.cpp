// BlockIngressService boundary smoke test.
//
// Sixth service-hub entry in the services-boundary-smoke bucket. This service
// is validation-adjacent, so the test intentionally does not submit a block or
// exercise BlockAcceptor. It locks the service-facing boundary only:
//   - Default-constructed service exposes Name() == "BlockIngress"
//   - Init fails cleanly when the required ILogger dependency is missing
//   - Init/Start/Stop lifecycle updates IsHealthy()
//   - Start is one-shot and Stop is idempotent
//   - The service is usable through the IBlockIngress interface pointer
//   - Metrics JSON reflects started state and absent validation queue

#include "consensus/cpu_budget_monitor.h"
#include "storage/disk_space_monitor.h"
#include "p2p/network_limits_monitor.h"
#include "mining/block_assembler.h"
#include "daemon/daemon_context.h"
#include "daemon/interfaces/block_ingress.h"
#include "daemon/services/block_ingress_service.h"
#include "common/test_logger.h"

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

void test_name_and_health_defaults() {
    std::cout << "Test 1: Name() and health defaults are stable\n";
    dinero::BlockIngressService svc;
    EXPECT(svc.Name() == "BlockIngress",
           "Name() must be exactly 'BlockIngress'");
    EXPECT(!svc.IsHealthy(),
           "New service must not report healthy before Start()");

    const std::string metrics = svc.GetMetrics();
    EXPECT(metrics.find(R"("service":"block_ingress")") != std::string::npos,
           "Metrics must identify block_ingress service");
    EXPECT(metrics.find(R"("started":false)") != std::string::npos,
           "Metrics must report started=false before Start()");
    EXPECT(metrics.find(R"("p2p_uses_validation_queue":false)") != std::string::npos,
           "Metrics must report validation queue disabled by default");
    std::cout << "  PASSED\n";
}

void test_init_fails_without_logger_interface() {
    std::cout << "Test 2: Init fails cleanly without ILogger dependency\n";
    DaemonContext ctx;
    dinero::BlockIngressService svc;
    EXPECT(!svc.Init(ctx),
           "Init must return false when ctx.logger_interface is missing");
    EXPECT(!svc.IsHealthy(),
           "Failed Init must not mark service healthy");
    std::cout << "  PASSED\n";
}

void test_full_lifecycle_with_logger_interface() {
    std::cout << "Test 3: Init/Start/Stop lifecycle is explicit\n";
    dinero::TestLogger logger;
    DaemonContext ctx;
    ctx.logger_interface = &logger;

    dinero::BlockIngressService svc;
    EXPECT(svc.Init(ctx),
           "Init must succeed with ILogger dependency");
    EXPECT(!svc.IsHealthy(),
           "Init alone must not mark service healthy");
    EXPECT(svc.Start(),
           "Start must succeed after Init");
    EXPECT(svc.IsHealthy(),
           "Start must mark service healthy");

    const std::string started_metrics = svc.GetMetrics();
    EXPECT(started_metrics.find(R"("started":true)") != std::string::npos,
           "Metrics must report started=true after Start()");
    EXPECT(started_metrics.find(R"("p2p_uses_validation_queue":false)") != std::string::npos,
           "Metrics must report validation queue disabled when ctx has none");

    EXPECT(!svc.Start(),
           "Second Start must fail cleanly instead of pretending to restart");
    EXPECT(svc.IsHealthy(),
           "Failed second Start must leave service healthy");

    svc.Stop();
    EXPECT(!svc.IsHealthy(),
           "Stop must mark service unhealthy");
    svc.Stop();
    EXPECT(!svc.IsHealthy(),
           "Second Stop must remain safe and idempotent");
    std::cout << "  PASSED\n";
}

void test_block_ingress_interface_pointer_available() {
    std::cout << "Test 4: Service exposes IBlockIngress boundary\n";
    dinero::TestLogger logger;
    DaemonContext ctx;
    ctx.logger_interface = &logger;

    dinero::BlockIngressService svc;
    EXPECT(svc.Init(ctx), "Init must succeed before interface exposure check");
    dinero::IBlockIngress* ingress = &svc;
    EXPECT(ingress != nullptr,
           "BlockIngressService must be addressable as IBlockIngress");
    EXPECT(static_cast<void*>(ingress) != nullptr,
           "IBlockIngress pointer must be non-null");
    svc.Stop();
    std::cout << "  PASSED\n";
}

} // namespace

int main() {
    std::cout << "BlockIngressService boundary smoke test\n";
    std::cout << "=======================================\n";
    test_name_and_health_defaults();
    test_init_fails_without_logger_interface();
    test_full_lifecycle_with_logger_interface();
    test_block_ingress_interface_pointer_available();
    std::cout << "\nAll assertions passed (" << g_pass << "/" << g_total << ")\n";
    return 0;
}
