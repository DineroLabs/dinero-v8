#include "daemon/services/p2p_service.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <string>
#include <thread>
#include <vector>

namespace dinero {

struct P2PServiceRateTestAccess {
    static bool Tx(P2PService& service, const std::string& peer) {
        return service.checkTxRate(peer);
    }
    static bool Headers(P2PService& service, const std::string& peer) {
        return service.checkHeadersRate(peer);
    }
    static void Expire(P2PService& service, const std::string& peer) {
        // No workers are running when the test advances these windows.
        service.tx_rate_tracker_.at(peer).window_start = 0;
        service.headers_rate_tracker_.at(peer).window_start = 0;
    }
    static auto Sizes(const P2PService& service) {
        // Only called after all worker threads have joined.
        return std::pair(service.tx_rate_tracker_.size(),
                         service.headers_rate_tracker_.size());
    }
};

TEST(P2PServiceRateLimits, IndependentPeerAndMessageBudgetsReset) {
    P2PService service;
    for (int i = 0; i < 10; ++i)
        EXPECT_TRUE(P2PServiceRateTestAccess::Tx(service, "a"));
    EXPECT_FALSE(P2PServiceRateTestAccess::Tx(service, "a"));
    for (int i = 0; i < 32; ++i)
        EXPECT_TRUE(P2PServiceRateTestAccess::Headers(service, "a"));
    EXPECT_FALSE(P2PServiceRateTestAccess::Headers(service, "a"));
    EXPECT_TRUE(P2PServiceRateTestAccess::Tx(service, "b"));
    EXPECT_TRUE(P2PServiceRateTestAccess::Headers(service, "b"));
    P2PServiceRateTestAccess::Expire(service, "a");
    EXPECT_TRUE(P2PServiceRateTestAccess::Tx(service, "a"));
    EXPECT_TRUE(P2PServiceRateTestAccess::Headers(service, "a"));
}

TEST(P2PServiceRateLimits, ConcurrentMessagesShareOnePeerBudget) {
    P2PService service;
    std::barrier start(8);
    std::atomic<int> tx_accepted{0}, headers_accepted{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&] {
            start.arrive_and_wait();
            for (int i = 0; i < 16; ++i) {
                tx_accepted += P2PServiceRateTestAccess::Tx(service, "shared");
                headers_accepted += P2PServiceRateTestAccess::Headers(service, "shared");
            }
        });
    }
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(tx_accepted.load(), 10);
    EXPECT_EQ(headers_accepted.load(), 32);
}

TEST(P2PServiceRateLimits, ConcurrentPeerInsertionPreservesEveryEntry) {
    // Concurrent operator[]/rehash used to corrupt the map and orphan nodes.
    // Exercise the real service methods and destruction under ASan/LSan too.
    constexpr int kThreads = 8;
    constexpr int kPeersPerThread = 2000;
    P2PService service;
    std::barrier start(kThreads);
    std::atomic<int> accepted{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            start.arrive_and_wait();
            for (int i = 0; i < kPeersPerThread; ++i) {
                const auto peer = std::to_string(t) + ":" + std::to_string(i);
                accepted += P2PServiceRateTestAccess::Tx(service, peer);
                accepted += P2PServiceRateTestAccess::Headers(service, peer);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(accepted.load(), 2 * kThreads * kPeersPerThread);
    const auto [tx_peers, header_peers] = P2PServiceRateTestAccess::Sizes(service);
    EXPECT_EQ(tx_peers, kThreads * kPeersPerThread);
    EXPECT_EQ(header_peers, kThreads * kPeersPerThread);
}

}  // namespace dinero
