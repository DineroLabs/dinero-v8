#pragma once
#include <chrono>
#include <thread>
#include "wallet/orchard_ownership_inventory.h"
#include <cstdio>
#include <cstdlib>
// #939 phase 3: Orchard delivery for native wallets on a full-archive v3 node.
// Included after orchard_account_creation_checks.h (OrchardCreationFixture)
// and wallet_v3_recovery_route_checks.h (NativeV3IndexScope, NativeTestCount).

namespace dinero {

namespace {
uint64_t NativeOrchardHead(CanonicalRecoveryFixture& f) {
    const auto view=f.f.service->getRuntimeAccountReplay();
    OrchardAdmissionFixture::Require(view.ok());
    return (*view)->Head().sequence;
}
} // namespace

// Repeated wallet and Orchard reads must not reverify the whole activation
// outbox. A new selected block extends only the newly authenticated tail.
TEST(WalletV3NativeOrchard, FullAccountReplayReusesPrefixAndExtendsTail) {
    OrchardCreationFixture f;
    const auto before=f.f.service->FullReplayCaptureCounts();
    {
        const auto first=f.f.service->getRuntimeAccountReplay();
        ASSERT_TRUE(first.ok());
        const auto built=f.f.service->FullReplayCaptureCounts();
        EXPECT_GE(built.first,before.first);
        const auto same=f.f.service->getRuntimeAccountReplay();
        ASSERT_TRUE(same.ok());
        EXPECT_EQ((*first)->Head(),(*same)->Head());
        EXPECT_EQ(f.f.service->FullReplayCaptureCounts(),built);
    }
    const auto cached=f.f.service->FullReplayCaptureCounts();
    f.MineEmpty();
    {
        const auto advanced=f.f.service->getRuntimeAccountReplay();
        ASSERT_TRUE(advanced.ok());
        EXPECT_EQ((*advanced)->Point((*advanced)->Head()).checkpoint.block_hash,
            f.f.service->GetActiveTip()->hash);
    }
    EXPECT_EQ(f.f.service->FullReplayCaptureCounts().first,cached.first);
    EXPECT_EQ(f.f.service->FullReplayCaptureCounts().second,cached.second+1);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(
        *f.f.service,f.f.service->GetActiveTip()));
    const auto disconnected=f.f.service->getRuntimeAccountReplay();
    ASSERT_TRUE(disconnected.ok());
    EXPECT_EQ((*disconnected)->Point((*disconnected)->Head()).checkpoint.block_hash,
        f.f.service->GetActiveTip()->hash);
    EXPECT_EQ(f.f.service->FullReplayCaptureCounts().first,cached.first);
    EXPECT_EQ(f.f.service->FullReplayCaptureCounts().second,cached.second+2);
}

namespace {
// Runs delivery passes on the calling thread until Orchard account 0 is at the head.
bool DeliverAllOrchardForReplayTest(OrchardCreationFixture& f,int max_passes) {
    auto use=WalletService::AcquireWalletUse(f.wallet);
    auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto session=use->Wallet().AcquireDatabaseLease()->Session();
    for(int i=0;i<max_passes;++i) {
        const auto next=f.f.service->resumeRuntimeWalletRecoveryV3IfNeeded(
            use->Wallet(),index->Index(),session,128*1024*1024,1);
        if(!next.ok())return false;
        if(*next)return true;
    }
    return false;
}
} // namespace

