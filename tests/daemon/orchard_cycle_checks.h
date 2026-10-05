#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
std::vector<orchard::Hash> CycleCommitments(const orchard::VerifiedAuthorization& auth) {
    std::vector<orchard::Hash> result(auth.Facts().action_count);
    for (size_t i=0;i<result.size();++i)
        std::copy_n(auth.Facts().commitments[i],32,result[i].begin());
    return result;
}
std::pair<orchard::WalletNote,size_t> CycleReceive(
        const orchard::WalletKeys& keys,const orchard::VerifiedAuthorization& auth) {
    for (uint32_t i=0;i<auth.Facts().action_count;++i) {
        auto note=orchard::WalletNote::Receive(auth,keys.ExportFullViewingKey(),orchard::WalletScope::External,i);
        if (note) return {std::move(*note),i};
    }
    throw std::runtime_error("actual cycle recipient note unavailable");
}
uint256 CycleHash(const uint8_t* bytes) {
    uint256 result;std::copy_n(bytes,32,result.begin());return result;
}
struct OrchardCycleFixture : CanonicalPoolFixture {
    std::shared_ptr<CanonicalPoolNotices> cycle_notices=std::make_shared<CanonicalPoolNotices>();
    ExecutionContext execution;
    OrchardCycleFixture() {
        f.service->setRuntimeBlockNotifications(cycle_notices);execution.daemon=&context;
    }
    orchard::SigningDomain Domain() const {
        orchard::SigningDomain domain;domain.network_code=2;domain.branch_id=Params().orchard_branch_id;
        const auto genesis=uint256::FromHexUnsafe(Params().genesis_hash);
        std::copy(genesis.begin(),genesis.end(),domain.genesis_wire.begin());return domain;
    }
    auto Shield(const orchard::WalletKeys& receiver) {
        const OutPoint point(f.blocks[1].vtx.front().GetTxid(),0);
        const auto& coin=f.replay->ProvenUtxos().at(point);
        orchard::EnvelopeInput input{};
        std::copy(point.txid.AsUint256().begin(),point.txid.AsUint256().end(),input.txid_wire.begin());
        input.output_index=0;input.sequence=UINT32_MAX;
        constexpr uint64_t fee=100000,deposit=500000;
        const std::vector<orchard::TransparentOutput> outputs{{coin.value.GetUna()-fee-deposit,f.script}};
        const std::vector<orchard::WalletPayment> payments{{deposit,receiver.Receiver(orchard::WalletScope::External,{})}};
        const std::vector<orchard::ResolvedInput> inputs{{input.txid_wire,0,input.sequence,coin.value.GetUna(),f.script}};
        const auto bundle=orchard::WalletBundlePlan::PrepareShield(receiver,payments)
            .Prove(orchard::SigningContext::Create(Domain(),0,inputs,outputs,fee));
        const auto draft=orchard::TransactionEnvelope::Create(0,{input},outputs,fee,bundle.Bytes());
        FirstBoundaryView view(*f.replay);
        const auto snapshot=consensus::OrchardCoinSnapshot::ResolveUnderChainstateLock(draft,view);
        const auto digest=consensus::OrchardTransparentSigningDigest(snapshot,Domain(),0);
        std::unique_ptr<secp256k1_context,decltype(&secp256k1_context_destroy)> signing(
            secp256k1_context_create(SECP256K1_CONTEXT_NONE),secp256k1_context_destroy);
        secp256k1_keypair pair;OrchardAdmissionFixture::Require(secp256k1_keypair_create(signing.get(),&pair,f.secret.data()));
        std::vector<uint8_t> signature(64);orchard::Hash aux{};
        OrchardAdmissionFixture::Require(secp256k1_schnorrsig_sign32(signing.get(),signature.data(),digest.data(),&pair,aux.data()));
        input.witness={signature};
        return std::pair{MempoolTransaction::FromOrchard(
            orchard::TransactionEnvelope::Create(0,{input},outputs,fee,bundle.Bytes())),bundle};
    }
    std::shared_ptr<const RuntimeBlockBody> Mine(const MempoolTransaction& body) {
        const auto admission=f.ingress->SubmitBody(body,TxOrigin::INTERNAL);
        if (!admission.accepted() && admission.code!=TxRejectCode::ALREADY_IN_MEMPOOL)
            throw std::runtime_error("cycle admission refused: "+admission.message);
        // RPC callers may already have admitted this exact witness-bearing body.
        // A duplicate txid alone is not enough to proceed to mining.
        const auto retained=f.ingress->mempool().getMempoolEntry(body.GetTxid().AsUint256());
        if (!retained || retained->tx.Serialize()!=body.Serialize())
            throw std::runtime_error("cycle admitted body differs from requested mining body");
        din::Json request;request["address"]=OrchardMiningPayout;
        const auto job=::rpc_mining_getjob(execution,request);
        OrchardAdmissionFixture::Require(!job.isMember("error") && job.isMember("job_id"));
        auto header=OrchardRpcMiningFixture::Header(job);header.nonce=OrchardRpcMiningFixture::Nonce(job);
        din::Json solution;solution["job_id"]=job["job_id"];solution["nonce"]=header.nonce;
        OrchardAdmissionFixture::Require(::rpc_mining_submit(execution,solution).isNull());
        const auto hash=header.GetHash();
        OrchardAdmissionFixture::Require(f.service->GetActiveTip()->hash==hash);
        const auto stored=f.service->getRuntimeBlockByHash(hash);
        OrchardAdmissionFixture::Require(stored.ok() && (*stored)->IsOrchardProfile());
        const auto& block=(*stored)->Orchard();
        OrchardAdmissionFixture::Require(block.Transactions().size()==2);
        EXPECT_EQ(block.Transactions()[1].Serialize(TxSerializationMode::WithWitness),body.Serialize());
        EXPECT_EQ(ReadBlockRpc(context,hash.GetHex(),0).asString(),util::hex(block.WireBytes()));
        EXPECT_EQ(f.ingress->mempool().size(),0u);
        EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.service));
        return *stored;
    }
};
}
TEST(OrchardCycle, RealProofsMineTransferUnshieldAndReopenUndo) {
    OrchardCycleFixture f;
    const auto receiver=orchard::WalletKeys::FromSeed(std::array<uint8_t,64>{53},0);
    const auto recipient=orchard::WalletKeys::FromSeed(std::array<uint8_t,64>{55},0);
    const auto [shield,shield_bundle]=f.Shield(receiver);
    const auto first=f.Mine(shield);EXPECT_EQ(first->Context()->height,102u);
    ASSERT_TRUE(f.f.db.getOrchardState().ok());
    ASSERT_EQ(f.f.db.getOrchardState()->pool_balance,500000u);
    const auto empty=orchard::OrchardFrontier::Empty();
    const auto shield_leaves=CycleCommitments(shield_bundle.Authorization());
    const auto shield_frontier=empty.Append(shield_leaves);
    EXPECT_EQ(f.f.db.getOrchardState()->anchor,CycleHash(shield_frontier.Root().data()));
    auto [note,note_index]=CycleReceive(receiver,shield_bundle.Authorization());
    ASSERT_EQ(note.Facts().amount,500000u);
    const auto witness=orchard::WalletWitness::ForAppendedLeaf(empty,shield_leaves,note_index);
    const std::vector<orchard::WalletSpendInput> spend{{note,witness}};
    const std::vector<orchard::WalletPayment> payment{{400000,recipient.Receiver(orchard::WalletScope::External,{})}};
    const auto transfer_bundle=orchard::WalletBundlePlan::PrepareSpend(receiver,spend,shield_frontier.Root(),payment)
        .Prove(orchard::SigningContext::Create(f.Domain(),0,{}, {},100000));
    const auto transfer=MempoolTransaction::FromOrchard(
        orchard::TransactionEnvelope::Create(0,{}, {},100000,transfer_bundle.Bytes()));
    const auto second=f.Mine(transfer);EXPECT_EQ(second->Context()->height,103u);
    ASSERT_TRUE(f.f.db.getOrchardState().ok());
    ASSERT_TRUE(f.f.db.getOrchardNullifierOwner(CycleHash(note.Facts().nullifier)).ok());
    EXPECT_EQ(f.f.db.getOrchardState()->pool_balance,400000u);
    EXPECT_EQ(*f.f.db.getOrchardNullifierOwner(CycleHash(note.Facts().nullifier)),second->Orchard().Header().GetHash());
    const auto transfer_leaves=CycleCommitments(transfer_bundle.Authorization());
    const auto transfer_frontier=shield_frontier.Append(transfer_leaves);
    EXPECT_EQ(f.f.db.getOrchardState()->anchor,CycleHash(transfer_frontier.Root().data()));
    auto [received,received_index]=CycleReceive(recipient,transfer_bundle.Authorization());
    ASSERT_EQ(received.Facts().amount,400000u);
    const auto received_witness=orchard::WalletWitness::ForAppendedLeaf(shield_frontier,transfer_leaves,received_index);
    const std::vector<orchard::WalletSpendInput> withdrawal{{received,received_witness}};
    const std::vector<orchard::TransparentOutput> cash{{300000,f.f.script}};
    const auto exit_bundle=orchard::WalletBundlePlan::PrepareSpend(recipient,withdrawal,transfer_frontier.Root(),{})
        .Prove(orchard::SigningContext::Create(f.Domain(),0,{},cash,100000));
    const auto exit=MempoolTransaction::FromOrchard(
        orchard::TransactionEnvelope::Create(0,{},cash,100000,exit_bundle.Bytes()));
    const auto third=f.Mine(exit);const auto exit_hash=exit.GetTxid().AsUint256();
    EXPECT_EQ(third->Context()->height,104u);
    const auto nullifier=CycleHash(received.Facts().nullifier);
    ASSERT_TRUE(f.f.db.getOrchardState().ok());
    ASSERT_TRUE(f.f.db.getOrchardNullifierOwner(nullifier).ok());
    EXPECT_EQ(f.f.db.getOrchardState()->pool_balance,0u);
    ASSERT_TRUE(f.f.db.getCoin(exit_hash,0).ok());EXPECT_EQ(f.f.db.getCoin(exit_hash,0)->amount,300000u);
    EXPECT_EQ(f.f.db.getCoin(exit_hash,0)->script_pubkey,util::hex(f.f.script));
    EXPECT_EQ(*f.f.db.getOrchardNullifierOwner(nullifier),third->Orchard().Header().GetHash());
    EXPECT_EQ(500000u,300000u+transfer.Orchard().ExplicitFee()+exit.Orchard().ExplicitFee());
    EXPECT_FALSE(f.f.ingress->SubmitBody(exit,TxOrigin::INTERNAL).accepted());
    EXPECT_EQ(f.f.ingress->mempool().size(),0u);
    f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    ASSERT_TRUE(f.f.db.getOrchardState().ok());
    EXPECT_EQ(f.f.db.getCoin(exit_hash,0).status(),Status::NotFound);
    EXPECT_EQ(f.f.db.getOrchardNullifierOwner(nullifier).status(),Status::NotFound);
    EXPECT_EQ(f.f.db.getOrchardState()->pool_balance,400000u);
    EXPECT_EQ(f.f.db.getOrchardState()->anchor,CycleHash(transfer_frontier.Root().data()));
    f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    ASSERT_TRUE(f.f.ingress->SubmitBody(exit,TxOrigin::INTERNAL).accepted());
    const auto reconnected=f.Submit(third->Orchard().WireBytes());ASSERT_TRUE(reconnected.accepted())<<reconnected.reason;
    ASSERT_TRUE(f.f.db.getOrchardState().ok());
    ASSERT_TRUE(f.f.db.getOrchardNullifierOwner(nullifier).ok());
    ASSERT_TRUE(f.f.db.getCoin(exit_hash,0).ok());
    EXPECT_EQ(f.f.db.getOrchardState()->pool_balance,0u);
    EXPECT_EQ(*f.f.db.getOrchardNullifierOwner(nullifier),third->Orchard().Header().GetHash());
    EXPECT_EQ(f.f.db.getCoin(exit_hash,0)->amount,300000u);
    EXPECT_EQ(f.f.ingress->mempool().size(),0u);EXPECT_EQ(f.cycle_notices->published,5u);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
#else
TEST(OrchardCycle, BackendUnavailableDoesNotCreateMiningTemplate) {
    MiningChainstateReadGuard owner;
    EXPECT_FALSE(owner.BuildOrchardTemplate({}, {}, {}, 1));
}
#endif
