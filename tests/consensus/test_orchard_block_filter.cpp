#include "orchard_forest_test_fixture.h"
#include "consensus/orchard_block_filter.h"
#include "consensus/chainparams.h"
#include <source_location>
template<class F> static void Reject(OrchardBlockCoinErrorCode code,F f) {
    bool rejected=false;try{f();}catch(const OrchardBlockCoinError& e){Require(e.Code()==code);rejected=true;}Require(rejected);
}
int main(int argc,char**argv) {
    try {
        Require(argc==2);SelectParams(Chain::REGTEST);Fixture f(argv[1]);
        const auto auth=Authorized(argv[1],false,20000);const auto& tx=auth.Transaction();
        View view;view.height=20000;
        for(size_t i=0;i<tx.Inputs().size();++i)
            view.coins.emplace(Point(tx.Inputs()[i]),auth.Transparent().Snapshot().Coins()[i]);
        OrchardBlockContext c{20001,H(2),H(1),20001,f.domain};
        const auto id=ParsedTransaction::DecodeExact(auth.Orchard().CanonicalBytes(),TransactionReadMode::StagedOrchard).GetTxid();
        const auto& out=tx.Outputs()[0];
        const auto child=Child(OutPoint(id,0),UTXOEntry(AmountUna::Una(out.amount_una),out.script_pub_key,c.height,false),f);
        const auto draft=CandidateWires(c,{auth.Orchard().CanonicalBytes(),Wire(child)});c.block_hash=draft.Header().GetHash();
        const auto before=PrepareOrchardBlockCoinsUnderChainstateLock(draft,c,view,{},true);
        // Independent explicit script list: coinbase, historical and Orchard
        // outputs, external spent inputs, and the same-block spent output.
        std::vector<Bytes> scripts{Bytes{0x51},out.script_pub_key,child.vout[0].scriptPubKey,out.script_pub_key};
        for(const auto& output:tx.Outputs())scripts.push_back(output.script_pub_key);
        for(const auto& [point,coin]:view.coins)scripts.push_back(coin.scriptPubKey);
        const auto expected=GCSFilter::Build(scripts,c.parent_hash);
        const auto actual=BuildOrchardBlockFilter(before);
        Require(actual.encoded_data==expected.encoded_data && actual.element_count==expected.element_count);
        for(const auto& script:scripts)Require(actual.Match(script));
        Reject(OrchardBlockCoinErrorCode::Filter,[&]{(void)CheckOrchardBlockFilter(draft,before);});
        const auto good=WithFilterHash(draft,expected.GetHash());c.block_hash=good.Header().GetHash();
        const auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(good,c,view,{},true);
        const auto checked=CheckOrchardBlockFilter(good,coins);
        Require(checked.encoded_data==expected.encoded_data && checked.element_count==expected.element_count);
        Reject(OrchardBlockCoinErrorCode::Context,[&]{(void)CheckOrchardBlockFilter(good,before);});
        const auto bad=WithFilterHash(draft,H(98));c.block_hash=bad.Header().GetHash();
        const auto bad_coins=PrepareOrchardBlockCoinsUnderChainstateLock(bad,c,view,{},true);
        Reject(OrchardBlockCoinErrorCode::Filter,[&]{(void)CheckOrchardBlockFilter(bad,bad_coins);});
        // Empty output script gives a genuinely empty filter. Zero hash is a
        // commitment value, never permission to skip the mandatory commitment.
        auto empty=CandidateWires(c,{});auto coinbase=empty.Transactions()[0].Historical();
        coinbase.vout[0].scriptPubKey.clear();
        Block legacy;legacy.header=empty.Header();legacy.vtx={coinbase};
        legacy.header.merkle_root=coinbase.GetTxid().AsUint256();
        const auto serialized=legacy.Serialize();
        const auto empty_draft=OrchardBlockCandidate::DecodeExact(Bytes(serialized.begin(),serialized.end()));c.block_hash=empty_draft.Header().GetHash();
        const auto empty_coins=PrepareOrchardBlockCoinsUnderChainstateLock(empty_draft,c,view,{},true);
        Require(BuildOrchardBlockFilter(empty_coins).IsEmpty());
        Reject(OrchardBlockCoinErrorCode::Filter,[&]{(void)CheckOrchardBlockFilter(empty_draft,empty_coins);});
        const auto empty_committed=WithFilterHash(empty_draft,uint256());c.block_hash=empty_committed.Header().GetHash();
        const auto final_coins=PrepareOrchardBlockCoinsUnderChainstateLock(empty_committed,c,view,{},true);
        Require(CheckOrchardBlockFilter(empty_committed,final_coins).IsEmpty());
        std::cout<<"PASS: mixed scripts, same-block inputs, exact candidate binding, missing/mismatched and empty filter commitments\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
