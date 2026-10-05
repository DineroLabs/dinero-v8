#include "orchard_forest_test_fixture.h"
#include "consensus/orchard_candidate_coin_view.h"
#include <iostream>

using Capture = OrchardCandidateCoinView;
using CaptureError = OrchardCandidateCoinErrorCode;
template<class F> static void CaptureReject(CaptureError code, F run) {
    bool rejected=false;
    try { run(); } catch(const OrchardCandidateCoinError& e) { Require(e.Code()==code);rejected=true; }
    Require(rejected);
}
template<class F> static void CoinLookupReject(Status status, F run) {
    bool rejected=false;
    try { run(); } catch(const OrchardCoinLookupError& e) { Require(e.SourceStatus()==status);rejected=true; }
    Require(rejected);
}
struct CandidateFixture {
    Fixture keys;
    VerifiedOrchardAuthorizations auth;
    View source;
    UtreexoForest forest;
    BlockHeader parent{};
    OrchardBlockContext context;
    Transaction child;
    std::optional<OrchardBlockCandidate> bare;
    BlockUtreexoData proof;
    explicit CandidateFixture(const std::string& base)
        :keys(base),auth(Authorized(base,false,20000)),
         context{20001,H(2),H(3),20001,keys.domain},bare(CandidateWires(context,{})) {
        source.height=20000;forest.setCanonicalEmptyRoots(true);
        for(size_t i=0;i<auth.Transaction().Inputs().size();++i) {
            const auto point=Point(auth.Transaction().Inputs()[i]);
            auto coin=auth.Transparent().Snapshot().Coins()[i];
            // Both pre-v2 metadata fields require the parent authority, even
            // though the actual accumulator membership remains valid without them.
            coin.height=5+i;coin.isCoinbase=i==0;
            source.coins.emplace(point,coin);
            Require(forest.add(HashUTXOForCreationHeight(point.txid.AsUint256(),point.vout,
                coin.value.GetUna(),coin.scriptPubKey,coin.height,coin.isCoinbase))!=UINT64_MAX);
        }
        parent.version=1;parent.timestamp=20000;
        Require(parent.IsReservedValid());
        const auto root=forest.getCommitment();Require(root.size()==32);
        std::copy(root.begin(),root.end(),parent.utreexo_root.begin());context.parent_hash=parent.GetHash();
        const auto id=ParsedTransaction::DecodeExact(auth.Orchard().CanonicalBytes(),TransactionReadMode::StagedOrchard).GetTxid();
        const auto& output=auth.Transaction().Outputs()[0];
        child=Child(OutPoint(id,0),UTXOEntry(AmountUna::Una(output.amount_una),output.script_pub_key,context.height,false),keys);
        bare.emplace(CandidateWires(context,{auth.Orchard().CanonicalBytes(),Wire(child)}));
        context.block_hash=bare->Header().GetHash();
        const auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(*bare,context,source,{},true);
        Require(coins.Authorizations().size()==1&&coins.Transactions().size()==3);
        proof=MixedProof(coins,forest);
        Require(proof.spend_proof.targets.size()==2&&proof.spent_outputs.size()==3);
    }
    Capture CaptureProof(const BlockUtreexoData& data) const {
        return Capture::Capture(WithProof(*bare,data),context,parent,UtreexoStump::fromForest(forest),source,
            [](const TxId&)->StatusOr<bool>{return false;});
    }
};
static void MixedFamilyAndIndependentLifetime(const std::string& base) {
    CandidateFixture f(base);
    const auto block=WithProof(*f.bare,f.proof);
    const auto captured=f.CaptureProof(f.proof);
    Require(captured.CapturedInputs()==2&&captured.ProvedOutputAbsences()==5);
    Require(captured.ParentHash()==f.parent.GetHash()&&captured.BlockHash()==block.Header().GetHash());
    const OutPoint unknown(TxId(H(99)),4);
    Require(captured.getCoin(unknown).status()==Status::Internal);
    CoinLookupReject(Status::Internal,[&]{(void)captured.hasCoin(unknown);});
    const OutPoint new_output(f.child.GetTxid(),0);
    Require(captured.getCoin(new_output).status()==Status::NotFound&&!captured.hasCoin(new_output));
    f.source.coins.clear();f.forest=UtreexoForest();
    const auto checked=PrepareOrchardBlockCoinsUnderChainstateLock(block,f.context,captured,{},true);
    Require(checked.Authorizations().size()==1&&checked.Transactions().size()==3);
    Require(checked.Transactions()[1].spent.size()==2&&checked.Transactions()[2].spent.size()==1);
    Require(checked.Transactions()[2].spent.front().second.height==20001);
    Require(checked.Changes().size()==6&&checked.TotalFees()>0);
    std::cout<<"CANDIDATE_COINS_PASS mixed_orchard_and_signed_child_after_parent_release\n";
}
static void ExactMetadataAndProof(const std::string& base) {
    CandidateFixture f(base);auto bad=f.proof;
    bad.spent_outputs.front().created_height=6;
    CaptureReject(CaptureError::Metadata,[&]{(void)f.CaptureProof(bad);});
    bad=f.proof;bad.spent_outputs.front().is_coinbase=false;
    CaptureReject(CaptureError::Metadata,[&]{(void)f.CaptureProof(bad);});
    bad=f.proof;bad.spent_outputs.back().created_height--;
    CaptureReject(CaptureError::Metadata,[&]{(void)f.CaptureProof(bad);});
    bad=f.proof;bad.spent_outputs.pop_back();
    CaptureReject(CaptureError::Metadata,[&]{(void)f.CaptureProof(bad);});
    bad=f.proof;bad.spend_proof.proof_hashes.push_back(UtreexoHash(32,0));
    CaptureReject(CaptureError::Proof,[&]{(void)f.CaptureProof(bad);});
    bad=f.proof;bad.spend_proof.proof_hashes.front()[0]^=1;
    CaptureReject(CaptureError::Proof,[&]{(void)f.CaptureProof(bad);});
    (void)f.CaptureProof(f.proof);
    std::cout<<"CANDIDATE_COINS_PASS legacy_and_ephemeral_metadata_exact_proof\n";
}
static void SourceErrorsAndHistoricalCollision(const std::string& base) {
    CandidateFixture f(base);const auto block=WithProof(*f.bare,f.proof);const auto stump=UtreexoStump::fromForest(f.forest);
    CaptureReject(CaptureError::HistoricalTransaction,[&]{(void)Capture::Capture(block,f.context,f.parent,stump,f.source,
        [](const TxId&)->StatusOr<bool>{return true;});});
    CoinLookupReject(Status::Corruption,[&]{(void)Capture::Capture(block,f.context,f.parent,stump,f.source,
        [](const TxId&)->StatusOr<bool>{return Status::Corruption;});});
    struct Unavailable final:ChainStateView {
        StatusOr<UTXOEntry> getCoin(const OutPoint&) const override{return Status::Internal;}
        bool hasCoin(const OutPoint&) const override{throw std::runtime_error("unexpected separate probe");}
        uint32_t getHeight() const override{return 20000;}
    } missing;
    CoinLookupReject(Status::Internal,[&]{(void)Capture::Capture(block,f.context,f.parent,stump,missing,
        [](const TxId&)->StatusOr<bool>{return false;});});
    const auto before=f.source.coins.size();(void)f.CaptureProof(f.proof);Require(f.source.coins.size()==before);
    // Complete membership and coin sources must agree. Preserve the original
    // full-view validator's refusal if a source claims an existing output for
    // a candidate whose transaction is supposedly absent.
    const OutPoint collision(f.child.GetTxid(),0);
    f.source.coins.emplace(collision,UTXOEntry(AmountUna::Una(123),Bytes{0x51},10,false));
    CaptureReject(CaptureError::OutputCollision,[&]{(void)f.CaptureProof(f.proof);});
    Require(f.source.coins.size()==before+1);f.source.coins.erase(collision);
    (void)f.CaptureProof(f.proof);
    std::cout<<"CANDIDATE_COINS_PASS unavailable_is_not_absence_and_historical_collision\n";
}
static void OrderingAndDomain(const std::string& base) {
    CandidateFixture f(base);
    auto reversed=CandidateWires(f.context,{Wire(f.child),f.auth.Orchard().CanonicalBytes()});
    auto context=f.context;context.block_hash=reversed.Header().GetHash();
    CaptureReject(CaptureError::InputOrder,[&]{(void)Capture::Capture(WithProof(reversed,f.proof),context,f.parent,
        UtreexoStump::fromForest(f.forest),f.source,[](const TxId&)->StatusOr<bool>{return false;});});
    const auto duplicate=CandidateWires(f.context,{f.auth.Orchard().CanonicalBytes(),f.auth.Orchard().CanonicalBytes()});
    context.block_hash=duplicate.Header().GetHash();
    CaptureReject(CaptureError::DuplicateTransaction,[&]{(void)Capture::Capture(WithProof(duplicate,f.proof),context,f.parent,
        UtreexoStump::fromForest(f.forest),f.source,[](const TxId&)->StatusOr<bool>{return false;});});
    const auto block=WithProof(*f.bare,f.proof);context=f.context;context.parent_hash=H(55);
    CaptureReject(CaptureError::Context,[&]{(void)Capture::Capture(block,context,f.parent,UtreexoStump::fromForest(f.forest),
        f.source,[](const TxId&)->StatusOr<bool>{return false;});});
    CaptureReject(CaptureError::Context,[&]{(void)Capture::Capture(block,f.context,f.parent,UtreexoStump(),
        f.source,[](const TxId&)->StatusOr<bool>{return false;});});
    std::cout<<"CANDIDATE_COINS_PASS forward_reference_duplicate_and_parent_binding\n";
}
static void EmptyExternalProof(const std::string& base) {
    CandidateFixture f(base);const auto bare=CandidateWires(f.context,{});auto context=f.context;context.block_hash=bare.Header().GetHash();
    const auto checked=PrepareOrchardBlockCoinsUnderChainstateLock(bare,context,f.source,{},true);
    const auto proof=MixedProof(checked,f.forest);
    const auto block=WithProof(bare,proof);const auto captured=Capture::Capture(block,context,f.parent,
        UtreexoStump::fromForest(f.forest),f.source,[](const TxId&)->StatusOr<bool>{return false;});
    Require(captured.CapturedInputs()==0&&captured.ProvedOutputAbsences()==2);
    const auto result=PrepareOrchardBlockCoinsUnderChainstateLock(block,context,captured,{},true);
    Require(result.Transactions().size()==1&&result.TotalFees()==0);
    std::cout<<"CANDIDATE_COINS_PASS coinbase_only_exact_empty_proof\n";
}
int main(int argc,char** argv) {
    try {
        Require(argc==2);SelectParams(Chain::REGTEST);
        MixedFamilyAndIndependentLifetime(argv[1]);ExactMetadataAndProof(argv[1]);
        SourceErrorsAndHistoricalCollision(argv[1]);OrderingAndDomain(argv[1]);EmptyExternalProof(argv[1]);
    } catch(const OrchardCandidateCoinError& e) {
        std::cerr<<"candidate coin view capture category="<<static_cast<int>(e.Code())<<": "<<e.what()<<'\n';return 1;
    } catch(const std::exception& e) {std::cerr<<"candidate coin view: "<<e.what()<<'\n';return 1;}
}
