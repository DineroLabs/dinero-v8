#pragma once
// Included by the actual selected-history/service fixture inside namespace dinero.
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
class OrchardAdmissionFixture {
public:
    ChainParams previous_params = Params();
    bool previous_stateless = GetConfig().utreexo_stateless;
    std::filesystem::path path;
    std::shared_ptr<ChainDB> database=std::make_shared<ChainDB>();
    ChainDB& db=*database;
    std::shared_ptr<ChainstateService> service=std::make_shared<ChainstateService>();
    std::shared_ptr<MempoolService> ingress;
    std::vector<uint8_t> script;
    orchard::Hash secret{};
    std::vector<uint8_t> last_bundle;
    CBlockIndex tip;
    std::vector<Block> blocks;
    std::unique_ptr<assumeutxo::AssumeUtxoReplayEngine> replay;
    const ChainWriteToken token = ChainWriteToken::CreateForTesting();
    static void Require(bool value,const char* file=__builtin_FILE(),int line=__builtin_LINE()) {
        if (!value) throw std::runtime_error(std::string("selected parent fixture requirement at ")+file+":"+std::to_string(line));
    }
    struct AdmissionLogger final:ILogger {
        bool refuse=false;
        void info(const std::string& message)override {
            if(refuse && message.find("Added Orchard transaction")!=std::string::npos)
                throw std::runtime_error("fixture before-publication refusal");
        }
        void log(LogLevel,const std::string& message)override{info(message);}
        void debug(const std::string&)override{} void warning(const std::string&)override{}
        void error(const std::string&)override{} void setLogLevel(LogLevel)override{}
        void setLogFile(const std::string&)override{} void shutdown()override{}
    };
    std::shared_ptr<AdmissionLogger> logger=std::make_shared<AdmissionLogger>();
    struct AdmissionNotifications final:RuntimeBlockNotifications {
        std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(const RuntimeBlockBody&,uint32_t,RuntimeBlockDirection)override {
            throw std::logic_error("Admission must not publish a block notification");
        }
    };
    std::vector<Block> BuildOwnedChain() {
        secret.back()=9;
        std::unique_ptr<secp256k1_context,decltype(&secp256k1_context_destroy)> ctx(
            secp256k1_context_create(SECP256K1_CONTEXT_NONE),secp256k1_context_destroy);
        secp256k1_keypair pair;secp256k1_xonly_pubkey key;
        Require(secp256k1_keypair_create(ctx.get(),&pair,secret.data()));
        Require(secp256k1_keypair_xonly_pub(ctx.get(),&key,nullptr,&pair));
        script={0x51,32};script.resize(34);
        Require(secp256k1_xonly_pubkey_serialize(ctx.get(),script.data()+2,&key));
        std::vector<Block> chain;consensus::ConsensusUTXOSet coins;
        consensus::BlockValidator validator(&coins);SeedDirect(coins);
        auto parent=SelectedGenesis().GetHash();
        for(uint32_t height=1;height<=101;++height) {
            Block block;block.header.version=1;block.header.prev_block_hash=parent;
            block.header.timestamp=SelectedGenesis().header.timestamp+height*120;
            block.header.difficulty=0x1d00ffff;block.header.ZeroReserved();
            block.vtx={MakeCoinbase(height)};block.vtx.front().vout.front().scriptPubKey=script;
            block.header.merkle_root=block.vtx.front().GetTxid().AsUint256();
            uint256 root;std::string error;
            Require(validator.ComputeUtreexoRootPure(block,height,root,error));block.header.utreexo_root=root;
            consensus::BlockUndo undo;Require(validator.ConnectBlock(block,height,block.GetHash(),undo,error));
            parent=block.GetHash();chain.push_back(std::move(block));
        }
        return chain;
    }
    MempoolTransaction Shield(uint32_t coin_height=1) {
        const auto point=OutPoint(blocks.at(coin_height).vtx.front().GetTxid(),0);
        const auto& coin=replay->ProvenUtxos().at(point);
        orchard::SigningDomain domain;domain.network_code=2;domain.branch_id=Params().orchard_branch_id;
        const auto genesis=uint256::FromHexUnsafe(Params().genesis_hash);
        std::copy(genesis.begin(),genesis.end(),domain.genesis_wire.begin());
        orchard::EnvelopeInput input{};std::copy(point.txid.AsUint256().begin(),point.txid.AsUint256().end(),input.txid_wire.begin());
        input.output_index=0;input.sequence=UINT32_MAX;
        constexpr uint64_t fee=100000,shield=5000;
        std::vector<orchard::TransparentOutput> outputs{{coin.value.GetUna()-fee-shield,script}};
        std::array<uint8_t,64> seed{51};auto keys=orchard::WalletKeys::FromSeed(seed,0);
        const std::vector<orchard::WalletPayment> payments{{shield,keys.Receiver(orchard::WalletScope::External,{})}};
        auto plan=orchard::WalletBundlePlan::PrepareShield(keys,payments);
        const std::vector<orchard::ResolvedInput> resolved{{input.txid_wire,0,input.sequence,coin.value.GetUna(),script}};
        const auto signing=orchard::SigningContext::Create(domain,0,resolved,outputs,fee);
        const auto bundle=std::move(plan).Prove(signing);last_bundle=bundle.Bytes();
        auto envelope=orchard::TransactionEnvelope::Create(0,{input},outputs,fee,bundle.Bytes());
        FirstBoundaryView view(*replay);
        const auto snapshot=consensus::OrchardCoinSnapshot::ResolveUnderChainstateLock(envelope,view);
        const auto digest=consensus::OrchardTransparentSigningDigest(snapshot,domain,0);
        std::unique_ptr<secp256k1_context,decltype(&secp256k1_context_destroy)> ctx(
            secp256k1_context_create(SECP256K1_CONTEXT_NONE),secp256k1_context_destroy);
        secp256k1_keypair pair;Require(secp256k1_keypair_create(ctx.get(),&pair,secret.data()));
        std::vector<uint8_t> signature(64);orchard::Hash aux{};
        Require(secp256k1_schnorrsig_sign32(ctx.get(),signature.data(),digest.data(),&pair,aux.data()));
        input.witness={signature};
        return MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::Create(0,{input},outputs,fee,bundle.Bytes()));
    }
    OrchardAdmissionFixture() {
        MutableParams().orchard_activation_height=102;
        MutableParams().orchard_branch_id=1;
        MutableParams().shielded_activation_height=1;
        MutableParams().shielded_epoch_reset_height=UINT32_MAX;
        MutableParams().shielded_spend_auth_epoch_reset_height=UINT32_MAX;
        GetConfig().utreexo_stateless=false;
        auto name=(std::filesystem::temp_directory_path()/"orchard_admission_XXXXXX").string();
        Require(mkdtemp(name.data())!=nullptr);path=name;
        // Generated separated-layout fixture. Normal ChainDB open correctly
        // creates the legacy layout and must never silently migrate it.
        {
            auto families=shielded_store_fixture::legacy;
            families.push_back(shielded_store_fixture::shielded);
            shielded_store_fixture::Raw raw(path,families);
            const uint32_t schema=4;
            raw.put("meta","schema_version",std::string(reinterpret_cast<const char*>(&schema),4));
            raw.put("meta","storage_layout_v1",shielded_store_fixture::ready);
            consensus::shielded::CommitmentTree tree;consensus::shielded::AnchorHistory anchors;
            const auto frontier=tree.SerializeFrontier();const auto history=anchors.SerializePersistenceBytes();
            raw.put(shielded_store_fixture::shielded,"Mshielded_frontier",{frontier.begin(),frontier.end()});
            raw.put(shielded_store_fixture::shielded,"Mshielded_anchor_history",{history.begin(),history.end()});
            raw.put("meta","shielded_tip",std::string(84,'\0'));
        }
        Require(db.init(path)==Status::Ok);
        blocks={SelectedGenesis()};const auto rest=BuildOwnedChain();
        Require(rest.size()==101);blocks.insert(blocks.end(),rest.begin(),rest.end());
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
        tip=CBlockIndex(blocks.back().header,101);tip.chainwork=work.GetHex();
        Require(db.setTip(token,tip.hash,tip.height,work)==Status::Ok);
        Require(db.setValidatedTip(token,tip.hash,tip.height)==Status::Ok);
        for(const auto& [point,entry]:replay->ProvenUtxos()) {
            Coin c;c.amount=entry.value.GetUna();c.script_pubkey=util::hex(entry.scriptPubKey);
            c.height=entry.height;c.coinbase=entry.isCoinbase;c.is_confidential=entry.is_confidential;c.commitment=entry.commitment;
            Require(db.putCoin(token,point.txid.AsUint256(),point.vout,c)==Status::Ok);
        }
        Require(db.putForestTipMarker(token,{101,tip.hash,uint256::FromHexUnsafe(replay->UtreexoRootHex())})==Status::Ok);
        const auto frontier=replay->ShieldedTree()->SerializeFrontier();
        const auto anchors=replay->ShieldedAnchors()->SerializePersistenceBytes();
        Require(db.putShieldedState(token,ChainDB::ShieldedStateRecord::Frontier,{frontier.begin(),frontier.end()})==Status::Ok);
        Require(db.putShieldedState(token,ChainDB::ShieldedStateRecord::AnchorHistory,{anchors.begin(),anchors.end()})==Status::Ok);
        const auto root=replay->ShieldedTree()->Root();uint256 tree_root;std::copy(root.begin(),root.end(),tree_root.begin());
        Require(db.putShieldedTipMarker(token,{101,tip.hash,tree_root,replay->ShieldedTree()->Size(),0})==Status::Ok);
        service->setOwnedChainDB(database);ShieldedStateStartupTestAccess::BoundaryState(*service,tip,*replay);
        auto headers=std::make_shared<consensus::HeaderChainSelector>();
        for(const auto& block:blocks)Require(headers->AddHeader(block.header));
        service->setHeaderChainSelector(headers);
        service->setRuntimeBlockNotifications(std::make_shared<AdmissionNotifications>());
        DaemonContext context;context.chainstate=service;context.config=std::make_shared<ConfigService>();
        context.logger_interface=logger.get();ingress=std::make_shared<MempoolService>();Require(ingress->Init(context));
    }
    ~OrchardAdmissionFixture() {
        ingress->Stop();ingress.reset();service->setRuntimeBlockNotifications(nullptr);service->setChainDB(nullptr);db.close();std::error_code ec;std::filesystem::remove_all(path,ec);
        MutableParams()=previous_params;GetConfig().utreexo_stateless=previous_stateless;
    }
    auto Read() {return ShieldedStateStartupTestAccess::Boundary(*service);}
    void CheckUnpublished() {
        ASSERT_EQ(db.getLegacyRetirementState().status(),Status::NotFound);
        ASSERT_TRUE(db.getTip().ok());EXPECT_EQ(db.getTip()->hash,tip.hash);
    }
};
}
TEST(OrchardSelectedAdmission, RealShieldPreflightPublicationAndDuplicate) {
    OrchardAdmissionFixture f;const auto body=f.Shield();auto& pool=f.ingress->mempool();
    unsigned observed=0;
    pool.setTxBodyAcceptedCallback([&](const MempoolTransaction& captured) {
        ++observed;EXPECT_EQ(captured.Serialize(),body.Serialize());EXPECT_TRUE(pool.hasTransaction(body.GetTxid().AsUint256()));
    });
    ASSERT_TRUE(pool.submitBody(body,"preflight",false,true).accepted());EXPECT_EQ(pool.size(),0u);EXPECT_EQ(observed,0u);
    const auto result=f.ingress->SubmitBody(body,TxOrigin::INTERNAL);ASSERT_TRUE(result.accepted())<<result.message;
    EXPECT_EQ(observed,1u);const auto entry=pool.getMempoolEntry(body.GetTxid().AsUint256());ASSERT_TRUE(entry);
    EXPECT_EQ(entry->height,101u);EXPECT_EQ(entry->fee,100000u);EXPECT_EQ(entry->tx.Serialize(),body.Serialize());
    EXPECT_TRUE(pool.isOutputSpentInMempool(body.Inputs().front()));
    EXPECT_EQ(f.ingress->SubmitBody(body,TxOrigin::INTERNAL).code,TxRejectCode::ALREADY_IN_MEMPOOL);
    EXPECT_EQ(observed,1u);f.CheckUnpublished();
}
TEST(OrchardSelectedAdmission, MissingOwnerAndInvalidInputRefuseThenRetry) {
    OrchardAdmissionFixture f;const auto body=f.Shield();auto& pool=f.ingress->mempool();
    f.service->setRuntimeBlockNotifications(nullptr);
    EXPECT_EQ(pool.submitBody(body,"provider",false).code,TxRejectCode::UNAVAILABLE);EXPECT_EQ(pool.size(),0u);
    f.service->setRuntimeBlockNotifications(std::make_shared<OrchardAdmissionFixture::AdmissionNotifications>());
    pool.setTxAcceptedCallback([](const Transaction&){});
    EXPECT_EQ(pool.submitBody(body,"observer",false).code,TxRejectCode::UNAVAILABLE);EXPECT_EQ(pool.size(),0u);
    pool.setTxAcceptedCallback({});
    const auto id=f.tip.hash;ASSERT_EQ(f.db.setValidatedTip(f.token,f.blocks[100].GetHash(),100),Status::Ok);
    EXPECT_EQ(pool.submitBody(body,"identity",false).code,TxRejectCode::UNAVAILABLE);EXPECT_EQ(pool.size(),0u);
    ASSERT_EQ(f.db.setValidatedTip(f.token,id,101),Status::Ok);
    const auto& tx=body.Orchard();auto inputs=tx.Inputs();inputs[0].witness[0][0]^=1;
    const auto bad=MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::Create(tx.LockTime(),inputs,tx.Outputs(),tx.ExplicitFee(),f.last_bundle));
    EXPECT_FALSE(pool.submitBody(bad,"signature",false).accepted());EXPECT_EQ(pool.size(),0u);
    ASSERT_TRUE(f.ingress->SubmitBody(body,TxOrigin::INTERNAL).accepted());f.CheckUnpublished();
}
TEST(OrchardSelectedAdmission, PublicationRollbackAndPostCommitObserverOutcome) {
    OrchardAdmissionFixture f;const auto body=f.Shield();auto& pool=f.ingress->mempool();
    f.logger->refuse=true;EXPECT_THROW(pool.submitBody(body,"rollback",false),std::runtime_error);
    EXPECT_EQ(pool.size(),0u);EXPECT_FALSE(pool.isOutputSpentInMempool(body.Inputs().front()));
    f.logger->refuse=false;
    pool.setTxBodyAcceptedCallback([&](const MempoolTransaction&){EXPECT_EQ(pool.size(),1u);throw std::runtime_error("fixture after-publication");});
    EXPECT_THROW(f.ingress->SubmitBody(body,TxOrigin::INTERNAL),std::runtime_error);
    EXPECT_TRUE(pool.hasTransaction(body.GetTxid().AsUint256()));EXPECT_TRUE(pool.isOutputSpentInMempool(body.Inputs().front()));f.CheckUnpublished();
}
TEST(OrchardSelectedAdmission, MatureInputsPendingConflictsAndSelectedDomain) {
    OrchardAdmissionFixture f;auto& pool=f.ingress->mempool();
    const auto immature=f.Shield(3);
    EXPECT_EQ(pool.submitBody(immature,"maturity",false).code,TxRejectCode::SCRIPT_VERIFY_FAILED);
    EXPECT_EQ(pool.size(),0u);
    const auto first=f.Shield(1);
    const auto branch=Params().orchard_branch_id;MutableParams().orchard_branch_id=branch+1;
    EXPECT_FALSE(pool.submitBody(first,"domain",false).accepted());EXPECT_EQ(pool.size(),0u);
    MutableParams().orchard_branch_id=branch;
    ASSERT_TRUE(f.ingress->SubmitBody(first,TxOrigin::INTERNAL).accepted());
    const auto second=f.Shield(2);
    const auto& tx=second.Orchard();auto copied=tx.Inputs();copied[0].txid_wire=first.Orchard().Inputs()[0].txid_wire;
    const auto conflict=MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::Create(0,copied,tx.Outputs(),tx.ExplicitFee(),f.last_bundle));
    EXPECT_EQ(pool.submitBody(conflict,"input-owner",false).code,TxRejectCode::DOUBLE_SPEND_NO_RBF);
    const auto result=f.ingress->SubmitBody(second,TxOrigin::INTERNAL);ASSERT_TRUE(result.accepted())<<result.message;
    EXPECT_EQ(pool.size(),2u);EXPECT_TRUE(pool.hasTransaction(first.GetTxid().AsUint256()));
    auto changed=tx.Inputs();changed[0].sequence=0xfffffffe;
    // Remove transparent-input overlap while retaining every actual action
    // nullifier. No signature/proof validity is claimed for this refused body.
    auto other_inputs=changed;other_inputs[0].txid_wire=immature.Orchard().Inputs()[0].txid_wire;
    const auto same_nullifiers=MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::Create(0,other_inputs,tx.Outputs(),tx.ExplicitFee(),f.last_bundle));
    EXPECT_EQ(pool.submitBody(same_nullifiers,"nullifier-owner",false).code,TxRejectCode::DOUBLE_SPEND_NO_RBF);
    EXPECT_EQ(pool.size(),2u);f.CheckUnpublished();
}
#else
TEST(OrchardSelectedAdmission, DefaultValidatorUnavailable) {
    MempoolChainstateReadGuard owner;
    EXPECT_EQ(owner.ValidateOrchard(MempoolTransaction{},{}).result.code,TxRejectCode::UNAVAILABLE);
}
#endif
