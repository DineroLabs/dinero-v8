#pragma once
#include "daemon/runtime_delivery_worker.h"
#include "daemon/runtime_block_reader.h"
#include "daemon/runtime_notification_composition.h"
#include "vault/vault_runtime.h"
#include "vault/state_snapshot.h"
namespace {
class VaultTipWorker : public WalletBatchRpc {
protected:
    using Worker=dinero::RuntimeDeliveryWorker;
    using Outcome=Worker::VaultOutcome;
    std::unique_ptr<PoolPaymentChainFixture> canonical;
    dinero::vault::VaultStateDomain domain;
    dinero::vault::VaultIdentity identity{};
    dinero::vault::AccountId account{"actual-operator"};
    std::unique_ptr<Worker> worker;
    std::shared_ptr<dinero::vault::VaultService> attached;
    auto selected(){return dinero::CaptureWalletSigningIdentity(service->get(),"owner");}
    dinero::vault::VaultRuntimeConfig runtime_config() {
        dinero::vault::VaultRuntimeConfig config;config.enabled=true;
        config.block_hash_at_height=dinero::vault::MakeChainstateBlockHashClosure(daemon);
        config.tx_included_at=dinero::vault::MakeChainstateTxIncludedClosure(daemon);
        config.capture_tip=dinero::vault::MakeChainstateVaultSnapshotClosure(daemon);return config;
    }
    void open() {
        dinero::vault::OpenExistingVaultRuntime(runtime_config(),ctx,service,selected(),identity);
        attached=dinero::vault::GetVaultRuntimeService();if(!attached)throw std::runtime_error("fixture authenticated attachment missing");
    }
    void record(uint32_t height) {
        const auto& block=canonical->blocks.at(height);const auto txid=block.vtx[0].GetTxid().AsUint256(),hash=block.GetHash();
        std::array<uint8_t,32> tx{},id{};std::copy(txid.begin(),txid.end(),tx.begin());std::copy(hash.begin(),hash.end(),id.begin());
        attached->recordDeposit(tx,0,account,20000,height,id);
    }
    std::string sealed() {
        auto lease=service->get().AcquireDatabaseLease();sqlite3_stmt* raw=nullptr;
        if(sqlite3_prepare_v2(lease->Database(),"SELECT hex(sealed) FROM wallet_vault_states",-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error("vault fixture prepare");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> stmt(raw,sqlite3_finalize);
        if(sqlite3_step(raw)!=SQLITE_ROW)throw std::runtime_error("vault fixture row");
        std::string result(reinterpret_cast<const char*>(sqlite3_column_text(raw,0)));
        if(sqlite3_step(raw)!=SQLITE_DONE)throw std::runtime_error("vault fixture EOF");return result;
    }
    Worker::Report wait_pass(uint64_t previous=0) {
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);auto report=worker->Snapshot();
        while(report.slices<=previous && report.running && std::chrono::steady_clock::now()<end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));report=worker->Snapshot();
        }
        EXPECT_GT(report.slices,previous);return report;
    }
    void SetUp() override {
        dinero::vault::ShutdownVaultRuntime();dinero::SelectParams(dinero::Chain::REGTEST);WalletBatchRpc::SetUp();if(HasFatalFailure())return;
        canonical=std::make_unique<PoolPaymentChainFixture>(root/"vault-canonical-source",modern.spk);
        service->get().setUTXOIndex(nullptr);daemon.chainstate.reset();chain.reset();chain=canonical->source;daemon.chainstate=chain;
        domain.network=static_cast<uint8_t>(dinero::GetActiveChain());dinero::uint256 genesis;
        ASSERT_TRUE(dinero::uint256::FromHex(dinero::Params().genesis_hash,genesis));std::copy(genesis.begin(),genesis.end(),domain.genesis.begin());
        dinero::vault::VaultServiceConfig config;config.operator_binding=dinero::vault::VaultOperatorBinding{modern.spk,account.raw};
        config.confirmation_policy.k_observe=1;config.confirmation_policy.k_credit=10;config.confirmation_policy.k_settle=20;
        auto readers=runtime_config();const auto included=readers.tx_included_at;
        auto bound=dinero::vault::WalletVaultStateOwner::CreateNewService(service,selected().session,domain,config,
            std::make_unique<dinero::vault::InMemorySigningBackend>(dinero::vault::BackendId{"actual-vault-tip-fixture"}),
            readers.block_hash_at_height,[included](const auto& out,uint64_t h,const auto& hash){return included(out.txid_raw,out.vout,h,hash);},readers.capture_tip);
        identity=bound.identity;bound.service.reset();
        worker=std::make_unique<Worker>(chain,nullptr,Worker::Limits{1,std::chrono::minutes(1)});
    }
    void TearDown() override {
        if(worker)worker->Stop();worker.reset();dinero::vault::ShutdownVaultRuntime();attached.reset();
        if(service)service->get().setUTXOIndex(nullptr);daemon.chainstate.reset();chain.reset();canonical.reset();WalletBatchRpc::TearDown();
    }
    void expect_settled(size_t count) {
        const auto state=attached->captureState();ASSERT_EQ(state.deposits.size(),count);
        for(const auto& row:state.deposits)EXPECT_EQ(row.deposit.stage,dinero::vault::DepositStage::SETTLED);
        EXPECT_EQ(attached->accountConfirmed(account),count*20000u);EXPECT_TRUE(state.withdrawals.empty());
        EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
    }
};
TEST_F(VaultTipWorker, AttachedOwnerAndChangedRevisionObservedWithoutRepeatedWrites) {
    worker->Start();auto report=wait_pass();EXPECT_EQ(report.vault,Outcome::NoAttachedRuntime);
    open();record(1);const auto initial_revision=attached->currentRevision();worker->RequestReplay();report=wait_pass(report.slices);
    ASSERT_EQ(report.vault,Outcome::ObservedTip);EXPECT_EQ(report.vault_revision,initial_revision+1);EXPECT_EQ(report.vault_height,101u);
    std::array<uint8_t,32> tip{};const auto hash=canonical->blocks.back().GetHash();std::copy(hash.begin(),hash.end(),tip.begin());EXPECT_EQ(report.vault_tip,tip);
    const auto saved=sealed();const auto revision=attached->currentRevision();worker->RequestReplay();report=wait_pass(report.slices);
    EXPECT_EQ(report.vault,Outcome::UnchangedSinceObservation);EXPECT_EQ(attached->currentRevision(),revision);EXPECT_EQ(sealed(),saved);
    record(2);worker->RequestReplay();report=wait_pass(report.slices);worker->Stop();
    EXPECT_EQ(report.vault,Outcome::ObservedTip);EXPECT_EQ(report.vault_revision,revision+2);expect_settled(2);
}
TEST_F(VaultTipWorker, IncoherentSourceAndLockedOwnerDeferWithoutStateEffects) {
    open();record(1);const auto saved=sealed();const auto before=dinero::vault::EncodeVaultState(attached->captureState());
    const auto tip=canonical->db.getTip();ASSERT_TRUE(tip.ok());auto wrong=tip->hash;wrong.begin()[0]^=1;
    {auto selected=chain->AcquireBlockIngressActivationLock();ASSERT_EQ(canonical->db.setTip(canonical->token,wrong,tip->height,tip->work),dinero::Status::Ok);}
    worker->Start();auto report=wait_pass();worker->Stop();EXPECT_EQ(report.vault,Outcome::Deferred);EXPECT_EQ(sealed(),saved);
    EXPECT_EQ(dinero::vault::EncodeVaultState(attached->captureState()),before);
    {auto selected=chain->AcquireBlockIngressActivationLock();ASSERT_EQ(canonical->db.setTip(canonical->token,tip->hash,tip->height,tip->work),dinero::Status::Ok);}
    service->get().lockWallet();worker->Start();report=wait_pass();worker->Stop();EXPECT_EQ(report.vault,Outcome::Deferred);
    EXPECT_EQ(dinero::vault::EncodeVaultState(attached->captureState()),before);EXPECT_EQ(sealed(),saved);
    service->get().unlockWallet("historical-rpc",0);worker->Start();report=wait_pass();worker->Stop();
    EXPECT_EQ(report.vault,Outcome::ObservedTip);expect_settled(1);
}
TEST_F(VaultTipWorker, RequiredStateWriteAndCommitRefuseBeforePublicationThenRetry) {
    open();record(1);const auto saved=sealed();const auto before=dinero::vault::EncodeVaultState(attached->captureState());auto* db=service->get().getCurrentDatabase();
    sql(db,"CREATE TRIGGER refuse_vault_tip BEFORE UPDATE ON wallet_vault_states BEGIN SELECT RAISE(ABORT,'fixture vault tip refusal'); END");
    worker->Start();auto report=wait_pass();worker->Stop();EXPECT_EQ(report.vault,Outcome::Deferred);
    sql(db,"DROP TRIGGER refuse_vault_tip");EXPECT_EQ(sealed(),saved);EXPECT_EQ(dinero::vault::EncodeVaultState(attached->captureState()),before);
    unsigned commits=0;sqlite3_commit_hook(db,[](void* p){++*static_cast<unsigned*>(p);return 1;},&commits);
    worker->Start();report=wait_pass();worker->Stop();sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_EQ(report.vault,Outcome::Deferred);EXPECT_GT(commits,0u);EXPECT_EQ(sealed(),saved);EXPECT_EQ(dinero::vault::EncodeVaultState(attached->captureState()),before);
    worker->Start();report=wait_pass();worker->Stop();EXPECT_EQ(report.vault,Outcome::ObservedTip);expect_settled(1);
}
TEST_F(VaultTipWorker, MailboxStopsBeforeOwnerReleaseAndReopenPreservesLedger) {
    open();record(1);worker->Start();const auto report=wait_pass();ASSERT_EQ(report.vault,Outcome::ObservedTip);
    auto adapter=dinero::MakeRuntimeVaultNotifications(*worker);dinero::RuntimeBlockBody body{canonical->blocks.back()};
    auto token=adapter->Prepare(body,101,dinero::RuntimeBlockDirection::Disconnect);ASSERT_TRUE(token);
    worker->Stop();auto wake=worker->CaptureWakeHandle();worker.reset();auto state=attached->captureState();const auto saved=sealed();
    {auto selected=chain->AcquireBlockIngressActivationLock();token->PublishAfterCommit();token.reset();wake.RequestReplay();}
    EXPECT_FALSE(wake.Running());EXPECT_EQ(sealed(),saved);EXPECT_FALSE(adapter->Prepare(body,101,dinero::RuntimeBlockDirection::Connect));
    dinero::vault::ShutdownVaultRuntime();attached.reset();service->get().open("owner");service->get().unlockWallet("historical-rpc",0);open();
    EXPECT_EQ(sealed(),saved);EXPECT_EQ(dinero::vault::EncodeVaultState(attached->captureState()),dinero::vault::EncodeVaultState(state));
    worker=std::make_unique<Worker>(chain,nullptr,Worker::Limits{1,std::chrono::minutes(1)});worker->Start();const auto reopened=wait_pass();worker->Stop();
    EXPECT_EQ(reopened.vault,Outcome::ObservedTip);expect_settled(1);auto after=attached->captureState();state.revision=after.revision;
    EXPECT_EQ(dinero::vault::EncodeVaultState(after),dinero::vault::EncodeVaultState(state));
}
} // namespace
