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
TEST(OrchardMiningRpc, LegacyGbtRefusesAtBoundaryAndCapablePoolGetsTypedContract) {
    OrchardRpcMiningFixture f;din::Json p;p["address"]=OrchardMiningPayout;
    const auto legacy=::rpc_getblocktemplate_v14(f.execution,p);
    EXPECT_TRUE(legacy.isMember("error"));EXPECT_FALSE(legacy.isMember("coinbasetxn"));
    p["capabilities"][0]="orchard-immutable-jobs-v1";
    const auto capable=::rpc_getblocktemplate_v14(f.execution,p);
    ASSERT_FALSE(capable.isMember("error"));EXPECT_EQ(capable["height"].asInt(),102);
    EXPECT_EQ(capable["profile"].asString(),"orchard-v1");
    EXPECT_EQ(capable["immutable_job_rpc"].asString(),"mining.getjob");
    EXPECT_FALSE(capable.isMember("coinbasetxn"));f.raw.f.CheckUnpublished();
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
TEST(OrchardMiningRpc, SharedPayoutJobConnectsExactRequestedOutputs) {
    OrchardRpcMiningFixture f;
    uint64_t reward=0;
    for(const auto& output:f.reference->Transactions().front().Historical().vout)
        reward+=output.value.GetUna();
    ASSERT_GT(reward,4u);
    const std::array<std::string,2> scripts{
        "5120"+std::string(64,'1'),"5120"+std::string(64,'2')};
    din::Json parameters;parameters["address"]=OrchardMiningPayout;
    parameters["coinbase_extranonce"]=uint32_t{37};
    parameters["coinbase_outputs"][0]["script_pubkey_hex"]=scripts[0];
    parameters["coinbase_outputs"][0]["value_una"]=reward/4;
    parameters["coinbase_outputs"][1]["script_pubkey_hex"]=scripts[1];
    parameters["coinbase_outputs"][1]["value_una"]=reward-reward/4;
    const auto job=::rpc_mining_getjob(f.execution,parameters);
    ASSERT_FALSE(job.isMember("error"))<<job["error"].asString();
    auto header=f.Header(job);header.nonce=f.Nonce(job);
    ASSERT_TRUE(f.Submit(job,header.nonce).isNull());
    const auto stored=ReadRuntimeBlockUnderLock(f.raw.f.db,f.raw.files.get(),header.GetHash(),102);
    ASSERT_TRUE(stored.ok());ASSERT_TRUE(stored->IsOrchardProfile());
    const auto& coinbase=stored->Orchard().Transactions().front().Historical();
    for(size_t i=0;i<scripts.size();++i) {
        std::vector<uint8_t> script;ASSERT_TRUE(util::unhex(scripts[i],script));
        const uint64_t value=i==0?reward/4:reward-reward/4;
        EXPECT_EQ(std::count_if(coinbase.vout.begin(),coinbase.vout.end(),[&](const auto& output){
            return output.value.GetUna()==value && output.scriptPubKey==script;
        }),1);
    }
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.raw.f.service));
}
TEST(OrchardMiningRpc, InvalidSharedPayoutSumIsRefusedWithoutPublishing) {
    OrchardRpcMiningFixture f;din::Json p;p["address"]=OrchardMiningPayout;
    p["coinbase_outputs"][0]["script_pubkey_hex"]="5120"+std::string(64,'1');
    p["coinbase_outputs"][0]["value_una"]=uint64_t{2100000000000001};
    const auto refused=::rpc_mining_getjob(f.execution,p);
    EXPECT_TRUE(refused.isMember("error"));
    f.raw.f.CheckUnpublished();EXPECT_EQ(f.raw.notices->published,0u);
}
TEST(OrchardMiningRpc, DerivedPayoutJobsRetainBaseAndUniqueChannelCoinbase) {
    OrchardRpcMiningFixture f;const auto base=f.Job();ASSERT_FALSE(base.isMember("error"));
    ASSERT_EQ(base["profile"].asString(),"orchard-v1");
    din::Json p;p["address"]=OrchardMiningPayout;p["base_job_id"]=base["job_id"];
    p["coinbase_extranonce"]=uint32_t{11};p["include_block"]=true;
    const auto one=::rpc_mining_getjob(f.execution,p);ASSERT_FALSE(one.isMember("error"));
    p["coinbase_extranonce"]=uint32_t{12};
    const auto two=::rpc_mining_getjob(f.execution,p);ASSERT_FALSE(two.isMember("error"));
    EXPECT_EQ(one["height"],base["height"]);EXPECT_EQ(two["prev_hash"],base["prev_hash"]);
    EXPECT_EQ(one["transaction_count"],base["transaction_count"]);
    EXPECT_EQ(two["total_fees_una"],base["total_fees_una"]);
    EXPECT_EQ(one["coinbase_value_una"],base["coinbase_value_una"]);
    EXPECT_NE(one["header_hex"],two["header_hex"]);EXPECT_NE(one["coinbase_hex"],two["coinbase_hex"]);
    EXPECT_EQ(one["block_hex"].asString().substr(0,256),one["header_hex"].asString());
    for(uint32_t channel=13;channel<52;++channel) {
        p["coinbase_extranonce"]=channel;
        const auto derived=::rpc_mining_getjob(f.execution,p);
        ASSERT_FALSE(derived.isMember("error"))<<channel;
    }
    EXPECT_TRUE(f.Submit(base,f.Nonce(base)).isNull());
    EXPECT_EQ(f.Submit(one,f.Nonce(one))["code"].asString(),"stale-job");
    EXPECT_EQ(f.Submit(two,f.Nonce(two))["code"].asString(),"stale-job");
}
TEST(OrchardMiningRpc, ExportedTypedFrameSurvivesCacheEvictionAndBoundedTimeRolling) {
    OrchardRpcMiningFixture f;din::Json p;p["address"]=OrchardMiningPayout;p["include_block"]=true;
    const auto retained=::rpc_mining_getjob(f.execution,p);ASSERT_FALSE(retained.isMember("error"));
    ASSERT_GT(retained["transaction_count"].asUInt64(),1u); // real Orchard shield body
    ASSERT_EQ(retained["coinbase_merkle_path"].size(),1u);
    for(unsigned i=0;i<40;++i)ASSERT_FALSE(f.Job().isMember("error"));
    EXPECT_EQ(f.Submit(retained,f.Nonce(retained))["code"].asString(),"stale-job");
    auto header=f.Header(retained);++header.timestamp;
    const auto target=uint256::FromHexUnsafe(retained["target"].asString());
    for(header.nonce=0;header.nonce<100000 && target<header.GetHash();++header.nonce){}
    ASSERT_FALSE(target<header.GetHash());
    std::vector<uint8_t> wire;ASSERT_TRUE(util::unhex(retained["block_hex"].asString(),wire));
    const auto prefix=header.SerializeForHash();std::copy(prefix.begin(),prefix.end(),wire.begin());
    const auto accepted=f.raw.Submit(wire);ASSERT_TRUE(accepted.accepted())<<accepted.reason;
    EXPECT_EQ(f.raw.f.service->GetActiveTip()->hash,header.GetHash());
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.raw.f.service));
}
TEST(OrchardMiningRpc, InvalidPoolPayoutShapesCannotPublishOrConsumeBase) {
    OrchardRpcMiningFixture f;const auto base=f.Job();ASSERT_FALSE(base.isMember("error"));
    din::Json p;p["address"]=OrchardMiningPayout;p["base_job_id"]=base["job_id"];
    p["coinbase_outputs"][0]["script_pubkey_hex"]="5120"+std::string(64,'1');
    p["coinbase_outputs"][0]["value_una"]=base["coinbase_value_una"];
    const auto good=p;
    for (const auto& value:std::vector<din::Json>{din::Json(-1),din::Json(0.25),din::Json("100")}) {
        p=good;p["coinbase_outputs"][0]["value_una"]=value;
        EXPECT_TRUE(::rpc_mining_getjob(f.execution,p).isMember("error"));
    }
    for(const auto& script:std::vector<std::string>{"","xyz","6a","0",std::string(20002,'1')}) {
        p=good;p["coinbase_outputs"][0]["script_pubkey_hex"]=script;
        EXPECT_TRUE(::rpc_mining_getjob(f.execution,p).isMember("error"));
    }
    p=good;p["base_job_id"]="expired-or-foreign";
    EXPECT_TRUE(::rpc_mining_getjob(f.execution,p).isMember("error"));
    p=good;p["coinbase_extranonce"]=uint64_t{0x100000000};
    EXPECT_TRUE(::rpc_mining_getjob(f.execution,p).isMember("error"));
    f.raw.f.CheckUnpublished();EXPECT_EQ(f.raw.notices->published,0u);
    EXPECT_TRUE(f.Submit(base,f.Nonce(base)).isNull());
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
