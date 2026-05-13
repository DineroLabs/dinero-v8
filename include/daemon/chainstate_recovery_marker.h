#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace dinero::daemon {

// Status: DISARMED — intentional, pending fleet-soak evidence.
//
// History:
//
//   Apr 18 2026: re-disarmed after a live fleet-fragmentation incident
//   caused by re-arming this flag prematurely. The underlying bug at
//   the time was that --reindex-chainstate produced output the post-
//   reindex startup consistency check rejected ("Startup is aborted to
//   avoid serving from an inconsistent chainstate"). Auto-recovery
//   therefore caused LA/MO to enter systemd restart loops.
//
//   Apr 18-20 2026: the original disarm reason appeared resolved in
//   local crash-boundary proofs.
//
//   Apr 28 2026: live fleet evidence invalidated that conclusion.
//   Manual --reindex-chainstate failed around height 9291 with a
//   reindex/header Utreexo root mismatch, and safe-mode markers reported
//   thousands of live UTXOs missing from the restored forest. Treat any
//   earlier "empirically safe" statement as stale. The recovery path must
//   remain manually gated until it proves byte-for-byte equivalence against
//   the live shielded-era chainstate transition on affected mainnet data.
//
// Why still disarmed: the recovery implementation is not yet proven safe
// on the live post-shielded state surface. Re-arming this flag would turn
// a manually-triggered operator intervention into an autonomous rebuild
// that can take nodes offline and, if it reproduces Apr 28 behavior, strand
// them in restart or safe-mode loops.
//
// Re-arming policy: re-arm only after the Apr 28 mismatch has a root-cause
// fix, a regression test that fails without that fix, successful
// --reindex-chainstate replay on copied affected mainnet data, and an
// explicit period of incident-free fleet operation. Document that in its
// own commit with reference to this comment. Do not re-arm as part of an
// unrelated pass.
inline constexpr bool kAutomaticChainstateRecoveryArmed = false;

struct ChainstateRecoveryMarker {
    long long timestamp = 0;
    std::string reason;
};

inline std::filesystem::path ChainstateRecoveryMarkerPath(const std::filesystem::path& datadir) {
    return datadir / "chainstate_recovery.marker";
}

inline bool WriteChainstateRecoveryMarker(const std::filesystem::path& datadir,
                                          const std::string& reason,
                                          std::string* error = nullptr) {
    try {
        std::filesystem::create_directories(datadir);
        std::ofstream out(ChainstateRecoveryMarkerPath(datadir), std::ios::trunc);
        if (!out.is_open()) {
            if (error) *error = "failed to open recovery marker for writing";
            return false;
        }
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        out << static_cast<long long>(now) << "\n";
        out << reason << "\n";
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

inline std::optional<ChainstateRecoveryMarker> ReadChainstateRecoveryMarker(const std::filesystem::path& datadir,
                                                                            std::string* error = nullptr) {
    try {
        const auto path = ChainstateRecoveryMarkerPath(datadir);
        if (!std::filesystem::exists(path)) {
            return std::nullopt;
        }

        std::ifstream in(path);
        if (!in.is_open()) {
            if (error) *error = "failed to open recovery marker for reading";
            return std::nullopt;
        }

        ChainstateRecoveryMarker marker;
        std::string timestamp_line;
        if (!std::getline(in, timestamp_line)) {
            if (error) *error = "missing recovery marker timestamp";
            return std::nullopt;
        }
        marker.timestamp = std::stoll(timestamp_line);

        std::getline(in, marker.reason);
        return marker;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

inline bool ClearChainstateRecoveryMarker(const std::filesystem::path& datadir,
                                          std::string* error = nullptr) {
    try {
        const auto path = ChainstateRecoveryMarkerPath(datadir);
        if (!std::filesystem::exists(path)) {
            return true;
        }
        return std::filesystem::remove(path);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

}  // namespace dinero::daemon
