#pragma once
// Included in the real service/replay test only, inside namespace dinero.
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
class FirstBoundaryView final:public consensus::ChainStateView {
    const assumeutxo::AssumeUtxoReplayEngine& replay_;
public:
    explicit FirstBoundaryView(const assumeutxo::AssumeUtxoReplayEngine& replay):replay_(replay){}
    StatusOr<consensus::UTXOEntry> getCoin(const OutPoint& p)const override {
        const auto i=replay_.ProvenUtxos().find(p);
        if(i==replay_.ProvenUtxos().end())return Status::NotFound;
        return i->second;
    }
    bool hasCoin(const OutPoint& p)const override {return replay_.ProvenUtxos().contains(p);}
    uint32_t getHeight()const override {return replay_.Height();}
};
template<class Fixture>
OrchardBlockCandidate BoundaryCoinbaseBlock(Fixture& f,
    const storage::LegacyRetirementRecord& retirement) {
    using namespace consensus;
    const auto require=SelectedParentFixture::Require;
    const uint32_t height=f.tip.height+1;
    Transaction cb;cb.version=2;
    TxInput input;input.prevout.vout=UINT32_MAX;if(height==4)input.scriptSig={0x54,0};
    else {
        auto value=height;std::vector<uint8_t> number;
        while(value){number.push_back(uint8_t(value));value>>=8;}
        if(number.back()&0x80)number.push_back(0);
        input.scriptSig.push_back(uint8_t(number.size()));
        input.scriptSig.insert(input.scriptSig.end(),number.begin(),number.end());input.scriptSig.push_back(0);
    }
    cb.vin={input};
    cb.vout.emplace_back(AmountUna::Una(1),std::vector<uint8_t>{0x51});
    const std::vector<WTxId> witness_ids{WTxId(uint256{})};
    cb.vout.emplace_back(AmountUna::Zero(),BuildWitnessCommitmentFromRoot(
        ComputeWitnessMerkleRootFromIds(witness_ids)));
    BlockHeader header{};header.version=1;header.prev_block_hash=f.tip.hash;
    header.timestamp=f.blocks.back().header.timestamp+120;header.difficulty=f.blocks.back().header.difficulty;
    const auto encode=[&](const BlockHeader& h) {
        auto copy=h;copy.merkle_root=cb.GetTxid().AsUint256();
        const auto prefix=copy.SerializeForHash();std::vector<uint8_t> bytes(prefix.begin(),prefix.end());
        bytes.push_back(1);const auto wire=cb.Serialize(TxSerializationMode::WithWitness);
        bytes.insert(bytes.end(),wire.begin(),wire.end());bytes.push_back(0);
        return OrchardBlockCandidate::DecodeExact(bytes);
    };
    FirstBoundaryView view(*f.replay);
    auto draft=encode(header);auto context=SelectedOrchardBlockContext(draft.Header(),height);require(bool(context));
    const auto preliminary=PrepareOrchardBlockCoinsUnderChainstateLock(draft,*context,view,{},true);
    const auto filter=BuildOrchardBlockFilter(preliminary).GetHash();
    std::vector<uint8_t> filter_script{0x6a,37,0x44,0x4e,0x52,0x46,1};
    filter_script.insert(filter_script.end(),filter.begin(),filter.end());
    cb.vout.emplace_back(AmountUna::Zero(),filter_script);
    const auto filtered=encode(header);context=SelectedOrchardBlockContext(filtered.Header(),height);
    OrchardStateLookups lookups{
        [&](const uint256& a)->StatusOr<bool>{auto v=f.db.getOrchardAnchorReferences(a);if(v.ok())return true;if(v.status()==Status::NotFound)return false;return v.status();},
        [&](const uint256& n)->StatusOr<bool>{auto v=f.db.getOrchardNullifierOwner(n);if(v.ok())return true;if(v.status()==Status::NotFound)return false;return v.status();}};
    const auto next=PrepareOrchardStateTransition(*context,std::nullopt,{},lookups);
    const auto sets=f.db.previewOrchardCommitmentSets(std::nullopt,next.Next(),next.Nullifiers());require(sets.ok());
    const auto root=ComputeOrchardStateRoot({context->domain,context->activation_height,context->height,context->parent_hash},retirement,next.Next(),*sets);
    cb.vout.emplace_back(AmountUna::Zero(),BuildStateCommitmentScript(root,StateCommitmentEncoding::Orchard));
    const auto state_draft=encode(header);context=SelectedOrchardBlockContext(state_draft.Header(),height);
    const auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(state_draft,*context,view,{},true);
    const auto forest=PrepareOrchardForestTransition(coins,f.blocks.back().header,*f.replay->Forest());
    header.utreexo_root=forest.Root();const auto final_draft=encode(header);
    BlockUtreexoData proof;proof.accumulator_root_before=f.replay->Forest()->getCommitment();
    proof.spend_proof=f.replay->Forest()->generateBlockProof({},GetUtreexoProofFormatVersion(height));
    auto wire=final_draft.WireBytes();require(wire.back()==0);wire.back()=1;
    const auto suffix=proof.serialize();wire.insert(wire.end(),suffix.begin(),suffix.end());
    return OrchardBlockCandidate::DecodeExact(wire);
}
struct BoundaryNotifications final:RuntimeBlockNotifications {
    ChainDB& db;ChainstateService& service;CBlockIndex& parent;CBlockIndex& child;
    bool refuse=false;unsigned prepared=0,published=0;bool coherent=true;
    BoundaryNotifications(ChainDB& d,ChainstateService& s,CBlockIndex& p,CBlockIndex& c):db(d),service(s),parent(p),child(c){}
    struct Prepared final:PreparedRuntimeBlockNotifications {
        BoundaryNotifications& owner;RuntimeBlockDirection direction;
        Prepared(BoundaryNotifications& o,RuntimeBlockDirection d):owner(o),direction(d){}
        void PublishAfterCommit()noexcept override {
            ++owner.published;const auto tip=owner.db.getTip();
            const auto* expected=direction==RuntimeBlockDirection::Connect?&owner.child:&owner.parent;
            owner.coherent &= tip.ok() && tip->hash==expected->hash &&
                ShieldedStateStartupTestAccess::BoundaryTipIs(owner.service,expected);
        }
    };
    std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(const RuntimeBlockBody& body,uint32_t height,RuntimeBlockDirection direction)override {
        SelectedParentFixture::Require(body.IsOrchardProfile() && height==4 && body.Orchard().Header().GetHash()==child.hash);
        ++prepared;if(refuse)return {};
        return std::make_unique<Prepared>(*this,direction);
    }
};
}
TEST(OrchardFirstBoundary, ConnectRefusalDisconnectAndReopenReconnect) {
    using Access=ShieldedStateStartupTestAccess;
    OwnedSelectedParentFixture f;const auto record=f.Read();ASSERT_TRUE(record);
    MutableParams().enforce_witness_commitment=true;MutableParams().witness_commitment_enforcement_height=4;
    const auto candidate=BoundaryCoinbaseBlock(f,*record);
    auto files=std::make_shared<BlockStorage>();ASSERT_EQ(files->init(f.path/"flatfiles"),Status::Ok);
    f.service.setBlockStorage(files);
    struct DetachFixtureStorage {
        ChainstateService& service;
        ~DetachFixtureStorage(){service.setRuntimeBlockNotifications(nullptr);service.setBlockStorage(nullptr);}
    } detach{f.service};
    auto headers=std::make_shared<consensus::HeaderChainSelector>();
    for(const auto& b:f.blocks)ASSERT_TRUE(headers->AddHeader(b.header));
    ASSERT_TRUE(headers->AddHeader(candidate.Header()));f.service.setHeaderChainSelector(headers);
    const auto work=ChainworkFromHex(f.tip.chainwork)+GetBlockProof(candidate.Header().difficulty);
    ASSERT_EQ(f.db.putHeader(f.token,candidate.Header().GetHash(),candidate.Header(),4,work),Status::Ok);
    const auto location=files->writeBlockBytes(candidate.Header().GetHash(),{candidate.WireBytes().begin(),candidate.WireBytes().end()});ASSERT_TRUE(location.ok());
    ChainDB::PersistedHeaderMetadata metadata;metadata.height=4;metadata.parent_hash=f.tip.hash;metadata.chainwork=work;
    metadata.status_flags=BLOCK_VALID_HEADER|BLOCK_HAVE_DATA;metadata.file_number=location->file_number;metadata.data_pos=location->offset;metadata.data_size=location->size;
    ASSERT_EQ(f.db.putHeaderMetadata(f.token,candidate.Header().GetHash(),metadata),Status::Ok);
    CBlockIndex child(candidate.Header(),4);child.pprev=&f.tip;child.chainwork=work.GetHex();child.status=metadata.status_flags;
    child.file_number=metadata.file_number;child.data_pos=metadata.data_pos;child.data_size=metadata.data_size;
    std::string error;bool invalid=false;
    EXPECT_FALSE(Access::ConnectBoundary(f.service,&child,error,invalid));EXPECT_FALSE(invalid);f.CheckUnpublished();
    auto notices=std::make_shared<BoundaryNotifications>(f.db,f.service,f.tip,child);f.service.setRuntimeBlockNotifications(notices);
    const auto point=f.replay->ProvenUtxos().begin()->first;
    const auto original=f.db.getCoin(point.txid.AsUint256(),point.vout);ASSERT_TRUE(original.ok());
    auto wrong=*original;++wrong.amount;
    ASSERT_EQ(f.db.putCoin(f.token,point.txid.AsUint256(),point.vout,wrong),Status::Ok);
    EXPECT_FALSE(Access::ConnectBoundary(f.service,&child,error,invalid));EXPECT_FALSE(invalid);
    EXPECT_EQ(error,"orchard-connect-boundary-history-unavailable");EXPECT_EQ(notices->prepared,0u);f.CheckUnpublished();
    ASSERT_EQ(f.db.putCoin(f.token,point.txid.AsUint256(),point.vout,*original),Status::Ok);
    notices->refuse=true;EXPECT_FALSE(Access::ConnectBoundary(f.service,&child,error,invalid));
    EXPECT_EQ(error,"orchard-connect-consumers-not-ready");EXPECT_EQ(notices->prepared,1u);EXPECT_EQ(notices->published,0u);f.CheckUnpublished();
    notices->refuse=false;ASSERT_TRUE(Access::ConnectBoundary(f.service,&child,error,invalid))<<error;
    EXPECT_FALSE(invalid);ASSERT_TRUE(f.db.getLegacyRetirementState().ok());EXPECT_EQ(f.db.getLegacyRetirementState()->record,*record);
    ASSERT_TRUE(f.db.getOrchardState().ok());EXPECT_EQ(f.db.getOrchardState()->height,4u);
    EXPECT_EQ(notices->published,1u);EXPECT_TRUE(notices->coherent);EXPECT_TRUE(Access::AuditBoundary(f.service));
    ASSERT_TRUE(Access::DisconnectBoundary(f.service,&child));EXPECT_EQ(notices->published,2u);EXPECT_TRUE(notices->coherent);
    f.CheckUnpublished();EXPECT_TRUE(f.Read());
    f.db.close();ASSERT_EQ(f.db.init(f.path),Status::Ok);
    ASSERT_TRUE(Access::ConnectBoundary(f.service,&child,error,invalid))<<error;
    EXPECT_EQ(notices->published,3u);EXPECT_TRUE(notices->coherent);EXPECT_TRUE(Access::AuditBoundary(f.service));
}
#else
TEST(OrchardFirstBoundary, UnavailableWithoutBackend) {
    const auto previous=Params();struct Restore{ChainParams p;~Restore(){MutableParams()=p;}}restore{previous};
    MutableParams().orchard_activation_height=1;MutableParams().orchard_branch_id=1;
    auto name=(std::filesystem::temp_directory_path()/"first_boundary_off_XXXXXX").string();
    ASSERT_NE(mkdtemp(name.data()),nullptr);
    struct RemoveFixture {std::string path;~RemoveFixture(){std::error_code ec;std::filesystem::remove_all(path,ec);}} cleanup{name};
    ChainDB db;ASSERT_EQ(db.init(name),Status::Ok);
    ChainstateService service;service.setChainDB(&db);
    CBlockIndex next;next.height=1;std::string error;bool invalid=false;
    EXPECT_FALSE(ShieldedStateStartupTestAccess::ConnectBoundary(service,&next,error,invalid));
    EXPECT_EQ(error,"orchard-connect-runtime-unavailable");
}
#endif
