#pragma once
namespace dinero {
namespace {
struct VaultSnapshotFixture {
    std::array<uint8_t,32> hash{};
    unsigned captures=0;
    std::function<void(vault::VaultTipSnapshot&)> edit;
    std::function<void()> during;
    std::unique_ptr<vault::VaultService> service;
    VaultSnapshotFixture() {
        hash.fill(41);
        service=std::make_unique<vault::VaultService>(
            std::make_unique<vault::InMemorySigningBackend>(vault::BackendId{"snapshot-fixture"}),
            vault::VaultServiceConfig{},
            [](uint64_t)->std::array<uint8_t,32>{throw std::runtime_error("unexpected narrow hash reader");},
            [](const auto&,uint64_t,const auto&)->bool{throw std::runtime_error("unexpected narrow inclusion reader");},
            [this](uint64_t height,const std::vector<vault::VaultDepositQuery>& queries) {
                ++captures;
                // This supported callback inspects the published state while
                // the service has released its mutex for source acquisition.
                const auto prior=service->metrics();(void)prior;
                if(during)during();
                vault::VaultTipSnapshot result{height,hash,{}};
                for(const auto& query:queries)result.deposits.push_back({query,hash,true});
                if(edit)edit(result);
                return result;
            });
    }
    void Add(uint8_t tag,const char* account,uint64_t amount) {
        std::array<uint8_t,32> txid{};txid.fill(tag);
        service->recordDeposit(txid,0,vault::AccountId{account},amount,100,hash);
    }
};
}
TEST(VaultSelectedSnapshot, OneCapturePublishesCompleteDepositSet) {
    VaultSnapshotFixture f;f.Add(1,"first",100);f.Add(2,"second",200);
    f.edit=[](const vault::VaultTipSnapshot& snapshot){
        EXPECT_EQ(snapshot.height,101u);EXPECT_EQ(snapshot.deposits.size(),2u);
    };
    f.service->tipChanged(101);EXPECT_EQ(f.captures,1u);
    EXPECT_EQ(f.service->metrics().total_open_credits,300u);
    EXPECT_EQ(f.service->accountMetrics(vault::AccountId{"first"}).pending,100u);
    EXPECT_EQ(f.service->accountMetrics(vault::AccountId{"second"}).pending,200u);
    const auto prior=f.service->entriesSince(0);f.service->tipChanged(101);
    EXPECT_EQ(f.captures,2u);EXPECT_EQ(f.service->entriesSince(0),prior);
}
TEST(VaultSelectedSnapshot, InterveningPublicationRequiresFreshCapture) {
    VaultSnapshotFixture f;f.Add(1,"first",100);
    bool add=true;
    f.during=[&]{if(add){add=false;f.Add(2,"second",200);}};
    EXPECT_THROW(f.service->tipChanged(101),std::runtime_error);
    EXPECT_EQ(f.captures,1u);EXPECT_TRUE(f.service->entriesSince(0).empty());
    EXPECT_EQ(f.service->metrics().total_open_credits,0u);
    f.service->tipChanged(101);EXPECT_EQ(f.captures,2u);
    EXPECT_EQ(f.service->metrics().total_open_credits,300u);
    EXPECT_EQ(f.service->accountMetrics(vault::AccountId{"second"}).pending,200u);
}
TEST(VaultSelectedSnapshot, IncompleteOrMismatchedCapturePreservesPublishedState) {
    VaultSnapshotFixture f;f.Add(1,"first",100);f.Add(2,"second",200);f.service->tipChanged(101);
    const auto prior=f.service->entriesSince(0);const auto metrics=f.service->metrics();
    const std::vector<std::function<void(vault::VaultTipSnapshot&)>> edits{
        [](auto& s){s.deposits.pop_back();},
        [](auto& s){s.deposits.back()=s.deposits.front();},
        [](auto& s){++s.deposits.back().query.amount;},
        [](auto& s){++s.height;},
        [](auto& s){s.block_hash={};},
        [](auto& s){s.deposits.back().block_hash.reset();},
        [](auto& s){s.deposits.back().block_hash=std::array<uint8_t,32>{};},
        [](auto&){throw std::runtime_error("complete capture unavailable");}
    };
    for(const auto& edit:edits) {
        f.edit=edit;EXPECT_THROW(f.service->tipChanged(105),std::runtime_error);
        EXPECT_EQ(f.service->entriesSince(0),prior);EXPECT_EQ(f.service->metrics(),metrics);
    }
    f.edit={};f.service->tipChanged(105);
    EXPECT_EQ(f.service->accountMetrics(vault::AccountId{"first"}).confirmed,100u);
    EXPECT_EQ(f.service->accountMetrics(vault::AccountId{"second"}).confirmed,200u);
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(VaultSelectedSnapshot, ActualSelectedHistoricalAndTypedOutputsShareTip) {
    VaultObservationFixture f;f.ArchiveHistorical(1);
    const auto [body,bundle]=f.Shield(f.Keys());const auto block=f.Mine(body);
    const auto read=vault::MakeChainstateVaultSnapshotClosure(f.context);
    vault::OutpointId old;old.txid_raw=VaultRawHash(f.f.blocks[1].vtx.front().GetTxid().AsUint256());
    vault::OutpointId typed;typed.txid_raw=VaultRawHash(body.GetTxid().AsUint256());
    const auto old_amount=f.f.blocks[1].vtx.front().vout.front().value.GetUna();
    const auto typed_amount=body.Orchard().Outputs().front().amount_una;
    std::vector<vault::VaultDepositQuery> queries{{old,1,old_amount},{typed,102,typed_amount}};
    const auto source=f.context.chainstate;f.context.chainstate.reset();
    const auto snapshot=read(102,queries);f.context.chainstate=source;
    EXPECT_EQ(snapshot.height,102u);EXPECT_EQ(snapshot.block_hash,VaultRawHash(block->Orchard().Header().GetHash()));
    ASSERT_EQ(snapshot.deposits.size(),2u);
    EXPECT_EQ(snapshot.deposits[0].query,queries[0]);EXPECT_EQ(snapshot.deposits[1].query,queries[1]);
    EXPECT_TRUE(snapshot.deposits[0].included);EXPECT_TRUE(snapshot.deposits[1].included);
    EXPECT_EQ(snapshot.deposits[0].block_hash,std::optional{VaultRawHash(f.f.blocks[1].GetHash())});
    EXPECT_EQ(snapshot.deposits[1].block_hash,std::optional{snapshot.block_hash});
    EXPECT_THROW(read(101,queries),std::runtime_error);
    auto wrong=queries;++wrong[1].amount;EXPECT_THROW(read(102,wrong),std::runtime_error);
    auto absent=queries;absent[1].outpoint.vout=UINT32_MAX;
    EXPECT_FALSE(read(102,absent).deposits[1].included);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    const auto disconnected=read(101,queries);ASSERT_EQ(disconnected.deposits.size(),2u);
    EXPECT_TRUE(disconnected.deposits[0].included);EXPECT_FALSE(disconnected.deposits[1].included);
    EXPECT_FALSE(disconnected.deposits[1].block_hash);
    const auto accepted=f.Submit(block->Orchard().WireBytes());ASSERT_TRUE(accepted.accepted()&&accepted.connected);
    EXPECT_TRUE(read(102,queries).deposits[1].included);
    f.f.db.close();EXPECT_THROW(read(102,queries),std::runtime_error);
    ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);EXPECT_TRUE(read(102,queries).deposits[1].included);
}
TEST(VaultSelectedSnapshot, ActualRuntimeRefusesUnavailableAndStaleTips) {
    VaultObservationFixture f;f.ArchiveHistorical(1);VaultRuntimeReset reset;
    auto config=VaultOwnerChainConfig(f);config.capture_tip=vault::MakeChainstateVaultSnapshotClosure(f.context);
    vault::InitializeVaultRuntime(std::move(config));const auto owner=vault::GetVaultRuntimeService();ASSERT_TRUE(owner);
    const auto txid=VaultRawHash(f.f.blocks[1].vtx.front().GetTxid().AsUint256());
    uint64_t amount=0,height=0;std::array<uint8_t,32> hash{};std::string error;
    ASSERT_TRUE(vault::VerifyOperatorDeposit(owner,txid,0,amount,height,hash,error))<<error;
    owner->recordDeposit(txid,0,vault::AccountId{"snapshot-owner"},amount,height,hash);
    EXPECT_THROW(owner->tipChanged(100),std::runtime_error);EXPECT_TRUE(owner->entriesSince(0).empty());
    f.f.db.close();EXPECT_THROW(owner->tipChanged(101),std::runtime_error);EXPECT_TRUE(owner->entriesSince(0).empty());
    ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);owner->tipChanged(101);
    EXPECT_EQ(owner->accountMetrics(vault::AccountId{"snapshot-owner"}).confirmed,amount);
    const auto prior=owner->entriesSince(0);const auto metrics=owner->metrics();
    EXPECT_THROW(owner->tipChanged(102),std::runtime_error);
    EXPECT_EQ(owner->entriesSince(0),prior);EXPECT_EQ(owner->metrics(),metrics);
    vault::ShutdownVaultRuntime();EXPECT_FALSE(vault::GetVaultRuntimeService());
    EXPECT_NO_THROW(owner->tipChanged(101));EXPECT_EQ(owner->entriesSince(0),prior);
}
#endif
} // namespace dinero
