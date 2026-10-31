#pragma once
namespace dinero {
namespace {
struct SnapshotCacheConfig {
    bool saved = GetConfig().utreexo_stateless;
    SnapshotCacheConfig() { GetConfig().utreexo_stateless = false; }
    ~SnapshotCacheConfig() { GetConfig().utreexo_stateless = saved; }
};
struct StatefulSnapshotCacheFixture {
    SnapshotCacheConfig config;
    WalletIndexOwnerFixture owner;
    SnapshotWalletSourceFixture base;
    CBlockIndex tip;
    std::vector<ChainDB::PreBaseCoinRecord> rows;
    StatefulSnapshotCacheFixture() {
        tip.hash = base.blocks.back().GetHash(); tip.height = base.blocks.size();
        ShieldedStateStartupTestAccess::Select(*owner.source, tip);
        ShieldedStateStartupTestAccess::CacheSnapshotBase(*owner.source, tip.hash, tip.height);
        auto* coins = static_cast<consensus::ConsensusUTXOSet*>(owner.source->GetConsensusUTXOSet());
        coins->ReplaceForestGuarded(*base.replay.Forest());
        for (const auto& [point, entry] : base.coins) {
            Coin coin; coin.amount = entry.value.GetUna(); coin.script_pubkey = util::hex(entry.scriptPubKey);
            coin.height = entry.height; coin.coinbase = entry.isCoinbase;
            coin.is_confidential = entry.is_confidential; coin.commitment = entry.commitment;
            rows.push_back({point.txid.AsUint256(), point.vout, std::move(coin)});
        }
        Store(tip.hash);
    }
    void Store(const uint256& hash) {
        if (owner.db.replacePreBaseCoins(ChainWriteToken::CreateForTesting(), hash, tip.height, rows) != Status::Ok)
            throw std::runtime_error("snapshot cache fixture store");
    }
    ~StatefulSnapshotCacheFixture() {
        // The selected tip belongs to this fixture; stop its borrower before
        // destroying the tip member, then let the ordinary owner clean up.
        owner.source->Stop(); owner.context.chainstate.reset(); owner.source.reset();
    }
};
}
TEST(StatefulSnapshotCache, RestoresOnlyLiveCoinsWithoutChangingSelectedForest) {
    StatefulSnapshotCacheFixture f;
    const auto live = std::find_if(f.base.coins.begin(), f.base.coins.end(),
        [](const auto& c) { return c.second.height > 0; }); ASSERT_NE(live, f.base.coins.end());
    auto forest = f.base.replay.Forest()->clone();
    const auto leaf = consensus::HashUTXOForCreationHeight(live->first.txid.AsUint256(), live->first.vout,
        live->second.value.GetUna(), live->second.scriptPubKey, live->second.height, live->second.isCoinbase);
    const auto position = forest.findLeafPosition(leaf); ASSERT_TRUE(position);
    ASSERT_TRUE(forest.removeAtKnownPosition(*position, leaf));
    auto* coins = static_cast<consensus::ConsensusUTXOSet*>(f.owner.source->GetConsensusUTXOSet());
    coins->ReplaceForestGuarded(std::move(forest)); const auto root = coins->GetForest().getCommitment();
    std::string error; ASSERT_TRUE(ShieldedStateStartupTestAccess::RestoreSnapshotCache(*f.owner.source, error)) << error;
    EXPECT_EQ(coins->GetCoin(live->first), nullptr); EXPECT_GT(coins->GetSetSize(), 0u);
    EXPECT_EQ(coins->GetForest().getCommitment(), root); EXPECT_EQ(coins->GetBestBlock(), f.tip.hash);
    const auto count = coins->GetSetSize();
    ASSERT_TRUE(ShieldedStateStartupTestAccess::RestoreSnapshotCache(*f.owner.source, error));
    EXPECT_EQ(coins->GetSetSize(), count);
}
TEST(StatefulSnapshotCache, ForeignOrUnreadableRowsCannotPublishPartialCache) {
    StatefulSnapshotCacheFixture f; auto wrong = f.tip.hash; wrong.data[0] ^= 1; f.Store(wrong);
    std::string error; EXPECT_FALSE(ShieldedStateStartupTestAccess::RestoreSnapshotCache(*f.owner.source, error));
    EXPECT_EQ(f.owner.source->GetConsensusUTXOSet()->GetSetSize(), 0u);
    f.rows.back().coin.script_pubkey = "not hex"; f.Store(f.tip.hash);
    EXPECT_FALSE(ShieldedStateStartupTestAccess::RestoreSnapshotCache(*f.owner.source, error));
    EXPECT_EQ(f.owner.source->GetConsensusUTXOSet()->GetSetSize(), 0u);
}
TEST(StatefulSnapshotCache, PromotedBaseStillRequiresExactCanonicalHash) {
    StatefulSnapshotCacheFixture f;
    ShieldedStateStartupTestAccess::CacheSnapshotBase(*f.owner.source, f.tip.hash, f.tip.height, true);
    std::string error; EXPECT_FALSE(ShieldedStateStartupTestAccess::RestoreSnapshotCache(*f.owner.source, error));
    ASSERT_EQ(f.owner.db.putHeightIndex(ChainWriteToken::CreateForTesting(), f.tip.height, f.tip.hash), Status::Ok);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::RestoreSnapshotCache(*f.owner.source, error)) << error;
    EXPECT_GT(f.owner.source->GetConsensusUTXOSet()->GetSetSize(), 0u);
}
TEST(StatefulSnapshotCache, StatelessProfileNeverMaterializesFullSnapshotMap) {
    StatefulSnapshotCacheFixture f; GetConfig().utreexo_stateless = true;
    std::string error; EXPECT_TRUE(ShieldedStateStartupTestAccess::RestoreSnapshotCache(*f.owner.source, error));
    EXPECT_EQ(f.owner.source->GetConsensusUTXOSet()->GetSetSize(), 0u);
}
}
