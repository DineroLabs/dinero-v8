#include "daemon/block_request_peer_order.h"

#include <iostream>
#include <vector>

int main() {
    using dinero::daemon::BlockRequestPeerCandidate;
    using dinero::daemon::OrderBlockRequestPeers;

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

    std::cout << "block request peer ordering: PASS\n";
    return 0;
}
