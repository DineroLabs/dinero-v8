/**
 * issue #241/#214: stalled-peer eviction on consecutive send failures.
 *
 * A peer whose socket stops draining costs every sender the full
 * SO_SNDTIMEO window; senders convoy on the per-socket send mutex and
 * block ingest collapses. RecordSendOutcomeShouldDisconnect drives the
 * eviction decision wired into P2PManager::send_to_peer.
 */

#include "daemon/peer_send_health.h"

#include <atomic>
#include <iostream>
#include <string>

namespace {

bool Require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "   ❌ " << message << std::endl;
    }
    return condition;
}

}  // namespace

int main() {
    std::cout << "=== peer_send_health regression tests ===" << std::endl;

    {
        std::cout << "\n1. failures below the threshold do not evict..." << std::endl;
        std::atomic<uint32_t> failures{0};
        bool evict = false;
        for (uint32_t i = 0; i + 1 < dinero::kMaxConsecutiveSendFailures; ++i) {
            evict = dinero::RecordSendOutcomeShouldDisconnect(failures, /*send_ok=*/false);
            if (!Require(!evict, "must not evict before the threshold (failure " +
                                     std::to_string(i + 1) + ")")) {
                return 1;
            }
        }
        std::cout << "   ✅ no eviction through failure "
                  << (dinero::kMaxConsecutiveSendFailures - 1) << std::endl;
    }

    {
        std::cout << "\n2. reaching the threshold evicts..." << std::endl;
        std::atomic<uint32_t> failures{0};
        bool evict = false;
        for (uint32_t i = 0; i < dinero::kMaxConsecutiveSendFailures; ++i) {
            evict = dinero::RecordSendOutcomeShouldDisconnect(failures, /*send_ok=*/false);
        }
        if (!Require(evict, "the threshold-th consecutive failure must request eviction")) {
            return 1;
        }
        // Robustness: further failures (queued senders racing the disconnect)
        // keep requesting eviction; disconnect_peer/cleanup_peer are idempotent.
        evict = dinero::RecordSendOutcomeShouldDisconnect(failures, /*send_ok=*/false);
        if (!Require(evict, "failures past the threshold must keep requesting eviction")) {
            return 1;
        }
        std::cout << "   ✅ eviction at failure " << dinero::kMaxConsecutiveSendFailures
                  << " and beyond" << std::endl;
    }

    {
        std::cout << "\n3. a successful send resets the streak..." << std::endl;
        std::atomic<uint32_t> failures{0};
        (void)dinero::RecordSendOutcomeShouldDisconnect(failures, false);
        (void)dinero::RecordSendOutcomeShouldDisconnect(failures, false);
        bool evict = dinero::RecordSendOutcomeShouldDisconnect(failures, /*send_ok=*/true);
        if (!Require(!evict, "a successful send must not evict")) {
            return 1;
        }
        // The streak restarted: the next two failures stay below the threshold.
        evict = dinero::RecordSendOutcomeShouldDisconnect(failures, false);
        evict = dinero::RecordSendOutcomeShouldDisconnect(failures, false) || evict;
        if (!Require(!evict, "success must RESET the streak — transient send "
                             "hiccups across a long connection must never "
                             "accumulate into an eviction")) {
            return 1;
        }
        std::cout << "   ✅ success resets the streak" << std::endl;
    }

    std::cout << "\n✅ All peer_send_health tests passed" << std::endl;
    return 0;
}
