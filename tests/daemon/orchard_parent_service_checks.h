#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
class OwnedSelectedParentFixture {
public:
    ChainParams previous_params = Params();
    bool previous_stateless = GetConfig().utreexo_stateless;
    std::filesystem::path path;
    std::shared_ptr<ChainDB> database=std::make_shared<ChainDB>();
    ChainDB& db=*database;
    std::shared_ptr<ChainstateService> service_owner=std::make_shared<ChainstateService>();
    ChainstateService& service=*service_owner;
    CBlockIndex tip;
    std::vector<Block> blocks;
    std::unique_ptr<assumeutxo::AssumeUtxoReplayEngine> replay;
    const ChainWriteToken token = ChainWriteToken::CreateForTesting();
    static void Require(bool value) { if (!value) throw std::runtime_error("selected parent fixture setup"); }
    explicit OwnedSelectedParentFixture(uint32_t parent_height=3,bool catalog_commitment=false,bool separated=true) {
        Require(parent_height>0&&parent_height<UINT32_MAX);
        MutableParams().orchard_activation_height=parent_height+1;
        MutableParams().orchard_branch_id=1;
        MutableParams().orchard_catalog_commitment_activation_height=catalog_commitment?parent_height+1:UINT32_MAX;
        MutableParams().shielded_activation_height=1;
        MutableParams().shielded_epoch_reset_height=UINT32_MAX;
        MutableParams().shielded_spend_auth_epoch_reset_height=UINT32_MAX;
        GetConfig().utreexo_stateless=false;
        auto name=(std::filesystem::temp_directory_path()/"selected_parent_XXXXXX").string();
        Require(mkdtemp(name.data())!=nullptr);path=name;
        // Generated separated-layout fixture. Normal ChainDB open correctly
        // creates the legacy layout and must never silently migrate it.
        {
            auto families=shielded_store_fixture::legacy;
            if(separated)families.push_back(shielded_store_fixture::shielded);
            shielded_store_fixture::Raw raw(path,families);
            const uint32_t schema=4;
            raw.put("meta","schema_version",std::string(reinterpret_cast<const char*>(&schema),4));
            if(separated) {
                raw.put("meta","storage_layout_v1",shielded_store_fixture::ready);
                consensus::shielded::CommitmentTree tree;consensus::shielded::AnchorHistory anchors;
                const auto frontier=tree.SerializeFrontier();const auto history=anchors.SerializePersistenceBytes();
                raw.put(shielded_store_fixture::shielded,"Mshielded_frontier",{frontier.begin(),frontier.end()});
                raw.put(shielded_store_fixture::shielded,"Mshielded_anchor_history",{history.begin(),history.end()});
            }
            raw.put("meta","shielded_tip",std::string(84,'\0'));
        }
        Require(db.init(path)==Status::Ok);
        blocks={SelectedGenesis()};const auto rest=BuildDeterministicChain(parent_height);
        Require(rest.size()==parent_height);blocks.insert(blocks.end(),rest.begin(),rest.end());
        replay=std::make_unique<assumeutxo::AssumeUtxoReplayEngine>();
        std::string error;Require(replay->SeedGenesis(blocks.front(),error));
        arith_uint256 work{0};
        for(uint32_t h=0;h<blocks.size();++h) {
            const auto& b=blocks[h];
            if(h) Require(replay->ConnectAndAdvance(b,h,b.GetHash(),error));
            work+=GetBlockProof(b.header.difficulty);
            Require(db.putHeader(token,b.GetHash(),b.header,h,work)==Status::Ok);
            Require(db.putHeightIndex(token,h,b.GetHash())==Status::Ok);
            Require(db.putBlock(token,b.GetHash(),b)==Status::Ok);
            for(uint32_t i=0;i<b.vtx.size();++i)
                Require(db.putTxIndex(token,b.vtx[i].GetTxid().AsUint256(),b.GetHash(),i)==Status::Ok);
        }
        tip=CBlockIndex(blocks.back().header,parent_height);tip.chainwork=work.GetHex();
        Require(db.setTip(token,tip.hash,tip.height,work)==Status::Ok);
        Require(db.setValidatedTip(token,tip.hash,tip.height)==Status::Ok);
        for(const auto& [point,entry]:replay->ProvenUtxos()) {
            Coin c;c.amount=entry.value.GetUna();c.script_pubkey=util::hex(entry.scriptPubKey);
            c.height=entry.height;c.coinbase=entry.isCoinbase;c.is_confidential=entry.is_confidential;c.commitment=entry.commitment;
            Require(db.putCoin(token,point.txid.AsUint256(),point.vout,c)==Status::Ok);
        }
        Require(db.putForestTipMarker(token,{int32_t(parent_height),tip.hash,uint256::FromHexUnsafe(replay->UtreexoRootHex())})==Status::Ok);
        const auto frontier=replay->ShieldedTree()->SerializeFrontier();
        const auto anchors=replay->ShieldedAnchors()->SerializePersistenceBytes();
        Require(db.putShieldedState(token,ChainDB::ShieldedStateRecord::Frontier,{frontier.begin(),frontier.end()})==Status::Ok);
        Require(db.putShieldedState(token,ChainDB::ShieldedStateRecord::AnchorHistory,{anchors.begin(),anchors.end()})==Status::Ok);
        const auto root=replay->ShieldedTree()->Root();uint256 tree_root;std::copy(root.begin(),root.end(),tree_root.begin());
        Require(db.putShieldedTipMarker(token,{int32_t(parent_height),tip.hash,tree_root,replay->ShieldedTree()->Size(),0})==Status::Ok);
        service.setOwnedChainDB(database);ShieldedStateStartupTestAccess::BoundaryState(service,tip,*replay);
    }
    ~OwnedSelectedParentFixture() {
        service.setChainDB(nullptr);db.close();std::error_code ec;std::filesystem::remove_all(path,ec);
        MutableParams()=previous_params;GetConfig().utreexo_stateless=previous_stateless;
    }
    auto Read() {return ShieldedStateStartupTestAccess::Boundary(service);}
    void CheckUnpublished() {
        ASSERT_EQ(db.getLegacyRetirementState().status(),db.hasSeparatedShieldedState()?Status::NotFound:Status::Invalid);
        ASSERT_TRUE(db.getTip().ok());EXPECT_EQ(db.getTip()->hash,tip.hash);
    }
};
}

