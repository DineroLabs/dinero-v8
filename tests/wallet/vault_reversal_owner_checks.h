#pragma once
#include <algorithm>
#include <limits>
#include <variant>
namespace {
namespace reversal_vault = dinero::vault;
struct ReversalOwnerFixture {
    reversal_vault::Ledger ledger;
    reversal_vault::DepositFlowMachine deposits{&ledger};
    const reversal_vault::AccountId account{"reversal-owner"};
    const reversal_vault::OutpointId deposit = CapOutpoint(31);
    std::array<uint8_t,32> original{}, replacement{};
    reversal_vault::ReorgWatcher watcher{
        &deposits, [this](uint64_t) { return replacement; },
        [](const auto&, uint64_t, const auto&) { return false; }};
    explicit ReversalOwnerFixture(bool settled = false) {
        original.fill(31); replacement.fill(32);
        deposits.observe(deposit, account, 100, 1);
        watcher.recordObservation(deposit, original);
        deposits.tipChanged(settled ? 6 : 2);
    }
    void spend(uint64_t amount) {
        if (!amount) return;
        ledger.append(reversal_vault::WithdrawalInitiated{
            ledger.nextSeq(), 0, account, CapOutpoint(32), amount, reversal_vault::BackendId{"fixture"}});
        ledger.append(reversal_vault::WithdrawalSettled{ledger.nextSeq(), 0, account, CapOutpoint(32)});
    }
};

TEST(VaultReversalOwner, UnspentPartlySpentAndFullySpentUseActualDebitWithoutOtherBucket) {
    for (bool settled : {false, true}) for (uint64_t spent : {0u, 60u, 100u}) {
        SCOPED_TRACE(settled);
        SCOPED_TRACE(spent);
        ReversalOwnerFixture fixture(settled); fixture.spend(spent);
        // The companion reversal never debits this opposite bucket. It must
        // therefore neither hide the actual loss nor be included as a debit.
        if (settled) OpenCapCredit(fixture.ledger, fixture.account, 33, 200);
        else fixture.ledger.append(reversal_vault::PolicyAdjustment{
            fixture.ledger.nextSeq(), 0, fixture.account, "separate confirmed funds", 200, 0});
        const auto before = fixture.ledger.entries();
        ASSERT_EQ(fixture.watcher.tipChanged(6), 1);
        const auto& entries = fixture.ledger.entries(); ASSERT_EQ(entries.size(), before.size()+2);
        EXPECT_TRUE(std::equal(before.begin(), before.end(), entries.begin()));
        const auto* compensation = std::get_if<reversal_vault::CompensatingDebit>(&entries.back());
        ASSERT_NE(compensation, nullptr); EXPECT_EQ(compensation->amount, 100u);
        EXPECT_EQ(compensation->operatorLoss, spent);
        EXPECT_EQ(fixture.ledger.totalOperatorLoss(), spent);
        EXPECT_EQ(fixture.ledger.accountOr(fixture.account).operatorLoss(), spent);
        EXPECT_EQ(fixture.ledger.accountOr(fixture.account).pending(), settled ? 200u : 0u);
        EXPECT_EQ(fixture.ledger.accountOr(fixture.account).confirmed(), settled ? 0u : 200u);
        const auto retained = fixture.ledger.reinstatement(fixture.account, fixture.deposit);
        ASSERT_TRUE(retained); EXPECT_EQ(retained->refund, 100-spent);
        EXPECT_EQ(retained->refund+retained->operatorLoss, retained->amount);
        EXPECT_EQ(fixture.deposits.tracked().at(fixture.deposit).stage, reversal_vault::DepositStage::REVERTED);
        const auto saved = entries; EXPECT_EQ(fixture.watcher.tipChanged(6), 0);
        EXPECT_EQ(entries, saved);
        const auto replay = reversal_vault::Ledger::replay(entries);
        EXPECT_EQ(replay.accounts(), fixture.ledger.accounts());
        EXPECT_EQ(replay.totalOpenCredits(), fixture.ledger.totalOpenCredits());
        EXPECT_EQ(replay.totalOperatorLoss(), spent);
    }
}

TEST(VaultReversalOwner, MissingAmountAndStageOwnersRefuseBeforeEitherEntryOrStage) {
    for (unsigned kind=0; kind<4; ++kind) {
        SCOPED_TRACE(kind); ReversalOwnerFixture fixture;
        if (kind==0) fixture.ledger = reversal_vault::Ledger{};
        if (kind==1) {
            fixture.ledger = reversal_vault::Ledger{};
            OpenCapCredit(fixture.ledger, fixture.account, 31, 99);
        }
        if (kind==2) fixture.ledger.append(reversal_vault::CreditSettled{
            fixture.ledger.nextSeq(), 0, fixture.account, fixture.deposit});
        if (kind==3) {
            fixture.ledger = reversal_vault::Ledger{};
            OpenCapCredit(fixture.ledger, reversal_vault::AccountId{"foreign"}, 31, 100);
        }
        const auto entries = fixture.ledger.entries(); const auto accounts = fixture.ledger.accounts();
        const auto next = fixture.ledger.nextSeq(); const auto tracked = fixture.deposits.tracked();
        const auto hashes = fixture.watcher.depositBlockHashes();
        EXPECT_THROW(fixture.watcher.tipChanged(6), reversal_vault::ReorgError);
        EXPECT_EQ(fixture.ledger.entries(), entries); EXPECT_EQ(fixture.ledger.accounts(), accounts);
        EXPECT_EQ(fixture.ledger.nextSeq(), next); EXPECT_EQ(fixture.deposits.tracked(), tracked);
        EXPECT_EQ(fixture.watcher.depositBlockHashes(), hashes);
    }
}

TEST(VaultReversalOwner, SecondEntrySequenceOrLossOverflowPreservesCompletePriorOwner) {
    for (bool loss_overflow : {false, true}) {
        SCOPED_TRACE(loss_overflow); ReversalOwnerFixture fixture; fixture.spend(100);
        if (loss_overflow) {
            // Preserve a historical explicitly-recorded loss near the capacity
            // limit. A new pair must not expose its first entry if adding its
            // derived compensation cannot be represented.
            OpenCapCredit(fixture.ledger, fixture.account, 34, 1, true);
            fixture.ledger.append(reversal_vault::CreditReverted{
                fixture.ledger.nextSeq(), 0, fixture.account, CapOutpoint(34)});
            fixture.ledger.append(reversal_vault::CompensatingDebit{
                fixture.ledger.nextSeq(), 0, fixture.account, CapOutpoint(34), 1,
                std::numeric_limits<uint64_t>::max()});
        } else {
            fixture.ledger.append(reversal_vault::PolicyAdjustment{
                std::numeric_limits<uint64_t>::max()-2, 0, std::nullopt, "sequence boundary", 0, 0});
        }
        const auto entries = fixture.ledger.entries(); const auto accounts = fixture.ledger.accounts();
        const auto next = fixture.ledger.nextSeq(); const auto open = fixture.ledger.totalOpenCredits();
        const auto loss = fixture.ledger.totalOperatorLoss(); const auto tracked = fixture.deposits.tracked();
        EXPECT_THROW(fixture.watcher.tipChanged(6), reversal_vault::ReorgError);
        EXPECT_EQ(fixture.ledger.entries(), entries); EXPECT_EQ(fixture.ledger.accounts(), accounts);
        EXPECT_EQ(fixture.ledger.nextSeq(), next); EXPECT_EQ(fixture.ledger.totalOpenCredits(), open);
        EXPECT_EQ(fixture.ledger.totalOperatorLoss(), loss); EXPECT_EQ(fixture.deposits.tracked(), tracked);
        EXPECT_FALSE(fixture.ledger.reinstatement(fixture.account, fixture.deposit));
    }
}

TEST(VaultReversalOwner, HistoricalRecordedLossRetainedAndNewPairReinstatesExactly) {
    ReversalOwnerFixture fixture(true); fixture.spend(60);
    // Historical explicit compensation is replayed without recalculation.
    const reversal_vault::AccountId old{"historical"}; OpenCapCredit(fixture.ledger, old, 40, 5, true);
    ReverseReinCredit(fixture.ledger, old, 40, 5, 3);
    const auto prefix = fixture.ledger.entries(); ASSERT_EQ(fixture.watcher.tipChanged(6), 1);
    EXPECT_EQ(fixture.ledger.totalOperatorLoss(), 63u);
    const auto retained = fixture.ledger.reinstatement(fixture.account, fixture.deposit);
    ASSERT_TRUE(retained); EXPECT_EQ(retained->refund, 40u); EXPECT_EQ(retained->operatorLoss, 60u);
    fixture.ledger.append(ReinstateEntry(fixture.ledger, fixture.account, fixture.deposit));
    EXPECT_EQ(fixture.ledger.accountOr(fixture.account).confirmed(), 40u);
    EXPECT_EQ(fixture.ledger.totalOperatorLoss(), 3u);
    EXPECT_EQ(fixture.ledger.accountOr(old).operatorLoss(), 3u);
    EXPECT_TRUE(std::equal(prefix.begin(), prefix.end(), fixture.ledger.entries().begin()));
    const auto replay = reversal_vault::Ledger::replay(fixture.ledger.entries());
    EXPECT_EQ(replay.accounts(), fixture.ledger.accounts()); EXPECT_EQ(replay.totalOperatorLoss(), 3u);
    const auto saved = fixture.ledger.entries();
    EXPECT_THROW(fixture.ledger.revertCredit({"missing"}, fixture.deposit, 0), reversal_vault::LedgerError);
    EXPECT_EQ(fixture.ledger.entries(), saved);
}

class VaultReversalOwnerWallet : public VaultRevertedCreditCapWallet {};
TEST_F(VaultReversalOwnerWallet, AuthenticatedWriteAndCommitThenReopenPreserveExactPairAndLoss) {
    const auto before = vault.service->captureState(); const auto bytes = cap_vault::EncodeVaultState(before);
    const auto cipher = sealed(); orphan = true; auto* db = service->get().getCurrentDatabase();
    sql(db, "CREATE TRIGGER deny_reversal_owner BEFORE UPDATE ON wallet_vault_states BEGIN SELECT RAISE(ABORT,'reversal owner refusal'); END");
    EXPECT_THROW(vault.service->tipChanged(10), std::runtime_error);
    sql(db, "DROP TRIGGER deny_reversal_owner");
    EXPECT_EQ(cap_vault::EncodeVaultState(vault.service->captureState()), bytes); EXPECT_EQ(sealed(), cipher);
    bool hit = false; sqlite3_commit_hook(db, [](void* p) { *static_cast<bool*>(p)=true; return 1; }, &hit);
    EXPECT_THROW(vault.service->tipChanged(10), std::runtime_error); sqlite3_commit_hook(db, nullptr, nullptr);
    EXPECT_TRUE(hit); EXPECT_EQ(cap_vault::EncodeVaultState(vault.service->captureState()), bytes); EXPECT_EQ(sealed(), cipher);
    vault.service->tipChanged(10); auto after = vault.service->captureState();
    ASSERT_EQ(after.entries.size(), before.entries.size()+2);
    ASSERT_TRUE(std::holds_alternative<cap_vault::CreditReverted>(after.entries[before.entries.size()]));
    const auto& compensation = std::get<cap_vault::CompensatingDebit>(after.entries.back());
    EXPECT_EQ(compensation.amount, 40u); EXPECT_EQ(compensation.operatorLoss, 0u);
    EXPECT_EQ(vault.service->totalOpenCredits(), 50u); EXPECT_EQ(vault.service->accountPending(account), 50u);
    auto ledger = cap_vault::ReplayVaultStateLedger(after); EXPECT_EQ(ledger.totalOperatorLoss(), 0u);
    ASSERT_TRUE(ledger.reinstatement(account, CapOutpoint(1)));
    EXPECT_EQ(ledger.reinstatement(account, CapOutpoint(1))->refund, 40u);
    const auto saved = cap_vault::EncodeVaultState(after); const auto saved_cipher = sealed(); reopen();
    EXPECT_EQ(cap_vault::EncodeVaultState(vault.service->captureState()), saved); EXPECT_EQ(sealed(), saved_cipher);
    orphan = false; vault.service->tipChanged(10);
    EXPECT_EQ(vault.service->accountConfirmed(account), 40u); EXPECT_EQ(vault.service->accountPending(account), 50u);
    EXPECT_EQ(cap_vault::ReplayVaultStateLedger(vault.service->captureState()).totalOperatorLoss(), 0u);
    EXPECT_TRUE(service->get().getPendingPayments().empty()); EXPECT_EQ(ingress->submits, 0);
}
} // namespace
