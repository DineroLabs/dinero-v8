#include "vault/ledger.h"
#include <algorithm>
#include <limits>
#include <type_traits>

namespace dinero::vault {
namespace {
void Require(bool value,const char* reason) {
    if(!value)throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,reason);
}
UnaAmount Add(UnaAmount a,UnaAmount b) {
    if(b>std::numeric_limits<UnaAmount>::max()-a)
        throw LedgerError(LedgerError::Kind::ARITHMETIC_OVERFLOW,"vault attributed total overflow");
    return a+b;
}
}
CreditAllocationState Ledger::captureUnambiguousCreditOrigins(const AccountId& account) const {
    Require(!hasCreditAllocation(account),"vault account already has allocation ownership");
    const auto found=accounts_.find(account);
    Require(found!=accounts_.end(),"vault allocation account has no retained credit history");
    const auto& saved=found->second;
    Require(saved.withdrawals().empty() && saved.locked()==0 && saved.operatorLoss()==0,
            "historical vault financial attribution is unavailable");
    CreditAllocationState next=allocations_;
    auto origin=[&](const OutpointId& deposit) {
        std::optional<LedgerSeq> result;
        for(const auto& [seq,p]:next.positions_) if(p.account==account && p.deposit==deposit) {
            Require(!result,"duplicate retained vault origin");result=seq;
        }
        Require(result.has_value(),"missing retained vault origin");return *result;
    };
    for(const auto& entry:entries_) {
        if(entryAccount(entry)!=std::optional<AccountId>{account})continue;
        std::visit([&](const auto& e){
            using T=std::decay_t<decltype(e)>;
            if constexpr(std::is_same_v<T,DepositObserved>) {
                // Observation itself creates no spendable financial origin.
            } else if constexpr(std::is_same_v<T,CreditOpened>) {
                next.open(e.seq,account,e.deposit,e.amount);
            } else if constexpr(std::is_same_v<T,CreditSettled>) {
                next.mature(origin(e.deposit),account,e.deposit);
            } else if constexpr(std::is_same_v<T,CreditReverted>) {
                next.revert(origin(e.deposit),account,e.deposit);
            } else if constexpr(std::is_same_v<T,CompensatingDebit>) {
                const auto& p=next.positions_.at(origin(e.deposit));
                Require(!p.active && e.amount==p.nominal && e.operatorLoss==0,
                        "historical compensated debit lacks source ownership");
            } else if constexpr(std::is_same_v<T,CreditReinstated>) {
                next.restore(origin(e.deposit),account,e.deposit);
            } else if constexpr(std::is_same_v<T,PolicyAdjustment>) {
                Require(e.deltaUserBalance==0,"historical balance adjustment lacks source ownership");
            } else {
                // A later aggregate balance cannot identify which origins an
                // old withdrawal used, even if its final amount is zero.
                Require(false,"historical withdrawal lacks recorded source allocation");
            }
        },entry);
    }
    const auto totals=next.amounts(account);
    Require(totals.pending==saved.pending() && totals.confirmed==saved.confirmed() &&
            totals.locked==saved.locked() && totals.available==saved.spendable() &&
            totals.operator_loss==saved.operatorLoss(),"retained origin balances do not reconcile");
    UnaAmount open=0;size_t origins=0;
    for(const auto& [seq,p]:next.positions_) {
        if(p.account!=account)continue;
        const auto d=saved.deposits().find(p.deposit);
        Require(d!=saved.deposits().end(),"orphaned retained credit origin");
        Require(std::visit([](const auto& v){return v.amount;},d->second)==p.nominal,
                "retained credit amount mismatch");
        if(!p.active)Require(std::holds_alternative<DepositRevertedState>(d->second),"retained credit stage mismatch");
        else if(p.stage==CreditAllocationState::Stage::Pending) {
            Require(std::holds_alternative<DepositCreditedState>(d->second),"retained credit stage mismatch");
            open=Add(open,p.nominal);
        } else Require(std::holds_alternative<DepositSettledState>(d->second),"retained credit stage mismatch");
        ++origins;
    }
    const auto declared=std::count_if(saved.deposits().begin(),saved.deposits().end(),
        [](const auto& row){return !std::holds_alternative<DepositObservedState>(row.second);});
    Require(origins==static_cast<size_t>(declared),"unclaimed retained deposit financial state");
    const auto cap=openCreditsByAccount_.find(account);
    Require(open==(cap==openCreditsByAccount_.end()?0:cap->second),"retained source cap mismatch");
    return next;
}
std::vector<CreditAllocationRef> Ledger::selectCreditAllocations(const AccountId& account,UnaAmount amount) const {
    Require(amount>0,"empty vault allocation request");
    const auto state=hasCreditAllocation(account)?allocations_:captureUnambiguousCreditOrigins(account);
    std::vector<CreditAllocationRef> refs;
    auto take=[&](CreditAllocationState::Stage stage){
        for(const auto& [seq,p]:state.positions()) {
            if(!amount)break;
            if(p.account!=account || !p.active || p.stage!=stage)continue;
            const auto available=p.remaining-p.reserved;
            const auto selected=std::min(amount,available);
            if(selected){refs.push_back({seq,selected});amount-=selected;}
        }
    };
    // This chooses only for a new reservation, before effects. The selection
    // is recorded, and replay/retry consume those exact references thereafter.
    take(CreditAllocationState::Stage::Confirmed);take(CreditAllocationState::Stage::Pending);
    Require(amount==0,"insufficient attributed vault principal");
    std::sort(refs.begin(),refs.end(),[](const auto& a,const auto& b){return a.credit_seq<b.credit_seq;});
    return refs;
}
LedgerSeq Ledger::creditPositionSeq(const AccountId& account,const OutpointId& deposit) const {
    Require(hasCreditAllocation(account),"vault account has no recorded allocation ownership");
    for(const auto& [seq,p]:allocations_.positions())if(p.account==account && p.deposit==deposit)return seq;
    throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,"vault deposit has no attributed origin");
}
void Ledger::syncAllocatedAccount(const AccountId& account) {
    Require(hasCreditAllocation(account),"vault account allocation missing");
    auto& a=accounts_.at(account);const auto amounts=allocations_.amounts(account);
    Require(totalOperatorLoss_>=a.operatorLoss_,"vault operator loss accounting underflow");
    const auto loss=Add(totalOperatorLoss_-a.operatorLoss_,amounts.operator_loss);
    UnaAmount open=0;
    for(const auto& [seq,p]:allocations_.positions()) {
        if(p.account!=account)continue;
        Require(a.deposits_.contains(p.deposit),"vault attributed deposit missing from account");
        if(!p.active)a.deposits_.at(p.deposit)=DepositRevertedState{p.nominal};
        else if(p.stage==CreditAllocationState::Stage::Pending) {
            a.deposits_.at(p.deposit)=DepositCreditedState{p.nominal};open=Add(open,p.nominal);
        } else a.deposits_.at(p.deposit)=DepositSettledState{p.nominal};
    }
    for(const auto& [id,r]:allocations_.reservations()) {
        if(r.account!=account || !r.payment)continue;
        auto state=r.inclusion?WithdrawalLifecycle{WithdrawalSettledState{r.amount,r.backend}}:
                               WithdrawalLifecycle{WithdrawalInitiatedState{r.amount,r.backend}};
        a.withdrawals_.insert_or_assign(r.payment->output,std::move(state));
    }
    const auto before=openCreditsByAccount_.contains(account)?openCreditsByAccount_.at(account):0;
    Require(totalOpenCredits_>=before,"vault source advance counter underflow");
    const auto allOpen=Add(totalOpenCredits_-before,open);
    // Values are still inside Ledger's unpublished append candidate.
    openCreditsByAccount_[account]=open;totalOpenCredits_=allOpen;totalOperatorLoss_=loss;
    a.pending_=amounts.pending;a.confirmed_=amounts.confirmed;a.locked_=amounts.locked;
    a.operatorLoss_=amounts.operator_loss;a.allocatedSpendable_=amounts.available;
}
bool Ledger::applyAllocationEntry(const LedgerEntry& entry) {
    bool handled=false;std::optional<AccountId> owner;
    std::visit([&](const auto& e){
        using T=std::decay_t<decltype(e)>;
        if constexpr(std::is_same_v<T,WithdrawalAllocationReserved> ||
                     std::is_same_v<T,WithdrawalAllocationDispatchStarted> ||
                     std::is_same_v<T,WithdrawalAllocationPaymentBound> ||
                     std::is_same_v<T,WithdrawalAllocationIncluded> ||
                     std::is_same_v<T,WithdrawalAllocationDisconnected> ||
                     std::is_same_v<T,WithdrawalAllocationReleased>) {
            Require(hasCreditAllocation(e.account),"vault allocation account was not enrolled from history");
            if constexpr(std::is_same_v<T,WithdrawalAllocationReserved>) {
                allocations_.reserve(e.request,e.account,e.amount,e.sources);
            } else {
                const auto& r=allocations_.reservation(e.request);
                Require(r.account==e.account,"foreign vault allocation request");
                if constexpr(std::is_same_v<T,WithdrawalAllocationDispatchStarted>)allocations_.beginDispatch(e.request);
                else if constexpr(std::is_same_v<T,WithdrawalAllocationPaymentBound>)allocations_.bind(e.request,e.payment,e.backend);
                else if constexpr(std::is_same_v<T,WithdrawalAllocationIncluded>)allocations_.include(e.request,e.inclusion);
                else if constexpr(std::is_same_v<T,WithdrawalAllocationDisconnected>)allocations_.disconnect(e.request,e.inclusion);
                else allocations_.release(e.request);
            }
            handled=true;owner=e.account;
        } else if constexpr(std::is_same_v<T,CreditPositionMatured> ||
                            std::is_same_v<T,CreditPositionReverted> ||
                            std::is_same_v<T,CreditPositionRestored>) {
            Require(hasCreditAllocation(e.account),"vault deposit allocation missing");
            if constexpr(std::is_same_v<T,CreditPositionMatured>)allocations_.mature(e.credit_seq,e.account,e.deposit);
            else if constexpr(std::is_same_v<T,CreditPositionReverted>)allocations_.revert(e.credit_seq,e.account,e.deposit);
            else allocations_.restore(e.credit_seq,e.account,e.deposit);
            handled=true;owner=e.account;
        }
    },entry);
    if(handled)syncAllocatedAccount(*owner);
    return handled;
}
}
