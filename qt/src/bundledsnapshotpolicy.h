#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace dinero::qt {
struct BundledSnapshotSelection {
    std::string primary;
    std::vector<std::string> fallbacks;
};

// Inputs are resolved paths in newest-to-oldest preference order. Older exact
// files remain available to the daemon for an interrupted imported lifecycle.
inline BundledSnapshotSelection SelectBundledSnapshots(const std::vector<std::string>& paths) {
    BundledSnapshotSelection result;
    for (const auto& path : paths) {
        if (path.empty()) continue;
        if (result.primary.empty()) {
            result.primary = path;
        } else if (path != result.primary && path.find(';') == std::string::npos &&
                   std::find(result.fallbacks.begin(), result.fallbacks.end(), path) == result.fallbacks.end()) {
            result.fallbacks.push_back(path);
        }
    }
    return result;
}
} // namespace dinero::qt
