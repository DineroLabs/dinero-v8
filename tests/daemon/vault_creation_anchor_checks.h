#pragma once
#include "vault_explicit_creation_checks.h"
#include <climits>
namespace dinero {
namespace {
vault::VaultCreationAnchor VaultFixtureAnchor(uint32_t height=17) {
    vault::VaultCreationAnchor anchor;anchor.height=height;anchor.block_hash.fill(47);return anchor;
}
}
TEST(VaultCreationAnchor, ExactCodecAndHistoricalFormatsPreserveAbsence) {
    vault::VaultStateSnapshot state;state.config.operator_binding=BindingForVaultFixture();
    const auto old=vault::EncodeVaultState(state);ASSERT_EQ(old[5],'3');
    EXPECT_FALSE(vault::DecodeVaultState(old).config.creation_anchor);
    state.config.creation_anchor=VaultFixtureAnchor();const auto bytes=vault::EncodeVaultState(state);
    ASSERT_EQ(bytes[5],'4');EXPECT_EQ(vault::DecodeVaultState(bytes).config.creation_anchor,state.config.creation_anchor);
    EXPECT_EQ(vault::EncodeVaultState(vault::DecodeVaultState(bytes)),bytes);
    for(size_t length=0;length<bytes.size();++length)
        EXPECT_THROW(vault::DecodeVaultState(std::span<const uint8_t>(bytes.data(),length)),std::exception)<<length;
    auto trailing=bytes;trailing.push_back(0);EXPECT_THROW(vault::DecodeVaultState(trailing),std::exception);
    // Empty inventories end with three eight-byte counts. The preceding forty
    // bytes are this format's exact height and hash, not a guessed field search.
    ASSERT_GE(bytes.size(),64u);const auto offset=bytes.size()-64;
    auto invalid=bytes;std::fill(invalid.begin()+offset+8,invalid.begin()+offset+40,0);
    EXPECT_THROW(vault::DecodeVaultState(invalid),std::exception);
    invalid=bytes;std::fill(invalid.begin()+offset,invalid.begin()+offset+8,0xff);
    EXPECT_THROW(vault::DecodeVaultState(invalid),std::exception);
    state.config.creation_anchor->height=uint32_t(INT32_MAX)+1u;
    EXPECT_THROW(vault::EncodeVaultState(state),std::exception);
    state.config.creation_anchor=VaultFixtureAnchor();state.config.operator_binding.reset();
    EXPECT_THROW(vault::EncodeVaultState(state),std::exception);
    state.config.creation_anchor.reset();auto historical=vault::EncodeVaultState(state);
    EXPECT_EQ(historical[5],'2');EXPECT_FALSE(vault::DecodeVaultState(historical).config.creation_anchor);
    historical[5]='1';EXPECT_FALSE(vault::DecodeVaultState(historical).config.creation_anchor);
}
TEST(VaultCreationAnchor, ImmutableSuccessorsAndAuthenticatedReopen) {
    VaultStateStoreFixture f;f.config.operator_binding=BindingForVaultFixture();
    auto historical=f.Create();f.config.creation_anchor=VaultFixtureAnchor();auto bound=f.Create();
    f.Fund(bound.service);bound.service->enqueueWithdrawal(vault::AccountId{"preserved-account"},200,{0x51});
    const auto saved=vault::EncodeVaultState(bound.service->captureState()),rows=f.Rows();
    EXPECT_EQ(bound.service->captureState().config.creation_anchor,f.config.creation_anchor);
    const auto session=f.Session();
    auto reject=[&](const vault::BoundVaultService& owner,auto edit) {
        const auto changes=sqlite3_total_changes(f.Db());
        {auto use=WalletService::AcquireWalletUse(f.wallet.service);
         auto tx=vault::VaultStateTransaction::OpenExisting(use->Wallet(),session,f.domain,owner.identity);
         auto next=tx->Current().state;++next.revision;edit(next.config.creation_anchor);
         EXPECT_THROW(tx->Stage(next),std::exception);}
        EXPECT_EQ(sqlite3_total_changes(f.Db()),changes);EXPECT_EQ(f.Rows(),rows);
    };
    reject(historical,[](auto& a){a=VaultFixtureAnchor();});
    reject(bound,[](auto& a){a.reset();});
    reject(bound,[](auto& a){++a->height;});
    reject(bound,[](auto& a){a->block_hash.back()^=1;});
    {auto use=WalletService::AcquireWalletUse(f.wallet.service);use->Wallet().open("owner");}
    auto reopened=f.Open(bound.identity);EXPECT_EQ(vault::EncodeVaultState(reopened.service->captureState()),saved);
    EXPECT_EQ(f.Rows(),rows);EXPECT_FALSE(f.Open(historical.identity).service->captureState().config.creation_anchor);
}
TEST(VaultCreationAnchor, MissingAndUnselectedSourceRefuseBeforeWalletWrites) {
    VaultRuntimeReset reset;VaultExplicitCreationFixture fixture;auto& f=*fixture.f;
    const auto old=fixture.CreateOwned();const auto rows=f.Rows();const auto changes=sqlite3_total_changes(f.Db());
    EXPECT_TRUE(din::rpc_vault_create(fixture.Context(),fixture.Request()).isMember("error"));
    {
        WalletIndexOwnerFixture chain;f.wallet.context.chainstate=chain.source;
        EXPECT_TRUE(din::rpc_vault_create(fixture.Context(),fixture.Request()).isMember("error"));
        f.wallet.context.chainstate.reset();
    }
    EXPECT_EQ(f.Rows(),rows);EXPECT_EQ(sqlite3_total_changes(f.Db()),changes);
    EXPECT_FALSE(f.Open(old).service->captureState().config.creation_anchor);
    EXPECT_FALSE(vault::GetVaultRuntimeService());
}
TEST(VaultCreationAnchor, ActualSelectedRpcAndCommitRefusalPreserveCreationBoundary) {
    VaultRuntimeReset reset;VaultExplicitChainCreationFixture fixture;auto& f=*fixture.f;
    const auto historical=fixture.CreateOwned();const auto rows=f.Rows();auto* db=f.Db();
    // Real ordinary startup has no legacy validation marker. Creation records
    // position only; the existing stronger canonical reader remains refusing.
    EXPECT_EQ(fixture.chain.db.getValidatedTip().status(),Status::NotFound);
    EXPECT_FALSE(fixture.chain.source->getCanonicalBlockHash(0).ok());
    const auto token=ChainWriteToken::CreateForTesting();
    const auto tip=fixture.chain.db.getTip();ASSERT_TRUE(tip.ok());
    auto wrong=tip->hash;wrong.begin()[0]^=1;
    ASSERT_EQ(fixture.chain.db.setTip(token,wrong,tip->height,tip->work),Status::Ok);
    EXPECT_TRUE(din::rpc_vault_create(fixture.Context(),fixture.Request()).isMember("error"));
    EXPECT_EQ(f.Rows(),rows);
    ASSERT_EQ(fixture.chain.db.setTip(token,tip->hash,tip->height,tip->work),Status::Ok);
    unsigned commits=0;sqlite3_commit_hook(db,[](void* p){++*static_cast<unsigned*>(p);return 1;},&commits);
    const auto refused=din::rpc_vault_create(fixture.Context(),fixture.Request());
    sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_TRUE(refused.isMember("error"));EXPECT_EQ(commits,1u);EXPECT_EQ(f.Rows(),rows);
    EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_FALSE(vault::GetVaultRuntimeService());
    const auto created=din::rpc_vault_create(fixture.Context(),fixture.Request());
    ASSERT_FALSE(created.isMember("error"))<<created.toStyledString();
    EXPECT_EQ(created["creation_height"].asUInt64(),0u);
    EXPECT_EQ(created["creation_block_hash"].asString(),Params().genesis_hash);
    const auto summaries=fixture.List();ASSERT_EQ(summaries.size(),2u);
    uint256 genesis;ASSERT_TRUE(uint256::FromHex(Params().genesis_hash,genesis));
    std::array<uint8_t,32> expected{};std::copy(genesis.begin(),genesis.end(),expected.begin());
    for(const auto& summary:summaries) {
        if(summary.identity==historical){EXPECT_FALSE(summary.creation_anchor);continue;}
        ASSERT_TRUE(summary.creation_anchor);EXPECT_EQ(summary.creation_anchor->height,0u);
        EXPECT_EQ(summary.creation_anchor->block_hash,expected);
        auto opened=f.Open(summary.identity);EXPECT_EQ(opened.service->captureState().config.creation_anchor,summary.creation_anchor);
        EXPECT_TRUE(opened.service->captureState().entries.empty());EXPECT_EQ(opened.service->accountConfirmed(vault::AccountId{"preserved-account"}),0u);
    }
    din::Json args(::Json::arrayValue);const auto listed=din::rpc_vault_list(fixture.Context(),args);
    ASSERT_FALSE(listed.isMember("error"));EXPECT_FALSE(listed["historical_completeness_verified"].asBool());
    unsigned anchored=0;for(const auto& summary:listed["vaults"])if(summary["creation_anchor_present"].asBool()) {
        ++anchored;EXPECT_EQ(summary["creation_height"].asUInt64(),0u);
        EXPECT_EQ(summary["creation_block_hash"].asString(),created["creation_block_hash"].asString());
    }
    EXPECT_EQ(anchored,1u);EXPECT_FALSE(vault::GetVaultRuntimeService());
    EXPECT_EQ(fixture.chain.db.getValidatedTip().status(),Status::NotFound);
    EXPECT_FALSE(fixture.chain.source->getCanonicalBlockHash(0).ok());
    fixture.chain.source->EnterSafeMode("fixture creation position unavailable");
    const auto preserved=f.Rows();
    EXPECT_TRUE(din::rpc_vault_create(fixture.Context(),fixture.Request()).isMember("error"));
    EXPECT_EQ(f.Rows(),preserved);
}
} // namespace dinero
