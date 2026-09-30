#include "vault/state_snapshot.h"
#include <algorithm>
#include <bit>
#include <climits>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace dinero::vault {
namespace {
constexpr size_t MaxBytes=64U*1024U*1024U;
constexpr uint64_t MaxRows=1000000;
[[noreturn]] void Invalid() {throw std::runtime_error("invalid or incomplete vault state snapshot");}
void Require(bool value) {if(!value)Invalid();}
struct Writer {
    std::vector<uint8_t> bytes;
    void Space(size_t n) {Require(n<=MaxBytes && bytes.size()<=MaxBytes-n);}
    void U8(uint8_t v) {Space(1);bytes.push_back(v);}
    void U64(uint64_t v) {Space(8);for(unsigned i=0;i<8;++i)bytes.push_back(static_cast<uint8_t>(v>>(8*i)));}
    void I64(int64_t v) {U64(std::bit_cast<uint64_t>(v));}
    void Raw(std::span<const uint8_t> v) {Space(v.size());bytes.insert(bytes.end(),v.begin(),v.end());}
    void Text(const std::string& v) {Space(8);Require(v.size()<=MaxBytes-bytes.size()-8);U64(v.size());Raw({reinterpret_cast<const uint8_t*>(v.data()),v.size()});}
    void Count(size_t n) {Require(n<=MaxRows);U64(n);}
};
struct Reader {
    std::span<const uint8_t> bytes;size_t at=0;
    std::span<const uint8_t> Raw(size_t n) {Require(n<=bytes.size()-at);auto v=bytes.subspan(at,n);at+=n;return v;}
    uint8_t U8() {return Raw(1)[0];}
    bool Bool() {auto v=U8();Require(v<=1);return v!=0;}
    uint64_t U64() {auto v=Raw(8);uint64_t out=0;for(unsigned i=0;i<8;++i)out|=uint64_t{v[i]}<<(8*i);return out;}
    int64_t I64() {return std::bit_cast<int64_t>(U64());}
    uint32_t U32() {auto v=U64();Require(v<=UINT32_MAX);return static_cast<uint32_t>(v);}
    std::string Text() {auto n=U64();Require(n<=bytes.size()-at);auto v=Raw(static_cast<size_t>(n));return {reinterpret_cast<const char*>(v.data()),v.size()};}
    size_t Count() {auto n=U64();Require(n<=MaxRows && n<=bytes.size()-at);return static_cast<size_t>(n);}
    template<size_t N> std::array<uint8_t,N> Fixed() {std::array<uint8_t,N> v{};auto raw=Raw(N);std::copy(raw.begin(),raw.end(),v.begin());return v;}
};
void PutOutpoint(Writer& w,const OutpointId& value) {w.Raw(value.txid_raw);w.U64(value.vout);}
OutpointId GetOutpoint(Reader& r) {OutpointId v;v.txid_raw=r.Fixed<32>();v.vout=r.U32();return v;}
bool LessOutpoint(const OutpointId& a,const OutpointId& b) {
    return a.txid_raw<b.txid_raw || (a.txid_raw==b.txid_raw && a.vout<b.vout);
}
bool Nonzero(std::span<const uint8_t> value) {return std::any_of(value.begin(),value.end(),[](uint8_t v){return v!=0;});}
void PutConfig(Writer& w,const VaultServiceConfig& c) {
    w.U64(c.ledger_caps.per_deposit);w.U64(c.ledger_caps.per_user);w.U64(c.ledger_caps.global);
    const auto& p=c.confirmation_policy;w.U64(p.k_observe);w.U64(p.k_credit);w.U64(p.k_settle);
    w.Count(p.size_matrix.size());for(const auto& [amount,k]:p.size_matrix){w.U64(amount);w.U64(k);}
    w.U64(c.withdrawal_caps.per_request);w.U64(c.withdrawal_caps.per_account_outstanding);
    Require(c.withdrawal_caps.global_queue_depth>=0);w.U64(static_cast<uint64_t>(c.withdrawal_caps.global_queue_depth));
    w.U64(c.withdrawal_policy.k_settle);w.U8(c.shadow_mode?1:0);
}
VaultServiceConfig GetConfig(Reader& r) {
    VaultServiceConfig c;c.ledger_caps={r.U64(),r.U64(),r.U64()};
    auto& p=c.confirmation_policy;p.k_observe=r.U64();p.k_credit=r.U64();p.k_settle=r.U64();
    const auto n=r.Count();p.size_matrix.reserve(n);for(size_t i=0;i<n;++i){auto amount=r.U64();auto k=r.U64();p.size_matrix.emplace_back(amount,k);}
    c.withdrawal_caps.per_request=r.U64();c.withdrawal_caps.per_account_outstanding=r.U64();auto depth=r.U64();Require(depth<=INT_MAX);c.withdrawal_caps.global_queue_depth=static_cast<int>(depth);
    c.withdrawal_policy.k_settle=r.U64();c.shadow_mode=r.Bool();return c;
}
void PutEntry(Writer& w,const LedgerEntry& e) {
    std::visit([&](const auto& v){
        using T=std::decay_t<decltype(v)>;
        uint8_t tag=0;
        if constexpr(std::is_same_v<T,DepositObserved>)tag=1;
        else if constexpr(std::is_same_v<T,CreditOpened>)tag=2;
        else if constexpr(std::is_same_v<T,CreditSettled>)tag=3;
        else if constexpr(std::is_same_v<T,CreditReverted>)tag=4;
        else if constexpr(std::is_same_v<T,WithdrawalInitiated>)tag=5;
        else if constexpr(std::is_same_v<T,WithdrawalSettled>)tag=6;
        else if constexpr(std::is_same_v<T,WithdrawalReverted>)tag=7;
        else if constexpr(std::is_same_v<T,CompensatingDebit>)tag=8;
        else if constexpr(std::is_same_v<T,PolicyAdjustment>)tag=9;
        static_assert(std::is_same_v<T,DepositObserved> || std::is_same_v<T,CreditOpened> ||
                      std::is_same_v<T,CreditSettled> || std::is_same_v<T,CreditReverted> ||
                      std::is_same_v<T,WithdrawalInitiated> || std::is_same_v<T,WithdrawalSettled> ||
                      std::is_same_v<T,WithdrawalReverted> || std::is_same_v<T,CompensatingDebit> ||
                      std::is_same_v<T,PolicyAdjustment>);
        w.U8(tag);w.U64(v.seq);w.I64(v.at);
        if constexpr(std::is_same_v<T,PolicyAdjustment>) {
            w.U8(v.account?1:0);if(v.account)w.Text(v.account->raw);
            w.Text(v.note);w.I64(v.deltaUserBalance);w.I64(v.deltaOperatorFloat);
        } else {
            w.Text(v.account.raw);
            if constexpr(std::is_same_v<T,WithdrawalInitiated> || std::is_same_v<T,WithdrawalSettled> || std::is_same_v<T,WithdrawalReverted>)PutOutpoint(w,v.request);
            else PutOutpoint(w,v.deposit);
            if constexpr(std::is_same_v<T,DepositObserved> || std::is_same_v<T,CreditOpened> || std::is_same_v<T,WithdrawalInitiated> || std::is_same_v<T,CompensatingDebit>)w.U64(v.amount);
            if constexpr(std::is_same_v<T,WithdrawalInitiated>)w.Text(v.backend.raw);
            if constexpr(std::is_same_v<T,CompensatingDebit>)w.U64(v.operatorLoss);
        }
    },e);
}
LedgerEntry GetEntry(Reader& r) {
    auto tag=r.U8();auto seq=r.U64();auto at=r.I64();
    if(tag==9) {
        std::optional<AccountId> account;if(r.Bool())account=AccountId{r.Text()};
        auto note=r.Text();auto user=r.I64();auto op=r.I64();return PolicyAdjustment{seq,at,std::move(account),std::move(note),user,op};
    }
    Require(tag>=1 && tag<=8);AccountId account{r.Text()};auto outpoint=GetOutpoint(r);
    switch(tag) {
        case 1:return DepositObserved{seq,at,std::move(account),outpoint,r.U64()};
        case 2:return CreditOpened{seq,at,std::move(account),outpoint,r.U64()};
        case 3:return CreditSettled{seq,at,std::move(account),outpoint};
        case 4:return CreditReverted{seq,at,std::move(account),outpoint};
        case 5:{auto amount=r.U64();BackendId backend{r.Text()};return WithdrawalInitiated{seq,at,std::move(account),outpoint,amount,std::move(backend)};}
        case 6:return WithdrawalSettled{seq,at,std::move(account),outpoint};
        case 7:return WithdrawalReverted{seq,at,std::move(account),outpoint};
        case 8:{auto amount=r.U64();auto loss=r.U64();return CompensatingDebit{seq,at,std::move(account),outpoint,amount,loss};}
    }
    Invalid();
}
void PutWithdrawalState(Writer& w,const WithdrawalState& state) {
    std::visit([&](const auto& v){
        using T=std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T,WithdrawalPending>)w.U8(1);
        else if constexpr(std::is_same_v<T,WithdrawalSigning>)w.U8(2);
        else if constexpr(std::is_same_v<T,WithdrawalBroadcast>) {w.U8(3);w.Raw(v.txid);w.U64(v.included_at_height);}
        else if constexpr(std::is_same_v<T,WithdrawalSettledOnChain>) {w.U8(4);w.Raw(v.txid);}
        else if constexpr(std::is_same_v<T,WithdrawalRevertedOnChain>) {w.U8(5);w.Raw(v.txid);}
        else if constexpr(std::is_same_v<T,WithdrawalFailed>) {w.U8(6);w.Text(v.reason);}
        else if constexpr(std::is_same_v<T,WithdrawalPaymentRetained>) {
            w.U8(7);w.Raw(v.txid);w.U64(v.vout);w.Raw(v.body_sha256);w.U64(v.fee_una);
        }
        else static_assert(std::is_same_v<T,void>,"new withdrawal state requires an explicit snapshot format");
    },state);
}
WithdrawalState GetWithdrawalState(Reader& r,bool version2) {
    switch(r.U8()) {
        case 1:return WithdrawalPending{};
        case 2:return WithdrawalSigning{};
        case 3:{auto txid=r.Fixed<32>();auto h=r.U64();return WithdrawalBroadcast{txid,h};}
        case 4:return WithdrawalSettledOnChain{r.Fixed<32>()};
        case 5:return WithdrawalRevertedOnChain{r.Fixed<32>()};
        case 6:return WithdrawalFailed{r.Text()};
        case 7:{Require(version2);WithdrawalPaymentRetained v;v.txid=r.Fixed<32>();v.vout=r.U32();v.body_sha256=r.Fixed<32>();v.fee_una=r.U64();return v;}
    }
    Invalid();
}
void CheckEntryOrder(const std::vector<LedgerEntry>& entries) {
    for(size_t i=1;i<entries.size();++i)Require(entrySeq(entries[i-1])<entrySeq(entries[i]));
}
}

