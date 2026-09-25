#include "orchard_block_coin_test_fixture.h"
#include "consensus/orchard_block_coins.h"
#include "consensus/chainparams.h"
#include "consensus/script_interpreter.h"
#include "consensus/script_validation.h"
#include "consensus/subsidy.h"

using CoinError = OrchardBlockCoinErrorCode;
static_assert(!std::is_default_constructible_v<PreparedOrchardBlockCoins>);
static_assert(!std::is_copy_assignable_v<PreparedOrchardBlockCoins>);
template<class F> static void CoinReject(CoinError code, F fn) {
    bool rejected=false;
    try { fn(); } catch(const OrchardBlockCoinError& e) { Require(e.Code()==code);rejected=true; }
    Require(rejected);
}
static PreparedOrchardBlockCoins Prepare(OrchardBlockContext c,const ChainStateView& view,
    std::vector<Bytes> wires,uint64_t reward=1) {
    const auto block=CandidateWires(c,std::move(wires),0,reward);c.block_hash=block.Header().GetHash();
    return PrepareOrchardBlockCoinsUnderChainstateLock(block,c,view,{},true);
}
static void Mixed(const std::string& base) {
    Fixture keys(base);
    const auto auth=Authorized(base,false,20000);
    const auto& tx=auth.Transaction();
    View view;view.height=20000;
    for(size_t i=0;i<tx.Inputs().size();++i)view.coins.emplace(Point(tx.Inputs()[i]),auth.Transparent().Snapshot().Coins()[i]);
    OrchardBlockContext c{20001,H(2),H(1),20001,keys.domain};
    const auto id=ParsedTransaction::DecodeExact(auth.Orchard().CanonicalBytes(),TransactionReadMode::StagedOrchard).GetTxid();
    const OutPoint parent_output(id,0);
    UTXOEntry child_coin(AmountUna::Una(tx.Outputs()[0].amount_una),tx.Outputs()[0].script_pub_key,c.height,false);
    const auto child=Child(parent_output,child_coin,keys);
    const std::vector<Bytes> wires{auth.Orchard().CanonicalBytes(),Wire(child)};
    const auto plan=Prepare(c,view,wires);
    Require(plan.Transactions().size()==3 && plan.Authorizations().size()==1 && plan.TotalFees()==789);
    Require(plan.Transactions()[2].spent[0].first==parent_output);
    Require(plan.Transactions()[2].spent[0].second.height==c.height);
    Require(plan.Changes().size()==6);
    Require(std::none_of(plan.Changes().begin(),plan.Changes().end(),[&](const auto& change){return change.outpoint==parent_output;}));
    Require(view.coins.size()==2); // Preparation never changes its source.
    Require(ApplyOrchardValueFlows(0,{GetOrchardValueFlow(plan.Authorizations()[0].Transparent())}).value()==5000);
    const auto reward=ConsensusSubsidy::GetBlockSubsidy(c.height,Params().sixty_second_activation_height).GetUna()+plan.TotalFees();
    Require(Prepare(c,view,wires,reward).TotalFees()==789);
    CoinReject(CoinError::Reward,[&]{(void)Prepare(c,view,wires,reward+1);});
    CoinReject(CoinError::MissingCoin,[&]{(void)Prepare(c,view,{Wire(child),auth.Orchard().CanonicalBytes()});});
    auto bad=child;bad.vin[0].witness[0][0]^=1;
    CoinReject(CoinError::Script,[&]{(void)Prepare(c,view,{auth.Orchard().CanonicalBytes(),Wire(bad)});});
    const auto point=Point(tx.Inputs()[0]);
    const auto conflict=Child(point,view.coins.at(point),keys);
    CoinReject(CoinError::MissingCoin,[&]{(void)Prepare(c,view,{auth.Orchard().CanonicalBytes(),Wire(conflict)});});
    CoinReject(CoinError::MissingCoin,[&]{(void)Prepare(c,view,{Wire(conflict),auth.Orchard().CanonicalBytes()});});
    auto collision=view;collision.coins.emplace(parent_output,child_coin);
    CoinReject(CoinError::OutputCollision,[&]{(void)Prepare(c,collision,wires);});
    auto immature=view;immature.coins.at(point).height=c.height;immature.coins.at(point).isCoinbase=true;
    CoinReject(CoinError::ImmatureCoinbase,[&]{(void)Prepare(c,immature,{Wire(conflict)});});
    auto duplicate=conflict;duplicate.vin.push_back(duplicate.vin[0]);
    CoinReject(CoinError::DuplicateInput,[&]{(void)Prepare(c,view,{Wire(duplicate)});});
    auto missing=view;missing.coins.erase(point);
    CoinReject(CoinError::MissingCoin,[&]{(void)Prepare(c,missing,wires);});
    auto inactive=c;inactive.activation_height=UINT32_MAX;
    CoinReject(CoinError::Context,[&]{(void)Prepare(inactive,view,wires);});
    class FailingView final:public ChainStateView {
    public:
        StatusOr<UTXOEntry> getCoin(const OutPoint&)const override{return Status::Io;}
        bool hasCoin(const OutPoint&)const override{throw std::runtime_error("unexpected hasCoin probe");}
        uint32_t getHeight()const override{return 20000;}
    } failing;
    std::vector<Bytes> oversized;
    for(unsigned i=0;i<12;++i) {
        auto large=conflict;large.lockTime=i;large.vout.clear();
        for(unsigned j=0;j<10;++j)large.vout.emplace_back(AmountUna::Una(1),Bytes(9000,0x61));
        oversized.push_back(Wire(large));
    }
    // Size rejection precedes any coin lookup or expensive authorization.
    CoinReject(CoinError::Body,[&]{(void)Prepare(c,failing,oversized);});
    bool io=false;
    try{(void)Prepare(c,failing,wires);}catch(const OrchardCoinLookupError&e){io=e.SourceStatus()==Status::Io;}
    Require(io);
    // Ordinary-only blocks use the same actual fee comparator, with no pool flow.
    const auto plain=Prepare(c,view,{Wire(conflict)});
    Require(plain.Authorizations().empty() && plain.TotalFees()==123);
}
int main(int argc,char**argv) {
    try {Require(argc==2);SelectParams(Chain::REGTEST);Mixed(argv[1]);
        std::cout<<"Mixed block coins: real signatures, cross-family spending, child ordering, net undo, reward and I/O separation passed\n";
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