TEST(OrchardParentService, OwnedCaptureAndFullComparisonLeaveStoresUnpublished) {
    OwnedSelectedParentFixture f;
    const auto expected=f.Read(); ASSERT_TRUE(expected);
    auto prepared=f.service.PrepareSelectedOrchardParent(); ASSERT_TRUE(prepared);
    EXPECT_EQ(prepared->Record(),*expected);
    EXPECT_EQ(f.service.CheckPreparedOrchardParent(*prepared),expected);
    f.CheckUnpublished();
    ChainstateService other; other.setOwnedChainDB(f.database);
    ShieldedStateStartupTestAccess::BoundaryState(other,f.tip,*f.replay);
    EXPECT_FALSE(other.CheckPreparedOrchardParent(*prepared));
    const auto before=f.database.use_count();
    f.service.setChainDB(nullptr);
    EXPECT_EQ(f.database.use_count(),before-1);
    EXPECT_FALSE(f.service.CheckPreparedOrchardParent(*prepared));
    // The prepared owner retains the real allocation after the service drops
    // its reference. No no-op-deleter alias of a stack database is used.
    const auto held=f.database.use_count();
    prepared.reset(); EXPECT_EQ(f.database.use_count(),held-1);
    f.CheckUnpublished();
}

TEST(OrchardParentService, RawAndRecursiveOwnersRefuseBeforeCapture) {
    SelectedParentFixture raw;
    EXPECT_FALSE(raw.service.PrepareSelectedOrchardParent());
    {
        auto lock=raw.service.AcquireBlockIngressActivationLock();
        EXPECT_FALSE(raw.service.PrepareSelectedOrchardParent());
    }
    OwnedSelectedParentFixture owned;
    {
        auto lock=owned.service.AcquireBlockIngressActivationLock();
        EXPECT_FALSE(owned.service.PrepareSelectedOrchardParent());
    }
    EXPECT_TRUE(owned.service.PrepareSelectedOrchardParent());
    raw.CheckUnpublished(); owned.CheckUnpublished();
}

