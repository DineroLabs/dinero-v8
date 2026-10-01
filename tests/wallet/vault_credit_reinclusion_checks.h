#pragma once
#include "vault/ledger_store.h"
#include <fstream>
namespace {
namespace rein_vault=dinero::vault;
rein_vault::CreditReinstated ReinstateEntry(const rein_vault::Ledger& ledger,const rein_vault::AccountId& a,const rein_vault::OutpointId& out) {
    const auto retained=ledger.reinstatement(a,out);
    if(!retained)throw std::runtime_error("fixture complete reversal unavailable");
    return {ledger.nextSeq(),0,a,out,retained->reversalSeq,retained->compensationSeq};
}
void ReverseReinCredit(rein_vault::Ledger& ledger,const rein_vault::AccountId& a,uint8_t tag,uint64_t amount,uint64_t loss) {
    const auto op=CapOutpoint(tag);
    ledger.append(rein_vault::CreditReverted{ledger.nextSeq(),0,a,op});
    ledger.append(rein_vault::CompensatingDebit{ledger.nextSeq(),0,a,op,amount,loss});
}
TEST(VaultCreditReinclusion, PartlySpentCreditRestoresOnlyActualDebitAndRecordedLoss) {
    const rein_vault::AccountId a{"restored"};
    for(bool settled:{false,true}) {
        SCOPED_TRACE(settled);rein_vault::Ledger ledger;OpenCapCredit(ledger,a,1,100,settled);
        const auto withdrawal=CapOutpoint(80);
        ledger.append(rein_vault::WithdrawalInitiated{ledger.nextSeq(),0,a,withdrawal,60,rein_vault::BackendId{"test"}});
        ledger.append(rein_vault::WithdrawalSettled{ledger.nextSeq(),0,a,withdrawal});
        ReverseReinCredit(ledger,a,1,100,60);
        ASSERT_TRUE(ledger.reinstatement(a,CapOutpoint(1)));EXPECT_EQ(ledger.reinstatement(a,CapOutpoint(1))->refund,40u);
        EXPECT_EQ(ledger.totalOperatorLoss(),60u);EXPECT_EQ(ledger.accountOr(a).confirmed(),0u);EXPECT_EQ(ledger.accountOr(a).pending(),0u);
        // An independent later balance must survive the exact reversal refund.
        ledger.append(rein_vault::PolicyAdjustment{ledger.nextSeq(),0,a,"later balance",7,0});
        const auto prefix=ledger.entries();ledger.append(ReinstateEntry(ledger,a,CapOutpoint(1)));
        EXPECT_EQ(ledger.accountOr(a).confirmed(),47u);EXPECT_EQ(ledger.accountOr(a).pending(),0u);
        EXPECT_EQ(ledger.accountOr(a).operatorLoss(),0u);EXPECT_EQ(ledger.totalOperatorLoss(),0u);EXPECT_EQ(ledger.totalOpenCredits(),0u);
        EXPECT_TRUE(std::holds_alternative<rein_vault::DepositSettledState>(ledger.accountOr(a).deposits().at(CapOutpoint(1))));
        EXPECT_TRUE(std::equal(prefix.begin(),prefix.end(),ledger.entries().begin()));
        EXPECT_FALSE(ledger.reinstatement(a,CapOutpoint(1)));
        const auto saved=ledger.entries();auto duplicate=std::get<rein_vault::CreditReinstated>(saved.back());duplicate.seq=ledger.nextSeq();
        EXPECT_THROW(ledger.append(duplicate),rein_vault::LedgerError);EXPECT_EQ(ledger.entries(),saved);
        const auto replay=rein_vault::Ledger::replay(saved);EXPECT_EQ(replay.accounts(),ledger.accounts());EXPECT_EQ(replay.totalOperatorLoss(),0u);
        ReverseReinCredit(ledger,a,1,100,53);const auto newest=ReinstateEntry(ledger,a,CapOutpoint(1));
        duplicate.seq=ledger.nextSeq();const auto before=ledger.entries();
    EXPECT_THROW(ledger.append(duplicate),rein_vault::LedgerError);EXPECT_EQ(ledger.entries(),before);
        ledger.append(newest);EXPECT_EQ(ledger.accountOr(a).confirmed(),47u);EXPECT_EQ(ledger.totalOperatorLoss(),0u);
        EXPECT_EQ(rein_vault::Ledger::replay(ledger.entries()).accounts(),ledger.accounts());
    }
}
TEST(VaultCreditReinclusion, MissingAmbiguousForeignAndOverflowingOwnersRefuseAtomically) {
    const rein_vault::AccountId a{"a"};rein_vault::Ledger ledger;OpenCapCredit(ledger,a,1,100,true);
    ledger.append(rein_vault::CreditReverted{ledger.nextSeq(),0,a,CapOutpoint(1)});
    EXPECT_FALSE(ledger.reinstatement(a,CapOutpoint(1)));
    ledger.append(rein_vault::CompensatingDebit{ledger.nextSeq(),0,a,CapOutpoint(1),100,20});
    auto correct=ReinstateEntry(ledger,a,CapOutpoint(1));
    auto refuse=[&](rein_vault::CreditReinstated bad){const auto entries=ledger.entries();const auto accounts=ledger.accounts();const auto next=ledger.nextSeq();
        bad.seq=next;
    EXPECT_THROW(ledger.append(bad),rein_vault::LedgerError);EXPECT_EQ(ledger.entries(),entries);EXPECT_EQ(ledger.accounts(),accounts);EXPECT_EQ(ledger.nextSeq(),next);};
    auto bad=correct;bad.reversalSeq++;refuse(bad);bad=correct;bad.compensationSeq++;refuse(bad);
    bad=correct;bad.account={"foreign"};refuse(bad);bad=correct;bad.deposit=CapOutpoint(2);refuse(bad);
    ledger.append(rein_vault::PolicyAdjustment{ledger.nextSeq(),0,a,"capacity",INT64_MAX,0});
    ledger.append(rein_vault::PolicyAdjustment{ledger.nextSeq(),0,a,"capacity",INT64_MAX,0});
    ledger.append(rein_vault::PolicyAdjustment{ledger.nextSeq(),0,a,"capacity",1,0});refuse(correct);
    ledger.append(rein_vault::PolicyAdjustment{ledger.nextSeq(),0,a,"release capacity",-100,0});correct.seq=ledger.nextSeq();
    EXPECT_NO_THROW(ledger.append(correct));
    for(unsigned kind=0;kind<3;++kind) {
        rein_vault::Ledger ambiguous;OpenCapCredit(ambiguous,a,1,100,true);ReverseReinCredit(ambiguous,a,1,kind==0?99:100,kind==1?101:20);
        if(kind==2)ambiguous.append(rein_vault::CompensatingDebit{ambiguous.nextSeq(),0,a,CapOutpoint(1),100,0});
        EXPECT_FALSE(ambiguous.reinstatement(a,CapOutpoint(1)));
        EXPECT_NO_THROW(rein_vault::Ledger::replay(ambiguous.entries()));
    }
}
TEST(VaultCreditReinclusion, VersionedSnapshotAndJsonPreserveHistoryAndExactReferences) {
    const rein_vault::AccountId a{"codec"};rein_vault::Ledger ledger;OpenCapCredit(ledger,a,1,100,true);ReverseReinCredit(ledger,a,1,100,20);
    rein_vault::VaultStateSnapshot state;state.revision=1;state.entries=ledger.entries();
    std::array<uint8_t,32> hash{};hash.fill(8);
    state.deposits.push_back({{CapOutpoint(1),a,100,1,rein_vault::DepositStage::REVERTED},hash});
    const auto old=rein_vault::EncodeVaultState(state);ASSERT_EQ(old.at(5),'2');EXPECT_EQ(rein_vault::EncodeVaultState(rein_vault::DecodeVaultState(old)),old);
    ledger.append(ReinstateEntry(ledger,a,CapOutpoint(1)));state.entries=ledger.entries();state.deposits[0].deposit.stage=rein_vault::DepositStage::SETTLED;
    for(unsigned binding=0;binding<3;++binding) {
        if(binding>0){rein_vault::VaultOperatorBinding op;op.account="operator";op.script_pub_key={0x51,0x20};op.script_pub_key.resize(34,7);state.config.operator_binding=op;}
        if(binding==2)state.config.creation_anchor=rein_vault::VaultCreationAnchor{1,hash};
        auto prior=state;prior.entries.pop_back();prior.deposits[0].deposit.stage=rein_vault::DepositStage::REVERTED;
        const auto old_bytes=rein_vault::EncodeVaultState(prior);EXPECT_EQ(old_bytes.at(5),'2'+binding);
        EXPECT_EQ(rein_vault::EncodeVaultState(rein_vault::DecodeVaultState(old_bytes)),old_bytes);
        const auto bytes=rein_vault::EncodeVaultState(state);ASSERT_EQ(bytes.at(5),'5');const auto decoded=rein_vault::DecodeVaultState(bytes);
        EXPECT_EQ(rein_vault::EncodeVaultState(decoded),bytes);EXPECT_EQ(rein_vault::ReplayVaultStateLedger(decoded).accounts(),ledger.accounts());
        auto prefix=bytes;prefix.pop_back();
    EXPECT_THROW(rein_vault::DecodeVaultState(prefix),std::runtime_error);
        auto trailing=bytes;trailing.push_back(0);
    EXPECT_THROW(rein_vault::DecodeVaultState(trailing),std::runtime_error);
        auto bad=state;std::get<rein_vault::CreditReinstated>(bad.entries.back()).reversalSeq++;
        EXPECT_THROW(rein_vault::ReplayVaultStateLedger(bad),std::runtime_error);
        bad=state;bad.deposits[0].deposit.stage=rein_vault::DepositStage::CREDITED;
    EXPECT_THROW(rein_vault::ReplayVaultStateLedger(bad),std::runtime_error);
    }
    char directory[]="/tmp/dinero-reinclusion-codec-XXXXXX";ASSERT_NE(mkdtemp(directory),nullptr);
    const std::filesystem::path path=std::filesystem::path(directory)/"ledger.jsonl";
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove_all(path,e);}} cleanup{directory};
    {rein_vault::FileLedgerStore store(path.string());for(const auto& row:state.entries)store.append(row);store.flush();EXPECT_EQ(store.loadAll(),state.entries);}
    {rein_vault::FileLedgerStore store(path.string());EXPECT_EQ(store.loadAll(),state.entries);}
    std::ifstream input(path);const std::string text((std::istreambuf_iterator<char>(input)),{});input.close();
    const auto pos=text.find("\"compensationSeq\":");ASSERT_NE(pos,std::string::npos);auto truncated=text;truncated.erase(pos,truncated.find(',',pos)-pos+1);
    {std::ofstream output(path,std::ios::trunc);output<<truncated;}
    {rein_vault::FileLedgerStore store(path.string());
    EXPECT_THROW(store.loadAll(),rein_vault::LedgerStoreError);}
}
class VaultCreditReinclusionWallet : public VaultRevertedCreditCapWallet {};
TEST_F(VaultCreditReinclusionWallet, SourceReinclusionWriteAndCommitRefuseBeforePublicationThenReopen) {
    orphan=true;vault.service->tipChanged(10);reopen();const auto reverted=cap_vault::EncodeVaultState(vault.service->captureState());const auto cipher=sealed();
    orphan=false;auto* db=service->get().getCurrentDatabase();
    sql(db,"CREATE TRIGGER deny_reinclusion BEFORE UPDATE ON wallet_vault_states BEGIN SELECT RAISE(ABORT,'reinclusion refusal'); END");
    EXPECT_THROW(vault.service->tipChanged(10),std::runtime_error);sql(db,"DROP TRIGGER deny_reinclusion");
    EXPECT_EQ(cap_vault::EncodeVaultState(vault.service->captureState()),reverted);EXPECT_EQ(sealed(),cipher);
    bool hit=false;sqlite3_commit_hook(db,[](void* p){*static_cast<bool*>(p)=true;return 1;},&hit);
    EXPECT_THROW(vault.service->tipChanged(10),std::runtime_error);sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_TRUE(hit);EXPECT_EQ(cap_vault::EncodeVaultState(vault.service->captureState()),reverted);EXPECT_EQ(sealed(),cipher);
    vault.service->tipChanged(10);EXPECT_EQ(vault.service->accountConfirmed(account),40u);EXPECT_EQ(vault.service->accountPending(account),50u);EXPECT_EQ(vault.service->totalOpenCredits(),50u);
    const auto saved=vault.service->captureState();ASSERT_TRUE(std::holds_alternative<cap_vault::CreditReinstated>(saved.entries.back()));
    const auto encoded=cap_vault::EncodeVaultState(saved);reopen();EXPECT_EQ(cap_vault::EncodeVaultState(vault.service->captureState()),encoded);
    vault.service->tipChanged(10);EXPECT_EQ(vault.service->captureState().entries,saved.entries);EXPECT_EQ(vault.service->totalOpenCredits(),50u);
    orphan=true;vault.service->tipChanged(10);orphan=false;vault.service->tipChanged(10);
    EXPECT_EQ(vault.service->accountConfirmed(account),40u);EXPECT_EQ(vault.service->accountPending(account),50u);EXPECT_EQ(vault.service->totalOpenCredits(),50u);
    EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->submits,0);EXPECT_EQ(ingress->tests,0);
}
TEST_F(VaultCreditReinclusionWallet, SettlementMaturityRequiredBeforeRestoringRevertedPosition) {
    // Deposit 1 is already settled in this fixture. Replace only the source
    // observation depth; the recorded credit remains reverted until mature.
    orphan=true;vault.service->tipChanged(10);const auto entries=vault.service->captureState().entries;
    orphan=false;vault.service->tipChanged(9);EXPECT_EQ(vault.service->captureState().entries,entries);
    EXPECT_EQ(vault.service->accountConfirmed(account),0u);EXPECT_EQ(vault.service->totalOpenCredits(),50u);
    vault.service->tipChanged(10);EXPECT_EQ(vault.service->accountConfirmed(account),40u);EXPECT_EQ(vault.service->totalOpenCredits(),50u);
}
class VaultCreditReinclusionEarly : public WalletBatchRpc {
protected:
    rein_vault::VaultStateDomain domain;rein_vault::BoundVaultService vault;
    rein_vault::AccountId account{"early"};bool included=true;unsigned source_failure=0;size_t requested=0;
    std::array<uint8_t,32> hash{};
    auto backend(){return std::make_unique<rein_vault::InMemorySigningBackend>(rein_vault::BackendId{"reinclusion-fixture"});}
    auto capture(){return [this](uint64_t height,const std::vector<rein_vault::VaultDepositQuery>& queries) {
        requested=queries.size();if(source_failure==1)throw std::runtime_error("fixture source unavailable");
        rein_vault::VaultTipSnapshot out{height,hash,{}};
        for(const auto& q:queries)out.deposits.push_back({q,hash,included});
        if(source_failure==2 && !out.deposits.empty())++out.deposits.back().query.amount;
        if(source_failure==3 && !out.deposits.empty())out.deposits.back().block_hash.reset();
        return out;
    };}
    auto identity(){return dinero::CaptureWalletSigningIdentity(service->get(),"owner");}
    std::string sealed(){
        auto lease=service->get().AcquireDatabaseLease();sqlite3_stmt* raw=nullptr;
        if(sqlite3_prepare_v2(lease->Database(),"SELECT hex(sealed) FROM wallet_vault_states",-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error("reinclusion fixture prepare");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> stmt(raw,sqlite3_finalize);
        if(sqlite3_step(raw)!=SQLITE_ROW)throw std::runtime_error("reinclusion fixture row");
        std::string value(reinterpret_cast<const char*>(sqlite3_column_text(raw,0)),static_cast<size_t>(sqlite3_column_bytes(raw,0)));
        if(sqlite3_step(raw)!=SQLITE_DONE)throw std::runtime_error("reinclusion fixture EOF");return value;
    }
    void SetUp() override {
        dinero::SelectParams(dinero::Chain::REGTEST);WalletBatchRpc::SetUp();if(HasFatalFailure())return;
        domain.network=static_cast<uint8_t>(dinero::GetActiveChain());dinero::uint256 genesis;
        ASSERT_TRUE(dinero::uint256::FromHex(dinero::Params().genesis_hash,genesis));std::copy(genesis.begin(),genesis.end(),domain.genesis.begin());hash.fill(23);
        rein_vault::VaultServiceConfig cfg;cfg.ledger_caps={1000,1000,1000};cfg.confirmation_policy.k_observe=2;cfg.confirmation_policy.k_credit=3;cfg.confirmation_policy.k_settle=6;
        vault=rein_vault::WalletVaultStateOwner::CreateNewService(service,identity().session,domain,cfg,backend(),[this](uint64_t){return hash;},[](const auto&,uint64_t,const auto&){return true;},capture());
        for(uint8_t n=1;n<=3;++n)vault.service->recordDeposit(CapOutpoint(n).txid_raw,0,account,n==1?100:10,n,hash);
        vault.service->tipChanged(3);ASSERT_EQ(vault.service->accountPending(account),100u);ASSERT_EQ(vault.service->totalOpenCredits(),100u);
    }
    void TearDown() override {vault.service.reset();WalletBatchRpc::TearDown();}
    void reopen(){const auto id=vault.identity;vault.service.reset();service->get().open("owner");service->get().unlockWallet("historical-rpc",0);
        vault=rein_vault::WalletVaultStateOwner::OpenExistingService(service,identity().session,domain,id,backend(),[this](uint64_t){return hash;},[](const auto&,uint64_t,const auto&){return true;},capture());}
};
TEST_F(VaultCreditReinclusionEarly, CreditedObservedAndDetectedOwnersResumeWithoutDuplicateCredits) {
    included=false;vault.service->tipChanged(3);EXPECT_EQ(vault.service->accountPending(account),0u);reopen();
    included=true;vault.service->tipChanged(4);EXPECT_EQ(requested,3u);EXPECT_EQ(vault.service->accountConfirmed(account),0u);EXPECT_EQ(vault.service->accountPending(account),10u);
    EXPECT_EQ(vault.service->totalOpenCredits(),10u);vault.service->tipChanged(6);
    EXPECT_EQ(vault.service->accountConfirmed(account),100u);EXPECT_EQ(vault.service->accountPending(account),20u);EXPECT_EQ(vault.service->totalOpenCredits(),20u);
    const auto state=vault.service->captureState();size_t restored=0,observed=0;
    for(const auto& e:state.entries){restored+=std::holds_alternative<rein_vault::CreditReinstated>(e);observed+=std::holds_alternative<rein_vault::DepositObserved>(e);}
    EXPECT_EQ(restored,1u);EXPECT_EQ(observed,3u);const auto saved=rein_vault::EncodeVaultState(state);reopen();
    EXPECT_EQ(rein_vault::EncodeVaultState(vault.service->captureState()),saved);vault.service->tipChanged(6);
    EXPECT_EQ(vault.service->captureState().entries,state.entries);EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->submits,0);
}
TEST_F(VaultCreditReinclusionEarly, UnavailableAndLateMisboundObservationPreserveWholeOwner) {
    included=false;vault.service->tipChanged(3);const auto state=rein_vault::EncodeVaultState(vault.service->captureState());const auto cipher=sealed();
    included=true;
    for(unsigned mode:{1u,2u,3u}) {
        source_failure=mode;
    EXPECT_THROW(vault.service->tipChanged(6),std::runtime_error);EXPECT_EQ(requested,3u);
        EXPECT_EQ(rein_vault::EncodeVaultState(vault.service->captureState()),state);EXPECT_EQ(sealed(),cipher);
    }
    source_failure=0;vault.service->tipChanged(6);EXPECT_EQ(vault.service->accountConfirmed(account),100u);EXPECT_EQ(vault.service->accountPending(account),20u);
}
} // namespace
