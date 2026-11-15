#pragma once
namespace dinero::consensus {
struct ReplayCoinRollbackTestAccess {
    static void Enable(ConsensusUTXOSet& set) { set.replay_rollback_enabled_ = true; }
    static void EnableRetiring(ConsensusUTXOSet& set) {
        set.replay_rollback_enabled_ = true;
        set.replay_failure_policy_ = ReplayFailurePolicy::RetireOwner;
    }
};
}
namespace dinero {
namespace {
OutPoint JournalPoint(uint32_t index) {
    uint256 txid; txid.data[0]=0xa7;
    for (unsigned i=0;i<4;++i) txid.data[1+i]=uint8_t(index>>(8*i));
    return OutPoint(TxId(txid),index);
}
consensus::UTXOEntry JournalCoin(uint32_t value) {
    return consensus::UTXOEntry(AmountUna::Una(value+1),{0x51},value,false);
}
void ExpectJournalState(const consensus::ConsensusUTXOSet& set,
                        const consensus::UTXOSnapshot& expected) {
    EXPECT_EQ(set.GetSetSize(),expected.utxos.size());
    EXPECT_EQ(consensus::ComputeUtxoRecordsDigest(set.GetUTXOs()),
              consensus::ComputeUtxoRecordsDigest(expected.utxos));
    EXPECT_EQ(set.GetForest().serialize(),expected.utreexo_forest_state);
    EXPECT_EQ(set.GetHeight(),expected.height);
    EXPECT_EQ(set.GetBestBlock(),expected.block_hash);
}
class JournalFaultSet : public consensus::ConsensusUTXOSet {
public:
    bool deny_second=false,throw_after_forest=false;
    size_t adds=0,replaces=0;
    bool AddCoin(const OutPoint& point,const consensus::UTXOEntry& coin) override {
        ++adds;
        if(deny_second && adds==2) return false;
        return ConsensusUTXOSet::AddCoin(point,coin);
    }
    void ReplaceForestGuarded(consensus::UtreexoForest next) override {
        ++replaces;
        ConsensusUTXOSet::ReplaceForestGuarded(std::move(next));
        if(throw_after_forest) throw std::runtime_error("injected post-forest failure");
    }
};
}
TEST(ReplayCoinRollback, SavesTouchedCoinsAndRestoresExactForestAndMetadata) {
    auto set=consensus::ConsensusUTXOSet::CreateForReplay();
    for(uint32_t i=0;i<20000;++i) ASSERT_TRUE(set->AddCoin(JournalPoint(i),JournalCoin(i)));
    consensus::UtreexoForest forest;
    ASSERT_NE(forest.add(consensus::UtreexoHash(32,0x41)),UINT64_MAX);
    ASSERT_NE(forest.add(consensus::UtreexoHash(32,0x42)),UINT64_MAX);
    ASSERT_TRUE(forest.removeAtKnownPosition(0,consensus::UtreexoHash(32,0x41)));
    set->ReplaceForestGuarded(std::move(forest));
    set->SetBestBlock(JournalPoint(7).txid.AsUint256(),7);
    const auto before=set->Snapshot();
    {
        auto rollback=set->BeginBlockConnectRollback();ASSERT_NE(rollback,nullptr);
        EXPECT_EQ(rollback->TouchedCoinCount(),0u);
        ASSERT_TRUE(set->SpendCoin(JournalPoint(1)));
        ASSERT_TRUE(set->AddCoin(JournalPoint(1),JournalCoin(900)));
        ASSERT_TRUE(set->DeleteCoin(JournalPoint(2)));
        ASSERT_TRUE(set->AddCoin(JournalPoint(30000),JournalCoin(30000)));
        ASSERT_TRUE(set->SpendCoin(JournalPoint(30000)));
        EXPECT_EQ(rollback->TouchedCoinCount(),3u);
        set->MutateForestGuarded([](consensus::UtreexoForest& f){
            f.setCanonicalEmptyRoots(true);f.rebuildRoots();
            if(f.add(consensus::UtreexoHash(32,0x43))==UINT64_MAX)
                throw std::runtime_error("fixture forest append");
        });
        set->SetBestBlock(JournalPoint(8).txid.AsUint256(),8);
    }
    ExpectJournalState(*set,before);
    // Ordinary instances retain their complete, independently restorable snapshot.
    consensus::ConsensusUTXOSet ordinary;ASSERT_TRUE(ordinary.AddCoin(JournalPoint(1),JournalCoin(1)));
    EXPECT_EQ(ordinary.BeginBlockConnectRollback(),nullptr);
    EXPECT_EQ(ordinary.Snapshot().utxos.size(),1u);
}
TEST(ReplayCoinRollback, CommitKeepsEffectsAndRejectsNestedOrBulkReplacement) {
    auto set=consensus::ConsensusUTXOSet::CreateForReplay();
    ASSERT_TRUE(set->AddCoin(JournalPoint(1),JournalCoin(1)));const auto before=set->Snapshot();
    {
        auto rollback=set->BeginBlockConnectRollback();ASSERT_NE(rollback,nullptr);
        EXPECT_THROW(set->BeginBlockConnectRollback(),std::logic_error);
        EXPECT_THROW(set->Snapshot(),std::logic_error);
        EXPECT_THROW(set->Restore(before),std::logic_error);
        EXPECT_THROW(set->Clear(),std::logic_error);
        EXPECT_THROW(set->BulkLoad(before.utxos,0,uint256()),std::logic_error);
        Block block;consensus::BlockUndo undo;consensus::UtreexoHash root;std::string error;
        EXPECT_THROW(set->ApplyBlock(block,1,uint256(),undo,root,error),std::logic_error);
        EXPECT_THROW(set->UndoBlock(block,1,undo,error),std::logic_error);
        ASSERT_TRUE(set->SpendCoin(JournalPoint(1)));
        ASSERT_TRUE(set->AddCoin(JournalPoint(2),JournalCoin(2)));
        set->SetBestBlock(JournalPoint(2).txid.AsUint256(),2);
        EXPECT_EQ(rollback->TouchedCoinCount(),2u);
        rollback->Commit();rollback->Commit();
    }
    EXPECT_FALSE(set->HaveCoin(JournalPoint(1)));EXPECT_TRUE(set->HaveCoin(JournalPoint(2)));
    EXPECT_EQ(set->GetHeight(),2u);
    const auto committed=set->Snapshot();
    {auto next=set->BeginBlockConnectRollback();ASSERT_NE(next,nullptr);ASSERT_TRUE(set->DeleteCoin(JournalPoint(2)));}
    ExpectJournalState(*set,committed);
}
TEST(ReplayCoinRollback, ActualValidatorLateFailuresRestoreThenRetryMatchesOrdinary) {
    auto blocks=BuildDeterministicChain(1);ASSERT_EQ(blocks.size(),1u);
    auto block=blocks.front();auto& coinbase=block.vtx.front();
    const auto amount=coinbase.vout.front().value.GetUna();
    coinbase.vout.front().value=AmountUna::Una(amount/2);
    auto output=coinbase.vout.front();output.value=AmountUna::Una(amount-amount/2);
    output.scriptPubKey.back()=0x73;coinbase.vout.push_back(output);
    block.header.merkle_root=consensus::ComputeMerkleRoot(block.vtx);
    consensus::ConsensusUTXOSet ordinary;SeedDirect(ordinary);
    consensus::BlockValidator oracle(&ordinary);std::string error;
    uint256 computed_root;
    ASSERT_TRUE(oracle.ComputeUtreexoRootPure(block,1,computed_root,error))<<error;
    block.header.utreexo_root=computed_root;
    consensus::BlockUndo expected_undo;
    ASSERT_TRUE(oracle.ConnectBlock(block,1,block.GetHash(),expected_undo,error))<<error;
    ASSERT_TRUE(expected_undo.pre_block_snapshot.has_value());
    const auto expected=ordinary.Snapshot();
    for(bool forest_failure:{false,true}) {
        JournalFaultSet set;consensus::ReplayCoinRollbackTestAccess::Enable(set);SeedDirect(set);
        const auto before=set.Snapshot();set.adds=0;
        set.deny_second=!forest_failure;set.throw_after_forest=forest_failure;
        consensus::BlockValidator validator(&set);consensus::BlockUndo undo;
        if(forest_failure) {
            EXPECT_THROW(validator.ConnectBlock(block,1,block.GetHash(),undo,error),std::runtime_error);
            EXPECT_EQ(set.replaces,1u);
        } else {
            EXPECT_FALSE(validator.ConnectBlock(block,1,block.GetHash(),undo,error));
            EXPECT_EQ(set.adds,2u);EXPECT_EQ(set.replaces,0u);
            EXPECT_NE(error.find("Failed to commit UTXO"),std::string::npos)<<error;
        }
        ExpectJournalState(set,before);
        set.deny_second=false;set.throw_after_forest=false;set.adds=0;
        ASSERT_TRUE(validator.ConnectBlock(block,1,block.GetHash(),undo,error))<<error;
        EXPECT_FALSE(undo.pre_block_snapshot.has_value());ExpectJournalState(set,expected);
    }
}
TEST(ReplayCoinRollback, ActualReplayPreservesEveryPrefixAndUndoTail) {
    const auto blocks=BuildDeterministicChain(20);ASSERT_EQ(blocks.size(),20u);
    assumeutxo::AssumeUtxoReplayEngine engine;std::string error;
    ASSERT_TRUE(engine.SeedGenesis(SelectedGenesis(),error));engine.SetUndoTailWindow(4);
    consensus::ConsensusUTXOSet direct;SeedDirect(direct);
    // The reference owns the SAME independent shielded state as the replay
    // engine. Undo serialization includes its frontier/anchor observations even
    // for this transparent-only chain. Keep the full serialized equality below.
    consensus::shielded::CommitmentTree direct_tree;
    consensus::shielded::NullifierSet direct_nullifiers;
    consensus::shielded::AnchorHistory direct_anchors;
    ASSERT_EQ(direct_nullifiers.Open(":memory:"),
              consensus::shielded::NullifierSet::OpenResult::Ok);
    consensus::BlockValidator validator(&direct);
    validator.setShieldedState(&direct_tree,&direct_nullifiers,&direct_anchors);
    validator.setValidationMode(consensus::ValidationMode::STATEFUL);
    ASSERT_EQ(engine.ShieldedTree()->SerializeFrontier(),direct_tree.SerializeFrontier());
    ASSERT_EQ(engine.ShieldedAnchors()->SerializeBytes(),direct_anchors.SerializeBytes());
    for(uint32_t h=1;h<=blocks.size();++h) {
        const auto& block=blocks[h-1];
        if(h==5) {
            auto bad=block;bad.vtx.front().vout.front().value=AmountUna::Una(1);
            bad.header.merkle_root=consensus::ComputeMerkleRoot(bad.vtx);
            const auto digest=engine.RecordsDigestHex(),root=engine.UtreexoRootHex();
            EXPECT_FALSE(engine.ConnectAndAdvance(bad,h,bad.GetHash(),error));
            EXPECT_EQ(engine.RecordsDigestHex(),digest);EXPECT_EQ(engine.UtreexoRootHex(),root);
            EXPECT_EQ(engine.Height(),h-1);
        }
        consensus::BlockUndo undo;
        ASSERT_TRUE(validator.ConnectBlock(block,h,block.GetHash(),undo,error))<<error;
        ASSERT_TRUE(engine.ConnectAndAdvance(block,h,block.GetHash(),error))<<error;
        EXPECT_EQ(engine.RecordsDigestHex(),consensus::ComputeUtxoRecordsDigest(direct.GetUTXOs()).GetHex());
        EXPECT_EQ(engine.Forest()->serialize(),direct.GetForest().serialize());
        ASSERT_FALSE(engine.UndoTail().empty());EXPECT_FALSE(engine.UndoTail().back().undo.pre_block_snapshot.has_value());
        EXPECT_EQ(engine.UndoTail().back().height,h);
        EXPECT_EQ(engine.UndoTail().back().undo.pre_block_shielded_frontier,undo.pre_block_shielded_frontier);
        EXPECT_EQ(engine.UndoTail().back().undo.pre_block_shielded_anchors,undo.pre_block_shielded_anchors);
        EXPECT_EQ(engine.UndoTail().back().undo.Serialize(),undo.Serialize());
        EXPECT_EQ(engine.UndoTail().size(),std::min<size_t>(4,h));
    }
}
namespace {
// Coinbase-only chain; block h pays the subsidy to widths[h-1] distinct outputs,
// so the forest grows by that many leaves.
std::vector<Block> BuildCoinbaseChain(const std::vector<uint32_t>& widths) {
    const uint32_t n=uint32_t(widths.size());
    std::vector<Block> chain;consensus::ConsensusUTXOSet set;consensus::BlockValidator validator(&set);
    SeedDirect(set);uint256 prev=SelectedGenesis().GetHash();
    for(uint32_t h=1;h<=n;++h) {
        Block b;b.header.version=1;b.header.prev_block_hash=prev;
        b.header.timestamp=SelectedGenesis().header.timestamp+h*120;
        b.header.difficulty=0x1d00ffff;b.header.nonce=0;b.header.ZeroReserved();
        const uint32_t outputs=widths[h-1];
        auto coinbase=MakeCoinbase(h);const auto template_out=coinbase.vout.front();
        const auto total=template_out.value.GetUna();coinbase.vout.clear();
        for(uint32_t i=0;i<outputs;++i) {
            auto out=template_out;
            out.value=AmountUna::Una(total/outputs+(i+1==outputs?total%outputs:0));
            for(unsigned k=0;k<4;++k) out.scriptPubKey[4+k]=uint8_t(i>>(8*k));
            coinbase.vout.push_back(out);
        }
        b.vtx.push_back(coinbase);b.header.merkle_root=consensus::ComputeMerkleRoot(b.vtx);
        uint256 root;std::string err;
        if(!validator.ComputeUtreexoRootPure(b,h,root,err)){ADD_FAILURE()<<err;break;}
        b.header.utreexo_root=root;consensus::BlockUndo undo;
        if(!validator.ConnectBlock(b,h,b.GetHash(),undo,err)){ADD_FAILURE()<<err;break;}
        prev=b.GetHash();chain.push_back(std::move(b));
    }
    return chain;
}
}
TEST(ReplayCoinRollback, RetireOwnerCommitsInPlaceAndRetiresOnUncommittedBlock) {
    auto set=consensus::ConsensusUTXOSet::CreateForReplay(consensus::ReplayFailurePolicy::RetireOwner);
    ASSERT_TRUE(set->AddCoin(JournalPoint(1),JournalCoin(1)));
    consensus::UtreexoForest expected_forest;
    ASSERT_NE(expected_forest.add(consensus::UtreexoHash(32,0x44)),UINT64_MAX);
    {
        auto rollback=set->BeginBlockConnectRollback();ASSERT_NE(rollback,nullptr);
        // The same lifecycle guards as the restoring owner while a block is open.
        EXPECT_THROW(set->BeginBlockConnectRollback(),std::logic_error);
        EXPECT_THROW(set->Snapshot(),std::logic_error);
        EXPECT_THROW(set->Clear(),std::logic_error);
        ASSERT_TRUE(set->SpendCoin(JournalPoint(1)));
        ASSERT_TRUE(set->AddCoin(JournalPoint(2),JournalCoin(2)));
        EXPECT_EQ(rollback->TouchedCoinCount(),0u);  // no coin journal
        auto live=rollback->TakeForestForInPlaceConnect();ASSERT_TRUE(live.has_value());
        EXPECT_THROW(rollback->TakeForestForInPlaceConnect(),std::logic_error);
        ASSERT_NE(live->add(consensus::UtreexoHash(32,0x44)),UINT64_MAX);
        set->ReplaceForestGuarded(std::move(*live));
        rollback->Commit();rollback->Commit();
    }
    EXPECT_FALSE(set->ReplayRetired());
    EXPECT_FALSE(set->HaveCoin(JournalPoint(1)));EXPECT_TRUE(set->HaveCoin(JournalPoint(2)));
    EXPECT_EQ(set->GetForest().serialize(),expected_forest.serialize());
    EXPECT_EQ(set->Snapshot().utxos.size(),1u);
    // An uncommitted block is never restored; it retires the owner for good.
    {auto rollback=set->BeginBlockConnectRollback();ASSERT_NE(rollback,nullptr);
     ASSERT_TRUE(set->DeleteCoin(JournalPoint(2)));}
    EXPECT_TRUE(set->ReplayRetired());
    EXPECT_FALSE(set->HaveCoin(JournalPoint(2)));
    EXPECT_THROW(set->BeginBlockConnectRollback(),std::logic_error);
    EXPECT_THROW(set->Snapshot(),std::logic_error);
    EXPECT_THROW(set->Restore(consensus::UTXOSnapshot{}),std::logic_error);
    EXPECT_THROW(set->BulkLoad({},0,uint256()),std::logic_error);
    // The default policy and ordinary instances are unchanged.
    auto restoring=consensus::ConsensusUTXOSet::CreateForReplay();
    {auto rollback=restoring->BeginBlockConnectRollback();ASSERT_NE(rollback,nullptr);
     EXPECT_FALSE(rollback->TakeForestForInPlaceConnect().has_value());}
    EXPECT_FALSE(restoring->ReplayRetired());
}
TEST(ReplayCoinRollback, RetireOwnerLateValidatorFailureRetiresAfterForestTransfer) {
    auto blocks=BuildDeterministicChain(1);ASSERT_EQ(blocks.size(),1u);
    auto block=blocks.front();auto& coinbase=block.vtx.front();
    const auto amount=coinbase.vout.front().value.GetUna();
    coinbase.vout.front().value=AmountUna::Una(amount/2);
    auto output=coinbase.vout.front();output.value=AmountUna::Una(amount-amount/2);
    output.scriptPubKey.back()=0x73;coinbase.vout.push_back(output);
    block.header.merkle_root=consensus::ComputeMerkleRoot(block.vtx);
    {consensus::ConsensusUTXOSet ordinary;SeedDirect(ordinary);consensus::BlockValidator oracle(&ordinary);
     uint256 root;std::string error;ASSERT_TRUE(oracle.ComputeUtreexoRootPure(block,1,root,error))<<error;
     block.header.utreexo_root=root;}
    // The second coin commit fails after the forest was transferred and mutated.
    JournalFaultSet set;consensus::ReplayCoinRollbackTestAccess::EnableRetiring(set);SeedDirect(set);
    set.adds=0;set.deny_second=true;
    consensus::BlockValidator validator(&set);consensus::BlockUndo undo;std::string error;
    EXPECT_FALSE(validator.ConnectBlock(block,1,block.GetHash(),undo,error));
    EXPECT_NE(error.find("Failed to commit UTXO"),std::string::npos)<<error;
    // Coin commits precede the forest commit: the transferred forest was never
    // replaced, so the owner holds partial state and must be retired.
    EXPECT_EQ(set.replaces,0u);
    EXPECT_FALSE(undo.pre_block_snapshot.has_value());
    EXPECT_TRUE(set.ReplayRetired());
    set.deny_second=false;
    EXPECT_THROW(validator.ConnectBlock(block,1,block.GetHash(),undo,error),std::logic_error);
}
TEST(ReplayCoinRollback, RetireOwnerReplayMatchesRestoringReplayAtEveryPrefix) {
    const auto blocks=BuildCoinbaseChain(std::vector<uint32_t>(30,64));ASSERT_EQ(blocks.size(),30u);
    assumeutxo::AssumeUtxoReplayEngine restoring;
    assumeutxo::AssumeUtxoReplayEngine retiring(consensus::ReplayFailurePolicy::RetireOwner);
    std::string error;
    ASSERT_TRUE(restoring.SeedGenesis(SelectedGenesis(),error))<<error;
    ASSERT_TRUE(retiring.SeedGenesis(SelectedGenesis(),error))<<error;
    restoring.SetUndoTailWindow(4);retiring.SetUndoTailWindow(4);
    for(uint32_t h=1;h<=blocks.size();++h) {
        const auto& block=blocks[h-1];
        ASSERT_TRUE(restoring.ConnectAndAdvance(block,h,block.GetHash(),error))<<error;
        ASSERT_TRUE(retiring.ConnectAndAdvance(block,h,block.GetHash(),error))<<error;
        EXPECT_EQ(retiring.Height(),restoring.Height());
        EXPECT_EQ(retiring.RecordsDigestHex(),restoring.RecordsDigestHex());
        EXPECT_EQ(retiring.UtreexoRootHex(),restoring.UtreexoRootHex());
        EXPECT_EQ(retiring.Forest()->serialize(),restoring.Forest()->serialize());
        ASSERT_EQ(retiring.UndoTail().size(),restoring.UndoTail().size());
        EXPECT_EQ(retiring.UndoTail().back().undo.Serialize(),restoring.UndoTail().back().undo.Serialize());
    }
}
TEST(ReplayCoinRollback, RetireOwnerReplayRetiresOnRejectedBlockAndFreshEngineRecovers) {
    const auto blocks=BuildDeterministicChain(12);ASSERT_EQ(blocks.size(),12u);
    assumeutxo::AssumeUtxoReplayEngine reference;std::string error;
    ASSERT_TRUE(reference.SeedGenesis(SelectedGenesis(),error))<<error;
    for(uint32_t h=1;h<=blocks.size();++h)
        ASSERT_TRUE(reference.ConnectAndAdvance(blocks[h-1],h,blocks[h-1].GetHash(),error))<<error;
    assumeutxo::AssumeUtxoReplayEngine engine(consensus::ReplayFailurePolicy::RetireOwner);
    ASSERT_TRUE(engine.SeedGenesis(SelectedGenesis(),error))<<error;
    for(uint32_t h=1;h<5;++h)
        ASSERT_TRUE(engine.ConnectAndAdvance(blocks[h-1],h,blocks[h-1].GetHash(),error))<<error;
    // A wrong committed root is detected only after the block's leaves were
    // added to the transferred forest: the engine then holds partial state.
    auto bad=blocks[4];bad.header.utreexo_root.data[0]^=0x5a;
    EXPECT_FALSE(engine.ConnectAndAdvance(bad,5,bad.GetHash(),error));
    // Retired: no caller may read a partially mutated prefix or continue it.
    EXPECT_THROW((void)engine.Forest(),std::exception);
    EXPECT_THROW((void)engine.ProvenUtxos(),std::exception);
    EXPECT_THROW((void)engine.UtxoCount(),std::exception);
    EXPECT_THROW((void)engine.RecordsDigestHex(),std::exception);
    EXPECT_THROW((void)engine.UtreexoRootHex(),std::exception);
    EXPECT_THROW((void)engine.ShieldedTree(),std::exception);
    EXPECT_THROW((void)engine.ShieldedNullifiers(),std::exception);
    EXPECT_THROW((void)engine.ShieldedAnchors(),std::exception);
    EXPECT_THROW((void)engine.CopyPrefixCoin(OutPoint()),std::exception);
    EXPECT_THROW((void)engine.Height(),std::exception);
    EXPECT_FALSE(engine.ConnectAndAdvance(blocks[4],5,blocks[4].GetHash(),error));
    // A fresh retiring engine replays the honest chain to the same result.
    assumeutxo::AssumeUtxoReplayEngine fresh(consensus::ReplayFailurePolicy::RetireOwner);
    ASSERT_TRUE(fresh.SeedGenesis(SelectedGenesis(),error))<<error;
    for(uint32_t h=1;h<=blocks.size();++h)
        ASSERT_TRUE(fresh.ConnectAndAdvance(blocks[h-1],h,blocks[h-1].GetHash(),error))<<error;
    EXPECT_EQ(fresh.RecordsDigestHex(),reference.RecordsDigestHex());
    EXPECT_EQ(fresh.UtreexoRootHex(),reference.UtreexoRootHex());
    EXPECT_EQ(fresh.Forest()->serialize(),reference.Forest()->serialize());
}
namespace {
class CountingRollback final : public consensus::BlockConnectRollback {
public:
    CountingRollback(std::unique_ptr<consensus::BlockConnectRollback> inner,size_t& takes)
        :inner_(std::move(inner)),takes_(takes){}
    void Commit() noexcept override {inner_->Commit();}
    size_t TouchedCoinCount() const noexcept override {return inner_->TouchedCoinCount();}
    std::optional<consensus::UtreexoForest> TakeForestForInPlaceConnect() override {
        auto forest=inner_->TakeForestForInPlaceConnect();if(forest)++takes_;return forest;
    }
private:
    std::unique_ptr<consensus::BlockConnectRollback> inner_;size_t& takes_;
};
class CountingReplaySet final : public consensus::ConsensusUTXOSet {
public:
    mutable size_t snapshots=0;size_t takes=0,rollbacks=0;
    consensus::UTXOSnapshot Snapshot() const override {++snapshots;return ConsensusUTXOSet::Snapshot();}
    std::unique_ptr<consensus::BlockConnectRollback> BeginBlockConnectRollback() override {
        auto inner=ConsensusUTXOSet::BeginBlockConnectRollback();if(!inner)return nullptr;
        ++rollbacks;return std::make_unique<CountingRollback>(std::move(inner),takes);
    }
};
}
TEST(ReplayCoinRollback, RetireOwnerTransfersForestOnceAndNeverSnapshotsPerBlock) {
    const auto blocks=BuildCoinbaseChain(std::vector<uint32_t>(20,64));ASSERT_EQ(blocks.size(),20u);
    consensus::ConsensusUTXOSet ordinary;SeedDirect(ordinary);consensus::BlockValidator oracle(&ordinary);
    for(bool retiring:{true,false}) {
        CountingReplaySet set;
        if(retiring) consensus::ReplayCoinRollbackTestAccess::EnableRetiring(set);
        else consensus::ReplayCoinRollbackTestAccess::Enable(set);
        SeedDirect(set);consensus::BlockValidator validator(&set);std::string error;
        for(uint32_t h=1;h<=blocks.size();++h) {
            consensus::BlockUndo undo;
            ASSERT_TRUE(validator.ConnectBlock(blocks[h-1],h,blocks[h-1].GetHash(),undo,error))<<error;
            EXPECT_FALSE(undo.pre_block_snapshot.has_value());
            EXPECT_EQ(set.rollbacks,h);
            // Retiring: one in-place forest transfer per block. Restoring: the
            // clone path, never a transfer.
            EXPECT_EQ(set.takes,retiring?h:0u);
            if(retiring){consensus::BlockUndo expected;
                ASSERT_TRUE(oracle.ConnectBlock(blocks[h-1],h,blocks[h-1].GetHash(),expected,error))<<error;}
        }
        EXPECT_EQ(set.snapshots,0u);  // no whole-set snapshot in either replay policy
        EXPECT_FALSE(set.ReplayRetired());
        EXPECT_EQ(consensus::ComputeUtxoRecordsDigest(set.GetUTXOs()),
                  consensus::ComputeUtxoRecordsDigest(ordinary.GetUTXOs()));
        EXPECT_EQ(set.GetForest().serialize(),ordinary.GetForest().serialize());
    }
}
TEST(ReplayCoinRollback, BenchmarkRetireOwnerReplayAgainstRestoringReplay) {
    // Wide blocks grow the forest quickly. The restoring path shares every
    // copy-on-write partition with a per-block clone, so its per-block cost
    // grows with the forest; the retiring path writes in place.
    // Wide blocks build a large forest. The timed blocks each write leaves
    // into most of the 256 partitions, as a busy real block does.
    std::vector<uint32_t> widths(60,5000);widths.resize(60+100,300);
    const auto blocks=BuildCoinbaseChain(widths);ASSERT_EQ(blocks.size(),widths.size());
    constexpr uint32_t kTimedFrom=61;
    const auto run=[&](consensus::ReplayFailurePolicy policy,std::string& digest) {
        assumeutxo::AssumeUtxoReplayEngine engine(policy);std::string error;
        EXPECT_TRUE(engine.SeedGenesis(SelectedGenesis(),error))<<error;
        for(uint32_t h=1;h<kTimedFrom;++h)
            EXPECT_TRUE(engine.ConnectAndAdvance(blocks[h-1],h,blocks[h-1].GetHash(),error))<<error;
        const auto start=std::chrono::steady_clock::now();
        for(uint32_t h=kTimedFrom;h<=blocks.size();++h)
            EXPECT_TRUE(engine.ConnectAndAdvance(blocks[h-1],h,blocks[h-1].GetHash(),error))<<error;
        const auto elapsed=std::chrono::steady_clock::now()-start;
        digest=engine.UtreexoRootHex()+engine.RecordsDigestHex();
        return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    };
    std::string restoring_digest,retiring_digest;
    const auto restoring_ms=run(consensus::ReplayFailurePolicy::RestoreBlock,restoring_digest);
    const auto retiring_ms=run(consensus::ReplayFailurePolicy::RetireOwner,retiring_digest);
    EXPECT_EQ(retiring_digest,restoring_digest);
    // Measured benchmark only; the deterministic gate is ...TransfersForestOnce...
    std::cout<<"[ReplayCoinRollback] restoring="<<restoring_ms<<"ms retiring="<<retiring_ms<<"ms"<<std::endl;
}
} // namespace dinero
