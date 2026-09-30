#include "rpc/wallet_request_dispatch.h"
// Actual retained wallet owner and bound handler with synthetic funded coins.
// A domain is not a pool enrollment certificate or authorization to retry send.
class WalletPoolRequestOwner : public WalletRequestOwner {};
TEST_F(WalletPoolRequestOwner, DomainSeparationAndExactReopen) {
    auto& wallet=service->get();const auto vault=request();auto pool=vault;
    pool.request->domain=dinero::PendingPaymentRequestDomain::PoolPayout;
    ASSERT_TRUE(retain(shifted(0),vault).success);const auto original=find(vault);ASSERT_TRUE(original);
    EXPECT_FALSE(find(pool));ASSERT_TRUE(retain(shifted(8),pool).success);
    const auto payment=find(pool);ASSERT_TRUE(payment);EXPECT_NE(payment->txid,original->txid);
    EXPECT_EQ(payment->intent,pool);EXPECT_EQ(find(vault)->signed_body,original->signed_body);EXPECT_EQ(format(),"DNPP03");
    const auto saved=envelope(wallet.getCurrentDatabase());
    wallet.open("owner");wallet.unlockWallet("historical-rpc",0);
    EXPECT_EQ(find(pool)->signed_body,payment->signed_body);EXPECT_EQ(find(vault)->signed_body,original->signed_body);
    EXPECT_EQ(envelope(wallet.getCurrentDatabase()),saved);EXPECT_EQ(wallet.getPendingPayments().size(),2u);
    const auto listing=rpc_context_wallet_listpendingpayments(ctx,din::Json());ASSERT_FALSE(listing.isMember("error"));
    ASSERT_EQ(listing["payments"].size(),2u);
    EXPECT_EQ(listing["payments"][0]["request"]["domain"].asString(),"vault_withdrawal");
    EXPECT_EQ(listing["payments"][1]["request"]["domain"].asString(),"pool_payout");
}
TEST_F(WalletPoolRequestOwner, ConflictingPoolPayloadAndUnknownDomainsRefuse) {
    auto& wallet=service->get();auto intent=request();intent.request->domain=dinero::PendingPaymentRequestDomain::PoolPayout;
    const auto input=shifted(0);
    for(uint64_t domain:{uint64_t{0},uint64_t{3},uint64_t{UINT64_MAX}}) {
        auto invalid=intent;invalid.request->domain=static_cast<dinero::PendingPaymentRequestDomain>(domain);
        EXPECT_THROW(find(invalid),std::runtime_error);EXPECT_FALSE(retain(input,invalid).success);
        EXPECT_TRUE(wallet.getPendingPayments().empty());EXPECT_EQ(count(wallet.getCurrentDatabase(),"transactions"),0);
    }
    ASSERT_TRUE(retain(input,intent).success);const auto saved=envelope(wallet.getCurrentDatabase());
    for(int change=0;change<4;++change) {
        auto other=intent;
        if(change==0)++other.amount_una;
        if(change==1)++other.request->fee_rate_hint;
        if(change==2)++other.request->maximum_fee_una;
        if(change==3)other.request->audit_context="changed pool allocation";
        EXPECT_THROW(find(other),std::runtime_error);EXPECT_FALSE(retain(input,other).success);
        EXPECT_EQ(envelope(wallet.getCurrentDatabase()),saved);
    }
    EXPECT_EQ(wallet.getPendingPayments().size(),1u);EXPECT_EQ(wallet.getLockedUTXOs().size(),2u);
}
class WalletPoolRequestDispatch : public WalletRequestDispatch {};
TEST_F(WalletPoolRequestDispatch, BoundUnknownOutcomeResolvesWithoutAnotherSubmission) {
    auto p=requested();p["request"]["domain"]="pool_payout";p["request"]["audit_context"]="synthetic pool allocation";preflight();
    ingress->submit=[&](const dinero::Transaction& tx)->dinero::TxAcceptResult {
        at_submission(tx);const auto records=service->get().getPendingPayments();
        EXPECT_EQ(records.size(),1u);
        if(!records.empty()) {EXPECT_EQ(records[0].intent.request->domain,dinero::PendingPaymentRequestDomain::PoolPayout);EXPECT_EQ(records[0].intent.request->audit_context,"synthetic pool allocation");}
        throw std::runtime_error("fixture submission outcome unavailable");
    };
    const auto selected=dinero::CaptureWalletSigningIdentity(service->get(),"owner");
    const auto result=dinero::DispatchBoundWalletRequest(ctx,p,service,selected);
    ASSERT_TRUE(result.isMember("error"));EXPECT_TRUE(result["payment_retained"].asBool());
    EXPECT_EQ(result["submission_status"].asString(),"outcome_unknown");
    ASSERT_EQ(ingress->tests,1);ASSERT_EQ(ingress->submits,1);
    auto& wallet=service->get();const auto record=wallet.getPendingPayments().at(0);
    daemon.chainstate.reset();daemon.tx_ingress=nullptr;check_retry(p,record.txid,record.signed_body);
    wallet.open("owner");wallet.unlockWallet("historical-rpc",0);check_retry(p,record.txid,record.signed_body);
    const auto saved=envelope(wallet.getCurrentDatabase());
    const auto stale=dinero::DispatchBoundWalletRequest(ctx,p,service,selected);EXPECT_TRUE(stale.isMember("error"));
    EXPECT_EQ(envelope(wallet.getCurrentDatabase()),saved);EXPECT_EQ(ingress->submits,1);
    const auto current=dinero::CaptureWalletSigningIdentity(wallet,"owner");
    const auto resolved=dinero::DispatchBoundWalletRequest(ctx,p,service,current);
    ASSERT_FALSE(resolved.isMember("error"))<<resolved.toStyledString();EXPECT_EQ(resolved["txid"].asString(),record.txid);
    EXPECT_FALSE(resolved["submitted_this_call"].asBool());EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);
}

