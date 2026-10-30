#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
#include "daemon/services/orchard_branch_replay.h"
#include "consensus/orchard_block_staging.h"
namespace dinero {
namespace {
// Compare every batch byte except setTip's local receipt timestamp. Two valid
// staging calls can cross a wall-clock second; that field is not chain state.
// Locate it through RocksDB's typed iterator, not a byte-pattern search, so no
// coin, proof, chainwork, marker, operation order, or batch header is excluded.
std::string HandoffBatchWithoutReceiptTime(const ChainDB& db,
    const rocksdb::WriteBatch& batch) {
    struct ReceiptTimeLocator final : rocksdb::WriteBatch::Handler {
        uint32_t meta_id;
        const std::string& bytes;
        std::optional<size_t> offset;
        ReceiptTimeLocator(uint32_t id,const std::string& data):meta_id(id),bytes(data) {}
        rocksdb::Status PutCF(uint32_t family,const rocksdb::Slice& key,
            const rocksdb::Slice& value) override {
            if(family!=meta_id || key.ToString()!="tip")return rocksdb::Status::OK();
            if(offset) return rocksdb::Status::Corruption("duplicate handoff tip");
            Reader reader(value.ToString());
            if(reader.readString().size()!=64)throw std::runtime_error("invalid handoff tip hash");
            (void)reader.read<uint32_t>(); // height
            (void)reader.read<arith_uint256>(); // chainwork
            const auto timestamp_position=reader.position();
            (void)reader.read<uint32_t>();
            if(reader.remaining()!=0)throw std::runtime_error("invalid handoff tip layout");
            const auto base=reinterpret_cast<uintptr_t>(bytes.data());
            const auto start=reinterpret_cast<uintptr_t>(value.data());
            if(start<base || start-base>bytes.size() || value.size()>bytes.size()-(start-base))
                throw std::runtime_error("handoff tip outside batch");
            offset=size_t(start-base)+timestamp_position;
            return rocksdb::Status::OK();
        }
        rocksdb::Status DeleteCF(uint32_t,const rocksdb::Slice&) override {
            return rocksdb::Status::OK(); // Bytes remain in the original batch.
        }
    } locator(ChainDBTransactionReadTestPeer::HandoffMeta(db)->GetID(),batch.Data());
    const auto status=batch.Iterate(&locator);
    if(!status.ok() || !locator.offset)throw std::runtime_error("handoff batch tip not uniquely located");
    auto bytes=batch.Data();
    std::fill_n(bytes.begin()+*locator.offset,sizeof(uint32_t),'\0');
    return bytes;
}
Transaction HandoffTimeLockedSpend(const OrchardAdmissionFixture& f,const OutPoint& point,
    const consensus::UTXOEntry& coin) {
    auto tx=SelectionSpend(f,point,coin,100000);
    tx.lockTime=500000000;
    const auto digest=consensus::ScriptVerifier::ComputeTaprootSighash(
        tx,0,{coin.value.GetUna()},{coin.scriptPubKey});
    OrchardAdmissionFixture::Require(digest.size()==32);
    std::unique_ptr<secp256k1_context,decltype(&secp256k1_context_destroy)> context(
        secp256k1_context_create(SECP256K1_CONTEXT_NONE),secp256k1_context_destroy);
    secp256k1_keypair pair;
    OrchardAdmissionFixture::Require(secp256k1_keypair_create(context.get(),&pair,f.secret.data()));
    std::vector<uint8_t> signature(64);std::array<uint8_t,32> auxiliary{};
    OrchardAdmissionFixture::Require(secp256k1_schnorrsig_sign32(context.get(),signature.data(),
        digest.data(),&pair,auxiliary.data()));
    tx.vin.front().witness={signature};return tx;
}
struct ProofHandoffFixture:CanonicalPoolFixture {
    std::shared_ptr<const OrchardMiningTemplate> first,second;
    std::unique_ptr<OrchardParentReplay> replay;
    std::vector<BlockHeader> history;
    std::set<TxId> ids;
    consensus::HeaderChainSelector headers;
    consensus::UtreexoForest forest;
    std::optional<OrchardBlockCandidate> body;
    std::shared_ptr<TypedForkNotices> observer=std::make_shared<TypedForkNotices>();
    ProofHandoffFixture() {
        MutableParams().contextual_locks_activation_height=102;
        f.service->setRuntimeBlockNotifications(observer);
        first=Build();OrchardAdmissionFixture::Require(bool(first));
        OrchardAdmissionFixture::Require(Submit(first->WireBytes()).connected);
        const auto& source=first->Transactions().at(1);
        const MempoolTransaction parent_tx(HandoffTimeLockedSpend(f,
            OutPoint(source.GetTxid(),0),source.OutputCoin(0,102)));
        const MempoolTransaction child_tx(HandoffTimeLockedSpend(f,
            OutPoint(parent_tx.GetTxid(),0),parent_tx.OutputCoin(0,103)));
        OrchardAdmissionFixture::Require(f.ingress->SubmitBody(parent_tx,TxOrigin::INTERNAL).accepted());
        OrchardAdmissionFixture::Require(f.ingress->SubmitBody(child_tx,TxOrigin::INTERNAL).accepted());
        BlockAssembler assembler(&f.db);WireOrchardAssembler(assembler,f);
        second=assembler.CreateOrchardBlock(OrchardMiningPayout);
        OrchardAdmissionFixture::Require(bool(second) && second->Transactions().size()==3);
        body.emplace(OrchardBlockCandidate::DecodeExact(second->WireBytes()));
        {
            auto selected=f.service->AcquireBlockIngressActivationLock();
            auto* live=f.service->GetConsensusUTXOSet();auto lock=live->LockForestShared();
            forest=live->GetForest();
            const auto work=*f.db.getBlockWork(first->Header().GetHash())+GetBlockProof(second->Header().difficulty);
            OrchardAdmissionFixture::Require(f.db.putHeader(f.token,second->Header().GetHash(),second->Header(),103,work)==Status::Ok);
        }
        replay=std::make_unique<OrchardParentReplay>(OrchardParentReplay::Target{
            this->parent->height,this->parent->hash,ChainworkFromHex(this->parent->chainwork)},
            OrchardParentReplay::Limits{100000,256*1024*1024});
        arith_uint256 work{0};
        for(uint32_t height=0;height<f.blocks.size();++height) {
            const auto& block=f.blocks[height];work+=GetBlockProof(block.header.difficulty);
            replay->Append(block,height,work);history.push_back(block.header);
            OrchardAdmissionFixture::Require(headers.AddHeader(block.header));
            for(const auto& tx:block.vtx)OrchardAdmissionFixture::Require(ids.insert(tx.GetTxid()).second);
        }
        replay->Finish();OrchardAdmissionFixture::Require(headers.AddHeader(first->Header()));
    }
    auto NewBranch() {
        return std::make_unique<OrchardBranchReplay>(*replay,history,ids,OrchardParentReplay::Target{
            103,second->Header().GetHash(),*f.db.getBlockWork(second->Header().GetHash())});
    }
    void Append(OrchardBranchReplay& branch,const OrchardBlockCandidate& block,uint32_t height) {
        const auto now=std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        branch.Append(block,height,*f.db.getBlockWork(block.Header().GetHash()),uint64_t(now));
    }
    auto Complete() {
        auto branch=NewBranch();Append(*branch,OrchardBlockCandidate::DecodeExact(first->WireBytes()),102);
        Append(*branch,*body,103);branch->Finish();return branch;
    }
    std::optional<uint64_t> Mtp(uint32_t height)const {
        uint256 ancestor;uint32_t selected=0,found=0,time=0;
        if(height>102 || !headers.GetAncestorHashByHash(first->Header().GetHash(),height,ancestor,selected) ||
            selected!=102 || !headers.GetMedianTimePastByHash(ancestor,time,found) || found!=height)return std::nullopt;
        return time;
    }
    auto Stage(const consensus::ValidatedOrchardBlock* checked,const OrchardBlockCandidate& candidate,
        const consensus::UtreexoForest& parent_forest,const consensus::OrchardBranchMtpLookup& mtp,
        rocksdb::WriteBatch& batch) {
        const auto context=consensus::SelectedOrchardBlockContext(candidate.Header(),103);
        OrchardAdmissionFixture::Require(bool(context));
        const bool witness=Params().enforce_witness_commitment && 103>=Params().witness_commitment_enforcement_height;
        return consensus::StageOrchardChainstateConnectUnderLock(f.db,f.token,*context,candidate,
            first->Header(),parent_forest,mtp,witness,false,batch,std::nullopt,checked);
    }
    auto Rows()const{return ChainDBTransactionReadTestPeer::HandoffRows(f.db);}
    void Refuse(const consensus::ValidatedOrchardBlock& checked,const OrchardBlockCandidate& candidate,
        const consensus::UtreexoForest& parent_forest,const consensus::OrchardBranchMtpLookup& mtp) {
        auto selected=f.service->AcquireBlockIngressActivationLock();
        const auto rows=Rows();const auto* tip=f.service->GetActiveTip();rocksdb::WriteBatch batch;
        EXPECT_THROW((void)Stage(&checked,candidate,parent_forest,mtp,batch),consensus::OrchardStateLookupError);
        EXPECT_EQ(batch.Count(),0u);EXPECT_EQ(Rows(),rows);EXPECT_EQ(f.service->GetActiveTip(),tip);
        EXPECT_EQ(f.service->GetConsensusUTXOSet()->GetForest().serialize(),forest.serialize());
    }
};
}

TEST(OrchardProofHandoff, BatchComparisonIgnoresOnlyLocalReceiptTime) {
    ProofHandoffFixture f;
    const auto hash=f.second->Header().GetHash();
    const auto work=*f.f.db.getBlockWork(hash);
    auto batch=[&](uint32_t timestamp,uint32_t height,const arith_uint256& chainwork,
        const uint256& tip_hash,bool changed_row=false) {
        rocksdb::WriteBatch result;
        VectorWriter writer;
        writer.writeString(tip_hash.GetHex());writer.write(height);
        writer.write(chainwork);writer.write(timestamp);
        result.Put(ChainDBTransactionReadTestPeer::HandoffMeta(f.f.db),"tip",writer.release_string());
        // A similarly named row must never have its trailing bytes masked.
        result.Put(ChainDBTransactionReadTestPeer::HandoffMeta(f.f.db),"validated_tip",
            changed_row?"changed":"unchanged");
        return result;
    };
    const auto earlier=batch(1000,103,work,hash);
    const auto later=batch(1001,103,work,hash);
    ASSERT_NE(earlier.Data(),later.Data()); // Deterministically reproduces CI's boundary.
    const auto expected=HandoffBatchWithoutReceiptTime(f.f.db,earlier);
    EXPECT_EQ(expected,HandoffBatchWithoutReceiptTime(f.f.db,later));
    EXPECT_NE(expected,HandoffBatchWithoutReceiptTime(f.f.db,batch(1001,104,work,hash)));
    EXPECT_NE(expected,HandoffBatchWithoutReceiptTime(f.f.db,batch(1001,103,work+arith_uint256{1},hash)));
    EXPECT_NE(expected,HandoffBatchWithoutReceiptTime(f.f.db,batch(1001,103,work,uint256{})));
    EXPECT_NE(expected,HandoffBatchWithoutReceiptTime(f.f.db,batch(1001,103,work,hash,true)));
    auto removed=later;removed.Delete(ChainDBTransactionReadTestPeer::HandoffMeta(f.f.db),"coin");
    EXPECT_NE(expected,HandoffBatchWithoutReceiptTime(f.f.db,removed));
    rocksdb::WriteBatch missing;
    EXPECT_THROW(HandoffBatchWithoutReceiptTime(f.f.db,missing),std::runtime_error);
    auto duplicate=later;duplicate.Put(ChainDBTransactionReadTestPeer::HandoffMeta(f.f.db),"tip","bad");
    EXPECT_THROW(HandoffBatchWithoutReceiptTime(f.f.db,duplicate),std::runtime_error);
    rocksdb::WriteBatch malformed;
    malformed.Put(ChainDBTransactionReadTestPeer::HandoffMeta(f.f.db),"tip","bad");
    EXPECT_THROW(HandoffBatchWithoutReceiptTime(f.f.db,malformed),std::runtime_error);
}

TEST(OrchardProofHandoff, CompletedBranchOnlyAndExactNormalStaging) {
    ProofHandoffFixture f;auto branch=f.NewBranch();
    const auto first=OrchardBlockCandidate::DecodeExact(f.first->WireBytes());
    EXPECT_THROW(branch->ProvenBlock(102,first.Header().GetHash()),std::runtime_error);
    f.Append(*branch,first,102);
    EXPECT_THROW(branch->ProvenBlock(102,first.Header().GetHash()),std::runtime_error);
    f.Append(*branch,*f.body,103);
    EXPECT_THROW(branch->ProvenBlock(103,f.body->Header().GetHash()),std::runtime_error);
    branch->Finish();const auto& checked=branch->ProvenBlock(103,f.body->Header().GetHash());
    EXPECT_THROW(branch->ProvenBlock(102,f.body->Header().GetHash()),std::runtime_error);
    ASSERT_TRUE(checked.MatchesWire(*f.body));ASSERT_TRUE(checked.MatchesProfile());
    ASSERT_EQ(checked.BranchMtp().size(),1u);ASSERT_TRUE(f.Mtp(102));
    EXPECT_EQ(checked.BranchMtp().at(102),*f.Mtp(102));
    auto selected=f.f.service->AcquireBlockIngressActivationLock();
    const auto rows=f.Rows();rocksdb::WriteBatch regular,detached;
    std::map<uint32_t,uint64_t> regular_mtp,detached_mtp;
    const auto ordinary=f.Stage(nullptr,*f.body,f.forest,[&](uint32_t h){auto v=f.Mtp(h);if(v)regular_mtp[h]=*v;return v;},regular);
    const auto reused=f.Stage(&checked,*f.body,f.forest,[&](uint32_t h){auto v=f.Mtp(h);if(v)detached_mtp[h]=*v;return v;},detached);
    EXPECT_EQ(HandoffBatchWithoutReceiptTime(f.f.db,regular),
        HandoffBatchWithoutReceiptTime(f.f.db,detached));EXPECT_GT(detached.Count(),0u);
    EXPECT_EQ(ordinary.block.undo.Serialize(),reused.block.undo.Serialize());
    EXPECT_EQ(ordinary.forest.After().serialize(),reused.forest.After().serialize());
    EXPECT_EQ(regular_mtp,checked.BranchMtp());EXPECT_EQ(detached_mtp,regular_mtp);EXPECT_EQ(f.Rows(),rows);
    auto poisoned=f.NewBranch();f.Append(*poisoned,first,102);
    const auto bad=OrchardBlockCandidate::DecodeExact(BranchReplayWrongProof(*f.second));
    EXPECT_ANY_THROW(f.Append(*poisoned,bad,103));EXPECT_THROW(poisoned->Finish(),std::runtime_error);
    EXPECT_THROW(poisoned->ProvenBlock(102,first.Header().GetHash()),std::runtime_error);
}

TEST(OrchardProofHandoff, ChangedWireProfileCoinsAndEphemeralAbsenceRefuse) {
    ProofHandoffFixture f;auto branch=f.Complete();const auto& checked=branch->ProvenBlock(103,f.body->Header().GetHash());
    const auto mtp=[&](uint32_t h){return f.Mtp(h);};
    const auto bad=OrchardBlockCandidate::DecodeExact(BranchReplayWrongProof(*f.second));
    ASSERT_EQ(bad.Header().GetHash(),f.body->Header().GetHash());EXPECT_FALSE(checked.MatchesWire(bad));
    f.Refuse(checked,bad,f.forest,mtp);
    const auto max_size=Params().max_block_size;MutableParams().max_block_size=max_size-1;
    f.Refuse(checked,*f.body,f.forest,mtp);MutableParams().max_block_size=max_size;
    auto selected=f.f.service->AcquireBlockIngressActivationLock();
    const auto point=checked.Coins().Transactions().at(1).spent.at(0).first;
    const auto prior=f.f.db.getCoin(point.txid.AsUint256(),point.vout);ASSERT_TRUE(prior.ok());
    auto changed=*prior;++changed.amount;ASSERT_EQ(f.f.db.putCoin(f.f.token,point.txid.AsUint256(),point.vout,changed),Status::Ok);
    f.Refuse(checked,*f.body,f.forest,mtp);
    ASSERT_EQ(f.f.db.putCoin(f.f.token,point.txid.AsUint256(),point.vout,*prior),Status::Ok);
    const auto ephemeral=checked.Coins().Transactions().at(1).created.at(0).first;
    ASSERT_EQ(checked.Coins().Transactions().at(2).spent.at(0).first,ephemeral);
    ASSERT_TRUE(std::none_of(checked.Coins().Changes().begin(),checked.Coins().Changes().end(),
        [&](const auto& change){return change.outpoint==ephemeral;}));
    ASSERT_EQ(f.f.db.getCoin(ephemeral.txid.AsUint256(),ephemeral.vout).status(),Status::NotFound);
    ASSERT_EQ(f.f.db.putCoin(f.f.token,ephemeral.txid.AsUint256(),ephemeral.vout,*prior),Status::Ok);
    f.Refuse(checked,*f.body,f.forest,mtp);
    ASSERT_EQ(f.f.db.deleteCoin(f.f.token,ephemeral.txid.AsUint256(),ephemeral.vout),Status::Ok);
    rocksdb::WriteBatch retry;EXPECT_NO_THROW((void)f.Stage(&checked,*f.body,f.forest,mtp,retry));
    EXPECT_GT(retry.Count(),0u);
}

TEST(OrchardProofHandoff, ChangedStateMembershipForestAndMtpRefuse) {
    ProofHandoffFixture f;auto branch=f.Complete();const auto& checked=branch->ProvenBlock(103,f.body->Header().GetHash());
    const auto mtp=[&](uint32_t h){return f.Mtp(h);};
    f.Refuse(checked,*f.body,consensus::UtreexoForest{},mtp);
    f.Refuse(checked,*f.body,f.forest,[&](uint32_t h)->std::optional<uint64_t>{auto v=f.Mtp(h);if(v)++*v;return v;});
    f.Refuse(checked,*f.body,f.forest,[](uint32_t)->std::optional<uint64_t>{return std::nullopt;});
    auto selected=f.f.service->AcquireBlockIngressActivationLock();
    const auto state=ChainDBTransactionReadTestPeer::HandoffOrchardRow(f.f.db,"O1S");
    ASSERT_GT(state.size(),80u);auto changed=state;changed[72]^=1;
    ChainDBTransactionReadTestPeer::HandoffPutOrchardRow(f.f.db,f.f.token,"O1S",changed);
    f.Refuse(checked,*f.body,f.forest,mtp);
    ChainDBTransactionReadTestPeer::HandoffPutOrchardRow(f.f.db,f.f.token,"O1S",state);
    const auto anchor=checked.State().Parent()->anchor;
    const auto key=std::string("O1A")+std::string(reinterpret_cast<const char*>(anchor.data),32);
    const auto references=ChainDBTransactionReadTestPeer::HandoffOrchardRow(f.f.db,key);
    ASSERT_EQ(references.size(),8u);auto altered=references;altered[0]^=3;
    ChainDBTransactionReadTestPeer::HandoffPutOrchardRow(f.f.db,f.f.token,key,altered);
    f.Refuse(checked,*f.body,f.forest,mtp);
    ChainDBTransactionReadTestPeer::HandoffPutOrchardRow(f.f.db,f.f.token,key,references);
    rocksdb::WriteBatch retry;EXPECT_NO_THROW((void)f.Stage(&checked,*f.body,f.forest,mtp,retry));
    EXPECT_GT(retry.Count(),0u);
}

TEST(OrchardProofHandoff, ActualReplacementDeliveryRetainsSuccessfulMtp) {
    ProofHandoffFixture f;ASSERT_NO_FATAL_FAILURE(BranchReplayHistory(f));
    ASSERT_TRUE(f.Submit(f.second->WireBytes()).connected);auto* a103=f.f.service->GetActiveTip();
    BlockAssembler assembler(&f.f.db);WireOrchardAssembler(assembler,f.f);
    const auto third=assembler.CreateOrchardBlock(OrchardMiningPayout);ASSERT_TRUE(third);
    ASSERT_TRUE(f.Submit(third->WireBytes()).connected);auto* a104=f.f.service->GetActiveTip();
    std::string error;ASSERT_TRUE(f.f.service->InvalidateBlock(a103->hash,error))<<error;
    const auto [header,bytes]=ForkSibling(*f.second);ASSERT_TRUE(f.Submit(bytes).connected);
    auto* selected=f.f.service->GetActiveTip();ASSERT_EQ(selected->hash,header.GetHash());
    ASSERT_TRUE(f.f.service->ReconsiderBlock(a103->hash,error))<<error;
    ASSERT_TRUE(f.Submit(third->WireBytes()).retained());ASSERT_EQ(f.f.service->GetActiveTip(),selected);
    auto plan=ShieldedStateStartupTestAccess::CaptureActivationPlan(*f.f.service);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::CompleteActivationPlan(*f.f.service,plan));
    const auto profile=consensus::SelectedOrchardBlockContext(f.body->Header(),103);ASSERT_TRUE(profile);
    RuntimeOutboxCursor before;
    {auto lock=f.f.service->AcquireBlockIngressActivationLock();before=ReadRuntimeOutboxUnderLock(f.f.db,*profile).head;}
    ShieldedStateStartupTestAccess::ApplyActivationPlan(*f.f.service,plan);
    ASSERT_EQ(f.f.service->GetActiveTip(),a104);
    auto lock=f.f.service->AcquireBlockIngressActivationLock();
    const auto page=ReadRuntimeOutboxUnderLock(f.f.db,*profile,before);ASSERT_EQ(page.events.size(),3u);
    ASSERT_EQ(page.events.at(1).context.block_hash,a103->hash);
    ASSERT_EQ(page.events.at(1).direction,RuntimeBlockDirection::Connect);
    ASSERT_TRUE(page.events.at(1).orchard_replay);
    const std::map<uint32_t,uint64_t> expected{{102,*f.Mtp(102)}};
    EXPECT_EQ(page.events.at(1).orchard_replay->branch_mtp,expected);
    EXPECT_EQ(page.events.at(1).body,f.second->WireBytes());
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
} // namespace dinero
#endif
