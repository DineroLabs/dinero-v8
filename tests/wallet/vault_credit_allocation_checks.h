#pragma once
#include "vault/ledger_store.h"
namespace {
namespace allocated_vault=dinero::vault;
struct CreditAllocationFixture {
    allocated_vault::Ledger ledger;
    allocated_vault::AccountId account{"allocated"};
    static allocated_vault::WithdrawalId id(uint8_t n) {allocated_vault::WithdrawalId v{};v.fill(n);return v;}
    static std::array<uint8_t,32> hash(uint8_t n) {std::array<uint8_t,32> v{};v.fill(n);return v;}
    allocated_vault::LedgerSeq open(uint8_t n,uint64_t amount,bool mature=false) {
        const auto op=CapOutpoint(n);
        ledger.append(allocated_vault::DepositObserved{ledger.nextSeq(),0,account,op,amount});
        const auto seq=ledger.nextSeq();ledger.append(allocated_vault::CreditOpened{seq,0,account,op,amount});
        if(mature) {
            if(ledger.hasCreditAllocation(account))ledger.append(allocated_vault::CreditPositionMatured{ledger.nextSeq(),0,account,seq,op});
            else ledger.append(allocated_vault::CreditSettled{ledger.nextSeq(),0,account,op});
        }
        return seq;
    }
    void reserve(uint8_t n,uint64_t amount) {
        ledger.append(allocated_vault::WithdrawalAllocationReserved{ledger.nextSeq(),0,account,id(n),amount,ledger.selectCreditAllocations(account,amount)});
    }
    void dispatch(uint8_t n) {
        ledger.append(allocated_vault::WithdrawalAllocationDispatchStarted{ledger.nextSeq(),0,account,id(n)});
        ledger.append(allocated_vault::WithdrawalAllocationPaymentBound{ledger.nextSeq(),0,account,id(n),{CapOutpoint(n),hash(n)},allocated_vault::BackendId{"fixture"}});
    }
    void include(uint8_t n) {ledger.append(allocated_vault::WithdrawalAllocationIncluded{ledger.nextSeq(),0,account,id(n),{7,hash(90)}});}
    void disconnect(uint8_t n) {ledger.append(allocated_vault::WithdrawalAllocationDisconnected{ledger.nextSeq(),0,account,id(n),{7,hash(90)}});}
    auto state() const {
        allocated_vault::VaultStateSnapshot out;out.revision=1;out.entries=ledger.entries();
        for(const auto& [seq,p]:ledger.creditAllocations().positions()) {
            const auto stage=!p.active?allocated_vault::DepositStage::REVERTED:
                p.stage==allocated_vault::CreditAllocationState::Stage::Pending?allocated_vault::DepositStage::CREDITED:allocated_vault::DepositStage::SETTLED;
            out.deposits.push_back({{p.deposit,p.account,p.nominal,1,stage},hash(99)});
        }
        for(const auto& [request,r]:ledger.creditAllocations().reservations()) {
            allocated_vault::VaultSavedWithdrawal row;
            row.request.request_id=request;row.request.account=r.account;row.request.amount=r.amount;
            row.request.destination_script_pub_key={0x51,0x20};row.request.destination_script_pub_key.resize(34,8);
            row.request.payment_terms=allocated_vault::WithdrawalPaymentTerms{1,100,"source allocation fixture"};
            if(r.released)row.state=allocated_vault::WithdrawalFailed{"released before dispatch"};
            else if(r.payment) {
                allocated_vault::WithdrawalPaymentRetained payment{r.payment->output.txid_raw,r.payment->output.vout,r.payment->body_hash,1};
                if(r.inclusion)row.state=allocated_vault::WithdrawalPaymentConfirmed{payment,*r.inclusion};else row.state=payment;
            } else if(r.dispatch_started)row.state=allocated_vault::WithdrawalSigning{};
            else row.state=allocated_vault::WithdrawalPending{};
            out.withdrawals.push_back(std::move(row));
        }
        return out;
    }
    void replay() const {
        const auto copy=allocated_vault::ReplayVaultStateLedger(state());
        EXPECT_EQ(copy.accounts(),ledger.accounts());EXPECT_EQ(copy.creditAllocations(),ledger.creditAllocations());
        EXPECT_EQ(copy.totalOpenCredits(),ledger.totalOpenCredits());EXPECT_EQ(copy.totalOperatorLoss(),ledger.totalOperatorLoss());
    }
};
TEST(VaultCreditAllocation, PartialDebitMaturesOnlyItsOriginAndRepeatedOrphanPreservesLaterFunds) {
    CreditAllocationFixture f;const auto a=f.open(1,100);f.open(2,200);f.reserve(11,60);f.dispatch(11);f.include(11);
    EXPECT_EQ(f.ledger.accountOr(f.account).pending(),240u);
    f.ledger.append(allocated_vault::CreditPositionMatured{f.ledger.nextSeq(),0,f.account,a,CapOutpoint(1)});
    EXPECT_EQ(f.ledger.accountOr(f.account).confirmed(),40u);EXPECT_EQ(f.ledger.accountOr(f.account).pending(),200u);
    EXPECT_EQ(f.ledger.totalOpenCredits(),200u);f.open(3,50);
    for(unsigned repeat=0;repeat<2;++repeat) {
        f.ledger.revertCredit(f.account,CapOutpoint(1),0);
        EXPECT_EQ(f.ledger.totalOperatorLoss(),60u);EXPECT_EQ(f.ledger.accountOr(f.account).spendable(),250u);
        EXPECT_EQ(f.ledger.accountOr(f.account).confirmed(),0u);EXPECT_EQ(f.ledger.accountOr(f.account).pending(),250u);f.replay();
        f.ledger.append(allocated_vault::CreditPositionRestored{f.ledger.nextSeq(),0,f.account,a,CapOutpoint(1)});
        EXPECT_EQ(f.ledger.accountOr(f.account).confirmed(),40u);EXPECT_EQ(f.ledger.accountOr(f.account).spendable(),290u);
        EXPECT_EQ(f.ledger.totalOperatorLoss(),0u);EXPECT_EQ(f.ledger.totalOpenCredits(),250u);f.replay();
    }
}
TEST(VaultCreditAllocation, MultiOriginOrphanReservationDisconnectAndReinclusionKeepExactSources) {
    CreditAllocationFixture f;const auto a=f.open(1,100,true),b=f.open(2,80);f.reserve(12,150);
    const auto sources=f.ledger.creditAllocations().reservations().at(f.id(12)).sources;
    ASSERT_EQ(sources.size(),2u);EXPECT_EQ(sources[0].credit_seq,a);EXPECT_EQ(sources[0].amount,100u);EXPECT_EQ(sources[1].amount,50u);
    f.ledger.revertCredit(f.account,CapOutpoint(1),0);const auto before=f.ledger.entries();
    EXPECT_THROW(f.dispatch(12),allocated_vault::LedgerError);EXPECT_EQ(f.ledger.entries(),before);
    EXPECT_EQ(f.ledger.accountOr(f.account).locked(),150u);EXPECT_EQ(f.ledger.accountOr(f.account).spendable(),30u);
    f.ledger.append(allocated_vault::CreditPositionRestored{f.ledger.nextSeq(),0,f.account,a,CapOutpoint(1)});
    f.dispatch(12);f.include(12);f.ledger.append(allocated_vault::CreditPositionMatured{f.ledger.nextSeq(),0,f.account,b,CapOutpoint(2)});
    EXPECT_EQ(f.ledger.accountOr(f.account).confirmed(),30u);EXPECT_EQ(f.ledger.totalOpenCredits(),0u);
    f.ledger.revertCredit(f.account,CapOutpoint(1),0);EXPECT_EQ(f.ledger.totalOperatorLoss(),100u);f.disconnect(12);
    EXPECT_EQ(f.ledger.totalOperatorLoss(),0u);EXPECT_EQ(f.ledger.accountOr(f.account).confirmed(),80u);
    EXPECT_EQ(f.ledger.accountOr(f.account).locked(),150u);EXPECT_EQ(f.ledger.accountOr(f.account).spendable(),30u);f.replay();
    f.include(12);EXPECT_EQ(f.ledger.totalOperatorLoss(),100u);EXPECT_EQ(f.ledger.accountOr(f.account).confirmed(),30u);
    EXPECT_EQ(f.ledger.creditAllocations().reservations().at(f.id(12)).sources,sources);f.replay();
}
TEST(VaultCreditAllocation, LateInvalidSourceAndWrongPaymentAnchorPreserveWholeLedger) {
    CreditAllocationFixture f;const auto a=f.open(1,100),b=f.open(2,100);
    for(const auto& refs:std::vector<std::vector<allocated_vault::CreditAllocationRef>>{{{a,50},{b,151}},{{a,50},{a,50}},{{a,50},{b+100,50}}}) {
        const auto before=f.ledger.entries();const auto accounts=f.ledger.accounts();const auto next=f.ledger.nextSeq();
        EXPECT_THROW(f.ledger.append(allocated_vault::WithdrawalAllocationReserved{next,0,f.account,f.id(13),100,refs}),allocated_vault::LedgerError);
        EXPECT_EQ(f.ledger.entries(),before);EXPECT_EQ(f.ledger.accounts(),accounts);EXPECT_FALSE(f.ledger.hasCreditAllocation(f.account));
    }
    f.reserve(13,100);f.dispatch(13);f.include(13);const auto before=f.ledger.entries();const auto accounts=f.ledger.accounts();
    auto bad=f.hash(91);
    EXPECT_THROW(f.ledger.append(allocated_vault::WithdrawalAllocationDisconnected{f.ledger.nextSeq(),0,f.account,f.id(13),{7,bad}}),allocated_vault::LedgerError);
    EXPECT_THROW(f.ledger.append(allocated_vault::WithdrawalAllocationPaymentBound{f.ledger.nextSeq(),0,f.account,f.id(13),{CapOutpoint(14),f.hash(14)},allocated_vault::BackendId{"fixture"}}),allocated_vault::LedgerError);
    EXPECT_THROW(f.ledger.append(allocated_vault::WithdrawalAllocationReleased{f.ledger.nextSeq(),0,f.account,f.id(13)}),allocated_vault::LedgerError);
    EXPECT_EQ(f.ledger.entries(),before);EXPECT_EQ(f.ledger.accounts(),accounts);f.replay();
}
TEST(VaultCreditAllocation, HistoricalUnattributedWithdrawalAndBalanceAdjustmentsRemainUnchanged) {
    for(bool withdrawal:{false,true}) {
        CreditAllocationFixture f;f.open(1,100,true);
        if(withdrawal) {
            f.ledger.append(allocated_vault::WithdrawalInitiated{f.ledger.nextSeq(),0,f.account,CapOutpoint(21),10,allocated_vault::BackendId{"legacy"}});
            f.ledger.append(allocated_vault::WithdrawalSettled{f.ledger.nextSeq(),0,f.account,CapOutpoint(21)});
        } else f.ledger.append(allocated_vault::PolicyAdjustment{f.ledger.nextSeq(),0,f.account,"recorded legacy balance",1,0});
        const auto before=f.ledger.entries();const auto accounts=f.ledger.accounts();
        EXPECT_THROW(f.reserve(22,10),allocated_vault::LedgerError);EXPECT_EQ(f.ledger.entries(),before);EXPECT_EQ(f.ledger.accounts(),accounts);
        EXPECT_FALSE(f.ledger.hasCreditAllocation(f.account));EXPECT_EQ(allocated_vault::Ledger::replay(before).accounts(),accounts);
    }
}
TEST(VaultCreditAllocation, VersionSixAndJsonRoundTripEveryNewEntryWithoutLosingOldPrefix) {
    CreditAllocationFixture f;const auto origin=f.open(1,100);const auto old=f.ledger.entries();
    f.reserve(23,10);f.ledger.append(allocated_vault::WithdrawalAllocationReleased{f.ledger.nextSeq(),0,f.account,f.id(23)});
    f.reserve(24,60);f.dispatch(24);f.include(24);f.disconnect(24);f.include(24);
    f.ledger.append(allocated_vault::CreditPositionMatured{f.ledger.nextSeq(),0,f.account,origin,CapOutpoint(1)});
    f.ledger.revertCredit(f.account,CapOutpoint(1),0);
    f.ledger.append(allocated_vault::CreditPositionRestored{f.ledger.nextSeq(),0,f.account,origin,CapOutpoint(1)});
    ASSERT_TRUE(std::equal(old.begin(),old.end(),f.ledger.entries().begin()));
    const auto state=f.state();const auto bytes=allocated_vault::EncodeVaultState(state);ASSERT_EQ(bytes.at(5),'6');
    const auto decoded=allocated_vault::DecodeVaultState(bytes);EXPECT_EQ(allocated_vault::EncodeVaultState(decoded),bytes);f.replay();
    auto downgrade=bytes;downgrade[5]='5';
    EXPECT_THROW(allocated_vault::DecodeVaultState(downgrade),std::runtime_error);
    auto partial=bytes;partial.pop_back();
    EXPECT_THROW(allocated_vault::DecodeVaultState(partial),std::runtime_error);
    auto extra=bytes;extra.push_back(0);
    EXPECT_THROW(allocated_vault::DecodeVaultState(extra),std::runtime_error);
    char directory[]="/tmp/dinero-allocation-codec-XXXXXX";ASSERT_NE(mkdtemp(directory),nullptr);
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove_all(path,e);}} cleanup{directory};
    const auto path=std::filesystem::path(directory)/"ledger.jsonl";
    {allocated_vault::FileLedgerStore store(path.string());for(const auto& entry:f.ledger.entries())store.append(entry);EXPECT_EQ(store.loadAll(),f.ledger.entries());}
    {allocated_vault::FileLedgerStore store(path.string());EXPECT_EQ(store.loadAll(),f.ledger.entries());}
}
TEST(VaultCreditAllocation, SavedRequestPaymentAndDepositBindingsRefusePartialInventories) {
    CreditAllocationFixture f;f.open(1,100);f.reserve(25,30);f.dispatch(25);f.include(25);
    const auto original=f.state();f.replay();
    for(unsigned mode=0;mode<6;++mode) {
        auto bad=original;
        if(mode==0)bad.withdrawals.clear();
        if(mode==1)bad.deposits.clear();
        if(mode==2)++bad.withdrawals[0].request.amount;
        if(mode==3)std::get<allocated_vault::WithdrawalPaymentConfirmed>(bad.withdrawals[0].state).payment.body_sha256[0]^=1;
        if(mode==4)std::get<allocated_vault::WithdrawalPaymentConfirmed>(bad.withdrawals[0].state).inclusion.block_hash[0]^=1;
        if(mode==5)bad.deposits[0].deposit.stage=allocated_vault::DepositStage::SETTLED;
        EXPECT_THROW(allocated_vault::ReplayVaultStateLedger(bad),std::runtime_error);
    }
}
class VaultCreditAllocationWallet : public VaultRetainedWithdrawal {};
TEST(VaultCreditAllocation, OnlyReplayedInclusionUndoCanRestoreAboveAdmissionCaps) {
    CreditAllocationFixture f;f.open(1,200,true);f.reserve(31,60);f.dispatch(31);f.include(31);f.reserve(32,60);
    auto before=f.state();before.config.withdrawal_caps.per_request=60;
    before.config.withdrawal_caps.per_account_outstanding=60;before.config.withdrawal_caps.global_queue_depth=1;
    EXPECT_NO_THROW(allocated_vault::ReplayVaultStateLedger(before));
    f.disconnect(31);auto restored=f.state();restored.config=before.config;
    const auto ledger=allocated_vault::ReplayVaultStateLedger(restored);
    EXPECT_EQ(ledger.accountOr(f.account).locked(),120u);EXPECT_EQ(ledger.accountOr(f.account).spendable(),80u);
    EXPECT_TRUE(ledger.creditAllocations().reservations().at(f.id(31)).previously_included);
    CreditAllocationFixture ordinary;ordinary.open(1,200,true);ordinary.reserve(31,60);ordinary.dispatch(31);ordinary.reserve(32,60);
    auto never_included=ordinary.state();never_included.config=before.config;
    EXPECT_THROW(allocated_vault::ReplayVaultStateLedger(never_included),std::runtime_error);
    EXPECT_FALSE(ordinary.ledger.creditAllocations().reservations().at(f.id(31)).previously_included);
}
TEST_F(VaultCreditAllocationWallet, ReserveWriteAndCommitRefuseBeforeWalletEffectsAndReopenKeepsSources) {
    const auto before=dinero::vault::EncodeVaultState(vault.service->captureState());auto* db=service->get().getCurrentDatabase();
    sql(db,"CREATE TRIGGER refuse_credit_allocation BEFORE UPDATE ON wallet_vault_states BEGIN SELECT RAISE(ABORT,'allocation refusal'); END");
    EXPECT_THROW(enqueue(),std::runtime_error);sql(db,"DROP TRIGGER refuse_credit_allocation");
    EXPECT_EQ(dinero::vault::EncodeVaultState(vault.service->captureState()),before);
    bool hit=false;sqlite3_commit_hook(db,[](void* p){*static_cast<bool*>(p)=true;return 1;},&hit);
    EXPECT_THROW(enqueue(),std::runtime_error);sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_TRUE(hit);
    EXPECT_EQ(dinero::vault::EncodeVaultState(vault.service->captureState()),before);
    const auto id=enqueue();const auto saved=vault.service->captureState();
    const auto ledger=dinero::vault::ReplayVaultStateLedger(saved);ASSERT_TRUE(ledger.creditAllocations().reservations().contains(id));
    EXPECT_EQ(vault.service->accountLocked(account),20000u);EXPECT_EQ(vault.service->accountSpendable(account),80000u);
    reopen();EXPECT_EQ(dinero::vault::EncodeVaultState(vault.service->captureState()),dinero::vault::EncodeVaultState(saved));
    EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
}
TEST_F(VaultCreditAllocationWallet, SuccessorCannotRewriteSourcePrefixOrAuthorizedRequest) {
    const auto id=enqueue();const auto before=vault.service->captureState();
    const auto bytes=dinero::vault::EncodeVaultState(before);
    for(unsigned mode=0;mode<3;++mode) {
        auto owner=dinero::vault::VaultStateTransaction::OpenExisting(service->get(),selected().session,domain,vault.identity);
        auto next=owner->Current().state;++next.revision;
        if(mode==0)next.withdrawals[0].request.destination_script_pub_key.back()^=1;
        if(mode==1)next.withdrawals[0].request.payment_terms->audit_context="replacement request";
        if(mode==2) {
            for(auto& entry:next.entries) {
                if(auto* observed=std::get_if<dinero::vault::DepositObserved>(&entry))observed->at+=1;
            }
        }
        EXPECT_NO_THROW(dinero::vault::ReplayVaultStateLedger(next));
        EXPECT_THROW(owner->Stage(next),std::runtime_error);
    }
    EXPECT_EQ(dinero::vault::EncodeVaultState(vault.service->captureState()),bytes);
    reopen();EXPECT_EQ(dinero::vault::EncodeVaultState(vault.service->captureState()),bytes);
    EXPECT_TRUE(std::holds_alternative<dinero::vault::WithdrawalPending>(vault.service->withdrawalState(id)));
    EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->submits,0);
}
} // namespace
