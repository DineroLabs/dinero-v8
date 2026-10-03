#include "orchard_state_test_fixture.h"

static_assert(!std::is_default_constructible_v<CheckedOrchardTransactions>);
static_assert(!std::is_copy_assignable_v<CheckedOrchardTransactions>);

static void MatchesBlockPreparation(const std::string& base) {
    const OrchardTransactionContext context{20001,H(1),20001,Fixture(base).domain};
    const std::vector<VerifiedOrchardAuthorizations> shield{Authorized(base,false,20000)};
    OrchardStateLookups view{[](const uint256&)->StatusOr<bool>{return false;},
                            [](const uint256&)->StatusOr<bool>{return false;}};
    const auto checked=CheckOrchardTransactions(context,{},shield,view);
    const auto block=PrepareOrchardStateTransition(
        {context.height,H(2),context.parent_hash,context.activation_height,context.domain},{},shield,view);
    Require(!checked.Parent() && checked.PoolBalance()==5000 && checked.TreeSize()==2);
    Require(checked.Anchor()==block.Next().anchor && checked.Frontier()==block.Next().frontier);
    Require(checked.Nullifiers()==block.Nullifiers() && checked.Fees()==block.Fees());
    Require(checked.Flows().size()==1 && checked.Flows()[0].fee==666);
    const std::vector<VerifiedOrchardAuthorizations> spends{Authorized(base,true,20001)};
    view.active_anchor=[&](const uint256& a)->StatusOr<bool>{return a==checked.Anchor();};
    const OrchardTransactionContext next{20002,H(2),20001,context.domain};
    const auto paid=CheckOrchardTransactions(next,block.Next(),spends,view);
    const auto paid_block=PrepareOrchardStateTransition({20002,H(3),H(2),20001,context.domain},block.Next(),spends,view);
    Require(paid.Parent()==block.Next() && paid.PoolBalance()==4500 && paid.TreeSize()==4);
    Require(paid.Anchor()==paid_block.Next().anchor && paid.Frontier()==paid_block.Next().frontier);
    Require(paid.Nullifiers()==paid_block.Nullifiers() && paid.Fees()==paid_block.Fees());
    // Checking a sequence grants no block identity and does not relax the real
    // block preparation gate, even after this same sequence was checked.
    StateReject(StateError::Context,[&]{(void)PrepareOrchardStateTransition(
        {context.height,{},context.parent_hash,context.activation_height,context.domain},{},shield,view);});
    StateReject(StateError::Context,[&]{(void)PrepareOrchardStateTransition(
        {context.height,H(1),context.parent_hash,context.activation_height,context.domain},{},shield,view);});
    std::cout<<"PASS MatchesBlockPreparation\n";
}

static void SelectedParentRulesRemainRequired(const std::string& base) {
    const OrchardTransactionContext context{20001,H(1),20001,Fixture(base).domain};
    const std::vector<VerifiedOrchardAuthorizations> shield{Authorized(base,false,20000)};
    OrchardStateLookups view{[](const uint256&)->StatusOr<bool>{return false;},
                            [](const uint256&)->StatusOr<bool>{return false;}};
    const auto block=PrepareOrchardStateTransition({20001,H(2),H(1),20001,context.domain},{},shield,view);
    const OrchardTransactionContext next{20002,H(2),20001,context.domain};
    const auto empty=CheckOrchardTransactions(next,block.Next(),{},view);
    Require(empty.Parent()==block.Next() && empty.Frontier()==block.Next().frontier);
    Require(empty.PoolBalance()==5000 && empty.Nullifiers().empty() && empty.Fees()==0);
    for(int kind=0;kind<6;++kind) {
        auto bad=block.Next();
        if(kind==0) bad.height--;
        if(kind==1) bad.block_hash=H(99);
        if(kind==2) bad.anchor=H(99);
        if(kind==3) bad.tree_size++;
        if(kind==4) bad.frontier.push_back(0);
        if(kind==5) bad.pool_balance=orchard::kMaxMoneyUna+1;
        StateReject(StateError::ParentState,[&]{(void)CheckOrchardTransactions(next,bad,{},view);});
    }
    StateReject(StateError::ParentState,[&]{(void)CheckOrchardTransactions(next,{}, {},view);});
    StateReject(StateError::ParentState,[&]{(void)CheckOrchardTransactions(context,block.Next(),{},view);});
    auto bad=context;bad.activation_height=UINT32_MAX;
    StateReject(StateError::Inactive,[&]{(void)CheckOrchardTransactions(bad,{},shield,view);});
    bad=context;bad.parent_hash={};
    StateReject(StateError::Context,[&]{(void)CheckOrchardTransactions(bad,{},shield,view);});
    bad=context;bad.domain.network_code=1;
    StateReject(StateError::Context,[&]{(void)CheckOrchardTransactions(bad,{},shield,view);});
    StateReject(StateError::Context,[&]{(void)CheckOrchardTransactions(next,block.Next(),shield,view);});
    std::cout<<"PASS SelectedParentRulesRemainRequired\n";
}