// The retained full view is not rebuilt from the origin because another
// reader happened to hold it when a block arrived. A rebuild takes minutes on
// a long chain, and until it ends nothing is delivered and every balance read
// is refused (seen on a mainnet wallet: ten minutes without a balance while
// mining). The reader is waited for, briefly; one that does not let go still
// gets a full capture, as before.
TEST(WalletV3NativeOrchard, FullAccountReplayWaitsForAReaderInsteadOfRebuilding) {
    OrchardCreationFixture f;
    {const auto first=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(first.ok());}
    const auto built=f.f.service->FullReplayCaptureCounts();

    // Another thread holds the view for a moment while a block arrives.
    {
        auto held=f.f.service->getRuntimeAccountReplay();
        ASSERT_TRUE(held.ok());
        f.MineEmpty();
        std::thread reader([view=std::move(*held)]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            view.reset();
        });
        held=Status::Invalid;
        const auto extended=f.f.service->getRuntimeAccountReplay();
        reader.join();
        ASSERT_TRUE(extended.ok());
        EXPECT_EQ((*extended)->Point((*extended)->Head()).checkpoint.block_hash,f.f.service->GetActiveTip()->hash);
    }
    EXPECT_EQ(f.f.service->FullReplayCaptureCounts().first,built.first) << "no full capture for a reader that lets go";
    EXPECT_EQ(f.f.service->FullReplayCaptureCounts().second,built.second+1);

    // The same for a reader that holds only the view's DATA (the hash lookup
    // a restore keeps after it has dropped the view): ExtendTail refuses for
    // it too, so it is waited for too.
    {
        const auto extensions=f.f.service->FullReplayCaptureCounts().second;
        std::function<StatusOr<uint256>(uint32_t)> lookup;
        {
            const auto view=f.f.service->getRuntimeAccountReplay();
            ASSERT_TRUE(view.ok());
            lookup=(*view)->SelectedHashes((*view)->Head());
        }
        f.MineEmpty();
        std::thread reader([held=std::move(lookup)]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            held=nullptr;
        });
        const auto extended=f.f.service->getRuntimeAccountReplay();
        reader.join();
        ASSERT_TRUE(extended.ok());
        EXPECT_EQ(f.f.service->FullReplayCaptureCounts().first,built.first) << "no full capture for a holder of the data alone";
        EXPECT_EQ(f.f.service->FullReplayCaptureCounts().second,extensions+1);
    }

    // A holder that does not let go (here, the caller itself) is answered
    // all the same, by a full capture, after the bounded wait.
    {
        const auto held=f.f.service->getRuntimeAccountReplay();
        ASSERT_TRUE(held.ok());
        f.MineEmpty();
        const auto started=std::chrono::steady_clock::now();
        const auto answer=f.f.service->getRuntimeAccountReplay();
        const auto waited=std::chrono::steady_clock::now()-started;
        ASSERT_TRUE(answer.ok());
        EXPECT_EQ((*answer)->Point((*answer)->Head()).checkpoint.block_hash,f.f.service->GetActiveTip()->hash);
        EXPECT_GE(waited,std::chrono::milliseconds(200));
        EXPECT_LT(waited,std::chrono::seconds(10));
    }
    EXPECT_EQ(f.f.service->FullReplayCaptureCounts().first,built.first+1);

    // The new capture is the cache: the next block is a tail extension again.
    const auto after_capture=f.f.service->FullReplayCaptureCounts();
    f.MineEmpty();
    {const auto next=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(next.ok());}
    EXPECT_EQ(f.f.service->FullReplayCaptureCounts().first,after_capture.first);
    EXPECT_EQ(f.f.service->FullReplayCaptureCounts().second,after_capture.second+1);
}

// A native-complete wallet on v3 creates an Orchard account, and an Orchard
// note sent to it is delivered by the v3 route: the account cursor advances
// only when delivery runs, matches the authenticated source, the note shows
// in the Orchard balance, the transparent receipts stay valid, and it all
// survives a restart. A reorg through the delivered event is rolled back by
// the authenticated disconnect event.
TEST(WalletV3NativeOrchard, NativeWalletReceivesOrchardNoteThroughV3Delivery) {
    OrchardCreationFixture f;   // native create() wallet, tip at activation
    NativeV3IndexScope v3(f,"v3-native-orchard-index");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    RegisterOrchardAccountRpc();
    const auto* create=g_rpcRegistry.lookup("wallet.orchard.createaccount");
    const auto* orchard_balance=g_rpcRegistry.lookup("wallet.orchard.getbalance");
    const auto* queue_shield=g_rpcRegistry.lookup("wallet.orchard.queueshield");
    const auto* queue_spend=g_rpcRegistry.lookup("wallet.orchard.queuespend");
    ASSERT_NE(create,nullptr);ASSERT_NE(orchard_balance,nullptr);
    const auto* recover_account0=g_rpcRegistry.lookup("wallet.orchard.recoveraccount0");
    ASSERT_NE(queue_shield,nullptr);ASSERT_NE(queue_spend,nullptr);ASSERT_NE(recover_account0,nullptr);
    auto ctx=f.RequestContext();
    const auto created=(*create)(ctx,OrchardBoundParamsForTest(ctx,OrchardCreationRequest()));
    ASSERT_FALSE(created.isMember("error")) << created.toStyledString();
    EXPECT_EQ(f.Account().account.Delivery().sequence,0u);

    const auto keys=f.AccountKeys();
    const auto [body,bundle]=f.Shield(keys);(void)bundle;
    (void)f.Mine(body);
    // Mining alone never moves the account: only delivery does.
    EXPECT_EQ(f.Account().account.Delivery().sequence,0u);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    const auto check_delivered=[&](uint64_t expected_una,const char* phase) {
        SCOPED_TRACE(phase);
        const auto view=f.f.service->getRuntimeAccountReplay();
        ASSERT_TRUE(view.ok());
        const auto account=f.Account();
        EXPECT_EQ(account.account.Scan().BalanceUna(),expected_una);
        EXPECT_EQ(account.account.Delivery().sequence,(*view)->Head().sequence);
        EXPECT_EQ(account.account.Delivery().digest,(*view)->Head().digest);
        EXPECT_EQ(account.account.Scan().Checkpoint(),(*view)->Point((*view)->Head()).checkpoint);
        din::Json request;request["account"]=3;
        const auto shown=(*orchard_balance)(ctx,OrchardBoundParamsForTest(ctx,request));
        ASSERT_FALSE(shown.isMember("error")) << shown.toStyledString();
        EXPECT_EQ(shown["confirmed_una"].asUInt64(),expected_una);
        EXPECT_TRUE(shown["account_caught_up_to_captured_source"].asBool()) << shown.toStyledString();
        // The Orchard route wrote neither receipt: transparent reads stay certified.
        const auto transparent=rpc_context_wallet_getbalance(ctx,din::obj());
        EXPECT_FALSE(transparent.isMember("error")) << transparent.toStyledString();
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->NativeV3TransparentState(),
            WalletManager::NativeV3State::Complete);
        EXPECT_FALSE(HasLegacyStateColumn(use->Wallet()));
    };
    check_delivered(500000,"delivered");
    // v8.2.4: Orchard spends and shields are served on v3; the end-to-end
    // round trip lives in WalletV3NativeOrchardSpend.

    // v8.2.4: restored-seed account-0 recovery is served on v3 (WalletV3NativeRestore).
    (void)recover_account0;

    f.wallet->Stop();
    f.context.wallet.reset();
    f.wallet.reset();
    f.wallet=std::make_shared<WalletService>();
    f.context.wallet=f.wallet;
    auto reopened=std::make_unique<UTXOIndex>(v3.path.string());
    ASSERT_TRUE(reopened->InitializeFullArchiveV3MetadataOnly());
    auto closed=ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(*f.f.service,std::move(reopened));
    ASSERT_TRUE(closed);closed.reset();
    ASSERT_TRUE(f.wallet->Init(f.context));
    ASSERT_TRUE(f.wallet->Start());
    ASSERT_TRUE(WalletService::AcquireWalletUse(f.wallet)->Wallet().isWalletLocked());
    // A new block while locked: transparent delivery still applies, but the
    // Orchard account (which needs the seed) waits for the unlock.
    f.MineEmpty();
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    WalletService::AcquireWalletUse(f.wallet)->Wallet().unlockWallet("canonical-fixture-pass",0);
    ctx=f.RequestContext();
    EXPECT_EQ(f.Account().account.Scan().BalanceUna(),500000u);
    EXPECT_LT(f.Account().account.Delivery().sequence,NativeOrchardHead(f));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    check_delivered(500000,"restarted");
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    check_delivered(500000,"restarted and redelivered");

    // Reorg through the delivered note: disconnect the empty block and the
    // block that carried the shield.
    const auto before_reorg=NativeOrchardHead(f);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    EXPECT_EQ(f.Account().account.Scan().BalanceUna(),500000u) << "no movement before delivery";
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    EXPECT_GT(NativeOrchardHead(f),before_reorg);
    check_delivered(0,"reorged");
    f.wallet->Stop();
}

