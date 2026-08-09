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

inline constexpr DaemonBootstrapOwner kProductionDaemonBootstrapOwner =
    DaemonBootstrapOwner::ApplicationMain;

constexpr bool ShouldScheduleWindowDaemonStart(DaemonBootstrapOwner owner) noexcept {
    return owner == DaemonBootstrapOwner::MainWindow;
}

} // namespace dinero::qt
