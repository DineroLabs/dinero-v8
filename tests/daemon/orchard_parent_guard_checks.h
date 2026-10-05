#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardParentGuards, NestedMiningSelectionUsesPreparedHistory) {
    OrchardAdmissionFixture f;
    ASSERT_TRUE(f.Read());
    {
        auto mining=ChainstateService::AcquireMiningReadGuard(f.service); ASSERT_TRUE(mining);
        // Sequential storage change after preparation: the retained validated
        // branch is immutable. A second historical replay here would encounter
        // this malformed body while the outer selected lock is held.
        auto changed=f.blocks.front(); changed.vtx.front().vout.front().value=AmountUna::Una(1);
        ASSERT_EQ(f.db.putBlock(f.token,changed.GetHash(),changed),Status::Ok);
        {
            auto pool=ChainstateService::AcquireMempoolChainstateRead(f.service); ASSERT_TRUE(pool);
            const auto checked=pool->ValidateBlockSelection({},102);
            EXPECT_TRUE(checked.result.accepted())<<checked.result.message;
            EXPECT_EQ(checked.parent_hash,f.tip.hash);
        }
        ASSERT_EQ(f.db.putBlock(f.token,f.blocks.front().GetHash(),f.blocks.front()),Status::Ok);
    }
    EXPECT_TRUE(f.Read()); f.CheckUnpublished();
}

TEST(OrchardParentGuards, PreparedHistoryDoesNotBypassCurrentCoinComparison) {
    OrchardAdmissionFixture f;
    auto mining=ChainstateService::AcquireMiningReadGuard(f.service); ASSERT_TRUE(mining);
    const auto point=f.replay->ProvenUtxos().begin()->first;
    const auto original=f.db.getCoin(point.txid.AsUint256(),point.vout); ASSERT_TRUE(original.ok());
    auto wrong=*original; ++wrong.amount;
    ASSERT_EQ(f.db.putCoin(f.token,point.txid.AsUint256(),point.vout,wrong),Status::Ok);
    {
        auto pool=ChainstateService::AcquireMempoolChainstateRead(f.service); ASSERT_TRUE(pool);
        EXPECT_FALSE(pool->ValidateBlockSelection({},102).result.accepted());
    }
    ASSERT_EQ(f.db.putCoin(f.token,point.txid.AsUint256(),point.vout,*original),Status::Ok);
    {
        auto pool=ChainstateService::AcquireMempoolChainstateRead(f.service); ASSERT_TRUE(pool);
        EXPECT_TRUE(pool->ValidateBlockSelection({},102).result.accepted());
    }
    f.CheckUnpublished();
}

TEST(OrchardParentGuards, RawOrUnpreparedRecursiveCallerNeverFallsBackToLockedReplay) {
    OrchardAdmissionFixture f;
    ASSERT_TRUE(f.Read());
    {
        auto selected=f.service->AcquireBlockIngressActivationLock();
        auto pool=ChainstateService::AcquireMempoolChainstateRead(f.service); ASSERT_TRUE(pool);
        EXPECT_FALSE(pool->ValidateBlockSelection({},102).result.accepted());
    }
    f.service->setChainDB(nullptr); f.service->setChainDB(&f.db);
    EXPECT_TRUE(f.Read());
    {
        auto pool=ChainstateService::AcquireMempoolChainstateRead(f.service); ASSERT_TRUE(pool);
        EXPECT_FALSE(pool->ValidateBlockSelection({},102).result.accepted());
    }
    f.service->setOwnedChainDB(f.database);
    {
        auto pool=ChainstateService::AcquireMempoolChainstateRead(f.service); ASSERT_TRUE(pool);
        EXPECT_TRUE(pool->ValidateBlockSelection({},102).result.accepted());
    }
    f.CheckUnpublished();
}
#else
TEST(OrchardParentGuards, BackendOffKeepsSelectionUnavailable) {
    auto service=std::make_shared<ChainstateService>();
    auto owner=ChainstateService::AcquireMempoolChainstateRead(service); ASSERT_TRUE(owner);
    EXPECT_EQ(owner->ValidateBlockSelection({},1).result.code,TxRejectCode::UNAVAILABLE);
}
#endif
