#pragma once

#include "daemon/services/wallet_catchup.h"
#include "primitives/block.h"

#include <gtest/gtest.h>

#include <optional>
#include <utility>
#include <vector>

namespace dinero {
namespace {

TEST(WalletCatchup, MissingMiddleBodyCannotSkipToLaterHeight) {
    Block first;
    first.header.nonce = 1;
    std::vector<uint32_t> requested, delivered;
    const auto load = [&](uint32_t height)
        -> std::optional<std::pair<uint256, Block>> {
        requested.push_back(height);
        if (height == 2) return std::nullopt;
        return std::make_pair(first.GetHash(), first);
    };
    EXPECT_THROW(wallet_catchup::RequireContiguous(0, 3, load,
        [&](uint32_t height, const uint256&, const Block&) {
            delivered.push_back(height);
        }), std::runtime_error);
    EXPECT_EQ(requested, (std::vector<uint32_t>{1, 2}));
    EXPECT_EQ(delivered, (std::vector<uint32_t>{1}));
}

TEST(WalletCatchup, WrongBodyForSelectedHashCannotAdvance) {
    Block body;
    body.header.nonce = 2;
    Block selected;
    selected.header.nonce = 4;
    uint32_t delivered = 0;
    EXPECT_THROW(wallet_catchup::RequireContiguous(100, 101,
        [&](uint32_t) -> std::optional<std::pair<uint256, Block>> {
            return std::make_pair(selected.GetHash(), body);
        }, [&](uint32_t, const uint256&, const Block&) { ++delivered; }),
        std::runtime_error);
    EXPECT_EQ(delivered, 0u);
}

TEST(WalletCatchup, CompleteSelectedPrefixIsDeliveredInOrder) {
    Block body;
    body.header.nonce = 3;
    std::vector<uint32_t> delivered;
    ASSERT_NO_THROW(wallet_catchup::RequireContiguous(149999, 150002,
        [&](uint32_t) -> std::optional<std::pair<uint256, Block>> {
            return std::make_pair(body.GetHash(), body);
        }, [&](uint32_t height, const uint256&, const Block&) {
            delivered.push_back(height);
        }));
    EXPECT_EQ(delivered, (std::vector<uint32_t>{150000, 150001, 150002}));
}

} // namespace
} // namespace dinero
