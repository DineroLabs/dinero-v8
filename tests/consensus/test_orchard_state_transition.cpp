#include "orchard_state_test_fixture.h"
int main(int argc,char**argv) {
    try {
        Require(argc==2); const std::string base=argv[1];
        const auto shield=Authorized(base,false,20000);
        const auto spend=Authorized(base,true,20001);
        const std::vector<VerifiedOrchardAuthorizations> shields{shield}, spends{spend};
        OrchardBlockContext c{20001,H(2),H(1),20001,Fixture(base).domain};
        unsigned anchor_reads=0,nf_reads=0;
        OrchardStateLookups view{
            [&](const uint256&)->StatusOr<bool>{++anchor_reads;return false;},
            [&](const uint256&)->StatusOr<bool>{++nf_reads;return false;}};
        const auto funded=PrepareOrchardStateTransition(c,std::nullopt,shields,view);
        Require(!funded.Parent() && funded.Next().pool_balance==5000 && funded.Next().tree_size==2);
        Require(funded.Nullifiers().size()==2 && funded.Flows().size()==1 && funded.Fees()==666);
        Require(anchor_reads==0 && nf_reads==2);
        const auto expected_anchor=Load(base+"/combined-spend.anchor");
        Require(std::equal(expected_anchor.begin(),expected_anchor.end(),funded.Next().anchor.begin()));
        auto next=c;next.height++;next.parent_hash=c.block_hash;next.block_hash=H(3);
        view.active_anchor=[&](const uint256& a)->StatusOr<bool>{return a==funded.Next().anchor;};
        const auto paid=PrepareOrchardStateTransition(next,funded.Next(),spends,view);
        Require(paid.Next().pool_balance==4500 && paid.Next().tree_size==4 && paid.Fees()==666);
        Require(paid.Parent()==funded.Next());
        const auto untouched=PrepareOrchardStateTransition(next,funded.Next(),{},view);
        Require(untouched.Next().pool_balance==5000 && untouched.Next().frontier==funded.Next().frontier);
        Require(untouched.Next().anchor==funded.Next().anchor && untouched.Fees()==0);
        auto disabled=c;disabled.activation_height=UINT32_MAX;
        StateReject(StateError::Inactive,[&]{(void)PrepareOrchardStateTransition(disabled,{},shields,view);});
        StateReject(StateError::ParentState,[&]{(void)PrepareOrchardStateTransition(next,{},spends,view);});
        StateReject(StateError::ParentState,[&]{(void)PrepareOrchardStateTransition(c,funded.Next(),shields,view);});
        for (int kind=0;kind<5;++kind) {
            auto bad=funded.Next();
            if(kind==0) bad.anchor=H(99); if(kind==1) bad.tree_size++;
            if(kind==2) bad.frontier.push_back(0); if(kind==3) bad.block_hash=H(99);
            if(kind==4) bad.height--;
            StateReject(StateError::ParentState,[&]{(void)PrepareOrchardStateTransition(next,bad,spends,view);});
        }
        auto poor=funded.Next();poor.pool_balance=499;
        StateReject(StateError::PoolBalance,[&]{(void)PrepareOrchardStateTransition(next,poor,spends,view);});
        const std::vector<VerifiedOrchardAuthorizations> repeated{shield,shield};
        StateReject(StateError::DuplicateTransaction,[&]{(void)PrepareOrchardStateTransition(c,{},repeated,view);});
        StateReject(StateError::Context,[&]{(void)PrepareOrchardStateTransition(next,funded.Next(),shields,view);});
        auto wrong_domain=c;wrong_domain.domain.network_code=1;
        StateReject(StateError::Context,[&]{(void)PrepareOrchardStateTransition(wrong_domain,{},shields,view);});
        view.active_anchor=[](const uint256&)->StatusOr<bool>{return false;};
        StateReject(StateError::Anchor,[&]{(void)PrepareOrchardStateTransition(next,funded.Next(),spends,view);});
        view.active_anchor=[](const uint256&)->StatusOr<bool>{return Status::Io;};
        LookupReject(Status::Io,[&]{(void)PrepareOrchardStateTransition(next,funded.Next(),spends,view);});
        view.spent_nullifier=[](const uint256&)->StatusOr<bool>{return true;};
        StateReject(StateError::SpentNullifier,[&]{(void)PrepareOrchardStateTransition(c,{},shields,view);});
        view.spent_nullifier=[](const uint256&)->StatusOr<bool>{return Status::Corruption;};
        LookupReject(Status::Corruption,[&]{(void)PrepareOrchardStateTransition(c,{},shields,view);});
        view.spent_nullifier={};
        LookupReject(Status::Internal,[&]{(void)PrepareOrchardStateTransition(c,{},shields,view);});
        Require(funded.Next().pool_balance==5000 && funded.Next().tree_size==2);
        std::cout<<"Orchard state transition: honest funding/spend, parent/tree binding, anchors, nullifiers, activation, pool and lookup failure separation passed\n";
    } catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