// A late full-archive wallet can have thousands of Orchard events to inspect
// after creation. The background worker must publish each transparent mining
// reward at the selected tip before it continues that independent replay.
// A partial Orchard slice must not claim an account-ready result.
TEST(WalletV3NativeOrchard, TransparentMiningRewardsAdvanceDuringOrchardReplay) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-orchard-sliced-index");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    const auto created=f.Call(0);
    ASSERT_FALSE(created.isMember("error")) << created.toStyledString();
    int64_t mined=0;
    for(int i=0;i<4;++i)mined+=f.MineToWallet();
    const auto source_head=NativeOrchardHead(f);
    ASSERT_GT(source_head,1u);

    auto use=WalletService::AcquireWalletUse(f.wallet);
    auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto session=use->Wallet().AcquireDatabaseLease()->Session();
    bool advanced_partial=false;
    const auto first=f.f.service->resumeRuntimeWalletRecoveryV3IfNeeded(
        use->Wallet(),index->Index(),session,128*1024*1024,1,&advanced_partial);
    ASSERT_TRUE(first.ok());
    EXPECT_FALSE(*first) << "one Orchard event is not the captured head";
    EXPECT_TRUE(advanced_partial);
    EXPECT_LT(f.Account(0).account.Delivery().sequence,source_head);
    EXPECT_EQ(use->Wallet().getBlockchainHeight(),f.f.service->GetActiveTip()->height);
    auto request=f.RequestContext();
    const auto balance=rpc_context_wallet_getbalance(request,din::obj());
    ASSERT_FALSE(balance.isMember("error")) << balance.toStyledString();
    EXPECT_NEAR(balance["total"].asDouble(),double(mined)/1e8,1e-8);

    // A newly mined block must be credited on the very next pass while the
    // independent Orchard account is still replaying older events.
    mined+=f.MineToWallet();
    advanced_partial=false;
    const auto second=f.f.service->resumeRuntimeWalletRecoveryV3IfNeeded(
        use->Wallet(),index->Index(),session,128*1024*1024,1,&advanced_partial);
    ASSERT_TRUE(second.ok());
    EXPECT_FALSE(*second);
    EXPECT_TRUE(advanced_partial);
    EXPECT_LT(f.Account(0).account.Delivery().sequence,NativeOrchardHead(f));
    const auto advanced=rpc_context_wallet_getbalance(request,din::obj());
    ASSERT_FALSE(advanced.isMember("error")) << advanced.toStyledString();
    EXPECT_NEAR(advanced["total"].asDouble(),double(mined)/1e8,1e-8);

    bool complete=false;
    for(uint64_t i=0;i<NativeOrchardHead(f)+1 && !complete;++i) {
        const auto next=f.f.service->resumeRuntimeWalletRecoveryV3IfNeeded(
            use->Wallet(),index->Index(),session,128*1024*1024,1);
        ASSERT_TRUE(next.ok());
        complete=bool(*next);
    }
    EXPECT_TRUE(complete);
    EXPECT_EQ(f.Account(0).account.Delivery().sequence,NativeOrchardHead(f));
}

