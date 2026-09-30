#pragma once
class VaultReservationMetrics : public VaultRetainedWithdrawal {
protected:
    void metrics(uint64_t spendable,uint64_t locked) {
        const auto value=vault.service->accountMetrics(account);
        EXPECT_EQ(value.spendable,spendable);EXPECT_EQ(value.locked,locked);
        EXPECT_EQ(value.confirmed,100000u);EXPECT_EQ(value.pending,0u);EXPECT_EQ(value.operator_loss,0u);
        EXPECT_EQ(vault.service->accountSpendable(account),spendable);
        EXPECT_EQ(vault.service->accountLocked(account),locked);
        EXPECT_EQ(value.spendable+value.locked,value.confirmed+value.pending);
    }
};
TEST_F(VaultReservationMetrics, PendingReservationsReduceSpendableAndSurviveReopen) {
    metrics(100000,0);(void)enqueue(60000);metrics(40000,60000);
    const auto saved=dinero::vault::EncodeVaultState(vault.service->captureState());
    EXPECT_THROW(enqueue(40001),std::runtime_error);metrics(40000,60000);
    EXPECT_EQ(dinero::vault::EncodeVaultState(vault.service->captureState()),saved);
    (void)enqueue(40000);metrics(0,100000);reopen();metrics(0,100000);
    const auto other=vault.service->accountMetrics(dinero::vault::AccountId{"not-owned"});
    EXPECT_EQ(other.spendable,0u);EXPECT_EQ(other.locked,0u);
    EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
}
TEST_F(VaultReservationMetrics, SigningWithoutBodyRemainsReserved) {
    const auto id=enqueue();metrics(80000,20000);daemon.tx_ingress=nullptr;
    EXPECT_THROW(vault.service->processNextWithdrawal(),std::runtime_error);
    EXPECT_TRUE(std::holds_alternative<dinero::vault::WithdrawalSigning>(vault.service->withdrawalState(id)));
    metrics(80000,20000);reopen();daemon.tx_ingress=ingress.get();metrics(80000,20000);
    EXPECT_THROW(vault.service->processNextWithdrawal(),std::runtime_error);metrics(80000,20000);
    EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
}
TEST_F(VaultReservationMetrics, RetainedBodyTransfersReservationWithoutDoubleCounting) {
    const auto first=enqueue();(void)enqueue(30000);metrics(50000,50000);
    ingress->submit=[&](const dinero::Transaction& tx) {
        EXPECT_TRUE(std::holds_alternative<dinero::vault::WithdrawalSigning>(vault.service->withdrawalState(first)));
        metrics(50000,50000);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());
    };
    EXPECT_EQ(vault.service->processNextWithdrawal(),std::optional<dinero::vault::WithdrawalId>{first});
    EXPECT_TRUE(std::holds_alternative<dinero::vault::WithdrawalPaymentRetained>(vault.service->withdrawalState(first)));
    metrics(50000,50000);reopen();metrics(50000,50000);
    EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);
}
TEST_F(VaultReservationMetrics, RefusedEnqueuePreservesMetricsAndReadDoesNotWrite) {
    auto* db=service->get().getCurrentDatabase();const auto saved=dinero::vault::EncodeVaultState(vault.service->captureState());
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*) {
        return action==SQLITE_UPDATE && table && std::strcmp(table,"wallet_vault_states")==0?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    EXPECT_THROW(enqueue(),std::exception);sqlite3_set_authorizer(db,nullptr,nullptr);metrics(100000,0);
    sqlite3_commit_hook(db,[](void*){return 1;},nullptr);
    EXPECT_THROW(enqueue(),std::exception);sqlite3_commit_hook(db,nullptr,nullptr);metrics(100000,0);
    EXPECT_EQ(dinero::vault::EncodeVaultState(vault.service->captureState()),saved);
    (void)enqueue();const auto changes=sqlite3_total_changes(db);metrics(80000,20000);
    EXPECT_EQ(sqlite3_total_changes(db),changes);EXPECT_EQ(sqlite3_get_autocommit(db),1);
    EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
}
