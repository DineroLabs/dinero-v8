#pragma once

// Whether this node serves relay circuits for NAT'd peers (and so advertises
// NODE_RELAY). Pure decision, factored out of P2PService so it is
// unit-testable without a daemon — same rationale as anchor_reconnect_policy.h.

#include <algorithm>
#include <cctype>
#include <string>

namespace dinero::daemon {

inline std::string LowerRelayValue(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

inline bool IsRelayModeOn(const std::string& mode) {
    const std::string value = LowerRelayValue(mode);
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

inline bool IsRelayModeOff(const std::string& mode) {
    const std::string value = LowerRelayValue(mode);
    return value == "0" || value == "false" || value == "no" || value == "off";
}

// `operator_mode` is the network.setrelayservice preference (off / automatic /
// custom) and wins over everything. `relay_mode` is the p2p.relay config value
// (on / off / auto). In auto, a node relays while it mines or while other
// nodes have dialed it back (verified reachable): those are the nodes a NAT'd
// peer can actually reach.
inline bool RelayRoleEnabled(const std::string& operator_mode, const std::string& relay_mode,
                             bool mining_active, bool verified_reachable) {
    if (operator_mode == "off") return false;
    if (operator_mode == "custom") return true;
    if (IsRelayModeOn(relay_mode)) return true;
    if (IsRelayModeOff(relay_mode)) return false;
    return mining_active || verified_reachable;
}

}  // namespace dinero::daemon
