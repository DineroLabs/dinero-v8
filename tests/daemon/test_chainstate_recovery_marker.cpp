#include "daemon/chainstate_recovery_marker.h"

#include <cassert>
#include <filesystem>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    using dinero::daemon::ChainstateRecoveryMarkerPath;
    using dinero::daemon::ClearChainstateRecoveryMarker;
    using dinero::daemon::ReadChainstateRecoveryMarker;
    using dinero::daemon::WriteChainstateRecoveryMarker;
    using dinero::daemon::kAutomaticChainstateRecoveryArmed;

    assert(!kAutomaticChainstateRecoveryArmed);

    const fs::path temp_root = fs::temp_directory_path() / "dinero-chainstate-recovery-marker-test";
    std::error_code ec;
    fs::remove_all(temp_root, ec);
    ec.clear();

    const std::string reason = "missing undo data for active tip height 3113";
    std::string error;
    const bool wrote = WriteChainstateRecoveryMarker(temp_root, reason, &error);
    assert(wrote);
    assert(error.empty());
    assert(fs::exists(ChainstateRecoveryMarkerPath(temp_root)));

    const auto marker = ReadChainstateRecoveryMarker(temp_root, &error);
    assert(marker.has_value());
    assert(error.empty());
    assert(marker->reason == reason);
    assert(marker->timestamp > 0);

    const bool cleared = ClearChainstateRecoveryMarker(temp_root, &error);
    assert(cleared);
    assert(error.empty());
    assert(!fs::exists(ChainstateRecoveryMarkerPath(temp_root)));

    fs::remove_all(temp_root, ec);
    std::cout << "test_chainstate_recovery_marker: PASS\n";
    return 0;
}
