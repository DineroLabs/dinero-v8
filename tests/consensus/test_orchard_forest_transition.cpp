#include "orchard_block_coin_test_fixture.h"
#include "consensus/orchard_forest_transition.h"
#include "consensus/chainparams.h"
#include "consensus/utreexo_delta_codec.h"

static uint256 RootHash(const UtreexoForest& forest) {
    const auto root=forest.getCommitment();Require(root.size()==32);
    uint256 result;std::copy(root.begin(),root.end(),result.begin());return result;
}
static UtreexoHash LeafHash(const OutPoint& point,const UTXOEntry& coin) {
    return HashUTXOForCreationHeight(point.txid.AsUint256(),point.vout,coin.value.GetUna(),
        coin.scriptPubKey,coin.height,coin.isCoinbase);
}
template<class F>static void ForestReject(OrchardForestErrorCode code,F fn) {
    bool rejected=false;
    try{fn();}catch(const OrchardForestError&e){Require(e.Code()==code);rejected=true;}
    Require(rejected);
}
static void RoundTrip(const std::string& base,size_t padding) {
    Fixture keys(base);const auto auth=Authorized(base,false,20000);const auto& tx=auth.Transaction();
    View view;view.height=20000;UtreexoForest forest;forest.setCanonicalEmptyRoots(true);
    std::vector<std::pair<OutPoint,UTXOEntry>> funding;
    for(size_t i=0;i<tx.Inputs().size();++i) {
        const auto point=Point(tx.Inputs()[i]);const auto coin=auth.Transparent().Snapshot().Coins()[i];
        view.coins.emplace(point,coin);funding.emplace_back(point,coin);
        Require(forest.add(LeafHash(point,coin))!=UINT64_MAX);
    }
    for(size_t i=0;i<padding;++i) {
        UtreexoHash unrelated(32,0);unrelated[0]=0xa0+i;
        Require(forest.add(unrelated)!=UINT64_MAX);
    }
    BlockHeader parent{};parent.version=1;parent.timestamp=20000;parent.utreexo_root=RootHash(forest);
    OrchardBlockContext c{20001,H(2),parent.GetHash(),20001,keys.domain};
    const auto id=ParsedTransaction::DecodeExact(auth.Orchard().CanonicalBytes(),TransactionReadMode::StagedOrchard).GetTxid();
    const OutPoint ephemeral(id,0);
    const UTXOEntry intermediate(AmountUna::Una(tx.Outputs()[0].amount_una),tx.Outputs()[0].script_pub_key,c.height,false);
    const auto child=Child(ephemeral,intermediate,keys);
    const auto candidate=CandidateWires(c,{auth.Orchard().CanonicalBytes(),Wire(child)});c.block_hash=candidate.Header().GetHash();
    const auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(candidate,c,view,{},true);
    const auto before=forest.dumpInternalState();
    const auto transition=PrepareOrchardForestTransition(coins,parent,forest);
    Require(forest.dumpInternalState()==before);
    Require(transition.Delta().deletedLeaves.size()==2 && transition.Delta().addedLeaves.size()==4);
    Require(transition.After().getNumLeaves()==padding+6 && transition.After().getActiveLeaves()==padding+4);
    Require(!transition.After().findLeafPosition(LeafHash(ephemeral,intermediate)));
    // Independent sequential oracle: exact known external deletions and the
    // four surviving outputs, never the transient parent output.
    auto expected=forest.cloneForHeight(c.height);
    for(const auto& [point,coin]:funding) {
        const auto hash=LeafHash(point,coin);const auto pos=expected.findLeafPosition(hash);Require(pos.has_value());
        Require(expected.removeAtKnownPosition(*pos,hash));
    }
    const auto& cb=candidate.Transactions()[0].Historical();
    for(size_t i=0;i<cb.vout.size();++i)
        Require(expected.add(LeafHash(OutPoint(cb.GetTxid(),i),UTXOEntry(cb.vout[i].value,cb.vout[i].scriptPubKey,c.height,true)))!=UINT64_MAX);
    Require(expected.add(LeafHash(OutPoint(id,1),UTXOEntry(AmountUna::Una(tx.Outputs()[1].amount_una),tx.Outputs()[1].script_pub_key,c.height,false)))!=UINT64_MAX);
    Require(expected.add(LeafHash(OutPoint(child.GetTxid(),0),UTXOEntry(child.vout[0].value,child.vout[0].scriptPubKey,c.height,false)))!=UINT64_MAX);
    Require(expected.dumpInternalState()==transition.After().dumpInternalState());
    std::string stored_delta,codec_error;UtreexoDelta decoded;
    Require(SerializeUtreexoDelta(transition.Delta(),stored_delta,codec_error));
    Require(DeserializeUtreexoDelta(stored_delta,decoded,codec_error));
    auto replayed=forest.cloneForHeight(c.height);
    Require(ApplyUtreexoDeltaForward(replayed,decoded,codec_error));
    Require(replayed.dumpInternalState()==transition.After().dumpInternalState());
    Require(!transition.MatchesHeader(candidate.Header())); // Draft has no computed root yet.
    auto header=candidate.Header();header.utreexo_root=transition.Root();
    auto bytes=candidate.WireBytes();const auto encoded=header.SerializeForHash();std::copy(encoded.begin(),encoded.end(),bytes.begin());
    const auto finalized=OrchardBlockCandidate::DecodeExact(bytes);c.block_hash=header.GetHash();
    const auto final_coins=PrepareOrchardBlockCoinsUnderChainstateLock(finalized,c,view,{},true);
    const auto final_transition=PrepareOrchardForestTransition(final_coins,parent,forest);
    Require(final_transition.MatchesHeader(header));
    auto wrong=header;wrong.utreexo_root.data[0]^=1;Require(!final_transition.MatchesHeader(wrong));
    // The persisted forest representation can be reopened before undo.
    const auto reopened=UtreexoForest::deserialize(final_transition.After().serialize());
    const auto restored=UndoOrchardForestTransition(reopened,final_transition);
    Require(restored.dumpInternalState()==before);
    ForestReject(OrchardForestErrorCode::Undo,[&]{(void)UndoOrchardForestTransition(forest,final_transition);});
    auto wrong_parent=parent;wrong_parent.nonce++;
    ForestReject(OrchardForestErrorCode::Context,[&]{(void)PrepareOrchardForestTransition(final_coins,wrong_parent,forest);});
    UtreexoForest absent;absent.setCanonicalEmptyRoots(true);
    ForestReject(OrchardForestErrorCode::ParentCommitment,[&]{(void)PrepareOrchardForestTransition(final_coins,parent,absent);});
    // A coin view claiming inputs outside an otherwise consistent forest is rejected.
    auto empty_parent=parent;empty_parent.utreexo_root=RootHash(absent);c.parent_hash=empty_parent.GetHash();
    const auto missing_candidate=CandidateWires(c,{auth.Orchard().CanonicalBytes(),Wire(child)});c.block_hash=missing_candidate.Header().GetHash();
    const auto missing_coins=PrepareOrchardBlockCoinsUnderChainstateLock(missing_candidate,c,view,{},true);
    ForestReject(OrchardForestErrorCode::MissingLeaf,[&]{(void)PrepareOrchardForestTransition(missing_coins,empty_parent,absent);});
}
int main(int argc,char**argv) {
    try{Require(argc==2);SelectParams(Chain::REGTEST);for(size_t n:{0,1,3,7})RoundTrip(argv[1],n);
        std::cout<<"Orchard forest: parent commitment, ordered mixed effects, ephemeral exclusion, exact delta undo and reopen passed\n";
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
