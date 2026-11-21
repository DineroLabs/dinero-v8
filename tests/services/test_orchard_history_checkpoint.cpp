// OrchardHistoryCheckpointStands (include/p2p/orchard_history_checkpoint.h):
// a new block that extends the chain does not undo verified wallet history;
// a reorganisation does.
#include "p2p/orchard_history_checkpoint.h"

#include <gtest/gtest.h>

#include <map>
#include <vector>

using dinero::p2p::OrchardHistoryCheckpointStands;
using dinero::p2p::OrchardPhoneHistoryTarget;
using dinero::uint256;

namespace {

uint256 Hash(uint8_t chain, uint32_t height) {
    uint256 h;
    h.begin()[0] = chain;
    for (unsigned i = 0; i < 4; ++i) h.begin()[1 + i] = uint8_t(height >> (8 * i));
    return h;
}

// Header chains by name: chain 1 is the main one; chain 2 forks from it above `fork`.
struct Headers {
    uint32_t fork{157'000};
    int asked{0};
    uint8_t ChainOf(const uint256& hash) const { return hash.begin()[0]; }
    uint32_t HeightOf(const uint256& hash) const {
        uint32_t h = 0;
        for (unsigned i = 0; i < 4; ++i) h |= uint32_t(hash.begin()[1 + i]) << (8 * i);
        return h;
    }
    std::optional<uint256> AncestorAt(const uint256& tip, uint32_t height) {
        ++asked;
        if (height > HeightOf(tip)) return std::nullopt;
        const uint8_t chain = ChainOf(tip);
        return Hash(chain == 2 && height <= fork ? 1 : chain, height);  // below the fork both chains are chain 1
    }
};

OrchardPhoneHistoryTarget Target(uint8_t chain, uint32_t height) {
    OrchardPhoneHistoryTarget t;
    t.activation = 150'000;
    t.height = height;
    t.hash = Hash(chain, height);
    t.catalog = Hash(0xc0, height);
    t.base = Hash(1, 155'650);
    return t;
}

bool Stands(Headers& headers, const OrchardPhoneHistoryTarget& verified, const OrchardPhoneHistoryTarget& current) {
    return OrchardHistoryCheckpointStands(verified, current, [&](const uint256& tip, uint32_t h) { return headers.AncestorAt(tip, h); });
}

}  // namespace

TEST(OrchardHistoryCheckpoint, StandsAtItsOwnTargetWithoutAskingTheHeaders) {
    Headers headers;
    EXPECT_TRUE(Stands(headers, Target(1, 157'497), Target(1, 157'497)));
    EXPECT_EQ(headers.asked, 0);
}

// What the buyer's wallet ran into: verified to 157,497, the tip at 157,498 and 157,499.
TEST(OrchardHistoryCheckpoint, StandsWhenNewBlocksExtendTheSameChain) {
    Headers headers;
    const auto verified = Target(1, 157'497);
    EXPECT_TRUE(Stands(headers, verified, Target(1, 157'498)));
    EXPECT_TRUE(Stands(headers, verified, Target(1, 157'499)));
    EXPECT_TRUE(Stands(headers, verified, Target(1, 160'000)));  // however far the chain has moved on
}

TEST(OrchardHistoryCheckpoint, FallsWhenAReorganisationReplacesItsBlock) {
    Headers headers;
    headers.fork = 157'490;
    const auto verified = Target(1, 157'497);       // on the old branch, above the fork
    EXPECT_FALSE(Stands(headers, verified, Target(2, 157'497)));  // same height, another block
    EXPECT_FALSE(Stands(headers, verified, Target(2, 157'600)));  // a longer chain that does not contain it
    EXPECT_FALSE(Stands(headers, verified, Target(2, 157'495)));  // a shorter one
    // A checkpoint below the fork is on both branches: it stands on either.
    const auto below = Target(1, 157'480);
    EXPECT_TRUE(Stands(headers, below, Target(2, 157'600)));
    EXPECT_TRUE(Stands(headers, below, Target(1, 157'600)));
}

TEST(OrchardHistoryCheckpoint, FallsWhenTheChainIsShorterOrTheSnapshotDiffers) {
    Headers headers;
    const auto verified = Target(1, 157'497);
    EXPECT_FALSE(Stands(headers, verified, Target(1, 157'496)));  // the tip went back
    auto other_base = Target(1, 157'600);
    other_base.base = Hash(1, 155'000);
    EXPECT_FALSE(Stands(headers, verified, other_base));
    auto other_activation = Target(1, 157'600);
    other_activation.activation = 149'000;
    EXPECT_FALSE(Stands(headers, verified, other_activation));
    // The same block claimed with another catalog is not the verified target.
    auto other_catalog = Target(1, 157'497);
    other_catalog.catalog = Hash(0xc1, 1);
    EXPECT_FALSE(Stands(headers, verified, other_catalog));
}

TEST(OrchardHistoryCheckpoint, WhatTheHeadersCannotTellDoesNotStand) {
    const auto verified = Target(1, 157'497);
    const auto current = Target(1, 157'600);
    EXPECT_FALSE(OrchardHistoryCheckpointStands(verified, current, [](const uint256&, uint32_t) { return std::optional<uint256>(); }));
    EXPECT_FALSE(OrchardHistoryCheckpointStands(verified, current, [](const uint256&, uint32_t) { return std::optional<uint256>(uint256()); }));
}
