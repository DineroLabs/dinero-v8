#include "consensus/active_chain_ancestry.h"
#pragma once
#include "vault/wallet_withdrawal_dispatch.h"
#include "vault/vault_runtime.h"
#include "wallet/wallet_transaction_signer.h"
#include "wallet/transaction_builder.h"
#include "daemon/runtime_delivery_worker.h"
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
namespace {
// Actual selected chain admission/mining/undo, encrypted wallet key and retained
// body, and production vault dispatcher. Funding observation is explicitly
// enrolled through addUTXO from the validated output; this fixture does not
// claim installation of the production wallet notification provider.
struct CanonicalVaultWithdrawalFixture : CanonicalRecoveryFixture {
    vault::VaultStateDomain domain;
    vault::VaultServiceConfig config;
    vault::WithdrawalPaymentTerms terms{1,10000,"canonical withdrawal fixture"};
    vault::AccountId account{"canonical-vault"};
    vault::BoundVaultService bound;
    std::unique_ptr<vault::WalletWithdrawalDispatchOwner> dispatch;
    std::string address;
    std::vector<uint8_t> script;
    CanonicalWalletUTXO funding;
    std::shared_ptr<const RuntimeBlockBody> deposit_block;
    static void Need(bool value){OrchardAdmissionFixture::Require(value);}
    static auto Raw(const uint256& hash){std::array<uint8_t,32> out{};std::copy(hash.begin(),hash.end(),out.begin());return out;}
    auto Selected(){return CaptureWalletSigningIdentity(wallet->get(),"canonical-recovery");}
    auto Backend(){return std::make_unique<vault::InMemorySigningBackend>(vault::BackendId{"canonical-wallet"});}
    auto Dispatcher() {
        dispatch=std::make_unique<vault::WalletWithdrawalDispatchOwner>(execution,wallet,Selected(),domain);
        return dispatch->Factory();
    }
    CanonicalVaultWithdrawalFixture(uint64_t maturity=2,bool bounded=false):CanonicalRecoveryFixture(true) {
        execution.walletName="canonical-recovery";
        auto& manager=wallet->get();address=manager.getNewAddress();script=TransactionBuilder::AddressToScriptPubKey(address);
        Need(script.size()==34);domain.network=static_cast<uint8_t>(GetActiveChain());
        domain.genesis=Raw(uint256::FromHexUnsafe(Params().genesis_hash));
        const OutPoint point(f.blocks[1].vtx[0].GetTxid(),0);const auto& coin=f.replay->ProvenUtxos().at(point);
        Transaction donation;donation.version=2;donation.vin.emplace_back();donation.vin[0].prevout.txid=point.txid;donation.vin[0].prevout.vout=point.vout;donation.vin[0].sequence=0xfffffffd;
        constexpr uint64_t fee=100000,amount=100000;Need(coin.value.GetUna()>fee+amount);
        donation.vout.emplace_back(AmountUna::Una(amount),script);
        donation.vout.emplace_back(AmountUna::Una(coin.value.GetUna()-fee-amount),f.script);
        const auto digest=consensus::ScriptVerifier::ComputeTaprootSighash(donation,0,{coin.value.GetUna()},{coin.scriptPubKey});Need(digest.size()==32);
        std::unique_ptr<secp256k1_context,decltype(&secp256k1_context_destroy)> signing(secp256k1_context_create(SECP256K1_CONTEXT_NONE),secp256k1_context_destroy);
        secp256k1_keypair pair;Need(secp256k1_keypair_create(signing.get(),&pair,f.secret.data()));
        std::vector<uint8_t> signature(64);std::array<uint8_t,32> aux{};
        Need(secp256k1_schnorrsig_sign32(signing.get(),signature.data(),digest.data(),&pair,aux.data()));donation.vin[0].witness={signature};
        deposit_block=Mine(MempoolTransaction(donation));
        const auto observed=f.service->getCanonicalOutputInclusion(donation.GetTxid().AsUint256(),0,102);
        Need(observed.ok() && observed->MatchesTransparent(amount,script));
        funding.txid=donation.GetTxid().AsUint256();funding.vout=0;funding.value=AmountUna::Una(amount);funding.height=102;funding.spk=script;
        const auto path=manager.getDerivationPath(util::hex(script));Need(bool(path));funding.path=*path;
        Need(manager.addUTXO(funding.GetTxIdHex(),0,amount,address,util::hex(script),102,false));manager.setBlockchainHeight(102);
        config.operator_binding=vault::VaultOperatorBinding{script,account.raw};
        config.confirmation_policy.k_observe=1;config.confirmation_policy.k_credit=1;config.confirmation_policy.k_settle=1;
        config.withdrawal_policy.k_settle=maturity;
        if(bounded){config.withdrawal_caps.per_request=20000;config.withdrawal_caps.per_account_outstanding=20000;config.withdrawal_caps.global_queue_depth=1;}
        const auto hash=vault::MakeChainstateBlockHashClosure(context);const auto included=vault::MakeChainstateTxIncludedClosure(context);
        bound=vault::WalletVaultStateOwner::CreateNewService(wallet,Selected().session,domain,config,Backend(),hash,
            [included](const auto& out,uint64_t h,const auto& block){return included(out.txid_raw,out.vout,h,block);},
            vault::MakeChainstateVaultSnapshotClosure(context),Dispatcher());
        bound.service->recordDeposit(Raw(funding.txid),0,account,amount,102,Raw(observed->block_hash));bound.service->tipChanged(102);
        Need(bound.service->accountSpendable(account)==amount);
    }
    ~CanonicalVaultWithdrawalFixture(){dispatch.reset();bound.service.reset();}
    void Reopen() {
        const auto identity=bound.identity;dispatch.reset();bound.service.reset();
        wallet->get().open("canonical-recovery");wallet->get().unlockWallet("canonical-fixture-pass",0);
        const auto hash=vault::MakeChainstateBlockHashClosure(context);const auto included=vault::MakeChainstateTxIncludedClosure(context);
        bound=vault::WalletVaultStateOwner::OpenExistingService(wallet,Selected().session,domain,identity,Backend(),hash,
            [included](const auto& out,uint64_t h,const auto& block){return included(out.txid_raw,out.vout,h,block);},
            vault::MakeChainstateVaultSnapshotClosure(context),Dispatcher());
    }
    auto Retain() {
        const auto id=bound.service->enqueueWithdrawal(account,20000,script,terms);
        UnsignedTransaction input;input.tx.version=2;input.tx.vin.emplace_back();input.tx.vin[0].prevout.txid=TxId(funding.txid);input.tx.vin[0].prevout.vout=0;input.tx.vin[0].sequence=UINT32_MAX;
        input.tx.vout.emplace_back(AmountUna::Una(20000),script);input.tx.vout.emplace_back(AmountUna::Una(79000),script);
        input.selected_utxos={funding};input.fee=1000;input.change_amount=79000;input.change_address=address;
        PendingPaymentIntent intent;intent.address=address;intent.amount_una=20000;
        PendingPaymentRequest request;request.owner=bound.identity;request.id=id;request.fee_rate_hint=terms.fee_rate_hint;
        request.maximum_fee_una=terms.maximum_fee_una;request.audit_context=terms.audit_context;intent.request=request;
        const auto result=SignAndStageWalletPayment(wallet->get(),Selected(),input,intent);
        if(!result.success)throw std::runtime_error("canonical vault signing: "+result.error);
        Need(bound.service->processNextWithdrawal()==std::optional<vault::WithdrawalId>{id});
        Need(std::holds_alternative<vault::WithdrawalPaymentRetained>(bound.service->withdrawalState(id)));
        return std::pair{id,MempoolTransaction(result.signed_tx.tx)};
    }
    auto Sources(const vault::WithdrawalId& id) {
        return vault::ReplayVaultStateLedger(bound.service->captureState()).creditAllocations().reservations().at(id).sources;
    }
    auto Bytes(){return vault::EncodeVaultState(bound.service->captureState());}
    void Observe(){bound.service->tipChanged(f.service->GetActiveTip()->height);}
    void Disconnect(){Need(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.service,f.service->GetActiveTip()));}
};
}
TEST(VaultCanonicalWithdrawal, ActualInclusionMaturityUndoReopenAndReinclusionKeepExactSources) {
    CanonicalVaultWithdrawalFixture f;const auto [id,body]=f.Retain();const auto sources=f.Sources(id);
    const auto retained=f.wallet->get().getPendingPayments().at(0).signed_body;
    const auto included=f.Mine(body);f.Observe();
    EXPECT_TRUE(std::holds_alternative<vault::WithdrawalPaymentRetained>(f.bound.service->withdrawalState(id)));
    EXPECT_EQ(f.bound.service->accountLocked(f.account),20000u);f.MineEmpty();f.Observe();
    const auto confirmed=std::get<vault::WithdrawalPaymentConfirmed>(f.bound.service->withdrawalState(id));
    EXPECT_EQ(confirmed.inclusion.height,103u);EXPECT_EQ(confirmed.inclusion.block_hash,f.Raw(included->Orchard().Header().GetHash()));
    EXPECT_EQ(f.bound.service->accountConfirmed(f.account),80000u);EXPECT_EQ(f.bound.service->accountLocked(f.account),0u);
    const auto settled=f.Bytes();f.Reopen();EXPECT_EQ(f.Bytes(),settled);const auto entries=f.bound.service->captureState().entries;f.Observe();EXPECT_EQ(f.Sources(id),sources);EXPECT_EQ(f.bound.service->captureState().entries,entries);
    f.Disconnect();f.Observe();EXPECT_TRUE(std::holds_alternative<vault::WithdrawalPaymentConfirmed>(f.bound.service->withdrawalState(id)));
    f.Disconnect();f.Observe();EXPECT_TRUE(std::holds_alternative<vault::WithdrawalPaymentRetained>(f.bound.service->withdrawalState(id)));
    EXPECT_EQ(f.bound.service->accountConfirmed(f.account),100000u);EXPECT_EQ(f.bound.service->accountLocked(f.account),20000u);
    EXPECT_EQ(f.bound.service->accountSpendable(f.account),80000u);EXPECT_EQ(f.Sources(id),sources);
    const auto undone=f.Bytes();f.Reopen();EXPECT_EQ(f.Bytes(),undone);EXPECT_EQ(f.wallet->get().getPendingPayments().at(0).signed_body,retained);
    const auto result=f.Submit(included->Orchard().WireBytes());ASSERT_TRUE(result.accepted()&&result.connected)<<result.reason;
    ASSERT_GT(f.f.service->GetActiveTip()->height,result.height);
    uint256 canonical_hash;
    ASSERT_TRUE(consensus::GetActiveChainHashAtHeight(f.f.service->GetActiveTip(),uint32_t(result.height),canonical_hash));
    EXPECT_EQ(canonical_hash,included->Orchard().Header().GetHash());
    const auto selected_tip=f.f.service->GetActiveTip()->hash;
    const auto repeated=f.Submit(included->Orchard().WireBytes());
    ASSERT_TRUE(repeated.accepted()&&repeated.connected)<<repeated.reason;
    EXPECT_EQ(repeated.block_hash,result.block_hash);EXPECT_EQ(repeated.height,result.height);
    EXPECT_EQ(f.f.service->GetActiveTip()->hash,selected_tip);
    f.MineEmpty();f.Observe();EXPECT_TRUE(std::holds_alternative<vault::WithdrawalPaymentConfirmed>(f.bound.service->withdrawalState(id)));
    EXPECT_EQ(f.Sources(id),sources);EXPECT_EQ(f.bound.service->accountConfirmed(f.account),80000u);
    EXPECT_EQ(f.wallet->get().getPendingPayments().at(0).signed_body,retained);
}
TEST(VaultCanonicalWithdrawal, RestoredReservationCanExceedAdmissionCapsButNewEnqueueStillRefuses) {
    CanonicalVaultWithdrawalFixture f(1,true);const auto [id,body]=f.Retain();const auto sources=f.Sources(id);
    const auto included=f.Mine(body);f.Observe();ASSERT_TRUE(std::holds_alternative<vault::WithdrawalPaymentConfirmed>(f.bound.service->withdrawalState(id)));
    const auto later=f.bound.service->enqueueWithdrawal(f.account,20000,f.script,f.terms);
    EXPECT_EQ(f.bound.service->withdrawalQueueDepth(),1);f.Disconnect();f.Observe();
    EXPECT_EQ(f.bound.service->withdrawalQueueDepth(),2);EXPECT_EQ(f.bound.service->accountLocked(f.account),40000u);
    EXPECT_TRUE(std::holds_alternative<vault::WithdrawalPending>(f.bound.service->withdrawalState(later)));
    EXPECT_EQ(f.Sources(id),sources);const auto restored=f.Bytes();f.Reopen();EXPECT_EQ(f.Bytes(),restored);
    EXPECT_THROW(f.bound.service->enqueueWithdrawal(f.account,1,f.script,f.terms),std::runtime_error);EXPECT_EQ(f.Bytes(),restored);
    EXPECT_EQ(f.wallet->get().getPendingPayments().size(),1u);
    const auto result=f.Submit(included->Orchard().WireBytes());ASSERT_TRUE(result.accepted()&&result.connected)<<result.reason;
    f.Observe();EXPECT_EQ(f.bound.service->withdrawalQueueDepth(),1);EXPECT_EQ(f.bound.service->accountLocked(f.account),20000u);
    EXPECT_EQ(f.Sources(id),sources);
}
TEST(VaultCanonicalWithdrawal, MissingOptionalIndexPreservesConfirmedAnchorAndRetainedBody) {
    CanonicalVaultWithdrawalFixture f(1);const auto [id,body]=f.Retain();const auto included=f.Mine(body);f.Observe();
    const auto confirmed=f.bound.service->withdrawalState(id);const auto sources=f.Sources(id);
    const auto location=f.f.db.getTxLocation(body.GetTxid().AsUint256());ASSERT_TRUE(location.ok());
    ASSERT_EQ(f.f.db.deleteTxIndex(f.f.token,body.GetTxid().AsUint256()),Status::Ok);
    f.Observe();EXPECT_EQ(f.bound.service->withdrawalState(id),confirmed);EXPECT_EQ(f.Sources(id),sources);
    EXPECT_EQ(f.bound.service->accountLocked(f.account),0u);f.Reopen();f.Observe();EXPECT_EQ(f.bound.service->withdrawalState(id),confirmed);
    ASSERT_EQ(f.f.db.putTxIndex(f.f.token,body.GetTxid().AsUint256(),location->first,location->second),Status::Ok);
    EXPECT_EQ(f.wallet->get().getPendingPayments().at(0).signed_body,body.Serialize());
}
TEST(VaultCanonicalWithdrawal, SqlAndCommitRefusalKeepUnpublishedWithdrawalAndCanonicalLease) {
    CanonicalVaultWithdrawalFixture f(1);const auto [id,body]=f.Retain();(void)f.Mine(body);
    const auto before=f.Bytes();auto* db=f.wallet->get().getCurrentDatabase();
    ASSERT_EQ(sqlite3_exec(db,"CREATE TRIGGER refuse_canonical_withdrawal BEFORE UPDATE ON wallet_vault_states BEGIN SELECT RAISE(ABORT,'fixture canonical refusal'); END",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW(f.Observe(),std::runtime_error);EXPECT_EQ(f.Bytes(),before);
    ASSERT_EQ(sqlite3_exec(db,"DROP TRIGGER refuse_canonical_withdrawal",nullptr,nullptr,nullptr),SQLITE_OK);
    struct Hook {ChainstateService& source;bool called=false,selected=false;};Hook hook{*f.f.service};
    sqlite3_commit_hook(db,[](void* p){auto& h=*static_cast<Hook*>(p);h.called=true;h.selected=ShieldedStateStartupTestAccess::VaultSelectedHeld(h.source);return 1;},&hook);
    EXPECT_THROW(f.Observe(),std::runtime_error);sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_TRUE(hook.called);EXPECT_TRUE(hook.selected);EXPECT_EQ(f.Bytes(),before);
    f.Reopen();EXPECT_EQ(f.Bytes(),before);f.Observe();EXPECT_TRUE(std::holds_alternative<vault::WithdrawalPaymentConfirmed>(f.bound.service->withdrawalState(id)));
    EXPECT_EQ(f.wallet->get().getPendingPayments().at(0).signed_body,body.Serialize());
}
TEST(VaultCanonicalWithdrawal, WrongIndexedBlockOrRequestedTipNeverPublishesInclusion) {
    CanonicalVaultWithdrawalFixture f(1);const auto [id,body]=f.Retain();(void)f.Mine(body);
    const auto before=f.Bytes();const auto location=f.f.db.getTxLocation(body.GetTxid().AsUint256());ASSERT_TRUE(location.ok());
    ASSERT_EQ(f.f.db.putTxIndex(f.f.token,body.GetTxid().AsUint256(),f.deposit_block->Orchard().Header().GetHash(),1),Status::Ok);
    EXPECT_THROW(f.Observe(),std::runtime_error);EXPECT_EQ(f.Bytes(),before);
    ASSERT_EQ(f.f.db.putTxIndex(f.f.token,body.GetTxid().AsUint256(),location->first,location->second),Status::Ok);
    EXPECT_THROW(f.bound.service->tipChanged(102),std::runtime_error);EXPECT_EQ(f.Bytes(),before);
    f.Observe();EXPECT_TRUE(std::holds_alternative<vault::WithdrawalPaymentConfirmed>(f.bound.service->withdrawalState(id)));
    EXPECT_EQ(f.wallet->get().getPendingPayments().at(0).signed_body,body.Serialize());
}
TEST(VaultCanonicalWithdrawal, RuntimeWorkerObservesAttributedPaymentOnceAtUnchangedTip) {
    CanonicalVaultWithdrawalFixture f(1);const auto [id,body]=f.Retain();const auto sources=f.Sources(id);
    (void)f.Mine(body);const auto retained=f.wallet->get().getPendingPayments().at(0).signed_body;
    // Attach the same authenticated owner through the actual runtime path.
    // Ordinary worker Start/pass/Stop only; no race or churn control.
    struct Attachment {
        explicit Attachment(CanonicalVaultWithdrawalFixture& fixture) {
            vault::ShutdownVaultRuntime();const auto identity=fixture.bound.identity;
            fixture.dispatch.reset();fixture.bound.service.reset();
            vault::VaultRuntimeConfig config;config.enabled=true;
            config.block_hash_at_height=vault::MakeChainstateBlockHashClosure(fixture.context);
            config.tx_included_at=vault::MakeChainstateTxIncludedClosure(fixture.context);
            config.capture_tip=vault::MakeChainstateVaultSnapshotClosure(fixture.context);
            vault::OpenExistingVaultRuntime(config,fixture.execution,fixture.wallet,fixture.Selected(),identity);
            fixture.bound.service=vault::GetVaultRuntimeService();CanonicalVaultWithdrawalFixture::Need(bool(fixture.bound.service));
        }
        ~Attachment(){vault::ShutdownVaultRuntime();}
    } attached(f);
    RuntimeDeliveryWorker worker(f.f.service,nullptr,RuntimeDeliveryWorker::Limits{1,std::chrono::minutes(1)});
    const auto wait_pass=[&](uint64_t previous) {
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);auto report=worker.Snapshot();
        while(report.slices<=previous && report.running && std::chrono::steady_clock::now()<end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));report=worker.Snapshot();
        }
        EXPECT_GT(report.slices,previous);return report;
    };
    worker.Start();const auto first=wait_pass(0);
    ASSERT_EQ(first.vault,RuntimeDeliveryWorker::VaultOutcome::ObservedTip);
    EXPECT_EQ(first.vault_height,103u);EXPECT_EQ(first.vault_revision,f.bound.service->currentRevision());
    ASSERT_TRUE(std::holds_alternative<vault::WithdrawalPaymentConfirmed>(f.bound.service->withdrawalState(id)));
    EXPECT_EQ(f.bound.service->accountConfirmed(f.account),80000u);EXPECT_EQ(f.bound.service->accountLocked(f.account),0u);
    const auto saved=f.Bytes();const auto revision=f.bound.service->currentRevision();
    worker.RequestReplay();const auto second=wait_pass(first.slices);worker.Stop();
    EXPECT_EQ(second.vault,RuntimeDeliveryWorker::VaultOutcome::UnchangedSinceObservation);
    EXPECT_EQ(second.vault_revision,revision);EXPECT_EQ(f.bound.service->currentRevision(),revision);
    EXPECT_EQ(f.Bytes(),saved);EXPECT_EQ(f.Sources(id),sources);
    EXPECT_EQ(f.wallet->get().getPendingPayments().at(0).signed_body,retained);
}
} // namespace dinero
#endif
