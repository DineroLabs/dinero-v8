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
    explicit StatefulSnapshotCacheFixture(bool store_frozen = true) {
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
        if (store_frozen) Store(tip.hash);
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
namespace {
struct PromotedLegacyCoinCacheFixture : StatefulSnapshotCacheFixture {
    const uint32_t base_height = 10;
    const ChainWriteToken token = ChainWriteToken::CreateForTesting();
    PromotedLegacyCoinCacheFixture() : StatefulSnapshotCacheFixture(false) {
        const auto base_hash = base.blocks.at(base_height - 1).GetHash();
        ShieldedStateStartupTestAccess::CacheSnapshotBase(*owner.source, base_hash, base_height, true);
        if (owner.db.putHeightIndex(token, base_height, base_hash) != Status::Ok ||
            owner.db.putHeader(token, tip.hash, base.blocks.back().header, tip.height,
                               arith_uint256(tip.height + 1)) != Status::Ok ||
            owner.db.putHeightIndex(token, tip.height, tip.hash) != Status::Ok ||
            owner.db.setTip(token, tip.hash, tip.height, arith_uint256(tip.height + 1)) != Status::Ok ||
            owner.db.putForestTipMarker(token, {int32_t(tip.height), tip.hash,
                                               base.blocks.back().header.utreexo_root}) != Status::Ok)
            throw std::runtime_error("legacy promoted durable binding");
        for (const auto& row : rows)
            if (owner.db.putCoin(token, row.txid, row.vout, row.coin) != Status::Ok)
                throw std::runtime_error("legacy ordinary coin persistence");
        auto index = ChainstateService::AcquireWalletIndexUse(owner.source);
        if (!index->Index().SetMetadata(assumeutxo::kLifecycleStateKey, "fully_validated") ||
            !index->Index().SetMetadata(assumeutxo::kLcBaseHeightKey, std::to_string(base_height)) ||
            !index->Index().SetMetadata(assumeutxo::kLcBaseBlockKey, base_hash.GetHex()))
            throw std::runtime_error("legacy promoted lifecycle binding");
    }
};
}
TEST(StatefulSnapshotCache, PromotedLegacyMissingFrozenStoreMigratesCompleteOrdinaryCoins) {
    PromotedLegacyCoinCacheFixture f;
    ASSERT_EQ(f.owner.db.getPreBaseCoinSetBase().status(), Status::NotFound);
    ASSERT_EQ(f.owner.db.getValidatedTip().status(), Status::NotFound);
    const auto recovery_marker = f.owner.root / "chainstate_recovery.marker";
    const std::string pending = "unchanged interrupted activation evidence\n";
    { std::ofstream out(recovery_marker); out << pending; }
    const auto root = f.owner.source->GetConsensusUTXOSet()->GetForest().getCommitment();
    std::string error;
    ASSERT_TRUE(ShieldedStateStartupTestAccess::RestoreSnapshotCache(*f.owner.source, error)) << error;
    const auto marker = f.owner.db.getPreBaseCoinSetBase();
    ASSERT_TRUE(marker.ok());
    EXPECT_EQ(marker->first, f.base.blocks.at(f.base_height - 1).GetHash());
    EXPECT_EQ(marker->second, f.base_height);
    size_t expected = 0;
    for (const auto& [point, coin] : f.base.coins) {
        const auto leaf = consensus::HashUTXOForCreationHeight(point.txid.AsUint256(), point.vout,
            coin.value.GetUna(), coin.scriptPubKey, coin.height, coin.isCoinbase);
        if (coin.height <= f.base_height && f.base.replay.Forest()->findLeafPosition(leaf)) ++expected;
    }
    EXPECT_EQ(f.owner.source->GetConsensusUTXOSet()->GetSetSize(), expected);
    EXPECT_EQ(f.owner.source->GetConsensusUTXOSet()->GetForest().getCommitment(), root);
    EXPECT_EQ(f.owner.source->GetConsensusUTXOSet()->GetBestBlock(), f.tip.hash);
    EXPECT_EQ(f.owner.db.getValidatedTip().status(), Status::NotFound);
    std::ifstream marker_input(recovery_marker);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(marker_input), {}), pending);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::RestoreSnapshotCache(*f.owner.source, error)) << error;
}
TEST(StatefulSnapshotCache, PromotedLegacyMigrationRefusesIncompleteOrForeignOrdinaryCoins) {
    for (const bool foreign : {false, true}) {
        PromotedLegacyCoinCacheFixture f;
        const auto& row = f.rows.back();
        if (foreign) {
            auto wrong = row.coin; ++wrong.amount;
            ASSERT_EQ(f.owner.db.putCoin(f.token, row.txid, row.vout, wrong), Status::Ok);
        } else {
            ASSERT_EQ(f.owner.db.deleteCoin(f.token, row.txid, row.vout), Status::Ok);
        }
        std::string error;
        EXPECT_FALSE(ShieldedStateStartupTestAccess::RestoreSnapshotCache(*f.owner.source, error));
        EXPECT_EQ(f.owner.db.getPreBaseCoinSetBase().status(), Status::NotFound);
        EXPECT_EQ(f.owner.source->GetConsensusUTXOSet()->GetSetSize(), 0u);
    }
}
TEST(StatefulSnapshotCache, PromotedLegacyMigrationCannotOverrideConflictingBindings) {
    for (unsigned conflict = 0; conflict < 3; ++conflict) {
        PromotedLegacyCoinCacheFixture f;
        if (conflict == 0) {
            ASSERT_EQ(f.owner.db.putHeightIndex(f.token, f.base_height, f.tip.hash), Status::Ok);
        } else if (conflict == 1) {
            auto wrong = f.base.blocks.back().header.utreexo_root; wrong.data[0] ^= 1;
            ASSERT_EQ(f.owner.db.putForestTipMarker(f.token, {int32_t(f.tip.height), f.tip.hash, wrong}), Status::Ok);
        } else {
            auto index = ChainstateService::AcquireWalletIndexUse(f.owner.source);
            ASSERT_TRUE(index->Index().SetMetadata("prebase_coins_snapshot_backfill_v2", "1"));
        }
        std::string error;
        EXPECT_FALSE(ShieldedStateStartupTestAccess::RestoreSnapshotCache(*f.owner.source, error));
        EXPECT_EQ(f.owner.db.getPreBaseCoinSetBase().status(), Status::NotFound);
        EXPECT_EQ(f.owner.source->GetConsensusUTXOSet()->GetSetSize(), 0u);
    }
}
}