TEST(WalletV3NativeOrchard, WorkerRevisitsPartialReplayBeforeNormalRetry) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-orchard-fast-revisit-index");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    ASSERT_FALSE(f.Call(0).isMember("error"));
    for(int i=0;i<3;++i)(void)f.MineEmpty();
    RuntimeDeliveryWorker worker(f.f.service,f.wallet,
        RuntimeDeliveryWorker::Limits{1,std::chrono::seconds(30)});
    worker.Start();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(12);
    RuntimeDeliveryWorker::Report report;
    do {
        report=worker.Snapshot();
        if(report.wallet==RuntimeDeliveryWorker::WalletOutcome::AppliedPrefix)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while(std::chrono::steady_clock::now()<deadline);
    worker.Stop();
    EXPECT_EQ(report.wallet,RuntimeDeliveryWorker::WalletOutcome::AppliedPrefix);
    EXPECT_GT(report.wallet_recovery_attempts,1u);
}

// First enrolment (and every address-pool extension) took the coverage
// projection, which holds the account replay, and kept it while the Orchard
// delivery read the replay again. A block arriving between the two made that
// second read rebuild the replay from the origin, because this thread itself
// still held the view.
TEST(WalletV3NativeOrchard, PendingPassReleasesTheReplayBeforeItsOrchardRead) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-orchard-pending-release-index");
    std::string enrol_error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&enrol_error),
        WalletService::CanonicalRecovery::AppliedPrefix) << enrol_error;
    ASSERT_FALSE(f.Call(0).isMember("error"));  // an Orchard account, so the pass has an Orchard read to make
    ASSERT_TRUE(DeliverAllOrchardForReplayTest(f,32));
    // Use receive addresses until the pool must be extended, and begin the
    // extension: the wallet is durably Pending again, as on a node in use.
    const auto request=f.RequestContext();
    bool low=false;
    for(int i=0;i<4*int(WalletManager::kNativeV3ReceivePool)&&!low;++i) {
        ASSERT_FALSE(NativeIssue(request).isMember("error"));
        low=WalletService::AcquireWalletUse(f.wallet)->Wallet().AcquireDatabaseLease()->NativeV3PoolNeedsExtension();
    }
    ASSERT_TRUE(low);
    ASSERT_TRUE(NativeBeginExtension(f));
    {const auto prime=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(prime.ok());}
    const auto before=f.f.service->FullReplayCaptureCounts();
    bool landed=false;
    f.f.service->SetV3PendingReconcileHookForTest([&]{if(!landed){landed=true;(void)f.MineEmpty();}});
    std::string error;
    (void)f.wallet->RecoverActiveWalletFromCanonicalSource(&error);
    f.f.service->SetV3PendingReconcileHookForTest({});
    ASSERT_TRUE(landed) << "the pass did not go through the pending branch: " << error;
    EXPECT_EQ(f.f.service->FullReplayCaptureCounts().first,before.first)
        << "the replay was rebuilt from the origin inside one delivery pass";
}

// The delivery worker comes back every retry interval. When another reader
// (a wallet request) holds the replay while a block has arrived, the worker
// waits for its next pass instead of rebuilding the replay from the origin:
// on a mainnet wallet that rebuild was ten minutes without a balance.
TEST(WalletV3NativeOrchard, WorkerWaitsForItsNextPassWhileAReaderHoldsTheReplay) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-orchard-worker-retry-index");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    ASSERT_FALSE(f.Call(0).isMember("error"));
    ASSERT_TRUE(DeliverAllOrchardForReplayTest(f,32));
    auto held=f.f.service->getRuntimeAccountReplay();
    ASSERT_TRUE(held.ok());
    const auto delivered_before=f.Account(0).account.Delivery().sequence;
    const auto before=f.f.service->FullReplayCaptureCounts();
    (void)f.MineEmpty();
    RuntimeDeliveryWorker worker(f.f.service,f.wallet,
        RuntimeDeliveryWorker::Limits{1,std::chrono::milliseconds(100)});
    worker.Start();
    // Several passes go by while the reader still holds the view.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EXPECT_EQ(f.f.service->FullReplayCaptureCounts().first,before.first)
        << "the worker rebuilt the replay instead of waiting for its next pass";
    // (This thread does not read the account meanwhile: that read takes the
    // replay too, and with the view held it would be the one to rebuild it.)
    EXPECT_GT(worker.Snapshot().wallet_recovery_attempts,3u) << "the worker kept coming back";
    held=Status::Invalid;  // the reader lets go
    const auto passes_at_release=worker.Snapshot().wallet_recovery_attempts;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(worker.Snapshot().wallet_recovery_attempts<passes_at_release+6&&std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    worker.Stop();
    EXPECT_EQ(f.f.service->FullReplayCaptureCounts().first,before.first);
    EXPECT_GT(f.f.service->FullReplayCaptureCounts().second,before.second) << "the tail was extended once the reader let go";
    EXPECT_GT(f.Account(0).account.Delivery().sequence,delivered_before) << "delivered on a later pass";
}

