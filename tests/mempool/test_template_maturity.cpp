#include "consensus/chainparams.h"
#include "daemon/mempool.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <filesystem>

namespace {
using namespace dinero;

class TemplateMaturityTest : public ::testing::Test {
protected:
    void SetUp() override {
        // These selection-only fixtures bypass script admission. Keep both
        // admission height 1 (addUnchecked) and target 110/111 below the
        // mainnet covenant activations; real admission is covered by DPI e2e.
        SelectParams(Chain::MAINNET);
        root_ = std::filesystem::temp_directory_path() /
            ("template_maturity_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_EQ(db_.init(root_ / "chaindb"), Status::Ok);
    }
    void TearDown() override {
        db_.close();
        std::filesystem::remove_all(root_);
    }
    OutPoint PutCoin(uint8_t id, uint32_t height, bool coinbase) {
        uint256 hash;
        hash.data[0] = id;
        OutPoint out{TxId(hash), 0};
        Coin coin;
        coin.amount = 100'000;
        coin.script_pubkey = "51";
        coin.height = height;
        coin.coinbase = coinbase;
        EXPECT_EQ(db_.putCoin(ChainWriteToken::CreateForTesting(), hash, 0, coin), Status::Ok);
        return out;
    }
    Transaction Spend(const OutPoint& out, uint64_t value = 90'000) {
        Transaction tx;
        tx.version = 2;
        tx.vin.emplace_back();
        tx.vin[0].prevout.txid = out.txid;
        tx.vin[0].prevout.vout = out.vout;
        tx.vout.emplace_back(AmountUna::Una(value), std::vector<uint8_t>{0x51});
        return tx;
    }
    static bool Contains(const std::vector<Transaction>& txs, const Transaction& tx) {
        return std::any_of(txs.begin(), txs.end(), [&](const auto& selected) {
            return selected.GetTxid() == tx.GetTxid();
        });
    }
    ChainDB db_;
    std::filesystem::path root_;
};

TEST_F(TemplateMaturityTest, RewindExcludesCoinbaseSpendAndDescendantsThenReleasesThem) {
    Mempool pool(&db_);
    const auto parent = Spend(PutCoin(1, 11, true));
    const auto child = Spend(OutPoint{parent.GetTxid(), 0}, 1'000);
    const auto unrelated = Spend(PutCoin(2, 1, true));
    pool.addUnchecked(parent);
    pool.addUnchecked(child);
    pool.addUnchecked(unrelated);
    ASSERT_EQ(pool.selectTransactionsForBlock(1'000'000, 4'000'000, 111).size(), 3U);

    const auto rewound = pool.selectTransactionsForBlock(1'000'000, 4'000'000, 110);
    EXPECT_FALSE(Contains(rewound, parent));
    EXPECT_FALSE(Contains(rewound, child));
    EXPECT_TRUE(Contains(rewound, unrelated));
    EXPECT_EQ(pool.size(), 3U);  // Selection must not evict pending payments.
    const auto mature = pool.selectTransactionsForBlock(1'000'000, 4'000'000, 111);
    ASSERT_EQ(mature.size(), 3U);
    const auto parent_pos = std::find_if(mature.begin(), mature.end(), [&](const auto& tx) {
        return tx.GetTxid() == parent.GetTxid();
    });
    const auto child_pos = std::find_if(mature.begin(), mature.end(), [&](const auto& tx) {
        return tx.GetTxid() == child.GetTxid();
    });
    EXPECT_LT(parent_pos, child_pos);
}

TEST_F(TemplateMaturityTest, FutureCoinbaseHeightDoesNotUnderflow) {
    Mempool pool(&db_);
    pool.addUnchecked(Spend(PutCoin(3, 200, true)));
    EXPECT_TRUE(pool.selectTransactionsForBlock(1'000'000, 4'000'000, 110).empty());
}

TEST_F(TemplateMaturityTest, OrdinaryOutputsDoNotRequireCoinbaseMaturity) {
    Mempool pool(&db_);
    pool.addUnchecked(Spend(PutCoin(4, 109, false)));
    EXPECT_EQ(pool.selectTransactionsForBlock(1'000'000, 4'000'000, 110).size(), 1U);
}

TEST_F(TemplateMaturityTest, FrozenCoinUsesLiveForestAuthorizationAndOriginalHeight) {
    Mempool pool(&db_);
    // Auxiliary row deliberately disagrees with the authorized frozen record.
    const auto out = PutCoin(5, 1, false);
    bool authorized = true;
    pool.setPreBaseCoinPredicate([out](const OutPoint& candidate) { return candidate == out; });
    pool.setPreBaseCoinResolver([&](const OutPoint& candidate)
        -> std::optional<consensus::UTXOEntry> {
        if (!authorized || candidate != out) return std::nullopt;
        consensus::UTXOEntry coin;
        coin.value = AmountUna::Una(100'000);
        coin.scriptPubKey = {0x51};
        coin.height = 11;
        coin.isCoinbase = true;
        return coin;
    });
    pool.addUnchecked(Spend(out));
    EXPECT_TRUE(pool.selectTransactionsForBlock(1'000'000, 4'000'000, 110).empty());
    EXPECT_EQ(pool.selectTransactionsForBlock(1'000'000, 4'000'000, 111).size(), 1U);
    authorized = false;
    EXPECT_TRUE(pool.selectTransactionsForBlock(1'000'000, 4'000'000, 111).empty());
}

TEST_F(TemplateMaturityTest, ChainstateGuardPrecedesMempoolLockAndCoversCoinLookup) {
    Mempool pool(&db_);
    const auto out = PutCoin(6, 1, false);
    pool.addUnchecked(Spend(out));
    bool held = false;
    unsigned acquisitions = 0;
    struct Guard final : Mempool::ChainstateReadGuard {
        bool& held;
        explicit Guard(bool& flag) : held(flag) { held = true; }
        ~Guard() override { held = false; }
    };
    pool.setChainstateReadGuardFactory([&]() {
        ++acquisitions;
        // A chainstate writer can notify the mempool before releasing its
        // lock. This must complete before selection owns m_mutex.
        Block empty;
        pool.onBlockConnected(empty, 110, {});
        return std::make_unique<Guard>(held);
    });
    pool.setPreBaseCoinPredicate([&](const OutPoint&) {
        EXPECT_TRUE(held);
        return false;
    });
    EXPECT_EQ(pool.selectTransactionsForBlock(1'000'000, 4'000'000, 111).size(), 1U);
    EXPECT_EQ(acquisitions, 1U);
    EXPECT_FALSE(held);
}
} // namespace