class WalletPoolRequestVault : public VaultPaymentBinding {};
TEST_F(WalletPoolRequestVault, VaultBindingKeepsDistinctRequestNamespaces) {
    const auto id=paid();const auto expected=retained(id);auto& wallet=service->get();
    const auto vault_payment=wallet.getPendingPayments().at(0);const auto stored=state_envelope();
    auto intent=vault_payment.intent;ASSERT_TRUE(intent.request);
    intent.request->domain=dinero::PendingPaymentRequestDomain::PoolPayout;
    auto extra=hd;extra.vout+=8;fund(extra);auto input=unsigned_tx({extra});
    input.tx.vout.clear();dinero::TxOutput recipient;
    recipient.value=dinero::AmountUna::Una(intent.amount_una);recipient.scriptPubKey=modern.spk;input.tx.vout.push_back(recipient);
    dinero::TxOutput change;change.value=dinero::AmountUna::Una(extra.value.GetUna()-intent.amount_una-input.fee);
    change.scriptPubKey=modern.spk;input.tx.vout.push_back(change);
    input.change_amount=change.GetValue();input.change_address=modern_address;
    const auto staged=dinero::SignAndStageWalletPayment(wallet,selected(),input,intent);
    ASSERT_TRUE(staged.success)<<staged.error;verify(staged.signed_tx.tx,{extra});
    const auto pool_payment=dinero::FindRetainedWalletPayment(wallet,selected(),intent);ASSERT_TRUE(pool_payment);
    EXPECT_NE(pool_payment->txid,vault_payment.txid);ASSERT_EQ(wallet.getPendingPayments().size(),2u);
    const auto saved=envelope(wallet.getCurrentDatabase());const auto changes=sqlite3_total_changes(wallet.getCurrentDatabase());
    const auto restored=open();
    EXPECT_EQ(std::get<dinero::vault::WithdrawalPaymentRetained>(restored.service->withdrawalState(id)),expected);
    EXPECT_EQ(sqlite3_total_changes(wallet.getCurrentDatabase()),changes);EXPECT_EQ(state_envelope(),stored);
    // A valid pool payment cannot replace the vault payment body even when
    // recipient, amount, fee terms, owner and request ID match exactly.
    {
        auto owner=dinero::vault::VaultStateTransaction::OpenExisting(wallet,selected().session,domain,vault.identity);
        auto next=owner->Current().state;++next.revision;
        auto& payment=std::get<dinero::vault::WithdrawalPaymentRetained>(next.withdrawals.at(0).state);
        SHA256(pool_payment->signed_body.data(),pool_payment->signed_body.size(),payment.body_sha256.data());
        ASSERT_NE(payment.body_sha256,expected.body_sha256);
        EXPECT_NO_THROW(dinero::vault::ReplayVaultStateLedger(next));EXPECT_THROW(owner->Stage(next),std::runtime_error);
    }
    EXPECT_EQ(state_envelope(),stored);EXPECT_EQ(envelope(wallet.getCurrentDatabase()),saved);
    reopen();EXPECT_EQ(retained(id),expected);EXPECT_EQ(state_envelope(),stored);
    EXPECT_EQ(dinero::FindRetainedWalletPayment(wallet,selected(),intent)->signed_body,pool_payment->signed_body);
    EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);
}