TEST(WalletV3NativeOrchard, OrdinaryBacklogPreemptsOrchardReplay) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-orchard-ordinary-priority-index");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    ASSERT_FALSE(f.Call(0).isMember("error"));
    const auto orchard_before=f.Account(0).account.Delivery().sequence;
    int64_t mined=0;
    for(int i=0;i<33;++i)mined+=f.MineToWallet();
    const auto tip=f.f.service->GetActiveTip()->height;
    auto use=WalletService::AcquireWalletUse(f.wallet);
    auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto session=use->Wallet().AcquireDatabaseLease()->Session();

    // A 32-event ordinary page is still behind the selected source. The
    // worker must return without spending this pass on Orchard history.
    const auto first=f.f.service->resumeRuntimeWalletRecoveryV3IfNeeded(
        use->Wallet(),index->Index(),session,128*1024*1024,1);
    ASSERT_TRUE(first.ok());
    EXPECT_FALSE(*first);
    EXPECT_LT(use->Wallet().getBlockchainHeight(),tip);
    EXPECT_EQ(f.Account(0).account.Delivery().sequence,orchard_before);

    const auto second=f.f.service->resumeRuntimeWalletRecoveryV3IfNeeded(
        use->Wallet(),index->Index(),session,128*1024*1024,1);
    ASSERT_TRUE(second.ok());
    EXPECT_FALSE(*second); // Orchard is still a partial account replay.
    EXPECT_EQ(use->Wallet().getBlockchainHeight(),tip);
    EXPECT_GT(f.Account(0).account.Delivery().sequence,orchard_before);
    const auto balance=rpc_context_wallet_getbalance(f.RequestContext(),din::obj());
    ASSERT_FALSE(balance.isMember("error")) << balance.toStyledString();
    EXPECT_NEAR(balance["total"].asDouble(),double(mined)/1e8,1e-8);
}

} // namespace dinero

// Retained account history grows by one row per delivered event. Each Orchard
// delivery pass re-authenticates every retained revision (unchanged), but must
// not re-prove the same viewing key for each of them: that curve work made
// per-event cost grow with history (Hotel). Proofs per pass stay constant.
TEST(WalletV3NativeOrchard, RetainedHistoryDoesNotReproveViewingKeyPerRevision) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-orchard-retained-cost-index");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    ASSERT_FALSE(f.Call(0).isMember("error"));
    const int blocks=std::getenv("DINERO_RETAINED_BENCH_BLOCKS")?
        std::atoi(std::getenv("DINERO_RETAINED_BENCH_BLOCKS")):40;
    ASSERT_GE(blocks,40);
    for(int i=0;i<blocks;++i)(void)f.MineEmpty();
    auto use=WalletService::AcquireWalletUse(f.wallet);
    auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto session=use->Wallet().AcquireDatabaseLease()->Session();
    std::vector<double> pass_ms;std::vector<uint64_t> pass_proofs;
    bool complete=false;
    for(int i=0;i<blocks*2+8 && !complete;++i) {
        const auto proofs=wallet::ValidatedViewingKeyScope::ProofsForTest();
        const auto t0=std::chrono::steady_clock::now();
        const auto next=f.f.service->resumeRuntimeWalletRecoveryV3IfNeeded(
            use->Wallet(),index->Index(),session,128*1024*1024,1);
        pass_ms.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count());
        pass_proofs.push_back(wallet::ValidatedViewingKeyScope::ProofsForTest()-proofs);
        ASSERT_TRUE(next.ok());
        complete=bool(*next);
    }
    ASSERT_TRUE(complete);
    ASSERT_GE(pass_ms.size(),size_t(blocks));
    // Late passes restore ~blocks retained revisions; proofs must not follow.
    const auto late=pass_proofs.size()-2;
    EXPECT_GE(f.Account(0).revision,uint64_t(blocks/2));
    EXPECT_LE(pass_proofs[late],8u) << "viewing key re-proven per retained revision";
    const auto mean=[&](size_t from,size_t to){double s=0;for(size_t i=from;i<to;++i)s+=pass_ms[i];return s/double(to-from);};
    std::printf("[retained-bench] passes=%zu early_ms=%.2f late_ms=%.2f late_proofs=%llu revision=%llu\n",
        pass_ms.size(),mean(4,14),mean(pass_ms.size()-11,pass_ms.size()-1),
        (unsigned long long)pass_proofs[late],(unsigned long long)f.Account(0).revision);
}