TEST(OrchardParentService, LegacyStorageRefusesPreparationBeforeBuildingBoundaryCache) {
    OwnedSelectedParentFixture f(3,false,false);
    ASSERT_FALSE(f.db.hasSeparatedShieldedState());
    EXPECT_EQ(f.db.getOrchardState().status(),Status::Invalid);
    EXPECT_FALSE(f.service.PrepareSelectedOrchardParent());
    EXPECT_FALSE(ShieldedStateStartupTestAccess::HasBoundaryCache(f.service));
    f.CheckUnpublished();
}
TEST(OrchardParentService, CompletedBoundaryCacheSharesOnlyFrozenEvidenceAcrossThreads) {
    OwnedSelectedParentFixture f;
    auto first=f.service.PrepareSelectedOrchardParent();ASSERT_TRUE(first);
    const auto* identity=&first->Record();const auto record=first->Record();
    first.reset();
    bool reused=false,checked=false;
    std::thread reader([&] {
        auto next=f.service.PrepareSelectedOrchardParent();
        if(next){reused=&next->Record()==identity;checked=f.service.CheckPreparedOrchardParent(*next)==record;}
    });reader.join();
    EXPECT_TRUE(reused);EXPECT_TRUE(checked);
    // The cache must hold no thread-affine request token or service cycle.
    EXPECT_NO_THROW(f.service.Stop());
}

TEST(OrchardParentService, CachedBoundaryStillRejectsChangedCoinsAndHistoricalBodies) {
    OwnedSelectedParentFixture f;
    auto first=f.service.PrepareSelectedOrchardParent();ASSERT_TRUE(first);
    const auto point=f.replay->ProvenUtxos().begin()->first;
    const auto original=f.db.getCoin(point.txid.AsUint256(),point.vout);ASSERT_TRUE(original.ok());
    auto wrong=*original;++wrong.amount;
    ASSERT_EQ(f.db.putCoin(f.token,point.txid.AsUint256(),point.vout,wrong),Status::Ok);
    auto cached=f.service.PrepareSelectedOrchardParent();ASSERT_TRUE(cached);
    EXPECT_FALSE(f.service.CheckPreparedOrchardParent(*cached));
    ASSERT_EQ(f.db.putCoin(f.token,point.txid.AsUint256(),point.vout,*original),Status::Ok);
    EXPECT_TRUE(f.service.CheckPreparedOrchardParent(*cached));
    cached.reset();first.reset();
    auto changed=f.blocks.front();changed.vtx.front().vout.front().value=AmountUna::Una(1);
    ASSERT_EQ(f.db.putBlock(f.token,changed.GetHash(),changed),Status::Ok);
    EXPECT_FALSE(f.service.PrepareSelectedOrchardParent());
    ASSERT_EQ(f.db.putBlock(f.token,f.blocks.front().GetHash(),f.blocks.front()),Status::Ok);
    EXPECT_TRUE(f.service.PrepareSelectedOrchardParent());
    f.CheckUnpublished();
}

TEST(OrchardParentService, CommittingAnEarlierStagedHistoricalEditInvalidatesNewerCache) {
    OwnedSelectedParentFixture f;
    rocksdb::WriteBatch batch;auto changed=f.blocks.front();
    changed.vtx.front().vout.front().value=AmountUna::Una(1);
    ASSERT_EQ(f.db.putBlock(f.token,changed.GetHash(),changed,&batch),Status::Ok);
    ASSERT_TRUE(f.service.PrepareSelectedOrchardParent()); // Captures before commit.
    ASSERT_EQ(f.db.writeBatch(f.token,std::move(batch)),Status::Ok);
    EXPECT_FALSE(f.service.PrepareSelectedOrchardParent());
    ASSERT_EQ(f.db.putBlock(f.token,f.blocks.front().GetHash(),f.blocks.front()),Status::Ok);
    EXPECT_TRUE(f.service.PrepareSelectedOrchardParent());
}

TEST(OrchardParentService, CachedHistoricalAuditRejectsCanonicalAndWorkChangesOnTheSameOwner) {
    OwnedSelectedParentFixture f;
    auto prepared=f.service.PrepareSelectedOrchardParent();ASSERT_TRUE(prepared);
    const auto expected=f.service.CheckPreparedOrchardParent(*prepared);ASSERT_TRUE(expected);
    ASSERT_EQ(f.service.CheckPreparedOrchardParent(*prepared),expected);
    ASSERT_EQ(f.db.putHeightIndex(f.token,1,f.blocks.front().GetHash()),Status::Ok);
    EXPECT_FALSE(f.service.CheckPreparedOrchardParent(*prepared));
    ASSERT_EQ(f.db.putHeightIndex(f.token,1,f.blocks[1].GetHash()),Status::Ok);
    EXPECT_EQ(f.service.CheckPreparedOrchardParent(*prepared),expected);
    const auto hash=f.blocks[1].GetHash();const auto work=f.db.getBlockWork(hash);ASSERT_TRUE(work.ok());
    rocksdb::WriteBatch staged;
    ASSERT_EQ(f.db.putHeader(f.token,hash,f.blocks[1].header,1,*work+arith_uint256(1),&staged),Status::Ok);
    EXPECT_EQ(f.service.CheckPreparedOrchardParent(*prepared),expected);
    ASSERT_EQ(f.db.writeBatch(f.token,std::move(staged)),Status::Ok);
    EXPECT_FALSE(f.service.CheckPreparedOrchardParent(*prepared));
    ASSERT_EQ(f.db.putHeader(f.token,hash,f.blocks[1].header,1,*work),Status::Ok);
    EXPECT_EQ(f.service.CheckPreparedOrchardParent(*prepared),expected);
    f.CheckUnpublished();
}

