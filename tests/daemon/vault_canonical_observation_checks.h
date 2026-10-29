#pragma once
namespace dinero {
namespace {
std::array<uint8_t,32> VaultRawHash(const uint256& hash) {
    std::array<uint8_t,32> result{};std::copy(hash.begin(),hash.end(),result.begin());return result;
}
}
TEST(VaultCanonicalObservation, UnavailableSourceNeverClaimsInclusion) {
    DaemonContext context;
    const auto hash=vault::MakeChainstateBlockHashClosure(context);
    const auto included=vault::MakeChainstateTxIncludedClosure(context);
    EXPECT_EQ(hash(0),(std::array<uint8_t,32>{}));
    EXPECT_THROW(included({},0,0,{}),std::runtime_error);
    EXPECT_THROW(included({},0,UINT64_MAX,{}),std::runtime_error);
}
TEST(VaultCanonicalObservation, WatcherRetainsCheckedHashAndRefusedObservation) {
    vault::Ledger ledger;vault::DepositFlowMachine machine(&ledger);
    vault::OutpointId point;point.txid_raw.fill(17);point.vout=0;
    std::array<uint8_t,32> original{},replacement{};original.fill(18);replacement.fill(19);
    machine.observe(point,vault::AccountId{"fixture"},100,100);machine.tipChanged(101);
    bool available=false;size_t hash_reads=0;
    vault::ReorgWatcher watcher(&machine,[&](uint64_t){++hash_reads;return replacement;},
        [&](const vault::OutpointId& observed,uint64_t height,const std::array<uint8_t,32>& hash){
            EXPECT_EQ(observed,point);EXPECT_EQ(height,100u);EXPECT_EQ(hash,replacement);
            if(!available)throw std::runtime_error("fixture source unavailable");return true;
        });
    watcher.recordObservation(point,original);
    EXPECT_THROW(watcher.tipChanged(102),std::runtime_error);
    EXPECT_EQ(watcher.depositBlockHashes().at(point),original);
    EXPECT_EQ(machine.tracked().at(point).stage,vault::DepositStage::CREDITED);
    available=true;hash_reads=0;
    EXPECT_EQ(watcher.tipChanged(102),0);EXPECT_EQ(hash_reads,1u);
    EXPECT_EQ(watcher.depositBlockHashes().at(point),replacement);
    EXPECT_EQ(machine.tracked().at(point).stage,vault::DepositStage::CREDITED);
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct VaultObservationFixture : CanonicalRecoveryFixture {
    void ArchiveHistorical(uint32_t height) {
        const auto& block=f.blocks.at(height);
        const auto location=files->writeBlock(block.GetHash(),block);
        OrchardAdmissionFixture::Require(location.ok());
        const auto work=f.db.getBlockWork(block.GetHash());OrchardAdmissionFixture::Require(work.ok());
        ChainDB::PersistedHeaderMetadata metadata;
        metadata.height=height;metadata.parent_hash=block.header.prev_block_hash;metadata.chainwork=*work;
        metadata.status_flags=BLOCK_HAVE_DATA|BLOCK_VALID_CHAIN|BLOCK_VALID_SCRIPTS;
        metadata.file_number=location->file_number;metadata.data_pos=location->offset;metadata.data_size=location->size;
        OrchardAdmissionFixture::Require(f.db.putHeaderMetadata(f.token,block.GetHash(),metadata)==Status::Ok);
    }
};
}
TEST(VaultCanonicalObservation, HistoricalAndMixedCanonicalOutputIdentities) {
    VaultObservationFixture f;
    const auto hash=vault::MakeChainstateBlockHashClosure(f.context);
    const auto included=vault::MakeChainstateTxIncludedClosure(f.context);
    f.ArchiveHistorical(1);
    const auto historical=f.f.blocks[1].vtx.front().GetTxid().AsUint256();
    EXPECT_EQ(hash(1),VaultRawHash(f.f.blocks[1].GetHash()));
    EXPECT_TRUE(included(VaultRawHash(historical),0,1,hash(1)));
    EXPECT_FALSE(included(VaultRawHash(historical),UINT32_MAX,1,hash(1)));
    const auto [body,bundle]=f.Shield(f.Keys());const auto stored=f.Mine(body);
    const auto block_hash=stored->Orchard().Header().GetHash();
    const auto txid=VaultRawHash(body.GetTxid().AsUint256());
    EXPECT_EQ(hash(102),VaultRawHash(block_hash));
    EXPECT_TRUE(included(txid,0,102,VaultRawHash(block_hash)));
    EXPECT_FALSE(included(txid,1,102,VaultRawHash(block_hash)));
    EXPECT_FALSE(included({},0,102,VaultRawHash(block_hash)));
    EXPECT_TRUE(included(VaultRawHash(stored->Orchard().Transactions().front().GetTxid().AsUint256()),0,102,VaultRawHash(block_hash)));
    EXPECT_THROW(included(txid,0,102,hash(1)),std::runtime_error);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    EXPECT_EQ(hash(102),(std::array<uint8_t,32>{}));
    EXPECT_THROW(included(txid,0,102,VaultRawHash(block_hash)),std::runtime_error);
    const auto connected=f.Submit(stored->Orchard().WireBytes());ASSERT_TRUE(connected.accepted()&&connected.connected);
    EXPECT_TRUE(included(txid,0,102,VaultRawHash(block_hash)));
}
TEST(VaultCanonicalObservation, CapturedOwnerAndUnavailableStorePreserveObservation) {
    VaultObservationFixture f;const auto [body,bundle]=f.Shield(f.Keys());const auto stored=f.Mine(body);
    const auto txid=VaultRawHash(body.GetTxid().AsUint256());const auto block_hash=VaultRawHash(stored->Orchard().Header().GetHash());
    const auto hash=vault::MakeChainstateBlockHashClosure(f.context);
    const auto included=vault::MakeChainstateTxIncludedClosure(f.context);
    const auto source=f.context.chainstate;f.context.chainstate.reset();
    EXPECT_EQ(hash(102),block_hash);EXPECT_TRUE(included(txid,0,102,block_hash));f.context.chainstate=source;
    f.f.db.close();EXPECT_EQ(hash(102),(std::array<uint8_t,32>{}));
    EXPECT_THROW(included(txid,0,102,block_hash),std::runtime_error);
    ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);EXPECT_TRUE(included(txid,0,102,block_hash));
    f.wallet->Stop();f.f.ingress->Stop();f.f.service->Stop();EXPECT_EQ(hash(102),(std::array<uint8_t,32>{}));
    EXPECT_THROW(included(txid,0,102,block_hash),std::runtime_error);
}
#endif
} // namespace dinero