namespace {
void NativeTestExec(WalletManager& wallet,const std::string& sql) {
    auto lease=wallet.AcquireDatabaseLease();
    OrchardAdmissionFixture::Require(sqlite3_exec(lease->Database(),sql.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK);
}
// Deliver every pending event one Orchard event per pass, as the worker does.
// Returns false (without asserting) when a pass refuses.
bool DeliverAllOrchard(OrchardCreationFixture& f,int max_passes) {
    auto use=WalletService::AcquireWalletUse(f.wallet);
    auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto session=use->Wallet().AcquireDatabaseLease()->Session();
    for(int i=0;i<max_passes;++i) {
        const auto next=f.f.service->resumeRuntimeWalletRecoveryV3IfNeeded(
            use->Wallet(),index->Index(),session,128*1024*1024,1);
        if(!next.ok())return false;
        if(*next)return true;
    }
    return false;
}
int RetainedRows(OrchardCreationFixture& f,uint32_t account) {
    return NativeTestCount(WalletService::AcquireWalletUse(f.wallet)->Wallet(),
        "SELECT count(*) FROM orchard_wallet_retained WHERE account="+std::to_string(account));
}
constexpr int kWindow=int(wallet::OrchardAccountDelivery::kRetainedWindow);
constexpr int kSlack=int(wallet::OrchardAccountDelivery::kRetainedSlack); // 1-2 accounts
// While alive, every reuse of a remembered catalog is also restored and must
// be identical (catalog revision, each account's revision, encoded state and
// archive revisions); a difference throws out of the delivery pass.
struct VerifyCatalogReuse {
    VerifyCatalogReuse(){wallet::OrchardAccountDelivery::VerifyCatalogRecoveryReuseForTest(true);}
    ~VerifyCatalogReuse(){wallet::OrchardAccountDelivery::VerifyCatalogRecoveryReuseForTest(false);}
};
} // namespace

// Active retained history stays within the 256-parent window plus slack (the
// global 65,536-row inventory cap would otherwise freeze the wallet). The
// pruned wallet keeps delivering, survives a restart, rewinds a shallow reorg
// inside the window, and refuses a rewind deeper than the window.
TEST(WalletV3NativeOrchard, ActiveRetainedHistoryIsBoundedAndStillRewinds) {
    OrchardCreationFixture f;
    VerifyCatalogReuse verify_reuse;
    NativeV3IndexScope v3(f,"v3-native-orchard-bounded-retention-index");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    ASSERT_FALSE(f.Call(0).isMember("error"));
    const int blocks=kWindow+kSlack+20;
    for(int i=0;i<blocks;++i)(void)f.MineEmpty();
    ASSERT_TRUE(DeliverAllOrchard(f,blocks*2+8));
    const int rows=RetainedRows(f,0);
    EXPECT_GE(rows,kWindow) << "window kept";
    EXPECT_LE(rows,kWindow+kSlack) << "retained history was not bounded";
    EXPECT_EQ(f.Account(0).account.Delivery().sequence,NativeOrchardHead(f));

    // Restart: the pruned inventory authenticates and keeps delivering.
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    for(int i=0;i<3;++i)(void)f.MineEmpty();
    ASSERT_TRUE(DeliverAllOrchard(f,16)) << "delivery after restart";
    EXPECT_EQ(f.Account(0).account.Delivery().sequence,NativeOrchardHead(f));

    // Shallow reorg inside the window rewinds through retained parents.
    for(int i=0;i<3;++i)
        ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    ASSERT_TRUE(DeliverAllOrchard(f,32)) << "shallow rewind inside the window";
    EXPECT_EQ(f.Account(0).account.Delivery().sequence,NativeOrchardHead(f));
    EXPECT_LE(RetainedRows(f,0),kWindow+kSlack);

    // A reorg deeper than every retained parent refuses instead of inventing
    // a missing parent; the account receipt is unchanged by the refusal.
    const int kept=RetainedRows(f,0);
    const auto before=f.Account(0).account.Delivery().sequence;
    for(int i=0;i<kept+2;++i)
        ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    EXPECT_FALSE(DeliverAllOrchard(f,kept*2+8)) << "rewind past the retained window must refuse";
    // Disconnects are outbox events, so the receipt advances while rewinding
    // inside the window, then stops at the cut: it never reaches the head.
    EXPECT_GT(f.Account(0).account.Delivery().sequence,before);
    EXPECT_LT(f.Account(0).account.Delivery().sequence,NativeOrchardHead(f));
    // The refusal is the typed window reason, not an arbitrary failure.
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
        const auto next=f.Account(0).account.Delivery().sequence+1;
        try {
            (void)wallet::OrchardAccountDelivery::PrepareCatalogRecovery(use->Wallet(),session,**view,
                wallet::OrchardAccountDelivery::CatalogRecoveryAction::Apply,next);
            ADD_FAILURE() << "rewind beyond the window was not refused";
        } catch(const std::exception& e) {
            EXPECT_STREQ(e.what(),wallet::OrchardAccountDelivery::kRewindBeyondWindow);
        }
    }
}

