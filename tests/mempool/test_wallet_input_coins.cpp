#include "consensus/chainparams.h"
#include "consensus/consensus_utxo_set.h"
#include "daemon/mempool.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>

namespace {
using namespace dinero;
class WalletInputCoins : public ::testing::Test {
protected:
    ChainDB db;
    consensus::ConsensusUTXOSet coins;
    std::filesystem::path root;
    void SetUp() override {
        SelectParams(Chain::MAINNET);
        root = std::filesystem::temp_directory_path() / ("wallet_input_coins_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_EQ(db.init(root / "chaindb"), Status::Ok);
        coins.SetBestBlock(uint256{}, 110);
    }
    void TearDown() override { db.close(); std::filesystem::remove_all(root); }
    static OutPoint Out(uint8_t id) { uint256 hash; hash.data[0] = id; return {TxId(hash), 0}; }
    static consensus::UTXOEntry CoinAt(uint32_t height = 1, bool coinbase = false) {
        return {AmountUna::Una(100000), {0x51}, height, coinbase};
    }
    void PutAux(const OutPoint& out) {
        Coin coin; coin.amount = 100000; coin.script_pubkey = "51"; coin.height = 1;
        ASSERT_EQ(db.putCoin(ChainWriteToken::CreateForTesting(), out.txid.AsUint256(), out.vout, coin), Status::Ok);
    }
    static Transaction Spend(const OutPoint& out) {
        Transaction tx; tx.version = 2; tx.vin.emplace_back();
        tx.vin[0].prevout.txid = out.txid; tx.vin[0].prevout.vout = out.vout;
        tx.vout.emplace_back(AmountUna::Una(90000), std::vector<uint8_t>{0x51}); return tx;
    }
};
TEST_F(WalletInputCoins, SnapshotMapCoinNeedsNoAuxiliaryRowAndSpentCoinCannotFallBack) {
    const auto out = Out(1);
    ASSERT_TRUE(coins.AddCoin(out, CoinAt()));
    Mempool pool(&db, &coins);
    auto result = pool.getConfirmedWalletCoins({out, Out(2)});
    ASSERT_EQ(result.size(), 2U); ASSERT_TRUE(result[0]); EXPECT_FALSE(result[1]);
    EXPECT_EQ(result[0]->value.GetUna(), 100000U);
    PutAux(out); // Deliberately stale: stateful mode must not trust this after spend.
    auto spent = coins.SpendCoin(out); ASSERT_TRUE(spent);
    EXPECT_FALSE(pool.getConfirmedWalletCoins({out})[0]);
    ASSERT_TRUE(coins.AddCoin(out, *spent)); // Undo restores eligibility.
    EXPECT_TRUE(pool.getConfirmedWalletCoins({out})[0]);
}
TEST_F(WalletInputCoins, StatelessPostBaseCoinUsesConfiguredDatabaseFallback) {
    const auto out = Out(2); PutAux(out);
    Mempool pool(&db, &coins, true);
    EXPECT_TRUE(pool.getConfirmedWalletCoins({out})[0]);
    ASSERT_EQ(db.deleteCoin(ChainWriteToken::CreateForTesting(), out.txid.AsUint256(), 0), Status::Ok);
    EXPECT_FALSE(pool.getConfirmedWalletCoins({out})[0]);
}
TEST_F(WalletInputCoins, FrozenCoinRequiresLiveForestEvenWhenAuxiliaryRowSurvives) {
    const auto out = Out(3); PutAux(out);
    Mempool pool(&db, &coins, true);
    bool live = true;
    pool.setPreBaseCoinPredicate([&](const OutPoint& candidate) { return candidate == out; });
    pool.setPreBaseCoinResolver([&](const OutPoint& candidate) -> std::optional<consensus::UTXOEntry> {
        if (candidate == out && live) return CoinAt(11, true);
        return std::nullopt;
    });
    auto result = pool.getConfirmedWalletCoins({out});
    ASSERT_TRUE(result[0]); EXPECT_EQ(result[0]->height, 11U); EXPECT_TRUE(result[0]->isCoinbase);
    live = false; EXPECT_FALSE(pool.getConfirmedWalletCoins({out})[0]);
}
TEST_F(WalletInputCoins, MempoolSpendAndUnconfirmedChangeAreNotCandidates) {
    const auto out = Out(4); ASSERT_TRUE(coins.AddCoin(out, CoinAt()));
    Mempool pool(&db, &coins);
    const auto tx = Spend(out); pool.addUnchecked(tx);
    auto result = pool.getConfirmedWalletCoins({out, OutPoint{tx.GetTxid(), 0}});
    EXPECT_FALSE(result[0]); EXPECT_FALSE(result[1]);
}
TEST_F(WalletInputCoins, MaturityUsesCurrentNextBlockAndRejectsFutureCoins) {
    const auto mature = Out(5), immature = Out(6), future = Out(7);
    ASSERT_TRUE(coins.AddCoin(mature, CoinAt(11, true)));
    ASSERT_TRUE(coins.AddCoin(immature, CoinAt(12, true)));
    ASSERT_TRUE(coins.AddCoin(future, CoinAt(111, false)));
    Mempool pool(&db, &coins);
    auto result = pool.getConfirmedWalletCoins({mature, immature, future});
    EXPECT_TRUE(result[0]); EXPECT_FALSE(result[1]); EXPECT_FALSE(result[2]);
    coins.SetBestBlock(uint256{}, 109);
    EXPECT_FALSE(pool.getConfirmedWalletCoins({mature})[0]);
}
TEST_F(WalletInputCoins, GuardPrecedesMempoolLockCoversBatchAndIsReleasedBeforeReturn) {
    const auto out = Out(8); ASSERT_TRUE(coins.AddCoin(out, CoinAt()));
    Mempool pool(&db, &coins);
    bool held = false; unsigned acquisitions = 0, lookups = 0;
    struct Guard final : Mempool::ChainstateReadGuard {
        bool& held; explicit Guard(bool& flag) : held(flag) { held = true; }
        ~Guard() override { held = false; }
    };
    pool.setChainstateReadGuardFactory([&]() {
        ++acquisitions;
        Block empty; pool.onBlockConnected(empty, 110, {}); // Needs m_mutex itself.
        return std::make_unique<Guard>(held);
    });
    pool.setPreBaseCoinPredicate([&](const OutPoint&) { EXPECT_TRUE(held); ++lookups; return false; });
    auto result = pool.getConfirmedWalletCoins({out, Out(9)});
    EXPECT_TRUE(result[0]); EXPECT_FALSE(result[1]);
    EXPECT_EQ(acquisitions, 1U); EXPECT_EQ(lookups, 2U); EXPECT_FALSE(held);
}
TEST_F(WalletInputCoins, UnavailableChainstateGuardFailsClosed) {
    const auto out = Out(10); ASSERT_TRUE(coins.AddCoin(out, CoinAt()));
    Mempool pool(&db, &coins);
    pool.setChainstateReadGuardFactory([]() -> std::unique_ptr<Mempool::ChainstateReadGuard> { return nullptr; });
    pool.setPreBaseCoinPredicate([](const OutPoint&) { ADD_FAILURE() << "must not read without guard"; return false; });
    auto result = pool.getConfirmedWalletCoins({out});
    ASSERT_EQ(result.size(), 1U); EXPECT_FALSE(result[0]);
}
} // namespace
