#include "daemonstartuppolicy.h"

using dinero::qt::DaemonBootstrapOwner;
using dinero::qt::ShouldScheduleWindowDaemonStart;
using dinero::qt::kProductionDaemonBootstrapOwner;
using dinero::qt::WalletIndexStartupMode;
using dinero::qt::ShouldArmBundledSnapshot;
using dinero::qt::ShouldOfferWalletIndexV3Upgrade;

static_assert(kProductionDaemonBootstrapOwner == DaemonBootstrapOwner::ApplicationMain,
              "production startup must retain a single daemon-bootstrap owner");
static_assert(!ShouldScheduleWindowDaemonStart(kProductionDaemonBootstrapOwner),
              "the production entry point already launched dinerod");
static_assert(ShouldScheduleWindowDaemonStart(DaemonBootstrapOwner::MainWindow),
              "standalone windows retain their self-contained fallback");
static_assert(ShouldArmBundledSnapshot(WalletIndexStartupMode::LegacyOrUnavailable),
              "fresh and legacy installs still need bundled snapshot recovery");
static_assert(!ShouldArmBundledSnapshot(WalletIndexStartupMode::FullArchiveV3),
              "a verified full-archive v3 source must not receive snapshot flags");
static_assert(!ShouldOfferWalletIndexV3Upgrade(true, false),
              "an eligible wallet must never be upgraded without pilot opt-in");
static_assert(!ShouldOfferWalletIndexV3Upgrade(false, true),
              "pilot opt-in cannot make an ineligible source upgradeable");
static_assert(ShouldOfferWalletIndexV3Upgrade(true, true),
              "a pilot may explicitly opt into an eligible offline upgrade");

int main() {
    return (!ShouldScheduleWindowDaemonStart(kProductionDaemonBootstrapOwner) &&
            ShouldScheduleWindowDaemonStart(DaemonBootstrapOwner::MainWindow) &&
            ShouldArmBundledSnapshot(WalletIndexStartupMode::LegacyOrUnavailable) &&
            !ShouldArmBundledSnapshot(WalletIndexStartupMode::FullArchiveV3) &&
            !ShouldOfferWalletIndexV3Upgrade(true, false) &&
            ShouldOfferWalletIndexV3Upgrade(true, true))
               ? 0
               : 1;
}