// The prune transaction itself authenticates every retained row of the owner
// before deleting: a tampered old ciphertext OUTSIDE the kept window refuses
// (and nothing is deleted) instead of being silently erased. Driven directly
// through BoundRetainedHistory so ordinary capture cannot refuse first.
TEST(WalletV3NativeOrchard, RetainedPruneRefusesTamperedOldRow) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-orchard-retention-tamper-index");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    ASSERT_FALSE(f.Call(0).isMember("error"));
    for(int i=0;i<40;++i)(void)f.MineEmpty();
    ASSERT_TRUE(DeliverAllOrchard(f,96));
    const int before=RetainedRows(f,0);
    ASSERT_GE(before,40);ASSERT_LE(before,kWindow) << "below the production threshold";
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    const auto session=wallet.AcquireDatabaseLease()->Session();
    const std::string oldest="(SELECT min(revision) FROM orchard_wallet_retained WHERE account=0)";
    const auto saved_hex=[&]{
        auto lease=wallet.AcquireDatabaseLease();sqlite3_stmt* q=nullptr;
        OrchardAdmissionFixture::Require(sqlite3_prepare_v2(lease->Database(),
            ("SELECT hex(sealed) FROM orchard_wallet_retained WHERE account=0 AND revision="+oldest).c_str(),
            -1,&q,nullptr)==SQLITE_OK&&sqlite3_step(q)==SQLITE_ROW);
        std::string hex(reinterpret_cast<const char*>(sqlite3_column_text(q,0)));sqlite3_finalize(q);return hex;
    }();
    // Corrupt only the GCM tag of the oldest row: it would be deleted by a
    // 16-row window, so only pre-delete authentication can notice it.
    NativeTestExec(wallet,"UPDATE orchard_wallet_retained SET sealed=substr(sealed,1,length(sealed)-16)||"
        "zeroblob(16) WHERE account=0 AND revision="+oldest);
    EXPECT_ANY_THROW((void)wallet::OrchardAccountDelivery::BoundRetainedHistory(wallet,session,**view,16))
        << "tampered row outside the window was pruned away silently";
    EXPECT_EQ(RetainedRows(f,0),before) << "refused prune deleted rows";
    NativeTestExec(wallet,"UPDATE orchard_wallet_retained SET sealed=x'"+saved_hex+
        "' WHERE account=0 AND revision="+oldest);
    EXPECT_EQ(wallet::OrchardAccountDelivery::BoundRetainedHistory(wallet,session,**view,16),size_t(before-16));
    EXPECT_EQ(RetainedRows(f,0),16);
    // The pruned, cut history still authenticates and keeps delivering.
    for(int i=0;i<3;++i)(void)f.MineEmpty();
    ASSERT_TRUE(DeliverAllOrchard(f,16));
    EXPECT_EQ(f.Account(0).account.Delivery().sequence,NativeOrchardHead(f));
}

// A current snapshot with no parent link (an undelivered account that issued
// receivers) can still hold retained rows. They are unreachable from current,
// so the bound authenticates and deletes all of them instead of skipping.
TEST(WalletV3NativeOrchard, RetainedPruneClearsRowsUnreachableFromCurrent) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-orchard-retention-unlinked-index");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    ASSERT_FALSE(f.Call(0).isMember("error"));
    for(int i=0;i<20;++i)ASSERT_FALSE(f.Issue(0).isMember("error")) << i;
    ASSERT_EQ(f.Account(0).account.ParentSnapshotRevision(),0u) << "fixture must be undelivered";
    const int before=RetainedRows(f,0);
    ASSERT_GE(before,20);
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);
    const auto session=use->Wallet().AcquireDatabaseLease()->Session();
    EXPECT_EQ(wallet::OrchardAccountDelivery::BoundRetainedHistory(use->Wallet(),session,**view,4),size_t(before));
    EXPECT_EQ(RetainedRows(f,0),0) << "unreachable retained rows left above the bound";
    // Issued receivers survive in the current state, and delivery proceeds.
    ASSERT_FALSE(f.Issue(0).isMember("error"));
    for(int i=0;i<3;++i)(void)f.MineEmpty();
    ASSERT_TRUE(DeliverAllOrchard(f,16));
    EXPECT_EQ(f.Account(0).account.Delivery().sequence,NativeOrchardHead(f));
}

