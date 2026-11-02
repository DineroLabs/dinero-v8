#include "daemon/block_request_peer_order.h"

#include <iostream>
#include <vector>

int main() {
    using dinero::daemon::BlockRequestPeerCandidate;
    using dinero::daemon::OrderBlockRequestPeers;
    using dinero::daemon::BlockBodyPeerAvailability;
    using dinero::daemon::LaggingArchivalBridgeFallback;

    const std::vector<BlockRequestPeerCandidate> peers{
        {"a-slow", 250.0},
        {"b-unknown", 0.0},
        {"z-fast", 8.0},
        {"m-middle", 40.0},
        {"z-fast", 12.0},  // duplicate identity keeps its best observation
    };

    const auto backfill = OrderBlockRequestPeers(peers, true);
    const std::vector<std::string> expected_backfill{
        "z-fast", "m-middle", "a-slow", "b-unknown"};
    if (backfill != expected_backfill) {
        std::cerr << "backfill peer order is not latency-prioritized\n";
        return 1;
    }

    const auto tip = OrderBlockRequestPeers(peers, false);
    const std::vector<std::string> expected_tip{
        "a-slow", "b-unknown", "m-middle", "z-fast"};
    if (tip != expected_tip) {
        std::cerr << "normal peer order is not deterministic identity order\n";
        return 1;
    }

    const std::vector<BlockBodyPeerAvailability> stalled{
        {"phone-a", 144654, false},
        {"phone-b", 144654, false},
        {"na-archival", 144646, true},
        {"eu-archival", 144645, true},
        {"limited", 144646, false},
    };
    const std::unordered_set<std::string> timed_out{"phone-a", "phone-b"};
    if (LaggingArchivalBridgeFallback(stalled, 144647, timed_out) !=
            std::vector<std::string>{"na-archival", "eu-archival"}) {
        std::cerr << "stalled high-advertising peers blocked archival fallback\n";
        return 1;
    }
    if (!LaggingArchivalBridgeFallback(stalled, 144647, {"phone-a"}).empty()) {
        std::cerr << "archival fallback bypassed a capable peer\n";
        return 1;
    }
    if (!LaggingArchivalBridgeFallback(stalled, 144647,
            {"phone-a", "phone-b", "na-archival", "eu-archival"}).empty()) {
        std::cerr << "archival fallback ignored unavailable bridges\n";
        return 1;
    }

    std::cout << "block request peer ordering: PASS\n";
    return 0;
}