TEST(OrchardParentService, NewUnselectedInputsAndIdenticalWritesDoNotDiscardCompletedPrefix) {
    OwnedSelectedParentFixture f;
    auto first=f.service.PrepareSelectedOrchardParent();ASSERT_TRUE(first);
    const auto* identity=&first->Record();const auto revision=f.db.readRevision();ASSERT_TRUE(revision);
    auto side=f.blocks[1];++side.header.nonce;
    ASSERT_EQ(f.db.putHeader(f.token,side.GetHash(),side.header,1,arith_uint256(1)),Status::Ok);
    ASSERT_EQ(f.db.putBlock(f.token,side.GetHash(),side),Status::Ok);
    ChainDB::PersistedHeaderMetadata metadata{};metadata.height=1;
    metadata.file_number=1;metadata.data_pos=100;metadata.data_size=100;
    ASSERT_EQ(f.db.putHeaderMetadata(f.token,side.GetHash(),metadata),Status::Ok);
    auto next=f.blocks.back().header;next.prev_block_hash=f.tip.hash;++next.nonce;
    ASSERT_EQ(f.db.putHeader(f.token,next.GetHash(),next,int(f.tip.height+1),arith_uint256(1)),Status::Ok);
    rocksdb::WriteBatch identical;
    ASSERT_EQ(f.db.putHeader(f.token,f.tip.hash,f.blocks.back().header,f.tip.height,
        ChainworkFromHex(f.tip.chainwork),&identical),Status::Ok);
    ASSERT_EQ(f.db.putBlock(f.token,f.blocks[1].GetHash(),f.blocks[1],&identical),Status::Ok);
    ASSERT_EQ(f.db.putHeightIndex(f.token,1,f.blocks[1].GetHash(),&identical),Status::Ok);
    ASSERT_EQ(f.db.writeBatch(f.token,std::move(identical)),Status::Ok);
    EXPECT_EQ(f.db.readRevision(),revision);
    auto cached=f.service.PrepareSelectedOrchardParent();ASSERT_TRUE(cached);
    EXPECT_EQ(&cached->Record(),identity);EXPECT_EQ(f.service.CheckPreparedOrchardParent(*cached),first->Record());
    metadata.height=f.tip.height;metadata.data_size=0;metadata.file_number=0;metadata.data_pos=0;
    ASSERT_EQ(f.db.putHeaderMetadata(f.token,f.tip.hash,metadata),Status::Ok);
    metadata.status_flags=BLOCK_HAVE_DATA;
    ASSERT_EQ(f.db.putHeaderMetadata(f.token,f.tip.hash,metadata),Status::Ok);
    EXPECT_EQ(f.db.readRevision(),revision);
    metadata.data_size=100;metadata.data_pos=100;
    ASSERT_EQ(f.db.putHeaderMetadata(f.token,f.tip.hash,metadata),Status::Ok);
    EXPECT_NE(f.db.readRevision(),revision);
    f.CheckUnpublished();
}

TEST(OrchardParentService, StagedNewKeyThatBecomesAnOverwriteInvalidatesAtCommit) {
    OwnedSelectedParentFixture f;
    auto side=f.blocks[1];++side.header.nonce;auto changed=side;
    changed.vtx.front().vout.front().value=AmountUna::Una(1);
    rocksdb::WriteBatch staged;
    ASSERT_EQ(f.db.putBlock(f.token,side.GetHash(),changed,&staged),Status::Ok);
    ASSERT_EQ(f.db.putBlock(f.token,side.GetHash(),side),Status::Ok);
    ASSERT_TRUE(f.service.PrepareSelectedOrchardParent());
    const auto before=f.db.readRevision();ASSERT_TRUE(before);
    ASSERT_EQ(f.db.writeBatch(f.token,std::move(staged)),Status::Ok);
    EXPECT_NE(f.db.readRevision(),before);
    f.CheckUnpublished();
}

