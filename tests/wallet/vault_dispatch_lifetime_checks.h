#pragma once
// Patched lifecycle paths only. No unsafe-original, race or deadlock controls.
class VaultDispatchLifetime : public VaultRetainedWithdrawal {};
TEST_F(VaultDispatchLifetime, ClosedOwnerRefusesBeforeWalletDispatch) {
    const auto id=enqueue();const auto factory=dispatch_owner->Factory();
    dispatch_owner->Close();EXPECT_NO_THROW(dispatch_owner->Close());
    EXPECT_THROW(dispatch_owner->Factory(),std::runtime_error);
    EXPECT_THROW(factory(vault.identity),std::runtime_error);
    EXPECT_THROW(vault.service->processNextWithdrawal(),std::runtime_error);
    EXPECT_TRUE(std::holds_alternative<dinero::vault::WithdrawalSigning>(vault.service->withdrawalState(id)));
    EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
    reopen();EXPECT_THROW(vault.service->processNextWithdrawal(),std::runtime_error);
    EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
}
TEST_F(VaultDispatchLifetime, CallbackCloseRefusesAndRetainedCommitCompletes) {
    const auto id=enqueue();
    ingress->submit=[&](const dinero::Transaction& tx) {
        EXPECT_THROW(dispatch_owner->Close(),std::logic_error);
        submission(tx,id);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());
    };
    EXPECT_EQ(vault.service->processNextWithdrawal(),std::optional<dinero::vault::WithdrawalId>{id});
    const auto saved=retained(id);EXPECT_EQ(ingress->submits,1);
    EXPECT_NO_THROW(dispatch_owner->Close());reopen();EXPECT_EQ(retained(id),saved);
    EXPECT_FALSE(vault.service->processNextWithdrawal());EXPECT_EQ(ingress->submits,1);
}
TEST_F(VaultDispatchLifetime, DestructionClosesRetainedFactoryAndService) {
    const auto factory=dispatch_owner->Factory();const auto id=enqueue();dispatch_owner.reset();
    EXPECT_THROW(factory(vault.identity),std::runtime_error);
    EXPECT_THROW(vault.service->processNextWithdrawal(),std::runtime_error);
    EXPECT_TRUE(std::holds_alternative<dinero::vault::WithdrawalSigning>(vault.service->withdrawalState(id)));
    EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
}
TEST_F(VaultDispatchLifetime, FailedDispatchReleasesOwnerForClose) {
    const auto id=enqueue();daemon.tx_ingress=nullptr;
    EXPECT_THROW(vault.service->processNextWithdrawal(),std::runtime_error);
    EXPECT_NO_THROW(dispatch_owner->Close());
    EXPECT_NO_THROW(dispatch_owner->Close());
    EXPECT_TRUE(std::holds_alternative<dinero::vault::WithdrawalSigning>(vault.service->withdrawalState(id)));
    EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
}
