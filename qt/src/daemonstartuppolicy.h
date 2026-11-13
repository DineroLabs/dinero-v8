#pragma once

namespace dinero::qt {

// dinero-qt has two startup layers. The application entry point performs the
// production daemon bootstrap before constructing MainWindow; standalone
// MainWindow users may instead ask the window to perform that bootstrap.
// Keeping the ownership explicit prevents both layers from racing to launch
// dinerod against the same datadir.
enum class DaemonBootstrapOwner {
    ApplicationMain,
    MainWindow,
};

// A verified full-archive v3 index has a complete local chain source.
// Bundled AssumeUTXO flags belong only to the legacy/fresh-install path;
// passing them to v3 startup changes the source profile and is refused.
enum class WalletIndexStartupMode {
    LegacyOrUnavailable,
    FullArchiveV3,
};

constexpr bool ShouldArmBundledSnapshot(WalletIndexStartupMode mode) noexcept {
    return mode != WalletIndexStartupMode::FullArchiveV3;
}

constexpr bool ShouldOfferWalletIndexV3Upgrade(bool eligibleV2,
                                               bool explicitPilotOptIn) noexcept {
    return eligibleV2 && explicitPilotOptIn;
}

inline constexpr DaemonBootstrapOwner kProductionDaemonBootstrapOwner =
    DaemonBootstrapOwner::ApplicationMain;

constexpr bool ShouldScheduleWindowDaemonStart(DaemonBootstrapOwner owner) noexcept {
    return owner == DaemonBootstrapOwner::MainWindow;
}

} // namespace dinero::qt
