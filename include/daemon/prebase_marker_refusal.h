#pragma once
#include "common/status.h"
#include "primitives/uint256.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace dinero {

// The offline wallet-index v3 tools accept only a full-archive source. A stored
// pre-base coin-set marker means the chain was bootstrapped from a snapshot,
// and the marker persists after the node fully validates and syncs. Return the
// refusal for an actual marker separately from a failure to READ it: a read
// error is not proof of a snapshot import and must not be reported as one.
// std::nullopt means no marker is stored (eligible on this check). `operation`
// names the caller ("upgrade" for the offline tools, "startup" for DaemonApp).
inline std::optional<std::string> PreBaseMarkerRefusal(
        const StatusOr<std::pair<uint256, uint32_t>>& prebase, const std::string& operation) {
    if (prebase.ok()) {
        return "Wallet index v3 " + operation + " refuses an imported pre-base source "
               "(snapshot base height " + std::to_string(prebase.value().second) +
               ", block " + prebase.value().first.GetHex() + ")";
    }
    if (prebase.status() == Status::NotFound) return std::nullopt;
    return "Wallet index v3 " + operation + " could not read the pre-base marker (" +
           StatusToString(prebase.status()) +
           "); this is a storage read failure, not proof of a snapshot import";
}

}  // namespace dinero