std::vector<uint8_t> EncodeVaultState(const VaultStateSnapshot& state) {
    Writer w;const std::array<uint8_t,6> magic{'D','N','V','S','0','2'};w.Raw(magic);w.U64(state.revision);PutConfig(w,state.config);
    CheckEntryOrder(state.entries);w.Count(state.entries.size());for(const auto& e:state.entries)PutEntry(w,e);
    auto deposits=state.deposits;std::sort(deposits.begin(),deposits.end(),[](const auto& a,const auto& b){return LessOutpoint(a.deposit.outpoint,b.deposit.outpoint);});
    w.Count(deposits.size());
    for(size_t i=0;i<deposits.size();++i) {
        const auto& d=deposits[i];if(i)Require(LessOutpoint(deposits[i-1].deposit.outpoint,d.deposit.outpoint));
        Require(static_cast<uint8_t>(d.deposit.stage)<=static_cast<uint8_t>(DepositStage::REVERTED) && Nonzero(d.observed_block));
        PutOutpoint(w,d.deposit.outpoint);w.Text(d.deposit.account.raw);w.U64(d.deposit.amount);w.U64(d.deposit.deposit_height);w.U8(static_cast<uint8_t>(d.deposit.stage));w.Raw(d.observed_block);
    }
    auto withdrawals=state.withdrawals;std::sort(withdrawals.begin(),withdrawals.end(),[](const auto& a,const auto& b){return a.request.request_id<b.request.request_id;});
    w.Count(withdrawals.size());
    for(size_t i=0;i<withdrawals.size();++i) {
        const auto& v=withdrawals[i];Require(Nonzero(v.request.request_id));if(i)Require(withdrawals[i-1].request.request_id<v.request.request_id);
        w.Raw(v.request.request_id);w.Text(v.request.account.raw);w.U64(v.request.amount);w.U64(v.request.destination_script_pub_key.size());w.Raw(v.request.destination_script_pub_key);w.I64(v.request.created_at);
        w.U8(v.request.payment_terms?1:0);
        if(v.request.payment_terms) {
            const auto& terms=*v.request.payment_terms;ValidateWithdrawalPaymentTerms(terms);
            w.U64(terms.fee_rate_hint);w.U64(terms.maximum_fee_una);w.Text(terms.audit_context);
        }
        PutWithdrawalState(w,v.state);
    }
    return std::move(w.bytes);
}
VaultStateSnapshot DecodeVaultState(std::span<const uint8_t> bytes) {
    Require(bytes.size()<=MaxBytes);Reader r{bytes};const auto magic=r.Fixed<6>();
    const bool version2=magic==std::array<uint8_t,6>{'D','N','V','S','0','2'};
    Require(version2 || magic==std::array<uint8_t,6>{'D','N','V','S','0','1'});
    VaultStateSnapshot out;out.revision=r.U64();out.config=GetConfig(r);
    const auto entries=r.Count();out.entries.reserve(entries);for(size_t i=0;i<entries;++i)out.entries.push_back(GetEntry(r));CheckEntryOrder(out.entries);
    const auto deposits=r.Count();out.deposits.reserve(deposits);
    for(size_t i=0;i<deposits;++i) {
        VaultSavedDeposit d;d.deposit.outpoint=GetOutpoint(r);d.deposit.account={r.Text()};d.deposit.amount=r.U64();d.deposit.deposit_height=r.U64();auto stage=r.U8();Require(stage<=static_cast<uint8_t>(DepositStage::REVERTED));d.deposit.stage=static_cast<DepositStage>(stage);d.observed_block=r.Fixed<32>();Require(Nonzero(d.observed_block));
        if(i)Require(LessOutpoint(out.deposits.back().deposit.outpoint,d.deposit.outpoint));out.deposits.push_back(std::move(d));
    }
    const auto withdrawals=r.Count();out.withdrawals.reserve(withdrawals);
    for(size_t i=0;i<withdrawals;++i) {
        VaultSavedWithdrawal v;v.request.request_id=r.Fixed<16>();Require(Nonzero(v.request.request_id));v.request.account={r.Text()};v.request.amount=r.U64();auto n=r.U64();Require(n<=bytes.size()-r.at);auto script=r.Raw(static_cast<size_t>(n));v.request.destination_script_pub_key.assign(script.begin(),script.end());v.request.created_at=r.I64();
        if(version2 && r.Bool()) {
            WithdrawalPaymentTerms terms;terms.fee_rate_hint=r.U64();terms.maximum_fee_una=r.U64();terms.audit_context=r.Text();
            ValidateWithdrawalPaymentTerms(terms);v.request.payment_terms=std::move(terms);
        }
        v.state=GetWithdrawalState(r,version2);
        if(i)Require(out.withdrawals.back().request.request_id<v.request.request_id);out.withdrawals.push_back(std::move(v));
    }
    Require(r.at==bytes.size());return out;
}
} // namespace dinero::vault
