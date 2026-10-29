#include "vault/credit_allocation.h"
#include "vault/ledger.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <set>

namespace dinero::vault {
namespace {
void Require(bool yes,const char* message) { if(!yes)throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,message); }
UnaAmount Add(UnaAmount a,UnaAmount b) {
    if(b>std::numeric_limits<UnaAmount>::max()-a)throw LedgerError(LedgerError::Kind::ARITHMETIC_OVERFLOW,"vault allocation amount overflow");return a+b;
}
template<size_t N> bool Nonzero(const std::array<uint8_t,N>& a) {
    return std::any_of(a.begin(),a.end(),[](uint8_t b){return b!=0;});
}
}
void CreditAllocationState::swap(CreditAllocationState& other) noexcept {
    static_assert(noexcept(positions_.swap(other.positions_)));
    static_assert(noexcept(reservations_.swap(other.reservations_)));
    positions_.swap(other.positions_);reservations_.swap(other.reservations_);
}
AllocatedAccountAmounts CreditAllocationState::amounts(const AccountId& account) const {
    AllocatedAccountAmounts sum;
    for(const auto& [seq,p]:positions_) {
        if(p.account!=account)continue;
        Require(p.reserved<=p.remaining && p.remaining<=p.nominal,"invalid vault credit allocation bounds");
        sum.locked=Add(sum.locked,p.reserved);
        if(p.active) {
            auto& bucket=p.stage==Stage::Pending?sum.pending:sum.confirmed;
            bucket=Add(bucket,p.remaining);sum.available=Add(sum.available,p.remaining-p.reserved);
        } else {
            // Outstanding signed reservations remain locked and unavailable.
            // Only an actual debit against an absent origin is realized loss.
            sum.operator_loss=Add(sum.operator_loss,p.nominal-p.remaining);
        }
    }
    (void)Add(sum.pending,sum.confirmed);return sum;
}
void CreditAllocationState::validate() const {
    std::map<LedgerSeq,UnaAmount> reserved,spent;
    std::set<AccountId> accounts;
    for(const auto& [id,r]:reservations_) {
        Require(Nonzero(id) && !r.account.raw.empty() && r.amount,"invalid vault allocation request");
        Require(!r.sources.empty(),"missing vault allocation sources");
        Require(!r.payment || (r.dispatch_started && !r.backend.raw.empty()),"vault payment without dispatch owner");
        Require(!r.inclusion || (r.payment.has_value() && r.previously_included),"vault inclusion without retained payment");
        Require(!r.previously_included || r.payment.has_value(),"vault prior inclusion without retained payment");
        Require(!r.released || (!r.dispatch_started && !r.payment && !r.inclusion),"released ambiguous vault payment");
        UnaAmount total=0;std::optional<LedgerSeq> prior;
        for(const auto& ref:r.sources) {
            Require(ref.amount && (!prior || *prior<ref.credit_seq),"unordered or duplicate vault allocation sources");
            const auto i=positions_.find(ref.credit_seq);
            Require(i!=positions_.end() && i->second.account==r.account,"foreign vault allocation source");
            total=Add(total,ref.amount);prior=ref.credit_seq;
            if(!r.released && !r.inclusion)reserved[ref.credit_seq]=Add(reserved[ref.credit_seq],ref.amount);
            if(r.inclusion)spent[ref.credit_seq]=Add(spent[ref.credit_seq],ref.amount);
        }
        Require(total==r.amount,"vault allocation principal mismatch");
    }
    for(const auto& [seq,p]:positions_) {
        Require(!p.account.raw.empty() && Nonzero(p.deposit.txid_raw) && p.nominal,"invalid vault credit origin");
        Require(p.reserved<=p.remaining && p.remaining<=p.nominal,"invalid vault credit amount");
        Require(p.reserved==reserved[seq],"vault credit reservation mismatch");
        Require(spent[seq]<=p.nominal && p.remaining==p.nominal-spent[seq],"vault credit debit mismatch");
        Require(p.stage==Stage::Pending || p.stage==Stage::Confirmed,"invalid vault credit stage");
        accounts.insert(p.account);
    }
    for(const auto& account:accounts)(void)amounts(account);
}
CreditAllocationState::Position& CreditAllocationState::position(
    LedgerSeq seq,const AccountId& account,const OutpointId& deposit) {
    const auto i=positions_.find(seq);
    Require(i!=positions_.end() && i->second.account==account && i->second.deposit==deposit,
            "vault credit origin mismatch");return i->second;
}
CreditAllocationState::Reservation& CreditAllocationState::reservation(const AllocationRequestId& id) {
    const auto i=reservations_.find(id);Require(i!=reservations_.end(),"missing vault allocation request");return i->second;
}
void CreditAllocationState::open(LedgerSeq seq,const AccountId& account,const OutpointId& deposit,UnaAmount amount) {
    update([&](auto& n){
        Require(!n.positions_.contains(seq),"duplicate vault credit sequence");
        for(const auto& [_,p]:n.positions_)Require(p.deposit!=deposit,"duplicate vault deposit origin");
        n.positions_.emplace(seq,Position{account,deposit,amount,amount,0,Stage::Pending,true});
    });
}
void CreditAllocationState::reserve(const AllocationRequestId& id,const AccountId& account,
    UnaAmount amount,const std::vector<CreditAllocationRef>& refs) {
    update([&](auto& n){
        Require(!n.reservations_.contains(id),"duplicate vault allocation request");
        Reservation r;r.account=account;r.amount=amount;r.sources=refs;
        for(const auto& ref:refs) {
            const auto i=n.positions_.find(ref.credit_seq);
            Require(i!=n.positions_.end() && i->second.account==account && i->second.active,
                    "unavailable vault allocation source");
            auto& p=i->second;
            Require(p.reserved<=p.remaining && ref.amount<=p.remaining-p.reserved,
                    "insufficient unreserved source credit");p.reserved=Add(p.reserved,ref.amount);
        }
        n.reservations_.emplace(id,std::move(r));
    });
}
void CreditAllocationState::beginDispatch(const AllocationRequestId& id) {
    update([&](auto& n){auto& r=n.reservation(id);
        Require(!r.released && !r.dispatch_started,"vault allocation already dispatched or released");
        for(const auto& ref:r.sources)Require(n.positions_.at(ref.credit_seq).active,"dispatch source is orphaned");
        r.dispatch_started=true;
    });
}
void CreditAllocationState::bind(const AllocationRequestId& id,const AllocationPayment& payment,const BackendId& backend) {
    update([&](auto& n){auto& r=n.reservation(id);
        Require(r.dispatch_started && !r.released && Nonzero(payment.output.txid_raw) && Nonzero(payment.body_hash) && !backend.raw.empty(),
                "invalid retained vault allocation payment");
        if(r.payment){Require(*r.payment==payment && r.backend==backend,"vault retained payment changed");return;}
        for(const auto& [other,prior]:n.reservations_)
            if(other!=id && prior.payment)Require(prior.payment->output!=payment.output,"vault payment output already bound");
        r.payment=payment;r.backend=backend;
    });
}
void CreditAllocationState::include(const AllocationRequestId& id,const AllocationInclusion& inclusion) {
    update([&](auto& n){auto& r=n.reservation(id);
        Require(r.payment && !r.released && Nonzero(inclusion.block_hash),"unbound vault canonical inclusion");
        if(r.inclusion){Require(*r.inclusion==inclusion,"vault inclusion changed without disconnect");return;}
        for(const auto& ref:r.sources) {
            auto& p=n.positions_.at(ref.credit_seq);
            Require(p.reserved>=ref.amount && p.remaining>=ref.amount,"vault inclusion source underflow");
            p.reserved-=ref.amount;p.remaining-=ref.amount;
        }
        r.inclusion=inclusion;r.previously_included=true;
    });
}
void CreditAllocationState::disconnect(const AllocationRequestId& id,const AllocationInclusion& inclusion) {
    update([&](auto& n){auto& r=n.reservation(id);
        Require(r.inclusion && *r.inclusion==inclusion,"vault disconnect does not own inclusion");
        for(const auto& ref:r.sources) {
            auto& p=n.positions_.at(ref.credit_seq);
            p.remaining=Add(p.remaining,ref.amount);p.reserved=Add(p.reserved,ref.amount);
        }
        r.inclusion.reset();
    });
}
void CreditAllocationState::release(const AllocationRequestId& id) {
    update([&](auto& n){auto& r=n.reservation(id);
        Require(!r.dispatch_started && !r.payment && !r.inclusion && !r.released,
                "vault allocation cannot release an ambiguous payment");
        for(const auto& ref:r.sources) {
            auto& p=n.positions_.at(ref.credit_seq);Require(p.reserved>=ref.amount,"vault release source underflow");p.reserved-=ref.amount;
        }
        r.released=true;
    });
}
void CreditAllocationState::mature(LedgerSeq seq,const AccountId& a,const OutpointId& d) {
    update([&](auto& n){auto& p=n.position(seq,a,d);Require(p.active && p.stage==Stage::Pending,"vault maturity origin state mismatch");p.stage=Stage::Confirmed;});
}
void CreditAllocationState::revert(LedgerSeq seq,const AccountId& a,const OutpointId& d) {
    update([&](auto& n){auto& p=n.position(seq,a,d);Require(p.active,"vault origin already reverted");p.active=false;});
}
void CreditAllocationState::restore(LedgerSeq seq,const AccountId& a,const OutpointId& d) {
    update([&](auto& n){auto& p=n.position(seq,a,d);Require(!p.active,"vault origin is not reverted");p.active=true;p.stage=Stage::Confirmed;});
}
}
