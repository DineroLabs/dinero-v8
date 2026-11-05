#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>

TEST(ChainDBScanView, RetainsCapturedCoinsWhileCallerUnlocksAndWriterCommits) {
    const auto root = std::filesystem::temp_directory_path() /
        ("dinero_scan_view_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    dinero::ChainDB db;
    ASSERT_EQ(db.init(root), dinero::Status::Ok);
    const auto token = dinero::ChainWriteToken::CreateForTesting();
    dinero::uint256 old_id, new_id;
    for (size_t i = 0; i < 32; ++i) {
        old_id.data[i] = static_cast<uint8_t>(i * 7 + 3);
        new_id.data[i] = static_cast<uint8_t>(255 - i * 3);
    }
    dinero::Coin old_coin; old_coin.amount = 17; old_coin.script_pubkey = "5100"; old_coin.height = 11; old_coin.coinbase = false;
    auto new_coin = old_coin; new_coin.amount = 29;
    ASSERT_EQ(db.putCoin(token, old_id, 0x1020304, old_coin), dinero::Status::Ok);
    std::mutex selected;
    std::unique_lock<std::mutex> caller(selected);
    bool writer_committed = false, callback_seen = false;
    const auto status = db.forEachUTXO([&](const auto& id, uint32_t vout, const auto& coin) {
        EXPECT_TRUE(writer_committed);
        EXPECT_FALSE(caller.owns_lock());
        EXPECT_EQ(id, old_id);
        EXPECT_EQ(vout, 0x1020304U);
        EXPECT_EQ(coin.amount, 17U);
        callback_seen = true;
        return true;
    }, [&] {
        caller.unlock();
        std::thread writer([&] {
            std::lock_guard<std::mutex> lock(selected);
            EXPECT_EQ(db.deleteCoin(token, old_id, 0x1020304), dinero::Status::Ok);
            EXPECT_EQ(db.putCoin(token, new_id, 23, new_coin), dinero::Status::Ok);
            writer_committed = true;
        });
        writer.join();
    });
    EXPECT_EQ(status, dinero::Status::Ok);
    EXPECT_TRUE(callback_seen);
    size_t rows = 0;
    EXPECT_EQ(db.forEachUTXO([&](const auto& id, uint32_t vout, const auto& coin) {
        EXPECT_EQ(id, new_id);
        EXPECT_EQ(vout, 23U);
        EXPECT_EQ(coin.amount, 29U);
        ++rows;
        return true;
    }), dinero::Status::Ok);
    EXPECT_EQ(rows, 1U);
    EXPECT_THROW(db.forEachUTXO([](const auto&, auto, const auto&) -> bool {
        throw std::runtime_error("owned visitor failure");
    }), std::runtime_error);
    db.close();
    std::filesystem::remove_all(root);
}
