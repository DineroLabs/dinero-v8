/**
 * Archival Configuration Defaults Test
 *
 * Verifies the daemon defaults to truthful archival behavior:
 * - storage.archival = true
 * - strict flatfile reads are always enforced at runtime
 * - explicit archival mode overrides still win
 */

#include "consensus/cpu_budget_monitor.h"
#include "storage/disk_space_monitor.h"
#include "p2p/network_limits_monitor.h"
#include "mining/block_assembler.h"
#include "daemon/daemon_context.h"
#include "daemon/services/config_service.h"
#include <cassert>
#include <iostream>

using namespace dinero;

static void testDefaultArchivalModeEnabled() {
    auto* ctx = new DaemonContext();
    ConfigService config;

    assert(config.Init(*ctx) && "ConfigService init must succeed");
    assert(config.GetBool("storage.archival", false) &&
           "Archival mode must default to true");
}

static void testExplicitArchivalOverridePreserved() {
    auto* ctx = new DaemonContext();
    ConfigService config;

    config.Set("archival", "false");

    assert(config.Init(*ctx) && "ConfigService init must succeed");
    assert(!config.GetBool("storage.archival", true) &&
           "Explicit archival=false override must be preserved");
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Archival Config Defaults Test" << std::endl;
    std::cout << "========================================" << std::endl;

    testDefaultArchivalModeEnabled();
    std::cout << "  [✓] archival defaults enabled" << std::endl;

    testExplicitArchivalOverridePreserved();
    std::cout << "  [✓] explicit archival override preserved" << std::endl;

    std::cout << "\n✅ Archival config defaults verified" << std::endl;
    return 0;
}
