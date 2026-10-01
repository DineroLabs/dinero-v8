#pragma once
#include "vault_explicit_creation_checks.h"
namespace dinero {
TEST(PoolGenesisSource, ActualStartedGenesisVerifiedWithoutWritingMarkers) {
    VaultRuntimeReset reset;VaultExplicitChainCreationFixture f;
    auto& db=f.chain.db;auto& source=*f.chain.source;
    ASSERT_EQ(db.getValidatedTip().status(),Status::NotFound);
    ASSERT_EQ(db.getForestTipMarker().status(),Status::NotFound);
    EXPECT_FALSE(source.getCanonicalBlockHash(0).ok());
    const auto first=source.getVerifiedGenesisBlockHash();ASSERT_TRUE(first.ok());
    EXPECT_EQ(first->GetHex(),Params().genesis_hash);
    const auto again=source.getVerifiedGenesisBlockHash();ASSERT_TRUE(again.ok());EXPECT_EQ(*again,*first);
    EXPECT_EQ(db.getValidatedTip().status(),Status::NotFound);
    EXPECT_EQ(db.getForestTipMarker().status(),Status::NotFound);
    EXPECT_FALSE(source.getCanonicalBlockHash(0).ok());
}
TEST(PoolGenesisSource, MissingExtraAndChangedDurableCoinsRefuseWithoutRepair) {
    VaultRuntimeReset reset;VaultExplicitChainCreationFixture f;
    auto& db=f.chain.db;auto& source=*f.chain.source;
    const auto token=ChainWriteToken::CreateForTesting();
    const auto genesis=SelectedGenesis();const auto id=genesis.vtx.front().GetTxid().AsUint256();
    const auto saved=db.getCoin(id,0);ASSERT_TRUE(saved.ok());
    ASSERT_TRUE(source.getVerifiedGenesisBlockHash().ok());
    auto changed=*saved;++changed.amount;
    ASSERT_EQ(db.putCoin(token,id,0,changed),Status::Ok);
    EXPECT_FALSE(source.getVerifiedGenesisBlockHash().ok());
    ASSERT_TRUE(db.getCoin(id,0).ok());EXPECT_EQ(db.getCoin(id,0)->amount,changed.amount);
    ASSERT_EQ(db.putCoin(token,id,0,*saved),Status::Ok);
    ASSERT_EQ(db.deleteCoin(token,id,0),Status::Ok);
    EXPECT_FALSE(source.getVerifiedGenesisBlockHash().ok());EXPECT_EQ(db.getCoin(id,0).status(),Status::NotFound);
    ASSERT_EQ(db.putCoin(token,id,0,*saved),Status::Ok);
    ASSERT_EQ(db.putCoin(token,id,77,*saved),Status::Ok);
    EXPECT_FALSE(source.getVerifiedGenesisBlockHash().ok());EXPECT_TRUE(db.getCoin(id,77).ok());
    ASSERT_EQ(db.deleteCoin(token,id,77),Status::Ok);
    EXPECT_TRUE(source.getVerifiedGenesisBlockHash().ok());
    EXPECT_EQ(db.getValidatedTip().status(),Status::NotFound);EXPECT_EQ(db.getForestTipMarker().status(),Status::NotFound);
}
TEST(PoolGenesisSource, InconsistentTipCheckpointAndPresentOwnersRefuse) {
    VaultRuntimeReset reset;VaultExplicitChainCreationFixture f;
    auto& db=f.chain.db;auto& source=*f.chain.source;const auto token=ChainWriteToken::CreateForTesting();
    const auto tip=db.getTip();ASSERT_TRUE(tip.ok());const auto checkpoint=db.getUtreexoCheckpoint(0);ASSERT_TRUE(checkpoint.ok());
    auto wrong=tip->hash;wrong.begin()[0]^=1;
    ASSERT_EQ(db.setTip(token,wrong,0,tip->work),Status::Ok);EXPECT_FALSE(source.getVerifiedGenesisBlockHash().ok());
    ASSERT_EQ(db.setTip(token,tip->hash,0,tip->work),Status::Ok);
    auto bad=*checkpoint;ASSERT_FALSE(bad.empty());bad.back()^=1;
    ASSERT_EQ(db.putUtreexoCheckpointWithChecksum(token,0,bad),Status::Ok);EXPECT_FALSE(source.getVerifiedGenesisBlockHash().ok());
    EXPECT_EQ(*db.getUtreexoCheckpoint(0),bad);
    ASSERT_EQ(db.putUtreexoCheckpointWithChecksum(token,0,*checkpoint),Status::Ok);
    ASSERT_EQ(db.setValidatedTip(token,wrong,0),Status::Ok);EXPECT_FALSE(source.getVerifiedGenesisBlockHash().ok());
    EXPECT_EQ(db.getValidatedTip()->hash,wrong);
    ASSERT_EQ(db.setValidatedTip(token,tip->hash,0),Status::Ok);
    const auto root=SelectedGenesis().header.utreexo_root;
    ASSERT_EQ(db.putForestTipMarker(token,{0,wrong,root}),Status::Ok);EXPECT_FALSE(source.getVerifiedGenesisBlockHash().ok());
    ASSERT_EQ(db.putForestTipMarker(token,{0,tip->hash,root}),Status::Ok);EXPECT_TRUE(source.getVerifiedGenesisBlockHash().ok());
    source.EnterSafeMode("isolated genesis source unavailable");EXPECT_FALSE(source.getVerifiedGenesisBlockHash().ok());
}
} // namespace dinero
