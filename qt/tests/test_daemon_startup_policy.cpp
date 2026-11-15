#include "daemonstartuppolicy.h"

using dinero::qt::DaemonBootstrapOwner;
using dinero::qt::ShouldScheduleWindowDaemonStart;
using dinero::qt::kProductionDaemonBootstrapOwner;
using dinero::qt::WalletIndexStartupMode;
using dinero::qt::ShouldArmBundledSnapshot;
using dinero::qt::ShouldOfferWalletIndexV3Upgrade;
using dinero::qt::WalletIndexUpgradeChoice;
using dinero::qt::ShouldLaunchDaemonAfterWalletIndexChoice;

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
static_assert(!ShouldOfferWalletIndexV3Upgrade(false),
              "an ineligible source must not be offered the offline upgrade");
static_assert(ShouldOfferWalletIndexV3Upgrade(true),
              "an eligible installed wallet must be offered the offline upgrade");
static_assert(!ShouldLaunchDaemonAfterWalletIndexChoice(WalletIndexUpgradeChoice::NotNow),
              "a declined full-archive v2 update must quit before daemon spawn");
static_assert(ShouldLaunchDaemonAfterWalletIndexChoice(WalletIndexUpgradeChoice::Update),
              "an accepted update proceeds to the verified upgrade");

int main() {
    return (!ShouldScheduleWindowDaemonStart(kProductionDaemonBootstrapOwner) &&
            ShouldScheduleWindowDaemonStart(DaemonBootstrapOwner::MainWindow) &&
            ShouldArmBundledSnapshot(WalletIndexStartupMode::LegacyOrUnavailable) &&
            !ShouldArmBundledSnapshot(WalletIndexStartupMode::FullArchiveV3) &&
            !ShouldOfferWalletIndexV3Upgrade(false) &&
            ShouldOfferWalletIndexV3Upgrade(true) &&
            !ShouldLaunchDaemonAfterWalletIndexChoice(WalletIndexUpgradeChoice::NotNow) &&
            ShouldLaunchDaemonAfterWalletIndexChoice(WalletIndexUpgradeChoice::Update))
               ? 0
               : 1;
}
