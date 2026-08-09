#include "daemonstartuppolicy.h"

using dinero::qt::DaemonBootstrapOwner;
using dinero::qt::ShouldScheduleWindowDaemonStart;
using dinero::qt::kProductionDaemonBootstrapOwner;

static_assert(kProductionDaemonBootstrapOwner == DaemonBootstrapOwner::ApplicationMain,
              "production startup must retain a single daemon-bootstrap owner");
static_assert(!ShouldScheduleWindowDaemonStart(kProductionDaemonBootstrapOwner),
              "the production entry point already launched dinerod");
static_assert(ShouldScheduleWindowDaemonStart(DaemonBootstrapOwner::MainWindow),
              "standalone windows retain their self-contained fallback");

int main() {
    return (!ShouldScheduleWindowDaemonStart(kProductionDaemonBootstrapOwner) &&
            ShouldScheduleWindowDaemonStart(DaemonBootstrapOwner::MainWindow))
               ? 0
               : 1;
}
