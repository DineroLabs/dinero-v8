#pragma once
// Actual state/payment owners in one SQLite transaction, patched paths only.
class VaultPaymentBinding : public VaultRetainedWithdrawal {
protected:
    dinero::vault::WithdrawalId paid() {
        const auto id=enqueue();ingress->submit=[&](const dinero::Transaction& tx){return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
        const auto result=vault.service->processNextWithdrawal();
        if(result!=std::optional<dinero::vault::WithdrawalId>{id})throw std::runtime_error("fixture payment did not retain");
        return id;
    }
    auto open() {
        const auto identity=selected();
        return dinero::vault::WalletVaultStateOwner::OpenExistingService(service,identity.session,domain,vault.identity,
            backend(),[](uint64_t){return std::array<uint8_t,32>{};},[](const auto&,uint64_t,const auto&){return false;},capture(),dispatch_owner->Factory());
    }
    std::string state_envelope() {
        auto* db=service->get().getCurrentDatabase();sqlite3_stmt* raw=nullptr;
        if(sqlite3_prepare_v2(db,"SELECT hex(sealed) FROM wallet_vault_states",-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error("state fixture prepare");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> stmt(raw,sqlite3_finalize);
        if(sqlite3_step(raw)!=SQLITE_ROW)throw std::runtime_error("state fixture row");
        std::string result(reinterpret_cast<const char*>(sqlite3_column_text(raw,0)));
        if(sqlite3_step(raw)!=SQLITE_DONE)throw std::runtime_error("state fixture EOF");return result;
    }
};
TEST_F(VaultPaymentBinding, ReopenBindsSameWalletBodyAndHistoryWithoutWrites) {
    const auto id=paid();const auto expected=retained(id);const auto stored=state_envelope();
    auto* db=service->get().getCurrentDatabase();const auto changes=sqlite3_total_changes(db);
    const auto restored=open();EXPECT_EQ(std::get<dinero::vault::WithdrawalPaymentRetained>(restored.service->withdrawalState(id)),expected);
    EXPECT_EQ(sqlite3_total_changes(db),changes);EXPECT_EQ(state_envelope(),stored);
    EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);
    reopen();EXPECT_EQ(retained(id),expected);EXPECT_EQ(state_envelope(),stored);
}
TEST_F(VaultPaymentBinding, MissingMalformedAndDeniedPaymentOwnerRefusePublication) {
    const auto id=paid();const auto expected=retained(id);auto* db=service->get().getCurrentDatabase();
    const auto original=envelope(db),stored=state_envelope();const auto live=vault.service;
    for(const std::string mutation:{"NULL","X'00'","CAST('invalid' AS TEXT)"}) {
        sql(db,"UPDATE wallet_meta SET pending_payment_owner="+mutation+" WHERE id=1");
        EXPECT_THROW(open(),std::runtime_error);EXPECT_EQ(vault.service,live);EXPECT_EQ(state_envelope(),stored);
        EXPECT_EQ(std::get<dinero::vault::WithdrawalPaymentRetained>(vault.service->withdrawalState(id)),expected);
        sql(db,"UPDATE wallet_meta SET pending_payment_owner=X'"+original+"' WHERE id=1");
    }
    sqlite3_set_authorizer(db,[](void*,int op,const char* table,const char* column,const char*,const char*) {
        return op==SQLITE_READ && table && column && std::strcmp(table,"wallet_meta")==0 && std::strcmp(column,"pending_payment_owner")==0?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    EXPECT_THROW(open(),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_NO_THROW(open());EXPECT_EQ(state_envelope(),stored);EXPECT_EQ(envelope(db),original);
    EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);
}
TEST_F(VaultPaymentBinding, SuccessorCannotRelabelRetainedBodyFeeOrTerms) {
    const auto id=paid();const auto stored=state_envelope();const auto original=envelope(service->get().getCurrentDatabase());
    for(int mode=0;mode<4;++mode) {
        {
            auto owner=dinero::vault::VaultStateTransaction::OpenExisting(service->get(),selected().session,domain,vault.identity);
            auto next=owner->Current().state;++next.revision;
            auto& row=next.withdrawals.at(0);auto& payment=std::get<dinero::vault::WithdrawalPaymentRetained>(row.state);
            if(mode==0)payment.body_sha256[0]^=1;
            if(mode==1)++payment.fee_una;
            if(mode==2)++row.request.payment_terms->fee_rate_hint;
            if(mode==3)row.request.payment_terms->audit_context="different owner terms";
            EXPECT_NO_THROW(dinero::vault::ReplayVaultStateLedger(next));
            EXPECT_THROW(owner->Stage(next),std::runtime_error);
        }
        EXPECT_EQ(state_envelope(),stored);EXPECT_EQ(envelope(service->get().getCurrentDatabase()),original);
    }
    EXPECT_NO_THROW(open());EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);
}
TEST_F(VaultPaymentBinding, CallerTransactionAndSeedRemainOwnedThroughRead) {
    const auto id=paid();auto& wallet=service->get();const auto original=envelope(wallet.getCurrentDatabase());
    {
        auto lease=wallet.AcquireDatabaseLease();auto seed=lease->CopyRecoverySeed(lease->Session());auto* db=lease->Database();
        EXPECT_THROW(lease->ReadPendingPaymentsInTransaction(*seed),std::runtime_error);
        sql(db,"BEGIN IMMEDIATE");
        const auto payments=lease->ReadPendingPaymentsInTransaction(*seed);ASSERT_EQ(payments.size(),1u);
        ASSERT_TRUE(payments[0].intent.request);EXPECT_EQ(payments[0].intent.request->id,id);EXPECT_FALSE(sqlite3_get_autocommit(db));
        sql(db,"ROLLBACK");EXPECT_TRUE(sqlite3_get_autocommit(db));
        EXPECT_THROW(lease->ReadPendingPaymentsInTransaction(*seed),std::runtime_error);
    }
    EXPECT_EQ(envelope(wallet.getCurrentDatabase()),original);EXPECT_NO_THROW(open());
}