TEST(OrchardParentService, CapturedBranchCannotOverrideChangedSelectedState) {
    OwnedSelectedParentFixture f;
    auto prepared=f.service.PrepareSelectedOrchardParent(); ASSERT_TRUE(prepared);
    const auto expected=f.service.CheckPreparedOrchardParent(*prepared); ASSERT_TRUE(expected);
    const auto point=f.replay->ProvenUtxos().begin()->first;
    const auto original=f.db.getCoin(point.txid.AsUint256(),point.vout); ASSERT_TRUE(original.ok());
    auto wrong=*original; ++wrong.amount;
    ASSERT_EQ(f.db.putCoin(f.token,point.txid.AsUint256(),point.vout,wrong),Status::Ok);
    EXPECT_FALSE(f.service.CheckPreparedOrchardParent(*prepared));
    ASSERT_EQ(f.db.putCoin(f.token,point.txid.AsUint256(),point.vout,*original),Status::Ok);
    EXPECT_EQ(f.service.CheckPreparedOrchardParent(*prepared),expected);
    ShieldedStateStartupTestAccess::RemoveBoundaryCoin(f.service,point);
    EXPECT_FALSE(f.service.CheckPreparedOrchardParent(*prepared));
    ShieldedStateStartupTestAccess::BoundaryState(f.service,f.tip,*f.replay);
    ASSERT_EQ(f.db.putHeightIndex(f.token,1,f.blocks.front().GetHash()),Status::Ok);
    // Selected-prefix preparation audits the canonical index while detached.
    // An existing proof must also refuse the changed selected index.
    EXPECT_FALSE(f.service.PrepareSelectedOrchardParent());
    EXPECT_FALSE(f.service.CheckPreparedOrchardParent(*prepared));
    ASSERT_EQ(f.db.putHeightIndex(f.token,1,f.blocks[1].GetHash()),Status::Ok);
    EXPECT_EQ(f.service.CheckPreparedOrchardParent(*prepared),expected);
    const auto marker=f.db.getForestTipMarker(); ASSERT_TRUE(marker.ok());
    auto bad=*marker; bad.forest_root.SetNull();
    ASSERT_EQ(f.db.putForestTipMarker(f.token,bad),Status::Ok);
    EXPECT_FALSE(f.service.CheckPreparedOrchardParent(*prepared));
    ASSERT_EQ(f.db.putForestTipMarker(f.token,*marker),Status::Ok);
    EXPECT_EQ(f.service.CheckPreparedOrchardParent(*prepared),expected);
    f.CheckUnpublished();
}

TEST(OrchardParentService, CorruptBodyOrWorkNeverProducesPreparedResult) {
    OwnedSelectedParentFixture f;
    ASSERT_TRUE(f.service.PrepareSelectedOrchardParent());
    auto changed=f.blocks.front(); changed.vtx.front().vout.front().value=AmountUna::Una(1);
    ASSERT_EQ(f.db.putBlock(f.token,changed.GetHash(),changed),Status::Ok);
    EXPECT_FALSE(f.service.PrepareSelectedOrchardParent());
    ASSERT_EQ(f.db.putBlock(f.token,f.blocks.front().GetHash(),f.blocks.front()),Status::Ok);
    EXPECT_TRUE(f.service.PrepareSelectedOrchardParent());
    const auto work=f.db.getBlockWork(f.tip.hash); ASSERT_TRUE(work.ok());
    ASSERT_EQ(f.db.putHeader(f.token,f.tip.hash,f.blocks.back().header,f.tip.height,*work+arith_uint256(1)),Status::Ok);
    EXPECT_FALSE(f.service.PrepareSelectedOrchardParent());
    ASSERT_EQ(f.db.putHeader(f.token,f.tip.hash,f.blocks.back().header,f.tip.height,*work),Status::Ok);
    EXPECT_TRUE(f.service.PrepareSelectedOrchardParent());
    f.CheckUnpublished();
}
#else
TEST(OrchardParentService, UnavailableWithoutBackend) {
    ChainstateService service;
    EXPECT_FALSE(service.PrepareSelectedOrchardParent());
}
#endif