// Two accounts share the global row budget; each is pruned independently to
// its own window, and the per-wallet window shrinks as accounts are added.
TEST(WalletV3NativeOrchard, RetainedHistoryBoundIsPerAccountWithinWalletBudget) {
    using D=wallet::OrchardAccountDelivery;
    EXPECT_EQ(D::RetainedWindowFor(1),D::kRetainedWindow);
    EXPECT_EQ(D::RetainedWindowFor(2),D::kRetainedWindow);
    EXPECT_LT(D::RetainedWindowFor(256),D::kRetainedWindow);
    // Every catalog this release can create keeps the full 256-parent window.
    for(size_t n=1;n<=D::kMaxRetainedAccounts;++n)
        ASSERT_EQ(D::RetainedWindowFor(n),D::kRetainedWindow) << n;
    EXPECT_EQ(D::RetainedWindowFor(1024),24u);
    EXPECT_LT(D::kRetainedBudget,wallet::OrchardOwnershipInventory::kMaxRows);
    // The bound holds for EVERY catalog size the format allows, including
    // pre-cap catalogs above kMaxRetainedAccounts.
    for(size_t n=1;n<=wallet::OrchardAccountCatalog::kMaxAccounts;++n){
        ASSERT_GT(D::RetainedWindowFor(n),0u) << n;
        ASSERT_LE(n*(D::RetainedWindowFor(n)+D::RetainedSlackFor(n)),D::kRetainedBudget) << n;
    }

    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-orchard-retention-two-accounts-index");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    ASSERT_FALSE(f.Call(0).isMember("error"));
    ASSERT_FALSE(f.Call(1).isMember("error"));
    const int blocks=kWindow+kSlack+12;
    for(int i=0;i<blocks;++i)(void)f.MineEmpty();
    ASSERT_TRUE(DeliverAllOrchard(f,blocks*2+8));
    for(const uint32_t account:{0u,1u}) {
        EXPECT_GE(RetainedRows(f,account),kWindow) << account;
        EXPECT_LE(RetainedRows(f,account),kWindow+kSlack) << account;
        EXPECT_EQ(f.Account(account).account.Delivery().sequence,NativeOrchardHead(f)) << account;
    }
}

// Delivery carries the catalog it committed into the next event. Restoring the
// wallet's whole current+retained inventory for every event cost ~12 s twice
// per block on a mainnet wallet (owner's Mac, v8.2.6 pilot, 2026-11-24), so the
// Orchard account fell behind a ~20 s block interval. Every reuse here is also
// restored and compared (VerifyCatalogReuse).
TEST(WalletV3NativeOrchard, DeliveryReusesItsCommittedCatalogInsteadOfRestoringEachEvent) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-orchard-carry-index");
    VerifyCatalogReuse verify_reuse;
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    ASSERT_FALSE(f.Call(0).isMember("error"));
    for(int i=0;i<6;++i)(void)f.MineEmpty();
    ASSERT_TRUE(DeliverAllOrchard(f,64));
    using Delivery=wallet::OrchardAccountDelivery;
    const auto builds=f.f.service->FullReplayCaptureCounts().first;
    const auto restores=Delivery::CatalogRecoveryRestores();
    constexpr int kEvents=20;
    for(int i=0;i<kEvents;++i){(void)f.MineEmpty();ASSERT_TRUE(DeliverAllOrchard(f,16)) << "event " << i;}
    EXPECT_EQ(f.Account(0).account.Delivery().sequence,NativeOrchardHead(f));
    EXPECT_LE(Delivery::CatalogRecoveryRestores()-restores,2u) << "delivery restored its catalog per event";
    // The remembered catalog holds no replay data: the retained view still
    // extends instead of being rebuilt from the origin.
    EXPECT_EQ(f.f.service->FullReplayCaptureCounts().first,builds);
    // Readers of the same bytes reuse what delivery committed.
    {
        const auto observed=Delivery::ObservedCatalogRestores();
        auto use=WalletService::AcquireWalletUse(f.wallet);
        const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
        (void)Delivery::ObserveCatalogForReplay(use->Wallet(),session,**view);
        EXPECT_EQ(Delivery::ObservedCatalogRestores(),observed) << "reader restored what delivery had committed";
    }
    // A write delivery did not make is never reused over: the next event restores.
    ASSERT_FALSE(f.Issue(0).isMember("error"));
    auto before=Delivery::CatalogRecoveryRestores();
    (void)f.MineEmpty();ASSERT_TRUE(DeliverAllOrchard(f,16));
    EXPECT_GE(Delivery::CatalogRecoveryRestores()-before,1u) << "reused across a foreign write";
    // Locking forgets the decrypted catalog; after unlock the next event restores.
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().lockWallet();use->Wallet().unlockWallet("canonical-fixture-pass",0);
    }
    before=Delivery::CatalogRecoveryRestores();
    (void)f.MineEmpty();ASSERT_TRUE(DeliverAllOrchard(f,16));
    EXPECT_GE(Delivery::CatalogRecoveryRestores()-before,1u) << "catalog survived a wallet lock";
    EXPECT_EQ(f.Account(0).account.Delivery().sequence,NativeOrchardHead(f));
}
