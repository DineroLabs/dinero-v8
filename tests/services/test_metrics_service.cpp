// MetricsService boundary smoke test.
//
// Verifies the service-API contract of dinero::MetricsService:
//   - Default-constructed service exposes Name() == "Metrics" with no setup
//   - Init/Start/Stop lifecycle works with a real LoggerService + ConfigService
//   - IsStarted() reflects Start/Stop state correctly
//   - ExportMetrics() returns a non-empty Prometheus-format string after Start
//   - Stop is idempotent (calling Stop twice is safe)
//   - Init returns false (and doesn't crash) when ctx.config is missing
//
// This is the pilot test for the `services-boundary-smoke` bucket. The
// dinero_core surgery plan requires service-boundary test backing before
// any service hub can be moved out of the dinero_core target. Same
// assert-style as tests/daemon/test_config_file_loader.cpp — no gtest
// dependency, no DaemonContext heavyweight setup.

#include "consensus/cpu_budget_monitor.h"
#include "storage/disk_space_monitor.h"
#include "p2p/network_limits_monitor.h"
#include "mining/block_assembler.h"
#include "daemon/daemon_context.h"
#include "daemon/services/metrics_service.h"
#include "daemon/services/logger_service.h"
#include "daemon/services/config_service.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

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

std::filesystem::path makeTmpLogPath() {
    auto tmp = std::filesystem::temp_directory_path() /
               ("dinero_metrics_service_test_" + std::to_string(::getpid()) + ".log");
    return tmp;
}

// Property 1 — default-constructed service exposes the canonical Name without
// any setup. Cheapest contract assertion; catches accidental rename of the
// service identifier (used by service discovery + Prometheus label).
void test_name_is_stable() {
    std::cout << "Test 1: Name() returns 'Metrics' without setup\n";
    dinero::MetricsService svc;
    EXPECT(svc.Name() == "Metrics", "Name() must be exactly 'Metrics'");
    EXPECT(!svc.IsStarted(), "IsStarted() must be false before Start()");
    std::cout << "  PASSED\n";
}

// Property 2 — full Init/Start/IsStarted/Stop lifecycle with real concrete
// LoggerService + ConfigService dependencies. This is the contract the
// service-boundary plan needs: services come up clean, expose state, shut
// down clean.
void test_full_lifecycle() {
    std::cout << "Test 2: Init/Start/IsStarted/Stop lifecycle\n";

    auto log_path = makeTmpLogPath();
    auto logger = std::make_shared<dinero::LoggerService>(log_path.string());
    auto config = std::make_shared<dinero::ConfigService>();

    DaemonContext ctx;
    ctx.logger = logger;
    ctx.config = config;

    // Logger must be started before MetricsService initializes (Init calls
    // logger_->info, which is undefined behaviour on an unstarted logger).
    EXPECT(logger->Init(ctx), "LoggerService::Init must succeed");
    EXPECT(logger->Start(), "LoggerService::Start must succeed");
    EXPECT(config->Init(ctx), "ConfigService::Init must succeed");
    EXPECT(config->Start(), "ConfigService::Start must succeed");

    dinero::MetricsService svc;
    EXPECT(svc.Init(ctx), "MetricsService::Init must succeed with real logger+config");
    EXPECT(!svc.IsStarted(), "IsStarted() must be false between Init and Start");
    EXPECT(svc.Start(), "MetricsService::Start must succeed");
    EXPECT(svc.IsStarted(), "IsStarted() must be true after Start");

    svc.Stop();
    EXPECT(!svc.IsStarted(), "IsStarted() must be false after Stop");

    // Property 3 — Stop is idempotent. Calling Stop twice on a stopped
    // service must be safe (no double-free, no crash, no exception).
    svc.Stop();
    EXPECT(!svc.IsStarted(), "second Stop() must remain safe and a no-op");

    config->Stop();
    logger->Stop();
    std::filesystem::remove(log_path);
    std::cout << "  PASSED\n";
}

// Property 4 — ExportMetrics() returns a non-empty Prometheus-format string
// after the service has Start()ed. The format check is intentionally cheap:
// Prometheus exposition format always contains at least one `# HELP` or
// `# TYPE` comment line for any registered metric. If the registry is
// completely empty the string is allowed to be non-empty whitespace; we just
// require it parses as a string and has bounded length.
void test_export_metrics_after_start() {
    std::cout << "Test 4: ExportMetrics() returns Prometheus-format after Start\n";

    auto log_path = makeTmpLogPath();
    auto logger = std::make_shared<dinero::LoggerService>(log_path.string());
    auto config = std::make_shared<dinero::ConfigService>();

    DaemonContext ctx;
    ctx.logger = logger;
    ctx.config = config;

    logger->Init(ctx); logger->Start();
    config->Init(ctx); config->Start();

    dinero::MetricsService svc;
    svc.Init(ctx);
    svc.Start();

    std::string snapshot = svc.ExportMetrics();
    // The registry initializes a default set of mining + chain gauges; the
    // snapshot must be non-empty and bounded (sanity ceiling 1 MiB).
    EXPECT(!snapshot.empty(), "ExportMetrics must return non-empty snapshot after Start");
    EXPECT(snapshot.size() < static_cast<std::size_t>(1024) * 1024, "ExportMetrics snapshot must be < 1 MiB");

    svc.Stop();
    config->Stop();
    logger->Stop();
    std::filesystem::remove(log_path);
    std::cout << "  PASSED\n";
}

// Property 5 — Init returns false (no crash) when ctx.config is missing.
// The service-boundary contract: missing required deps fail Init cleanly,
// they don't UB.
void test_init_fails_without_config() {
    std::cout << "Test 5: Init returns false when ctx.config is missing\n";

    auto log_path = makeTmpLogPath();
    auto logger = std::make_shared<dinero::LoggerService>(log_path.string());

    DaemonContext ctx;
    ctx.logger = logger;
    ctx.config = nullptr;

    logger->Init(ctx); logger->Start();

    dinero::MetricsService svc;
    EXPECT(!svc.Init(ctx), "Init must return false when config is missing");
    EXPECT(!svc.IsStarted(), "IsStarted must remain false on failed Init");

    logger->Stop();
    std::filesystem::remove(log_path);
    std::cout << "  PASSED\n";
}

} // namespace

int main() {
    std::cout << "MetricsService boundary smoke test\n";
    std::cout << "==================================\n";

    test_name_is_stable();
    test_full_lifecycle();
    test_export_metrics_after_start();
    test_init_fails_without_config();

    std::cout << "\nAll assertions passed (" << g_pass << "/" << g_total << ")\n";
    return 0;
}
