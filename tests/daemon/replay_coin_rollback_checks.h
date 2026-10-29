#pragma once
namespace dinero::consensus {
struct ReplayCoinRollbackTestAccess {
    static void Enable(ConsensusUTXOSet& set) { set.replay_rollback_enabled_ = true; }
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
} // namespace dinero
