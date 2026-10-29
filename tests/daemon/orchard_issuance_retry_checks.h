#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
#include "rpc/orchard_issuance_retry.h"
namespace dinero {
TEST(OrchardIssuanceRetry, CreationRecapturesCatalogAndPreservesOtherAccount) {
    OrchardCatalogRecoveryFixture f;
    auto source_use=ChainstateService::AcquireWalletIndexUse(f.f.service);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    const auto session=f.Session();unsigned attempts=0;bool changed=false;
    std::vector<uint8_t> other_bytes;uint64_t other_revision=0;
    const auto issued=rpc::detail::RetryOrchardIssuance([&] {
        ++attempts;
        const auto view=f.f.service->getRuntimeAccountReplayForWallet(wallet,session);
        OrchardAdmissionFixture::Require(view.ok());
        const auto provider=[&](RuntimeOutboxCursor cursor) {
            EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
            EXPECT_TRUE(sqlite3_get_autocommit(wallet.getCurrentDatabase()));
            if(!changed) {
                changed=true;const auto other=f.Call(17);OrchardAdmissionFixture::Require(!other.isMember("error"));
                const auto state=f.Account(17);other_bytes=OrchardCreationBytes(state.account);other_revision=state.revision;
            }
            return (*view)->Point(cursor);
        };
        return wallet::OrchardDetachedIssuanceTestAccess::Issue(wallet,session,{f.Domain(),102,3},**view,true,provider);
    });
    EXPECT_EQ(attempts,2u);EXPECT_EQ(issued.revision,1u);ASSERT_EQ(f.Catalog()->accounts.size(),2u);
    const auto expected=wallet::OrchardAccountState::Begin(f.Domain(),f.AccountKeys(3).ExportFullViewingKey(),102,f.parent->hash)
        .IssueReceiver(orchard::WalletScope::External);
    EXPECT_EQ(issued.address,expected.second.EncodeAddress(orchard::WalletNetwork::Regtest));
    EXPECT_EQ(OrchardCreationBytes(f.Account(3).account),OrchardCreationBytes(expected.first));
    EXPECT_EQ(f.Account(17).revision,other_revision);EXPECT_EQ(OrchardCreationBytes(f.Account(17).account),other_bytes);
}
TEST(OrchardIssuanceRetry, ReceiverRetriesWithoutSkippingOrOverwriting) {
    OrchardCatalogRecoveryFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));
    auto source_use=ChainstateService::AcquireWalletIndexUse(f.f.service);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();const auto session=f.Session();
    const auto before=f.Account(3),other_before=f.Account(17);const auto expected=before.account.IssueReceiver(orchard::WalletScope::External);
    unsigned attempts=0;bool changed=false;std::vector<uint8_t> other_bytes;
    const auto issued=rpc::detail::RetryOrchardIssuance([&] {
        ++attempts;const auto view=f.f.service->getRuntimeAccountReplayForWallet(wallet,session);OrchardAdmissionFixture::Require(view.ok());
        const auto provider=[&](RuntimeOutboxCursor cursor) {
            EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
            if(!changed) {changed=true;OrchardAdmissionFixture::Require(!f.Issue(17).isMember("error"));other_bytes=OrchardCreationBytes(f.Account(17).account);}
            return (*view)->Point(cursor);
        };
        return wallet::OrchardDetachedIssuanceTestAccess::Issue(wallet,session,{f.Domain(),102,3},**view,false,provider);
    });
    EXPECT_EQ(attempts,2u);EXPECT_EQ(issued.revision,before.revision+1);
    EXPECT_EQ(issued.address,expected.second.EncodeAddress(orchard::WalletNetwork::Regtest));
    EXPECT_EQ(OrchardCreationBytes(f.Account(3).account),OrchardCreationBytes(expected.first));
    EXPECT_EQ(f.Account(17).revision,other_before.revision+1);EXPECT_EQ(OrchardCreationBytes(f.Account(17).account),other_bytes);
}
TEST(OrchardIssuanceRetry, ExhaustionPreservesRequestedAccountAndStableRetry) {
    OrchardCatalogRecoveryFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));
    auto source_use=ChainstateService::AcquireWalletIndexUse(f.f.service);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();const auto session=f.Session();
    const auto before=f.Account(3),other_before=f.Account(17);unsigned attempts=0;bool returned=false;
    EXPECT_THROW({
        (void)rpc::detail::RetryOrchardIssuance([&] {
            ++attempts;bool changed=false;const auto view=f.f.service->getRuntimeAccountReplayForWallet(wallet,session);OrchardAdmissionFixture::Require(view.ok());
            const auto provider=[&](RuntimeOutboxCursor cursor) {
                EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
                if(!changed){changed=true;OrchardAdmissionFixture::Require(!f.Issue(17).isMember("error"));}
                return (*view)->Point(cursor);
            };
            return wallet::OrchardDetachedIssuanceTestAccess::Issue(wallet,session,{f.Domain(),102,3},**view,false,provider);
        });returned=true;
    },wallet::OrchardAccountDelivery::CatalogChanged);
    EXPECT_FALSE(returned);EXPECT_EQ(attempts,4u);EXPECT_EQ(f.Account(3).revision,before.revision);
    EXPECT_EQ(OrchardCreationBytes(f.Account(3).account),OrchardCreationBytes(before.account));EXPECT_EQ(f.Account(17).revision,other_before.revision+4);
    const auto expected=before.account.IssueReceiver(orchard::WalletScope::External);
    const auto stable=f.Issue(3);ASSERT_FALSE(stable.isMember("error"));
    EXPECT_EQ(stable["address"].asString(),expected.second.EncodeAddress(orchard::WalletNetwork::Regtest));
    EXPECT_EQ(f.Account(3).revision,before.revision+1);EXPECT_EQ(OrchardCreationBytes(f.Account(3).account),OrchardCreationBytes(expected.first));
}
TEST(OrchardIssuanceRetry, SqlCommitAndSessionFailuresAreNotRetried) {
    OrchardCatalogRecoveryFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));
    auto source_use=ChainstateService::AcquireWalletIndexUse(f.f.service);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();const auto session=f.Session();auto* db=wallet.getCurrentDatabase();
    const auto before=f.Snapshot();unsigned attempts=0;
    const auto call=[&](uint64_t captured_session) {
        return rpc::detail::RetryOrchardIssuance([&] {
            ++attempts;const auto view=f.f.service->getRuntimeAccountReplay();OrchardAdmissionFixture::Require(view.ok());
            const auto provider=[&](RuntimeOutboxCursor cursor){return (*view)->Point(cursor);};
            return wallet::OrchardDetachedIssuanceTestAccess::Issue(wallet,captured_session,{f.Domain(),102,3},**view,false,provider);
        });
    };
    {DetachedCatalogSqlHooks hooks(db);hooks.deny_final_read=true;EXPECT_THROW((void)call(session),std::runtime_error);}
    EXPECT_EQ(attempts,1u);EXPECT_EQ(f.Snapshot(),before);attempts=0;
    {DetachedCatalogSqlHooks hooks(db);hooks.refuse_final_commit=true;EXPECT_THROW((void)call(session),std::runtime_error);}
    EXPECT_EQ(attempts,1u);EXPECT_EQ(f.Snapshot(),before);attempts=0;
    EXPECT_THROW((void)call(session+1),std::runtime_error);EXPECT_EQ(attempts,1u);EXPECT_EQ(f.Snapshot(),before);
    EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
}
} // namespace dinero
#else
TEST(OrchardIssuanceRetry, BackendOffRefusesIssuance) {
    ExecutionContext context;din::Json params;params["account"]=Json::UInt(3);
    EXPECT_TRUE(rpc_context_wallet_orchard_createaccount(context,params).isMember("error"));
    EXPECT_TRUE(rpc_context_wallet_orchard_getnewaddress(context,params).isMember("error"));
}
#endif
