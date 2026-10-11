#pragma once
din::Json rpc_context_wallet_proofstatus(const ExecutionContext&,const din::Json&);
namespace din {Json rpc_getutreexocommitment(const ExecutionContext&,const Json&);}
namespace dinero {
namespace {
din::Json ProofRootRequest(std::string root) {auto p=din::arr();p.append(root);return p;}
void ExpectProofRefusal(const din::Json& r) {
    EXPECT_TRUE(r.isMember("error"))<<r.toStyledString();
    for(const auto* key:{"stale","current_root","current_height","current_block_hash"})
        EXPECT_FALSE(r.isMember(key))<<key<<r.toStyledString();
}
void CheckProofSnapshot(const ExecutionContext& ctx,ChainstateService& service) {
    const auto snapshot=service.getUtreexoRpcSnapshot();ASSERT_TRUE(snapshot.ok());
    const auto root=util::hex(snapshot->commitment);
    auto uppercase=root;for(auto& c:uppercase)if(c>='a'&&c<='f')c=char(c-'a'+'A');
    const auto match=rpc_context_wallet_proofstatus(ctx,ProofRootRequest(uppercase));
    ASSERT_FALSE(match.isMember("error"))<<match.toStyledString();EXPECT_FALSE(match["stale"].asBool());
    EXPECT_EQ(match["client_root"].asString(),root);EXPECT_EQ(match["current_root"].asString(),root);
    EXPECT_EQ(match["current_height"].asUInt64(),snapshot->height);
    EXPECT_EQ(match["current_block_hash"].asString(),snapshot->block_hash.GetHex());
    auto other=root;other[0]=other[0]=='0'?'1':'0';
    const auto stale=rpc_context_wallet_proofstatus(ctx,ProofRootRequest(other));
    ASSERT_FALSE(stale.isMember("error"));EXPECT_TRUE(stale["stale"].asBool());
    const auto commitment=din::rpc_getutreexocommitment(ctx,din::arr());
    ASSERT_FALSE(commitment.isMember("error"))<<commitment.toStyledString();
    EXPECT_EQ(commitment["commitment"].asString(),root);
    EXPECT_EQ(commitment["verified_height"].asUInt64(),snapshot->height);
    EXPECT_EQ(commitment["verified_block_hash"].asString(),snapshot->block_hash.GetHex());
    EXPECT_EQ(commitment["retains_full_state"].asBool(),!snapshot->compact);
}
}
TEST(WalletProofSnapshot, MissingOwnerReturnsOnlyError) {
    DaemonContext daemon;daemon.chainstate=std::make_shared<ChainstateService>();
    ExecutionContext ctx;ctx.daemon=&daemon;
    ExpectProofRefusal(rpc_context_wallet_proofstatus(ctx,ProofRootRequest(std::string(64,'0'))));
    const auto commitment=din::rpc_getutreexocommitment(ctx,din::arr());
    EXPECT_TRUE(commitment.isMember("error"));EXPECT_FALSE(commitment.isMember("commitment"));
}
TEST(WalletProofSnapshot, StrictInputBeforeServices) {
    ExecutionContext ctx;
    std::vector<din::Json> invalid{din::Json{},din::arr(),din::Json("root")};
    for(const auto& root:std::vector<std::string>{"",std::string(63,'0'),std::string(65,'0'),
        std::string(64,'g'),std::string(63,'a')+'\0',std::string(63,'a')+char(0xff)})invalid.push_back(ProofRootRequest(root));
    auto extra=ProofRootRequest(std::string(64,'0'));extra.append("extra");invalid.push_back(extra);
    auto numeric=din::arr();numeric.append(3);invalid.push_back(numeric);
    for(const auto& p:invalid)ExpectProofRefusal(rpc_context_wallet_proofstatus(ctx,p));
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(WalletProofSnapshot, FullOwnerAndDurableMismatch) {
    CanonicalRecoveryFixture f;ExecutionContext ctx;ctx.daemon=&f.context;
    ASSERT_NO_FATAL_FAILURE(CheckProofSnapshot(ctx,*f.f.service));
    const auto before=f.f.db.getValidatedTip();ASSERT_TRUE(before.ok());
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token,before->hash,before->height-1),Status::Ok);
    ExpectProofRefusal(rpc_context_wallet_proofstatus(ctx,ProofRootRequest(std::string(64,'0'))));
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token,before->hash,before->height),Status::Ok);
    ASSERT_NO_FATAL_FAILURE(CheckProofSnapshot(ctx,*f.f.service));
}
TEST(WalletProofSnapshot, CompactBoundaryDisconnectReconnect) {
    CompactTransitionFixture f;ExecutionContext ctx;ctx.daemon=&f.context;
    ASSERT_EQ(f.service->GetConsensusUTXOSet(),nullptr);
    ASSERT_NO_FATAL_FAILURE(CheckProofSnapshot(ctx,*f.service));
    const auto previous=f.service->getUtreexoRpcSnapshot();ASSERT_TRUE(previous.ok());
    const auto first=f.storage->first->Header().GetHash();std::string error;
    ASSERT_TRUE(f.service->InvalidateBlock(first,error))<<error;
    ASSERT_NO_FATAL_FAILURE(CheckProofSnapshot(ctx,*f.service));
    const auto boundary=f.service->getUtreexoRpcSnapshot();ASSERT_TRUE(boundary.ok());
    EXPECT_EQ(boundary->height,101u);EXPECT_NE(previous->block_hash,boundary->block_hash);
    const auto old=rpc_context_wallet_proofstatus(ctx,ProofRootRequest(util::hex(previous->commitment)));
    ASSERT_FALSE(old.isMember("error"));EXPECT_EQ(old["stale"].asBool(),previous->commitment!=boundary->commitment);
    ASSERT_TRUE(f.service->ReconsiderBlock(first,error))<<error;f.service->ActivateBestChain();
    ASSERT_NO_FATAL_FAILURE(CheckProofSnapshot(ctx,*f.service));
    const auto restored=f.service->getUtreexoRpcSnapshot();ASSERT_TRUE(restored.ok());
    EXPECT_EQ(restored->block_hash,previous->block_hash);EXPECT_EQ(restored->commitment,previous->commitment);
}
TEST(WalletProofSnapshot, HistoricalCompactCatalogAndStopRefusal) {
    HistoricalPreparationFixture f;ExecutionContext ctx;ctx.daemon=&f.context;
    ASSERT_EQ(f.service->GetConsensusUTXOSet(),nullptr);
    ASSERT_NO_FATAL_FAILURE(CheckProofSnapshot(ctx,*f.service));
    const auto before=f.service->getUtreexoRpcSnapshot();ASSERT_TRUE(before.ok());
    EXPECT_TRUE(before->compact);EXPECT_EQ(before->height,100u);
    {
        auto selected=f.service->AcquireBlockIngressActivationLock();
        EXPECT_FALSE(f.service->getUtreexoRpcSnapshot().ok());
    }
    ASSERT_NO_FATAL_FAILURE(CheckProofSnapshot(ctx,*f.service));
    f.service->Stop();
    ExpectProofRefusal(rpc_context_wallet_proofstatus(ctx,ProofRootRequest(util::hex(before->commitment))));
    const auto commitment=din::rpc_getutreexocommitment(ctx,din::arr());
    EXPECT_TRUE(commitment.isMember("error"));EXPECT_FALSE(commitment.isMember("commitment"));
}
TEST(WalletProofSnapshot, FullRootMismatchNeverReportsFresh) {
    CanonicalRecoveryFixture f;ExecutionContext ctx;ctx.daemon=&f.context;
    const auto before=f.f.service->getUtreexoRpcSnapshot();ASSERT_TRUE(before.ok());
    auto* coins=f.f.service->GetConsensusUTXOSet();ASSERT_NE(coins,nullptr);
    std::unique_ptr<consensus::UtreexoForest> saved;
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        {const auto read=coins->LockForestShared();saved=std::make_unique<consensus::UtreexoForest>(coins->GetForest());}
        coins->ReplaceForestGuarded(consensus::UtreexoForest{});
    }
    ExpectProofRefusal(rpc_context_wallet_proofstatus(ctx,ProofRootRequest(util::hex(before->commitment))));
    {auto selected=f.f.service->AcquireBlockIngressActivationLock();coins->ReplaceForestGuarded(*saved);}
    ASSERT_NO_FATAL_FAILURE(CheckProofSnapshot(ctx,*f.f.service));
}
TEST(WalletProofSnapshot, CompactStorageBindingMismatchPreservesOwner) {
    CompactTransitionFixture f;auto& disk=*f.storage;ExecutionContext ctx;ctx.daemon=&f.context;
    const auto before=f.service->getUtreexoRpcSnapshot();ASSERT_TRUE(before.ok());
    const auto binding=storage::ReadOrchardCompactStorageBinding(disk.reopened);ASSERT_TRUE(binding);
    {
        auto selected=f.service->AcquireBlockIngressActivationLock();
        rocksdb::WriteBatch bad;bad.Put(storage::OrchardCompactStorageKey,"malformed");
        ASSERT_EQ(disk.reopened.writeBatch(disk.f.token,std::move(bad),true),Status::Ok);
    }
    ExpectProofRefusal(rpc_context_wallet_proofstatus(ctx,ProofRootRequest(util::hex(before->commitment))));
    {
        auto selected=f.service->AcquireBlockIngressActivationLock();
        rocksdb::WriteBatch restore;restore.Put(storage::OrchardCompactStorageKey,*binding);
        ASSERT_EQ(disk.reopened.writeBatch(disk.f.token,std::move(restore),true),Status::Ok);
    }
    ASSERT_NO_FATAL_FAILURE(CheckProofSnapshot(ctx,*f.service));
    const auto after=f.service->getUtreexoRpcSnapshot();ASSERT_TRUE(after.ok());
    EXPECT_EQ(after->commitment,before->commitment);EXPECT_EQ(after->block_hash,before->block_hash);
}
#endif
}
