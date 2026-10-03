#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
class OrchardRpcMiningFixture {
public:
    RawIngressFixture raw;
    ExecutionContext execution;
    std::shared_ptr<const OrchardMiningTemplate> reference;
    OrchardRpcMiningFixture() {
        execution.daemon=&raw.context;
        reference=raw.Build();OrchardAdmissionFixture::Require(bool(reference));
    }
    din::Json Job() {
        din::Json p;p["address"]=OrchardMiningPayout;
        return ::rpc_mining_getjob(execution,p);
    }
    static BlockHeader Header(const din::Json& job) {
        std::vector<uint8_t> bytes;
        OrchardAdmissionFixture::Require(job.isMember("header_hex") && util::unhex(job["header_hex"].asString(),bytes));
        const auto header=BlockHeader::Deserialize(bytes);OrchardAdmissionFixture::Require(bool(header));
        return *header;
    }
    static uint32_t Nonce(const din::Json& job,bool meets_target=true) {
        auto header=Header(job);const auto target=uint256::FromHexUnsafe(job["target"].asString());
        for(uint32_t nonce=0;nonce<100000;++nonce) {
            header.nonce=nonce;if((!(target<header.GetHash()))==meets_target)return nonce;
        }
        throw std::runtime_error("bounded regtest nonce search exhausted");
    }
    din::Json Submit(const din::Json& job,uint32_t nonce) {
        din::Json p;p["job_id"]=job["job_id"];p["nonce"]=nonce;
        return ::rpc_mining_submit(execution,p);
    }
};
}
TEST(OrchardMiningRpc, ActualGetJobAndSolvedSubmitRetainTypedBody) {
    OrchardRpcMiningFixture f;const auto job=f.Job();ASSERT_FALSE(job.isMember("error"))<<job["error"].asString();
    EXPECT_EQ(job["height"].asInt(),102);EXPECT_EQ(job["prev_hash"].asString(),f.raw.parent->hash.GetHex());
    auto header=f.Header(job);header.nonce=f.Nonce(job);
    const auto accepted=f.Submit(job,header.nonce);ASSERT_TRUE(accepted.isNull());
    const auto hash=header.GetHash();ASSERT_EQ(f.raw.f.service->GetActiveTip()->hash,hash);
    const auto stored=ReadRuntimeBlockUnderLock(f.raw.f.db,f.raw.files.get(),hash,102);
    ASSERT_TRUE(stored.ok());ASSERT_TRUE(stored->IsOrchardProfile());
    EXPECT_EQ(stored->Orchard().Header().SerializeForHash(),header.SerializeForHash());
    ASSERT_EQ(stored->Orchard().Transactions().size(),f.reference->Transactions().size());
    for(size_t i=1;i<stored->Orchard().Transactions().size();++i)
        EXPECT_EQ(stored->Orchard().Transactions()[i].Serialize(TxSerializationMode::WithWitness),
            f.reference->Transactions()[i].Serialize(TxSerializationMode::WithWitness));
    EXPECT_EQ(stored->Orchard().WireBytes().size(),f.reference->WireBytes().size());
    EXPECT_EQ(std::vector<uint8_t>(stored->Orchard().WireBytes().begin()+128,stored->Orchard().WireBytes().end()),
        std::vector<uint8_t>(f.reference->WireBytes().begin()+128,f.reference->WireBytes().end()));
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.raw.f.service));
    EXPECT_EQ(f.raw.notices->published,1u);
    EXPECT_EQ(f.Submit(job,header.nonce)["code"].asString(),"stale-job");
}
TEST(OrchardMiningRpc, MissingProviderAndWrongOwnerRefuseThenRetry) {
    OrchardRpcMiningFixture f;f.raw.f.service->setRuntimeBlockNotifications(nullptr);
    EXPECT_TRUE(f.Job().isMember("error"));f.raw.f.CheckUnpublished();
    f.raw.f.service->setRuntimeBlockNotifications(f.raw.notices);
    const auto job=f.Job();ASSERT_FALSE(job.isMember("error"))<<job["error"].asString();const auto nonce=f.Nonce(job);
    DaemonContext unrelated;DaemonContext::setInstance(&unrelated);
    const auto refused=f.Submit(job,nonce);DaemonContext::setInstance(&f.raw.context);
    EXPECT_EQ(refused["code"].asString(),"stale-job");
    f.raw.f.CheckUnpublished();EXPECT_EQ(f.raw.notices->published,0u);
    EXPECT_TRUE(f.Submit(job,nonce).isNull());EXPECT_EQ(f.raw.notices->published,1u);
}
TEST(OrchardMiningRpc, InvalidTimeAndHighHashPreserveStoredJob) {
    OrchardRpcMiningFixture f;const auto job=f.Job();ASSERT_FALSE(job.isMember("error"))<<job["error"].asString();
    const auto nonce=f.Nonce(job);din::Json p;p["job_id"]=job["job_id"];p["nonce"]=nonce;
    p["ntime"]=job["min_time"].asInt64()-1;
    EXPECT_EQ(::rpc_mining_submit(f.execution,p)["code"].asString(),"invalid-ntime");
    p["ntime"]=job["max_time"].asInt64()+1;
    EXPECT_EQ(::rpc_mining_submit(f.execution,p)["code"].asString(),"invalid-ntime");
    EXPECT_EQ(f.Submit(job,f.Nonce(job,false))["code"].asString(),"high-hash");
    EXPECT_EQ(f.raw.notices->published,0u);f.raw.f.CheckUnpublished();
    EXPECT_TRUE(f.Submit(job,nonce).isNull());EXPECT_EQ(f.raw.notices->published,1u);
}
TEST(OrchardMiningRpc, CanonicalParentChangeRejectsOldJob) {
    OrchardRpcMiningFixture f;const auto job=f.Job();ASSERT_FALSE(job.isMember("error"))<<job["error"].asString();
    const auto nonce=f.Nonce(job);auto competitor=f.Header(job);competitor.nonce=nonce+1;
    const auto target=uint256::FromHexUnsafe(job["target"].asString());
    while(target<competitor.GetHash() && competitor.nonce<100000)++competitor.nonce;
    ASSERT_FALSE(target<competitor.GetHash());
    auto bytes=f.reference->WireBytes();const auto prefix=competitor.SerializeForHash();std::copy(prefix.begin(),prefix.end(),bytes.begin());
    const auto connected=f.raw.Submit(bytes);ASSERT_TRUE(connected.accepted())<<connected.reason;
    EXPECT_EQ(f.Submit(job,nonce)["code"].asString(),"stale-job");
    EXPECT_EQ(f.raw.f.service->GetActiveTip()->hash,competitor.GetHash());EXPECT_EQ(f.raw.notices->published,1u);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.raw.f.service));
}
#else
TEST(OrchardMiningRpc, MissingServiceRefusesWithoutBackend) {
    ExecutionContext context;din::Json p;p["address"]="unused-isolated-address";
    EXPECT_EQ(::rpc_mining_getjob(context,p)["error"].asString(),"DaemonContext not available");
}
#endif