static void ConflictsAndLookupFailures(const std::string& base) {
    const OrchardTransactionContext context{20001,H(1),20001,Fixture(base).domain};
    const auto shield=Authorized(base,false,20000);
    const std::vector<VerifiedOrchardAuthorizations> shields{shield}, repeated{shield,shield};
    OrchardStateLookups view{[](const uint256&)->StatusOr<bool>{return false;},
                            [](const uint256&)->StatusOr<bool>{return false;}};
    const auto checked=CheckOrchardTransactions(context,{},shields,view);
    StateReject(StateError::DuplicateTransaction,[&]{(void)CheckOrchardTransactions(context,{},repeated,view);});
    const auto block=PrepareOrchardStateTransition({20001,H(2),H(1),20001,context.domain},{},shields,view);
    const OrchardTransactionContext next{20002,H(2),20001,context.domain};
    const std::vector<VerifiedOrchardAuthorizations> spends{Authorized(base,true,20001)};
    StateReject(StateError::Anchor,[&]{(void)CheckOrchardTransactions(next,block.Next(),spends,view);});
    view.active_anchor=[](const uint256&)->StatusOr<bool>{return Status::Io;};
    LookupReject(Status::Io,[&]{(void)CheckOrchardTransactions(next,block.Next(),spends,view);});
    view.active_anchor=[&](const uint256& a)->StatusOr<bool>{return a==checked.Anchor();};
    auto poor=block.Next();poor.pool_balance=499;
    StateReject(StateError::PoolBalance,[&]{(void)CheckOrchardTransactions(next,poor,spends,view);});
    view.spent_nullifier=[](const uint256&)->StatusOr<bool>{return true;};
    StateReject(StateError::SpentNullifier,[&]{(void)CheckOrchardTransactions(context,{},shields,view);});
    view.spent_nullifier=[](const uint256&)->StatusOr<bool>{return Status::Corruption;};
    LookupReject(Status::Corruption,[&]{(void)CheckOrchardTransactions(context,{},shields,view);});
    view.spent_nullifier={};
    LookupReject(Status::Internal,[&]{(void)CheckOrchardTransactions(context,{},shields,view);});
    view.spent_nullifier=[](const uint256&)->StatusOr<bool>{return false;};
    const auto retry=CheckOrchardTransactions(next,block.Next(),spends,view);
    Require(retry.PoolBalance()==4500 && checked.PoolBalance()==5000 && checked.TreeSize()==2);
    Require(checked.Frontier()==block.Next().frontier && checked.Nullifiers().size()==2);
    std::cout<<"PASS ConflictsAndLookupFailures\n";
}
int main(int argc,char**argv) {
    try {
        Require(argc==2);
        MatchesBlockPreparation(argv[1]);
        SelectedParentRulesRemainRequired(argv[1]);
        ConflictsAndLookupFailures(argv[1]);
    } catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
