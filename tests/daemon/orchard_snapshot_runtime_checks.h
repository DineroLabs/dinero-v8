#pragma once
#include "p2p/orchard_catalog_transfer.h"
#include "p2p/orchard_catalog_server.h"
#include "p2p/orchard_catalog_client.h"
#include "daemon/services/orchard_boundary_replay.h"
#include "daemon/orchard_snapshot_store.h"
#include "daemon/services/block_ingress_service.h"
#include "consensus/validation_queue.h"
#include "daemon/reopen_candidate_journal.h"
#include "daemon/datadir_guard.h"
#include <sstream>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
struct OrchardSnapshotStoreTestAccess {
    static auto Authenticate(const consensus::OrchardSnapshotSection& proof,const consensus::HeaderChainSelector& headers){
        return OrchardSnapshotStore::Authenticate(proof,headers,Params().state_commitment_burial_depth);
    }
    static void CreateFresh(const std::filesystem::path& path){OrchardSnapshotStore::CreateFreshStore(path);}
    static auto Capture(const ChainDB& db,consensus::OrchardSnapshotProfile profile,const std::vector<uint8_t>& accumulator) {
        return OrchardSnapshotStore::Capture(db,profile,accumulator);
    }
    static auto CaptureActivation(const ChainDB& db,const consensus::HeaderChainSelector& headers,
        const std::function<bool()>& cancelled={}){return OrchardSnapshotStore::CaptureActivation(db,headers,cancelled);}
    static auto ReplayActivation(std::shared_ptr<const consensus::OrchardSnapshotSection> proof,
        std::shared_ptr<const consensus::HeaderChainSelector> headers,uint32_t height,const uint256& hash){
        return OrchardSnapshotStore::ReplayActivation(std::move(proof),std::move(headers),height,hash);}
    static auto Install(AnnotatedRecursiveMutex& mutex,ChainDB& db,const ChainWriteToken& token,BlockStorage& files,
        const consensus::HeaderChainSelector& headers,const consensus::OrchardSnapshotFile& file) {
        std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);
        return OrchardSnapshotStore::Install(mutex,db,token,files,headers,file);
    }
    static auto Restore(AnnotatedRecursiveMutex& mutex,const ChainDB& db,const consensus::HeaderChainSelector& headers) {
        std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);return OrchardSnapshotStore::Restore(mutex,db,headers);
    }
    static bool Recover(AnnotatedRecursiveMutex& mutex,ChainDB& db,const ChainWriteToken& token,
        const consensus::HeaderChainSelector& headers) {
        std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);return OrchardSnapshotStore::RecoverBaseMarkers(mutex,db,token,headers);
    }
};
TEST(OrchardReopenFences, CommittedPoolFenceExportsTypedCandidatesForCanonicalReadmission){
    CanonicalPoolFixture fixture;const auto first=fixture.Build();ASSERT_TRUE(first);
    const auto survivor=CanonicalPoolSurvivor(fixture);ASSERT_TRUE(fixture.f.ingress->SubmitBody(survivor,TxOrigin::INTERNAL).accepted());
    std::vector<std::vector<uint8_t>> candidates;bool orchard=false,historical=false;
    auto fence=MempoolService::FenceForReopen(fixture.f.ingress,std::chrono::milliseconds(100));
    fixture.f.ingress->ForEachReopenCandidate(*fence,[&](const auto& entry){
        orchard|=entry.tx.IsOrchard();historical|=!entry.tx.IsOrchard();candidates.push_back(entry.tx.Serialize());
    });
    ASSERT_EQ(candidates.size(),2u);EXPECT_TRUE(orchard);EXPECT_TRUE(historical);
    EXPECT_ANY_THROW(MempoolService::AcquirePoolUse(fixture.f.ingress));
    EXPECT_ANY_THROW(fixture.f.ingress->Stop());
    fence->CommitToStop();
    EXPECT_ANY_THROW(fixture.f.ingress->ForEachReopenCandidate(*fence,[](const auto&){}));
    fixture.f.ingress->Stop();
    auto reopened=std::make_shared<MempoolService>();ASSERT_TRUE(reopened->Init(fixture.context));
    fixture.context.mempool=reopened;fixture.f.ingress=reopened;
    for(const auto& bytes:candidates){
        const auto body=DecodeRelayTransaction(bytes,RelayTransactionReadMode::AvailableFamilies);
        const auto accepted=reopened->SubmitBody(body,TxOrigin::INTERNAL);EXPECT_TRUE(accepted.accepted())<<accepted.message;
    }
    EXPECT_EQ(reopened->mempool().size(),2u);EXPECT_EQ(fixture.f.service->GetActiveTip()->height,101u);
}
#ifndef _WIN32
struct ReopenIntentTestAccess {
    using Journal=daemon::ReopenCandidateJournal;
    static void Select(Journal& journal,const daemon::DatadirGuard& guard,const std::string& wallet,
        const std::function<void()>& latch=[] {}){journal.SelectForReopen(guard,wallet,latch);}
    static void Check(const Journal& journal,const daemon::DatadirGuard& guard,const std::string& wallet){journal.CheckPending(guard,wallet);}
    static void Archive(Journal& journal,const daemon::DatadirGuard& guard,const std::string& wallet){journal.ArchivePending(guard,wallet);}
};
namespace {
auto ReopenJournalEvidence(){
    auto path=(std::filesystem::temp_directory_path()/"reopen-journal-evidence-XXXXXX").string();
    OrchardAdmissionFixture::Require(::mkdtemp(path.data())!=nullptr);
    const auto result=std::filesystem::canonical(path);
    std::cout<<"Reopen journal evidence: "<<result.string()<<std::endl;return result;
}
}
TEST(OrchardReopenJournal, SealedMixedCandidatesColdOpenAndOrdinaryCanonicalReadmission){
    using Journal=daemon::ReopenCandidateJournal;CanonicalPoolFixture fixture;
    ASSERT_TRUE(fixture.Build());const auto survivor=CanonicalPoolSurvivor(fixture);
    ASSERT_TRUE(fixture.f.ingress->SubmitBody(survivor,TxOrigin::INTERNAL).accepted());
    const auto proof=CanonicalPoolUnverifiedProof(fixture,survivor);ASSERT_TRUE(proof);
    ASSERT_TRUE(fixture.f.ingress->mempool().setCachedUtxoTxPayload(survivor.GetTxid().AsUint256(),*proof));
    const auto path=ReopenJournalEvidence();daemon::DatadirGuard guard;std::string error;ASSERT_TRUE(guard.Acquire(path,error))<<error;
    auto fence=MempoolService::FenceForReopen(fixture.f.ingress,std::chrono::milliseconds(100));
    auto journal=Journal::Capture(guard,fixture.f.ingress,*fence);ASSERT_EQ(journal->Count(),2u);
    const auto name=journal->Name();bool found_proof=false;
    journal->ForEach(guard,[&](const auto& entry){if(!entry.proof.empty()){EXPECT_EQ(entry.proof,*proof);found_proof=true;}});
    EXPECT_TRUE(found_proof);fence->CommitToStop();fixture.f.ingress->Stop();journal.reset();guard.Release();
    ASSERT_TRUE(guard.Acquire(path,error))<<error;journal=Journal::Open(guard,name);ASSERT_EQ(journal->Count(),2u);
    auto reopened=std::make_shared<MempoolService>();ASSERT_TRUE(reopened->Init(fixture.context));fixture.context.mempool=reopened;fixture.f.ingress=reopened;
    unsigned admitted=0;bool orchard=false,historical=false;
    journal->ForEach(guard,[&](const auto& entry){
        const auto body=DecodeRelayTransaction(entry.body,RelayTransactionReadMode::AvailableFamilies);
        orchard|=body.IsOrchard();historical|=!body.IsOrchard();
        const auto result=reopened->SubmitBody(body,TxOrigin::WALLET);EXPECT_TRUE(result.accepted())<<result.message;++admitted;
    });
    EXPECT_EQ(admitted,2u);EXPECT_TRUE(orchard);EXPECT_TRUE(historical);EXPECT_EQ(reopened->mempool().size(),2u);
    // Keeping the immutable journal also retains unresolved candidates after
    // a visitor/normal admission failure; there is no destructive consume API.
    EXPECT_ANY_THROW(journal->ForEach(guard,[&](const auto&){throw std::runtime_error("interrupted readmission");}));
    unsigned kept=0;journal->ForEach(guard,[&](const auto&){++kept;});EXPECT_EQ(kept,2u);
    EXPECT_EQ(fixture.f.service->GetActiveTip()->height,101u);
}
TEST(OrchardReopenJournal, CancelledCaptureAndInactiveFenceNeverAuthorizeStopOrLoseCandidates){
    using Journal=daemon::ReopenCandidateJournal;CanonicalPoolFixture fixture;const auto survivor=CanonicalPoolSurvivor(fixture);
    ASSERT_TRUE(fixture.f.ingress->SubmitBody(survivor,TxOrigin::INTERNAL).accepted());
    const auto path=ReopenJournalEvidence();daemon::DatadirGuard guard;std::string error;ASSERT_TRUE(guard.Acquire(path,error))<<error;
    {auto fence=MempoolService::FenceForReopen(fixture.f.ingress,std::chrono::milliseconds(100));
     EXPECT_ANY_THROW(Journal::Capture(guard,fixture.f.ingress,*fence,[]{return true;}));}
    EXPECT_EQ(fixture.f.ingress->mempool().size(),1u);
    unsigned partial=0;
    for(const auto& item:std::filesystem::directory_iterator(path)){
        if(!item.path().filename().string().starts_with(".core-reopen-candidates-v1-"))continue;
        ++partial;EXPECT_TRUE(std::filesystem::is_regular_file(item.path()/"candidates.partial"));
        EXPECT_FALSE(std::filesystem::exists(item.path()/"candidates.sealed"));
        EXPECT_ANY_THROW(Journal::Open(guard,item.path().filename().string()));
    }
    EXPECT_EQ(partial,1u);
    auto fence=MempoolService::FenceForReopen(fixture.f.ingress,std::chrono::milliseconds(100));fence->CommitToStop();
    EXPECT_ANY_THROW(Journal::Capture(guard,fixture.f.ingress,*fence));
    EXPECT_ANY_THROW(fixture.f.ingress->mempool()); // Committed admission stays closed; fixture owns final Stop.
}
TEST(OrchardReopenJournal, CorruptForeignAliasedAndReplacedRootsRefuseBeforeVisitors){
    using Journal=daemon::ReopenCandidateJournal;CanonicalPoolFixture fixture;const auto survivor=CanonicalPoolSurvivor(fixture);
    ASSERT_TRUE(fixture.f.ingress->SubmitBody(survivor,TxOrigin::INTERNAL).accepted());
    const auto path=ReopenJournalEvidence();daemon::DatadirGuard guard;std::string error;ASSERT_TRUE(guard.Acquire(path,error))<<error;
    auto capture=[&]{auto fence=MempoolService::FenceForReopen(fixture.f.ingress,std::chrono::milliseconds(100));return Journal::Capture(guard,fixture.f.ingress,*fence);};
    auto intact=capture();const auto name=intact->Name();unsigned visits=0;
    const auto magic=Params().magic;MutableParams().magic^=1;EXPECT_ANY_THROW(Journal::Open(guard,name));MutableParams().magic=magic;
    EXPECT_ANY_THROW(Journal::Open(guard,"../"+name));
    {daemon::DatadirGuard absent;EXPECT_ANY_THROW(Journal::Open(absent,name));}
    auto damaged=capture();const auto corrupt=path/damaged->Name()/"candidates.sealed";
    ASSERT_EQ(::chmod(corrupt.c_str(),0600),0);const int fd=::open(corrupt.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);ASSERT_GE(fd,0);
    uint8_t byte;ASSERT_EQ(::pread(fd,&byte,1,72+9),1);byte^=1;ASSERT_EQ(::pwrite(fd,&byte,1,72+9),1);
    ASSERT_EQ(::fchmod(fd,0400),0);ASSERT_EQ(::fsync(fd),0);ASSERT_EQ(::close(fd),0);
    EXPECT_ANY_THROW(damaged->ForEach(guard,[&](const auto&){++visits;}));EXPECT_EQ(visits,0u);
    EXPECT_ANY_THROW(Journal::Open(guard,damaged->Name()));
    auto linked=capture();const auto linked_file=path/linked->Name()/"candidates.sealed";
    std::filesystem::create_hard_link(linked_file,path/"extra-hard-link");EXPECT_ANY_THROW(Journal::Open(guard,linked->Name()));
    auto aliased=capture();const auto alias=path/aliased->Name()/"candidates.sealed";
    std::filesystem::rename(alias,path/"outside.sealed");std::filesystem::create_symlink(path/"outside.sealed",alias);
    EXPECT_ANY_THROW(Journal::Open(guard,aliased->Name()));
    auto both_names=capture();const auto both=path/both_names->Name();
    std::filesystem::create_hard_link(both/"candidates.sealed",both/"candidates.partial");
    EXPECT_ANY_THROW(Journal::Open(guard,both_names->Name()));
    // Preserve both roots; the held lock cannot authorize a replacement inode.
    const auto saved=path.string()+".held-root";std::filesystem::rename(path,saved);std::filesystem::create_directory(path);
    EXPECT_ANY_THROW(Journal::Open(guard,name));
    EXPECT_ANY_THROW(intact->ForEach(guard,[&](const auto&){++visits;}));
    std::filesystem::rename(path,path.string()+".replacement-root");std::filesystem::rename(saved,path);
    intact->ForEach(guard,[&](const auto&){++visits;});EXPECT_EQ(visits,1u);EXPECT_EQ(fixture.f.ingress->mempool().size(),1u);
}
TEST(OrchardReopenIntent, OnlyExplicitCurrentCaptureIsSelectedAndSurvivesGuardReopen){
    using Journal=daemon::ReopenCandidateJournal;using Access=ReopenIntentTestAccess;
    CanonicalPoolFixture fixture;const auto survivor=CanonicalPoolSurvivor(fixture);
    ASSERT_TRUE(fixture.f.ingress->SubmitBody(survivor,TxOrigin::INTERNAL).accepted());
    const auto path=ReopenJournalEvidence();daemon::DatadirGuard guard;std::string error,wallet="unchanged";
    ASSERT_TRUE(guard.Acquire(path,error))<<error;
    auto capture=[&]{auto fence=MempoolService::FenceForReopen(fixture.f.ingress,std::chrono::milliseconds(100));return Journal::Capture(guard,fixture.f.ingress,*fence);};
    auto abandoned=capture();EXPECT_FALSE(Journal::OpenPending(guard,wallet));EXPECT_TRUE(wallet.empty());
    auto current=capture();auto opened=Journal::Open(guard,current->Name());
    EXPECT_ANY_THROW(Access::Select(*opened,guard,"recovery-owner")); // Read access is not a current capture.
    bool latched=false;Access::Select(*current,guard,"recovery-owner",[&]{latched=true;});EXPECT_TRUE(latched);
    auto newer=capture();EXPECT_ANY_THROW(Access::Select(*newer,guard,"different-owner"));
    EXPECT_ANY_THROW(Access::Check(*current,guard,"different-owner"));
    const auto name=current->Name();current.reset();opened.reset();guard.Release();
    ASSERT_TRUE(guard.Acquire(path,error))<<error;
    auto pending=Journal::OpenPending(guard,wallet);ASSERT_TRUE(pending);EXPECT_EQ(wallet,"recovery-owner");EXPECT_EQ(pending->Name(),name);
    unsigned count=0;pending->ForEach(guard,[&](const auto& row){EXPECT_EQ(row.body,survivor.Serialize());++count;});EXPECT_EQ(count,1u);
    // A failed/rejected readmission retains the exact selected candidates.
    EXPECT_ANY_THROW(pending->ForEach(guard,[](const auto&){throw std::runtime_error("normal admission unavailable");}));
    auto again=Journal::OpenPending(guard,wallet);ASSERT_TRUE(again);EXPECT_EQ(again->Name(),name);
    Access::Archive(*pending,guard,wallet);EXPECT_FALSE(Journal::OpenPending(guard,wallet));
    EXPECT_TRUE(std::filesystem::is_regular_file(path/(std::string(".core-reopen-pending-v1.completed-")+name.substr(std::string(".core-reopen-candidates-v1-").size()))));
    EXPECT_TRUE(std::filesystem::is_regular_file(path/name/"candidates.sealed"));
    EXPECT_TRUE(std::filesystem::is_regular_file(path/abandoned->Name()/"candidates.sealed"));
    EXPECT_ANY_THROW(Access::Select(*pending,guard,"recovery-owner"));
}
TEST(OrchardReopenIntent, PartialPublicationAndCompletionNeverBecomeMissingWork){
    using Journal=daemon::ReopenCandidateJournal;using Access=ReopenIntentTestAccess;CanonicalPoolFixture fixture;
    const auto survivor=CanonicalPoolSurvivor(fixture);ASSERT_TRUE(fixture.f.ingress->SubmitBody(survivor,TxOrigin::INTERNAL).accepted());
    const auto path=ReopenJournalEvidence();daemon::DatadirGuard guard;std::string error,wallet;
    ASSERT_TRUE(guard.Acquire(path,error))<<error;
    auto capture=[&]{auto fence=MempoolService::FenceForReopen(fixture.f.ingress,std::chrono::milliseconds(100));return Journal::Capture(guard,fixture.f.ingress,*fence);};
    auto current=capture();
    EXPECT_ANY_THROW(Access::Select(*current,guard,"named",[]{throw std::runtime_error("publication interrupted");}));
    EXPECT_TRUE(std::filesystem::is_regular_file(path/".core-reopen-pending-v1.partial"));
    EXPECT_FALSE(std::filesystem::exists(path/".core-reopen-pending-v1"));
    EXPECT_ANY_THROW(Journal::OpenPending(guard,wallet));
    EXPECT_ANY_THROW(Access::Select(*current,guard,"named"));
    EXPECT_EQ(fixture.f.ingress->mempool().size(),1u);
    // Retain the interrupted artifact; an explicit test-only new root gets a
    // separate control namespace, never an automatic production cleanup.
    const auto other=ReopenJournalEvidence();guard.Release();ASSERT_TRUE(guard.Acquire(other,error))<<error;
    auto next=capture();Access::Select(*next,guard,"named");
    std::filesystem::create_hard_link(other/".core-reopen-pending-v1",other/"interrupted-completion");
    EXPECT_ANY_THROW(Journal::OpenPending(guard,wallet));
    EXPECT_ANY_THROW(Access::Archive(*next,guard,"named"));
}
TEST(OrchardReopenIntent, CorruptForeignAliasedAndReplacedControlRefusesBeforeReadmission){
    using Journal=daemon::ReopenCandidateJournal;using Access=ReopenIntentTestAccess;CanonicalPoolFixture fixture;
    const auto survivor=CanonicalPoolSurvivor(fixture);ASSERT_TRUE(fixture.f.ingress->SubmitBody(survivor,TxOrigin::INTERNAL).accepted());
    for(unsigned variant=0;variant<5;++variant){
        const auto path=ReopenJournalEvidence();daemon::DatadirGuard guard;std::string error,wallet="unchanged";
        ASSERT_TRUE(guard.Acquire(path,error))<<error;
        auto fence=MempoolService::FenceForReopen(fixture.f.ingress,std::chrono::milliseconds(100));
        auto current=Journal::Capture(guard,fixture.f.ingress,*fence);Access::Select(*current,guard,"named");fence.reset();
        const auto control=path/".core-reopen-pending-v1";
        if(variant==0){
            ASSERT_EQ(::chmod(control.c_str(),0600),0);const int fd=::open(control.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);ASSERT_GE(fd,0);
            uint8_t byte;ASSERT_EQ(::pread(fd,&byte,1,80),1);byte^=1;ASSERT_EQ(::pwrite(fd,&byte,1,80),1);
            ASSERT_EQ(::fchmod(fd,0400),0);ASSERT_EQ(::fsync(fd),0);ASSERT_EQ(::close(fd),0);
        }else if(variant==1){
            std::filesystem::rename(control,path/"outside-control");std::filesystem::create_symlink(path/"outside-control",control);
        }else if(variant==2){
            std::filesystem::create_hard_link(control,path/"extra-control-link");
        }else if(variant==3){
            const auto magic=Params().magic;MutableParams().magic^=1;
            EXPECT_ANY_THROW(Journal::OpenPending(guard,wallet));MutableParams().magic=magic;
            EXPECT_EQ(wallet,"unchanged");ASSERT_TRUE(Journal::OpenPending(guard,wallet));continue;
        }else{
            // Replace the actual held root, retaining both original names.
            std::filesystem::rename(path,path.string()+".held-root");std::filesystem::create_directory(path);
        }
        EXPECT_ANY_THROW(Journal::OpenPending(guard,wallet));EXPECT_EQ(wallet,"unchanged");
    }
}
#endif
TEST(OrchardCatalogClient, SequentialConnectionBoundDownloadAuthenticatesWithoutPublishing){
    using namespace consensus;using Transfer=p2p::OrchardCatalogTransfer;using Client=p2p::OrchardCatalogClient;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    auto headers=std::make_shared<HeaderChainSelector>();for(const auto& b:source.f.blocks)ASSERT_TRUE(headers->AddHeader(b.header));
    ASSERT_TRUE(headers->AddHeader(first->Header()));const auto proof=OrchardSnapshotStoreTestAccess::CaptureActivation(source.f.db,*headers);
    uint256 nonce;nonce.data[0]=74;const auto request=Transfer::Request::Selected(*headers,nonce);Transfer::Sender sender(request,proof,source.f.path.parent_path());
    Client client(request,headers,source.f.path.parent_path());Client::Peer peer{"archive",12,ServiceFlags::NODE_ORCHARD_CATALOG_V1};
    auto now=Client::Clock::now();EXPECT_FALSE(client.Begin({"archive",12,0},now));ASSERT_TRUE(client.Begin(peer,now));
    EXPECT_FALSE(client.Begin(peer,now));const auto start=client.Next(now);ASSERT_TRUE(start);EXPECT_EQ(start->command,Transfer::RequestCommand);
    EXPECT_FALSE(client.Receive({"archive",13,peer.services},Transfer::ManifestCommand,sender.Description().Encode(),now));
    ASSERT_TRUE(client.Receive(peer,Transfer::ManifestCommand,sender.Description().Encode(),now));
    const auto source_state=source.f.db.getOrchardState().value();unsigned received=0;
    for(unsigned ticks=0;ticks<sender.Description().Pages()+4&&client.Current()!=Client::State::Ready;++ticks){
        if(const auto outbound=client.Next(now)){
            ASSERT_EQ(outbound->peer.session,12u);ASSERT_EQ(outbound->command,Transfer::PageRequestCommand);
            ASSERT_TRUE(client.Receive(peer,Transfer::PageCommand,sender.Serve(outbound->bytes),now));++received;
        }
        now+=std::chrono::seconds(1);
    }
    ASSERT_EQ(client.Current(),Client::State::Ready);EXPECT_EQ(received,sender.Description().Pages());
    const auto downloaded=client.Take();ASSERT_TRUE(downloaded);EXPECT_EQ(HashOrchardSnapshotSection(*downloaded),HashOrchardSnapshotSection(proof));
    EXPECT_ANY_THROW(client.Take());EXPECT_EQ(source.f.db.getOrchardState().value(),source_state);
    EXPECT_EQ(source.f.service->GetActiveTip()->hash,first->Header().GetHash());
}
TEST(OrchardCatalogClient, TimeoutCancellationAndCorruptPageNeverYieldProof){
    using namespace consensus;using Transfer=p2p::OrchardCatalogTransfer;using Client=p2p::OrchardCatalogClient;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    auto headers=std::make_shared<HeaderChainSelector>();for(const auto& b:source.f.blocks)ASSERT_TRUE(headers->AddHeader(b.header));
    ASSERT_TRUE(headers->AddHeader(first->Header()));uint256 nonce;nonce.data[0]=75;const auto request=Transfer::Request::Selected(*headers,nonce);
    const auto proof=OrchardSnapshotStoreTestAccess::CaptureActivation(source.f.db,*headers);Transfer::Sender sender(request,proof,source.f.path.parent_path());
    Client::Peer peer{"archive",22,ServiceFlags::NODE_ORCHARD_CATALOG_V1};auto now=Client::Clock::now();
    Client timeout(request,headers,source.f.path.parent_path());ASSERT_TRUE(timeout.Begin(peer,now));ASSERT_TRUE(timeout.Next(now));
    EXPECT_FALSE(timeout.Next(now+std::chrono::minutes(11)));EXPECT_EQ(timeout.Current(),Client::State::Failed);EXPECT_ANY_THROW(timeout.Take());
    Client cancelled(request,headers,source.f.path.parent_path());ASSERT_TRUE(cancelled.Begin(peer,now));cancelled.Cancel();
    EXPECT_FALSE(cancelled.Next(now));EXPECT_FALSE(cancelled.Receive(peer,Transfer::ManifestCommand,sender.Description().Encode(),now));
    EXPECT_ANY_THROW(cancelled.Take());
    Client damaged(request,headers,source.f.path.parent_path());ASSERT_TRUE(damaged.Begin(peer,now));ASSERT_TRUE(damaged.Next(now));
    ASSERT_TRUE(damaged.Receive(peer,Transfer::ManifestCommand,sender.Description().Encode(),now));const auto outgoing=damaged.Next(now);ASSERT_TRUE(outgoing);
    const auto retry=damaged.Next(now+std::chrono::seconds(15));ASSERT_TRUE(retry);EXPECT_EQ(retry->bytes,outgoing->bytes);
    auto bytes=sender.Serve(outgoing->bytes);bytes.at(32)^=1; // Matching transfer, wrong sequential page.
    EXPECT_FALSE(damaged.Receive(peer,Transfer::PageCommand,bytes,now+std::chrono::seconds(16)));
    EXPECT_EQ(damaged.Current(),Client::State::Failed);EXPECT_ANY_THROW(damaged.Take());
    Client foreign(request,headers,source.f.path.parent_path());ASSERT_TRUE(foreign.Begin(peer,now));ASSERT_TRUE(foreign.Next(now));
    auto manifest=sender.Description();manifest.request.nonce.data[0]^=1;
    EXPECT_FALSE(foreign.Receive(peer,Transfer::ManifestCommand,manifest.Encode(),now));
    EXPECT_EQ(foreign.Current(),Client::State::Manifest);
    EXPECT_ANY_THROW(foreign.Take());
    EXPECT_FALSE(foreign.Next(now+std::chrono::minutes(11)));EXPECT_EQ(foreign.Current(),Client::State::Failed);
}
TEST(OrchardCatalogClient, ArchivalPreparationBudgetAndLateRepliesDoNotExtendPageDeadline){
    using namespace consensus;using Transfer=p2p::OrchardCatalogTransfer;using Client=p2p::OrchardCatalogClient;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    auto headers=std::make_shared<HeaderChainSelector>();for(const auto& b:source.f.blocks)ASSERT_TRUE(headers->AddHeader(b.header));
    ASSERT_TRUE(headers->AddHeader(first->Header()));uint256 nonce;nonce.data[0]=76;const auto request=Transfer::Request::Selected(*headers,nonce);
    const auto proof=OrchardSnapshotStoreTestAccess::CaptureActivation(source.f.db,*headers);Transfer::Sender sender(request,proof,source.f.path.parent_path());
    Client::Peer peer{"archive",23,ServiceFlags::NODE_ORCHARD_CATALOG_V1};const auto now=Client::Clock::now();
    Client client(request,headers,source.f.path.parent_path());ASSERT_TRUE(client.Begin(peer,now));ASSERT_TRUE(client.Next(now));
    EXPECT_FALSE(client.Next(now+std::chrono::minutes(10)));EXPECT_EQ(client.Current(),Client::State::Manifest);
    const auto ready=now+std::chrono::minutes(10)+std::chrono::seconds(30);
    ASSERT_TRUE(client.Receive(peer,Transfer::ManifestCommand,sender.Description().Encode(),ready));
    const auto page=client.Next(ready);ASSERT_TRUE(page);
    auto old=sender.Serve(page->bytes);old.at(0)^=1;
    EXPECT_FALSE(client.Receive(peer,Transfer::PageCommand,old,ready+std::chrono::seconds(29)));
    EXPECT_EQ(client.Current(),Client::State::Pages);EXPECT_ANY_THROW(client.Take());
    EXPECT_FALSE(client.Next(ready+std::chrono::seconds(30)));EXPECT_EQ(client.Current(),Client::State::Failed);
    EXPECT_ANY_THROW(client.Take());
}
TEST(OrchardActivationOrigin, FirstBlockIsVerifiedFromEmptyAndStreamingStateMatchesCanonical){
    using namespace consensus;using Access=OrchardSnapshotStoreTestAccess;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;source.f.service->setRuntimeBlockNotifications(std::make_shared<CanonicalPoolNotices>());
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    auto& live=ShieldedStateStartupTestAccess::CatalogWriterCoins(*source.f.service);
    auto proof=std::make_shared<OrchardSnapshotSection>(Access::Capture(source.f.db,OrchardSnapshotProfile::Compact,
        UtreexoStump::fromForest(live.GetForest()).serialize()));
    ASSERT_GT(proof->state.tree_size,0u);const auto expected_first=proof->state;
    std::vector<OrchardBlockCandidate> suffix;
    for(unsigned i=0;i<2;++i){BlockAssembler assembler(&source.f.db);WireOrchardAssembler(assembler,source.f);
        const auto next=SolveCatalogTemplate(assembler.CreateOrchardBlock(OrchardMiningPayout));ASSERT_TRUE(next);
        ASSERT_TRUE(source.Submit(next->WireBytes()).connected);suffix.push_back(OrchardBlockCandidate::DecodeExact(next->WireBytes()));}
    auto headers=std::make_shared<HeaderChainSelector>();for(const auto& b:source.f.blocks)ASSERT_TRUE(headers->AddHeader(b.header));
    ASSERT_TRUE(headers->AddHeader(first->Header()));for(const auto& b:suffix)ASSERT_TRUE(headers->AddHeader(b.Header()));
    auto owner=Access::ReplayActivation(proof,headers,104,suffix.back().Header().GetHash());EXPECT_EQ(owner->Height(),101u);
    // Mutating the caller's section cannot rewrite the store-authenticated origin.
    proof->state.pool_balance+=1;
    const auto a=owner->Append(OrchardBlockCandidate::DecodeExact(first->WireBytes()),uint64_t(first->Header().timestamp)+7200);
    ASSERT_TRUE(a);EXPECT_FALSE(a->State().Parent());EXPECT_EQ(a->State().Next(),expected_first);
    EXPECT_EQ(a->ParentMembership().nullifier_count,0u);EXPECT_EQ(a->ParentMembership().anchor_references,0u);
    for(const auto& body:suffix){const auto step=owner->Append(body,uint64_t(body.Header().timestamp)+7200);
        ASSERT_TRUE(step);EXPECT_TRUE(step->State().Parent());EXPECT_TRUE(step->MatchesWire(body));}
    EXPECT_EQ(owner->State(),source.f.db.getOrchardState().value());
    const auto catalog=source.f.db.getOrchardCatalogState(suffix.back().Header().GetHash());ASSERT_TRUE(catalog.ok());
    EXPECT_EQ(owner->Catalog().Encode(),*catalog);EXPECT_NO_THROW(owner->Finish());
    EXPECT_EQ(source.f.service->GetActiveTip()->hash,suffix.back().Header().GetHash());
}
TEST(OrchardActivationOrigin, MissingFirstBlockForeignTargetAndIncompleteFinishFailClosed){
    using namespace consensus;using Access=OrchardSnapshotStoreTestAccess;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    auto headers=std::make_shared<HeaderChainSelector>();for(const auto& b:source.f.blocks)ASSERT_TRUE(headers->AddHeader(b.header));
    ASSERT_TRUE(headers->AddHeader(first->Header()));
    auto proof=std::make_shared<const OrchardSnapshotSection>(Access::CaptureActivation(source.f.db,*headers));
    auto bad=first->Header().GetHash();bad.data[0]^=1;
    EXPECT_ANY_THROW(Access::ReplayActivation(proof,headers,102,bad));
    auto owner=Access::ReplayActivation(proof,headers,102,first->Header().GetHash());
    EXPECT_ANY_THROW(owner->Finish());
    EXPECT_ANY_THROW(owner->Height());
    auto wrong=Access::ReplayActivation(proof,headers,102,first->Header().GetHash());
    auto bytes=first->WireBytes();bytes.at(4)^=1; // Canonically framed foreign parent/header.
    EXPECT_ANY_THROW(wrong->Append(OrchardBlockCandidate::DecodeExact(bytes),uint64_t(first->Header().timestamp)+7200));
    EXPECT_ANY_THROW(wrong->Height());
    auto forged=std::make_shared<OrchardSnapshotSection>(*proof);forged->state.pool_balance+=1;
    EXPECT_ANY_THROW(Access::ReplayActivation(forged,headers,102,first->Header().GetHash()));
    EXPECT_EQ(source.f.service->GetActiveTip()->hash,first->Header().GetHash());
}
TEST(OrchardCatalogTransfer, ServingCacheSharesPayloadButBindsNonceAndConnection){
    using namespace consensus;using Transfer=p2p::OrchardCatalogTransfer;using Server=p2p::OrchardCatalogServer;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    HeaderChainSelector headers;for(const auto& b:source.f.blocks)ASSERT_TRUE(headers.AddHeader(b.header));ASSERT_TRUE(headers.AddHeader(first->Header()));
    uint256 nonce;nonce.data[0]=61;const auto request=Transfer::Request::Selected(headers,nonce);
    struct Reply{Server::Peer peer;std::string command;std::vector<uint8_t> bytes;};
    std::mutex mutex;std::condition_variable cv;std::deque<Reply> replies;std::atomic<unsigned> builds=0;
    Server server([&](const auto& r){r.CheckSelected(headers);},[&](const auto& cancel){
        ++builds;return std::make_shared<const OrchardSnapshotSection>(OrchardSnapshotStoreTestAccess::CaptureActivation(source.f.db,headers,cancel));
    },[&](const auto& peer,const auto& command,const auto& bytes){
        std::lock_guard lock(mutex);replies.push_back({peer,command,bytes});cv.notify_all();return true;
    });
    const auto receive=[&]()->Reply{
        std::unique_lock lock(mutex);if(!cv.wait_for(lock,std::chrono::seconds(15),[&]{return !replies.empty();}))throw std::runtime_error("catalog reply timeout");
        auto reply=std::move(replies.front());replies.pop_front();return reply;
    };
    EXPECT_FALSE(server.Enqueue({"peer",0},Transfer::RequestCommand,request.Encode()));
    auto large=request.Encode();large.push_back(0);EXPECT_FALSE(server.Enqueue({"peer",1},Transfer::RequestCommand,large));
    ASSERT_TRUE(server.Enqueue({"peer",1},Transfer::RequestCommand,request.Encode()));
    const auto one=receive();ASSERT_EQ(one.command,Transfer::ManifestCommand);
    auto next_request=request;next_request.nonce.data[0]++;
    ASSERT_TRUE(server.Enqueue({"peer",2},Transfer::RequestCommand,next_request.Encode()));
    const auto two=receive();const auto a=Transfer::Manifest::Decode(one.bytes),b=Transfer::Manifest::Decode(two.bytes);
    EXPECT_EQ(builds.load(),1u);EXPECT_EQ(a.digest,b.digest);EXPECT_NE(a.Id(),b.Id());EXPECT_EQ(two.peer.session,2u);
    Transfer::Receiver receiver(next_request,two.bytes,headers);
    while(receiver.NextPage()<b.Pages()){
        ASSERT_TRUE(server.Enqueue({"peer",2},Transfer::PageRequestCommand,receiver.RequestNext()));
        const auto reply=receive();ASSERT_EQ(reply.command,Transfer::PageCommand);ASSERT_EQ(reply.peer.session,2u);receiver.Accept(reply.bytes);
    }
    EXPECT_NO_THROW(receiver.Finish(headers));server.Stop();
    EXPECT_FALSE(server.Enqueue({"peer",3},Transfer::RequestCommand,request.Encode()));
    EXPECT_EQ(source.f.service->GetActiveTip()->hash,first->Header().GetHash());
}
TEST(OrchardCatalogTransfer, ServerBoundsIngressAndCancelsPreparationOnStop){
    using Server=p2p::OrchardCatalogServer;using Transfer=p2p::OrchardCatalogTransfer;
    Transfer::Request request;request.magic=1;request.network=2;request.genesis.data[0]=1;request.nonce.data[0]=2;
    request.parent.data[0]=3;request.base.data[0]=4;request.branch=1;request.activation=2;
    std::mutex mutex;std::condition_variable cv;bool entered=false;std::atomic<bool> cancelled_seen=false;
    std::atomic<unsigned> sends=0;
    Server server([](const auto&){},[&](const auto& cancel)->std::shared_ptr<const consensus::OrchardSnapshotSection>{
        {std::lock_guard lock(mutex);entered=true;cv.notify_all();}
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(!cancel()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        cancelled_seen=cancel();return {};
    },[&](const auto&,const auto&,const auto&){++sends;return true;});
    ASSERT_TRUE(server.Enqueue({"first",1},Transfer::RequestCommand,request.Encode()));
    {std::unique_lock lock(mutex);ASSERT_TRUE(cv.wait_for(lock,std::chrono::seconds(5),[&]{return entered;}));}
    unsigned accepted=0;for(unsigned i=0;i<100;++i)accepted+=server.Enqueue({std::to_string(i),1},Transfer::RequestCommand,request.Encode());
    EXPECT_EQ(accepted,Server::MaximumQueue);server.Stop();EXPECT_TRUE(cancelled_seen);EXPECT_EQ(sends,0u);
}
TEST(OrchardCatalogTransfer, ArchivalSourceReconstructsFirstProofAfterTipAdvances){
    using namespace consensus;using Access=OrchardSnapshotStoreTestAccess;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    source.f.service->setRuntimeBlockNotifications(std::make_shared<CanonicalPoolNotices>());
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    HeaderChainSelector headers;for(const auto& b:source.f.blocks)ASSERT_TRUE(headers.AddHeader(b.header));
    ASSERT_TRUE(headers.AddHeader(first->Header()));
    auto& live=ShieldedStateStartupTestAccess::CatalogWriterCoins(*source.f.service);
    const auto original=Access::Capture(source.f.db,OrchardSnapshotProfile::Compact,
        UtreexoStump::fromForest(live.GetForest()).serialize());
    ASSERT_GT(original.state.tree_size,0u);ASSERT_FALSE(original.nullifiers.empty());
    BlockAssembler assembler(&source.f.db);WireOrchardAssembler(assembler,source.f);
    const auto second=SolveCatalogTemplate(assembler.CreateOrchardBlock(OrchardMiningPayout));ASSERT_TRUE(second);
    ASSERT_TRUE(source.Submit(second->WireBytes()).connected);ASSERT_TRUE(headers.AddHeader(second->Header()));
    const auto before=source.f.db.getOrchardState();ASSERT_TRUE(before.ok());ASSERT_EQ(before->height,103u);
    const auto retired=source.f.db.getLegacyRetirementState();ASSERT_TRUE(retired.ok());
    const auto archive=Access::CaptureActivation(source.f.db,headers);
    EXPECT_EQ(HashOrchardSnapshotSection(archive),HashOrchardSnapshotSection(original));
    EXPECT_EQ(archive.state.height,102u);EXPECT_EQ(archive.anchors.size(),1u);
    EXPECT_EQ(source.f.db.getOrchardState().value(),*before);
    EXPECT_EQ(source.f.db.getLegacyRetirementState().value(),*retired);
    EXPECT_EQ(source.f.service->GetActiveTip()->hash,second->Header().GetHash());
    uint256 nonce;nonce.data[0]=13;using Transfer=p2p::OrchardCatalogTransfer;
    const auto request=Transfer::Request::Selected(headers,nonce);Transfer::Sender sender(request,archive);
    Transfer::Receiver receiver(request,sender.Description().Encode(),headers);
    for(uint32_t i=0;i<sender.Description().Pages();++i)receiver.Accept(sender.Serve(receiver.RequestNext()));
    EXPECT_EQ(HashOrchardSnapshotSection(receiver.Finish(headers)),HashOrchardSnapshotSection(original));
}
TEST(OrchardCatalogTransfer, ArchivalSourceCancellationAndUnburiedHeadersNeverPublish){
    using namespace consensus;using Access=OrchardSnapshotStoreTestAccess;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    HeaderChainSelector headers;for(const auto& b:source.f.blocks)ASSERT_TRUE(headers.AddHeader(b.header));
    ASSERT_TRUE(headers.AddHeader(first->Header()));
    const auto before=source.f.db.getOrchardState();ASSERT_TRUE(before.ok());
    unsigned calls=0;EXPECT_ANY_THROW(Access::CaptureActivation(source.f.db,headers,[&]{return ++calls>=4;}));
    EXPECT_EQ(calls,4u);EXPECT_ANY_THROW(Access::CaptureActivation(source.f.db,headers,[]{return true;}));
    MutableParams().state_commitment_burial_depth=1;EXPECT_ANY_THROW(Access::CaptureActivation(source.f.db,headers));
    MutableParams().state_commitment_burial_depth=0;
    HeaderChainSelector missing;for(const auto& b:source.f.blocks)ASSERT_TRUE(missing.AddHeader(b.header));
    EXPECT_ANY_THROW(Access::CaptureActivation(source.f.db,missing));
    MutableParams().orchard_catalog_commitment_activation_height=UINT32_MAX;
    EXPECT_ANY_THROW(Access::CaptureActivation(source.f.db,headers));
    EXPECT_EQ(source.f.db.getOrchardState().value(),*before);
    EXPECT_EQ(source.f.service->GetActiveTip()->hash,first->Header().GetHash());
}
TEST(OrchardCatalogTransfer, ArchivalSourceMissingNodesAndForgedRetirementRefuse){
    using namespace consensus;using Access=OrchardSnapshotStoreTestAccess;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    HeaderChainSelector headers;for(const auto& b:source.f.blocks)ASSERT_TRUE(headers.AddHeader(b.header));
    ASSERT_TRUE(headers.AddHeader(first->Header()));
    const auto before=source.f.db.getOrchardState();ASSERT_TRUE(before.ok());
    const auto catalog=source.f.db.getOrchardCatalogState(first->Header().GetHash());ASSERT_TRUE(catalog.ok());
    const auto state=storage::catalog::State::Decode(*catalog);
    const auto node=source.f.db.getOrchardCatalogNode(state.transactions);ASSERT_TRUE(node.ok());
    const auto node_key=std::string("Morchard_catalog_v1/node/")+std::string(reinterpret_cast<const char*>(state.transactions.data),32);
    std::string retirement;source.f.db.close();
    {shielded_store_fixture::Raw raw(source.f.path);
        ASSERT_TRUE(raw.db->Get(rocksdb::ReadOptions(),raw.cf(shielded_store_fixture::shielded),"R1S",&retirement).ok());}
    // Raw(path) deliberately opens read-only. Only these disposable fault
    // injections/restorations use its explicit writable-existing-CF mode.
    std::vector<std::string> families;
    ASSERT_TRUE(rocksdb::DB::ListColumnFamilies(rocksdb::Options(),source.f.path.string(),&families).ok());
    for(unsigned fault=0;fault<3;++fault){
        SCOPED_TRACE(fault);
        {shielded_store_fixture::Raw raw(source.f.path,families);
            if(fault==0){rocksdb::WriteOptions w;w.sync=true;ASSERT_TRUE(raw.db->Delete(w,raw.cf("utreexo"),node_key).ok());}
            else if(fault==1){auto corrupt=*node;corrupt.back()^=1;raw.put("utreexo",node_key,corrupt);}
            else {
                // Still structurally decodable: change the frozen SHR1, not
                // a size/tag. Only the selected DNORST02 root exposes the lie.
                auto forged=retirement;forged.at(81)^=1;
                raw.put(shielded_store_fixture::shielded,"R1S",forged);
            }}
        ASSERT_EQ(source.f.db.init(source.f.path),Status::Ok);
        if(fault==2)ASSERT_TRUE(source.f.db.getLegacyRetirementState().ok());
        EXPECT_ANY_THROW(Access::CaptureActivation(source.f.db,headers));
        EXPECT_EQ(source.f.db.getOrchardState().value(),*before);
        EXPECT_EQ(source.f.service->GetActiveTip()->hash,first->Header().GetHash());
        source.f.db.close();{shielded_store_fixture::Raw raw(source.f.path,families);
            raw.put("utreexo",node_key,*node);raw.put(shielded_store_fixture::shielded,"R1S",retirement);}
    }
    ASSERT_EQ(source.f.db.init(source.f.path),Status::Ok);
    EXPECT_NO_THROW(Access::CaptureActivation(source.f.db,headers));
}
TEST(OrchardCatalogTransfer, BoundaryProofStreamsWithoutPeerGrantedPublication){
    using namespace consensus;using Transfer=p2p::OrchardCatalogTransfer;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    HeaderChainSelector headers;for(const auto& block:source.f.blocks)ASSERT_TRUE(headers.AddHeader(block.header));
    ASSERT_TRUE(headers.AddHeader(first->Header()));
    auto& live=ShieldedStateStartupTestAccess::CatalogWriterCoins(*source.f.service);
    const auto proof=OrchardSnapshotStoreTestAccess::Capture(source.f.db,OrchardSnapshotProfile::Compact,
        UtreexoStump::fromForest(live.GetForest()).serialize());
    uint256 nonce;nonce.data[0]=7;const auto request=Transfer::Request::Selected(headers,nonce);
    EXPECT_EQ(Transfer::Request::Decode(request.Encode()),request);
    shielded_store_fixture::TempDir directory;Transfer::Sender sender(request,proof,directory.path);
    const auto manifest=sender.Description().Encode();Transfer::Receiver receiver(request,manifest,headers,directory.path);
    for(uint32_t i=0;i<sender.Description().Pages();++i){
        const auto request_page=receiver.RequestNext();EXPECT_EQ(Transfer::PageRequest::Decode(request_page).index,i);
        const auto page=sender.Serve(request_page);receiver.Accept(page);receiver.Accept(page);EXPECT_EQ(receiver.NextPage(),i+1);
    }
    const auto decoded=receiver.Finish(headers);
    EXPECT_ANY_THROW(receiver.RequestNext());
    EXPECT_TRUE(decoded.nodes.empty());ASSERT_TRUE(decoded.disk_nodes);
    EXPECT_EQ(HashOrchardSnapshotSection(decoded),HashOrchardSnapshotSection(proof));
    ASSERT_NO_THROW(OrchardSnapshotStoreTestAccess::Authenticate(decoded,headers));
    EXPECT_ANY_THROW(receiver.Accept(sender.Page(0)));
    EXPECT_ANY_THROW(receiver.Finish(headers));
    // No consumer database or selected owner is passed to this transport.
    // Successful framing/authentication is not installation or wallet history.
    EXPECT_EQ(source.f.service->GetActiveTip()->hash,first->Header().GetHash());
}
TEST(OrchardCatalogTransfer, ForeignUnburiedTruncatedAndCorruptTransfersRefuse){
    using namespace consensus;using Transfer=p2p::OrchardCatalogTransfer;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    HeaderChainSelector headers;for(const auto& block:source.f.blocks)ASSERT_TRUE(headers.AddHeader(block.header));ASSERT_TRUE(headers.AddHeader(first->Header()));
    auto& live=ShieldedStateStartupTestAccess::CatalogWriterCoins(*source.f.service);
    const auto proof=OrchardSnapshotStoreTestAccess::Capture(source.f.db,OrchardSnapshotProfile::Compact,
        UtreexoStump::fromForest(live.GetForest()).serialize());
    uint256 nonce;nonce.data[0]=8;const auto request=Transfer::Request::Selected(headers,nonce);
    Transfer::Sender sender(request,proof);const auto manifest=sender.Description().Encode();
    for(const auto offset:{6u,10u,11u,43u,75u,79u,83u,87u,119u}){
        auto bad=manifest;bad.at(offset)^=1;
        EXPECT_ANY_THROW(Transfer::Receiver(request,bad,headers));
    }
    auto truncated=manifest;truncated.pop_back();EXPECT_ANY_THROW(Transfer::Manifest::Decode(truncated));
    auto trailing=manifest;trailing.push_back(0);EXPECT_ANY_THROW(Transfer::Manifest::Decode(trailing));
    auto huge=sender.Description();huge.bytes=Transfer::MaximumBytes+1;EXPECT_ANY_THROW(huge.Encode());
    Transfer::PageRequest wrong_page{sender.Description().Id(),sender.Description().Pages()};
    EXPECT_ANY_THROW(sender.Serve(wrong_page.Encode()));
    wrong_page.index=0;wrong_page.transfer.data[0]^=1;EXPECT_ANY_THROW(sender.Serve(wrong_page.Encode()));
    auto short_request=Transfer::PageRequest{sender.Description().Id(),0}.Encode();short_request.pop_back();
    EXPECT_ANY_THROW(sender.Serve(short_request));
    MutableParams().state_commitment_burial_depth=1;
    EXPECT_ANY_THROW(Transfer::Request::Selected(headers,nonce));
    EXPECT_ANY_THROW(Transfer::Receiver(request,manifest,headers));
    MutableParams().state_commitment_burial_depth=0;
    {Transfer::Receiver incomplete(request,manifest,headers);
     EXPECT_ANY_THROW(incomplete.Finish(headers));
     EXPECT_ANY_THROW(incomplete.NextPage());}
    {Transfer::Receiver wrong(request,manifest,headers);auto page=sender.Page(0);page.at(0)^=1;
     EXPECT_ANY_THROW(wrong.Accept(page));
     EXPECT_ANY_THROW(wrong.Accept(sender.Page(0)));}
    {Transfer::Receiver wrong(request,manifest,headers);auto page=sender.Page(0);page.at(32)=255;
     EXPECT_ANY_THROW(wrong.Accept(page));}
    {Transfer::Receiver wrong(request,manifest,headers);
     for(uint32_t i=0;i<sender.Description().Pages();++i){auto page=sender.Page(i);if(!i)page.back()^=1;wrong.Accept(page);}
     EXPECT_ANY_THROW(wrong.Finish(headers));}
    {Transfer::Receiver wrong(request,manifest,headers);auto page=sender.Page(0);page.pop_back();EXPECT_ANY_THROW(wrong.Accept(page));}
    auto forged=proof;forged.state.pool_balance+=1;
    // The peer can legitimately hash a self-consistent lie. Only the selected
    // coinbase commitment check distinguishes it from genuine authenticated data.
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::Authenticate(forged,headers));
    Transfer::Sender liar(request,forged);Transfer::Receiver lied_to(request,liar.Description().Encode(),headers);
    for(uint32_t i=0;i<liar.Description().Pages();++i)lied_to.Accept(liar.Page(i));
    EXPECT_ANY_THROW(lied_to.Finish(headers));
    EXPECT_EQ(source.f.service->GetActiveTip()->hash,first->Header().GetHash());
}
TEST(OrchardSnapshotRuntime, FullNodeRejectsBadDnorst02WithCompactBootstrapOff) {
    using namespace consensus;
    CanonicalPoolFixture fixture(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    const auto config=ShieldedStateStartupTestAccess::SnapshotBootstrapConfig(*fixture.f.service);
    ASSERT_TRUE(config->CompactSnapshotBootstrapEnabled());config->Set("assumeutxo_compact_bootstrap","0");
    ASSERT_FALSE(GetConfig().utreexo_stateless);ASSERT_NE(fixture.f.service->GetConsensusUTXOSet(),nullptr);
    const auto honest=fixture.Build();ASSERT_TRUE(honest);ASSERT_LT(honest->Transactions().size(),253u);
    auto cb=honest->Transactions().front().Historical();
    const auto original=FindStateCommitment(cb,StateCommitmentEncoding::Orchard);
    ASSERT_EQ(original.status,StateCommitmentStatus::Ok);
    bool changed=false;
    for(auto& output:cb.vout) {
        auto& script=output.scriptPubKey;
        if(script.size()==39&&script[0]==0x6a&&script[2]=='D'&&script[3]=='N'&&script[4]=='R'&&script[5]=='S') {
            script.back()^=1;changed=true;
        }
    }
    ASSERT_TRUE(changed);ASSERT_NE(FindStateCommitment(cb,StateCommitmentEncoding::Orchard).root,original.root);
    auto bytes=honest->WireBytes();const auto wire=cb.Serialize(TxSerializationMode::WithWitness);
    ASSERT_EQ(wire.size(),honest->Transactions().front().Historical().Serialize(TxSerializationMode::WithWitness).size());
    ASSERT_EQ(bytes.at(128),honest->Transactions().size());std::copy(wire.begin(),wire.end(),bytes.begin()+129);
    auto header=honest->Header();std::vector<TxId> ids{cb.GetTxid()};
    for(size_t i=1;i<honest->Transactions().size();++i)ids.push_back(honest->Transactions()[i].GetTxid());
    header.merkle_root=ComputeTransactionMerkleRoot(ids);
    const auto encode=[&] {const auto prefix=header.SerializeForHash();std::copy(prefix.begin(),prefix.end(),bytes.begin());};
    encode();auto draft=OrchardBlockCandidate::DecodeExact(bytes);
    FirstBoundaryView view(*fixture.f.replay);
    const auto context=SelectedOrchardBlockContext(draft.Header(),honest->Height());ASSERT_TRUE(context);
    const auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(draft,*context,view,{},true);
    const auto& parent=fixture.f.blocks.back().header;const auto& forest=*fixture.f.replay->Forest();
    header.utreexo_root=PrepareOrchardForestTransition(coins,parent,forest).Root();
    header=OrchardAdmissionFixture::SolveHeader(header);encode();
    const auto forged=OrchardBlockCandidate::DecodeExact(bytes);
    const auto final_context=SelectedOrchardBlockContext(header,honest->Height());ASSERT_TRUE(final_context);
    const auto final_coins=PrepareOrchardBlockCoinsUnderChainstateLock(forged,*final_context,view,{},true);
    std::string error;ASSERT_TRUE(forged.CheckIdentityCommitments(true,error))<<error;
    ASSERT_NO_THROW(CheckOrchardBlockUtreexoProof(forged,final_coins,parent,forest));
    ASSERT_TRUE(PrepareOrchardForestTransition(final_coins,parent,forest).MatchesHeader(header));
    const auto refused=fixture.Submit(bytes);EXPECT_FALSE(refused.connected);
    EXPECT_EQ(refused.code,BlockRejectCode::INVALID_TRANSACTION);
    EXPECT_EQ(refused.reason,"Orchard state commitment rejected");
    EXPECT_EQ(fixture.f.service->GetActiveTip(),fixture.parent);fixture.f.CheckUnpublished();
    const auto valid=SolveCatalogTemplate(honest);ASSERT_TRUE(valid);
    const auto accepted=fixture.Submit(valid->WireBytes());EXPECT_TRUE(accepted.connected)<<accepted.reason;
    EXPECT_FALSE(config->CompactSnapshotBootstrapEnabled());
}
TEST(OrchardSnapshotRuntime, RealRegtestActivationFullAndCompactImportReopenAndFloor) {
    using namespace consensus;using Access=OrchardSnapshotStoreTestAccess;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);
    const auto accepted=source.Submit(first->WireBytes());ASSERT_TRUE(accepted.connected)<<accepted.reason;
    HeaderChainSelector headers;for(const auto& block:source.f.blocks)ASSERT_TRUE(headers.AddHeader(block.header));
    ASSERT_TRUE(headers.AddHeader(first->Header()));
    auto& live=ShieldedStateStartupTestAccess::CatalogWriterCoins(*source.f.service);
    const auto full=Access::Capture(source.f.db,OrchardSnapshotProfile::Full,live.GetForest().serialize());
    ASSERT_GT(full.state.tree_size,0u);ASSERT_GT(full.state.pool_balance,0u);ASSERT_FALSE(full.nullifiers.empty());
    for(const auto profile:{OrchardSnapshotProfile::Full,OrchardSnapshotProfile::Compact}) {
        auto section=full;section.profile=profile;OrchardSnapshotCoins coins;
        if(profile==OrchardSnapshotProfile::Full)coins=live.GetUTXOs();else section.accumulator=section.base_catalog.stump;
        SnapshotMetadata header;header.version=SNAPSHOT_VERSION_V6;header.block_hash=section.state.block_hash;
        header.block_height=section.state.height;header.timestamp=first->Header().timestamp;header.utxo_count=coins.size();
        std::ostringstream output(std::ios::binary);ASSERT_NO_THROW(WriteOrchardSnapshotFile(output,header,section,coins));
        const auto bytes=output.str();std::istringstream input(bytes,std::ios::binary);
        const auto decoded=ReadOrchardSnapshotFile(input,bytes.size(),profile);
        shielded_store_fixture::TempDir directory;
        const auto database_path=directory.path/"chainstate";
        ASSERT_NO_THROW(Access::CreateFresh(database_path));EXPECT_ANY_THROW(Access::CreateFresh(database_path));
        ChainDB db;ASSERT_EQ(db.init(database_path),Status::Ok);BlockStorage files;
        ASSERT_EQ(files.init(directory.path/"files"),Status::Ok);AnnotatedRecursiveMutex mutex;
        const auto stale=source.f.blocks[1].GetHash();ASSERT_EQ(db.putHeightIndex(source.f.token,1,stale),Status::Ok);
        EXPECT_ANY_THROW(Access::Install(mutex,db,source.f.token,files,headers,decoded));
        EXPECT_EQ(db.getBlockHashByHeight(1).value(),stale);EXPECT_EQ(db.getOrchardState().status(),Status::NotFound);
        ASSERT_EQ(db.deleteHeightIndex(source.f.token,1),Status::Ok);
        MutableParams().state_commitment_burial_depth=1;
        EXPECT_ANY_THROW(Access::Install(mutex,db,source.f.token,files,headers,decoded));
        EXPECT_EQ(db.getTip().status(),Status::NotFound);EXPECT_EQ(db.getOrchardState().status(),Status::NotFound);
        MutableParams().state_commitment_burial_depth=0;
        auto installed=Access::Install(mutex,db,source.f.token,files,headers,decoded);
        std::string factory_marker;EXPECT_EQ(db.getRaw(storage::OrchardSnapshotFreshKey,factory_marker),Status::NotFound);
        EXPECT_EQ(bool(installed.full),profile==OrchardSnapshotProfile::Full);
        EXPECT_EQ(bool(installed.compact),profile==OrchardSnapshotProfile::Compact);
        ASSERT_TRUE(db.getTip().ok());EXPECT_EQ(db.getTip()->hash,section.state.block_hash);
        ASSERT_TRUE(db.getHeaderMetadata(section.state.block_hash).ok());
        EXPECT_EQ(db.getHeaderMetadata(section.state.block_hash)->status_flags&(BLOCK_VALID_CHAIN|BLOCK_VALID_SCRIPTS|BLOCK_HAVE_UNDO),0u);
        EXPECT_EQ(db.getOrchardUndoParent(section.state).status(),Status::Corruption);
        EXPECT_ANY_THROW(Access::Install(mutex,db,source.f.token,files,headers,decoded));
        EXPECT_EQ(db.getOrchardState()->block_hash,section.state.block_hash);
        installed.full.reset();installed.compact.reset();db.close();ASSERT_EQ(db.init(database_path),Status::Ok);
        auto restored=Access::Restore(mutex,db,headers);
        if(profile==OrchardSnapshotProfile::Full) {
            ASSERT_TRUE(restored.full);EXPECT_EQ(restored.full->GetSetSize(),coins.size());
            EXPECT_EQ(restored.full->GetForest().serialize(),section.accumulator);
            const auto block=OrchardBlockCandidate::DecodeExact(section.base_body);
            EXPECT_ANY_THROW(PreparedOrchardChainstateWrite::Disconnect(mutex,db,source.f.token,*restored.full,
                *SelectedOrchardBlockContext(block.Header(),section.state.height),block,source.f.blocks.back().header,
                restored.full->GetForest(),true,true));
        } else {
            ASSERT_TRUE(restored.compact);std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);
            ASSERT_TRUE(restored.compact->SelectedUnderLock(mutex));
            EXPECT_EQ(restored.compact->SelectedUnderLock(mutex)->Encode(),section.base_catalog.Encode());
        }
        // A forged canonical marker cannot enroll an owner after restart.
        const auto original=db.getForestTipMarker();ASSERT_TRUE(original.ok());auto forged=*original;forged.forest_root.data[0]^=1;
        ASSERT_EQ(db.putForestTipMarker(source.f.token,forged),Status::Ok);
        EXPECT_ANY_THROW(Access::Restore(mutex,db,headers));
        ASSERT_EQ(db.putForestTipMarker(source.f.token,*original),Status::Ok);
        ASSERT_NO_THROW(Access::Restore(mutex,db,headers));
        restored.full.reset();restored.compact.reset();
        // Reproduce the marker-above-history-tip gap. Repair may publish only
        // this exact authenticated base, never a plausible marker alone.
        const auto genesis=source.f.blocks.front().GetHash();const auto genesis_work=db.getBlockWork(genesis);ASSERT_TRUE(genesis_work.ok());
        ASSERT_EQ(db.setTip(source.f.token,genesis,0,*genesis_work),Status::Ok);
        ASSERT_EQ(db.setValidatedTip(source.f.token,genesis,0),Status::Ok);
        ASSERT_EQ(db.putForestTipMarker(source.f.token,forged),Status::Ok);
        EXPECT_ANY_THROW(Access::Recover(mutex,db,source.f.token,headers));EXPECT_EQ(db.getTip()->hash,genesis);
        ASSERT_EQ(db.putForestTipMarker(source.f.token,*original),Status::Ok);
        ASSERT_TRUE(Access::Recover(mutex,db,source.f.token,headers));EXPECT_EQ(db.getTip()->hash,section.state.block_hash);
        EXPECT_FALSE(Access::Recover(mutex,db,source.f.token,headers));ASSERT_NO_THROW(Access::Restore(mutex,db,headers));
        const auto base=storage::ReadOrchardSnapshotBase(db);ASSERT_TRUE(base);
        const auto key=storage::OrchardSnapshotProofKey(base->proof_digest,0);std::string proof;
        ASSERT_EQ(db.getRaw(key,proof),Status::Ok);auto corrupted=proof;corrupted.back()^=1;
        {rocksdb::WriteBatch batch;batch.Put(key,corrupted);ASSERT_EQ(db.writeBatch(source.f.token,std::move(batch),true),Status::Ok);}
        EXPECT_ANY_THROW(Access::Restore(mutex,db,headers));EXPECT_EQ(db.getTip()->hash,section.state.block_hash);
        {rocksdb::WriteBatch batch;batch.Put(key,proof);ASSERT_EQ(db.writeBatch(source.f.token,std::move(batch),true),Status::Ok);}
        ASSERT_NO_THROW(Access::Restore(mutex,db,headers));db.close();
    }
}
TEST(OrchardSnapshotRuntime, ActualServiceBootstrapExportAndRestartBothProfiles) {
    using namespace consensus;using Access=OrchardSnapshotStoreTestAccess;
    OrchardCycleFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto receiver=orchard::WalletKeys::FromSeed(std::array<uint8_t,64>{53},0);
    const auto recipient=orchard::WalletKeys::FromSeed(std::array<uint8_t,64>{55},0);
    const auto [shield_body,shield_bundle]=source.Shield(receiver);
    ASSERT_TRUE(source.f.ingress->SubmitBody(shield_body,TxOrigin::INTERNAL).accepted());
    BlockAssembler first_assembler(&source.f.db);WireOrchardAssembler(first_assembler,source.f);
    const auto first=SolveCatalogTemplate(first_assembler.CreateOrchardBlock(OrchardMiningPayout));ASSERT_TRUE(first);
    ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    auto headers=std::make_shared<HeaderChainSelector>();
    for(const auto& block:source.f.blocks)ASSERT_TRUE(headers->AddHeader(block.header));
    ASSERT_TRUE(headers->AddHeader(first->Header()));source.f.service->setHeaderChainSelector(headers);
    shielded_store_fixture::TempDir exports;
    const auto full_path=exports.path/"full.v6";
    const auto exported=source.f.service->ExportSnapshot(full_path);ASSERT_TRUE(exported.success)<<exported.error_message;
    // A checksum-valid full container whose logical state computes a different
    // DNORST02 root must remain rejected when compact bootstrap is switched off.
    const auto bad_root_path=exports.path/"bad-root-full.v6";
    {
        std::ifstream input(full_path,std::ios::binary);
        auto forged=ReadOrchardSnapshotFile(input,std::filesystem::file_size(full_path),OrchardSnapshotProfile::Full);
        ++forged.orchard.state.pool_balance;
        ASSERT_TRUE(WriteOrchardSnapshotFileAtomically(bad_root_path,forged.metadata,forged.orchard,forged.coins).success);
    }
    auto& live=ShieldedStateStartupTestAccess::CatalogWriterCoins(*source.f.service);
    auto compact=Access::Capture(source.f.db,OrchardSnapshotProfile::Compact,
        UtreexoStump::fromForest(live.GetForest()).serialize());
    SnapshotMetadata metadata;metadata.version=SNAPSHOT_VERSION_V6;metadata.block_hash=first->Header().GetHash();
    metadata.block_height=first->Height();metadata.timestamp=first->Header().timestamp;
    const auto compact_path=exports.path/"compact.v6";
    ASSERT_TRUE(WriteOrchardSnapshotFileAtomically(compact_path,metadata,compact,{}).success);
    source.f.service->setRuntimeBlockNotifications(std::make_shared<TypedForkNotices>());
    const auto empty=orchard::OrchardFrontier::Empty();const auto leaves=CycleCommitments(shield_bundle.Authorization());
    const auto frontier=empty.Append(leaves);auto [note,note_index]=CycleReceive(receiver,shield_bundle.Authorization());
    const auto witness=orchard::WalletWitness::ForAppendedLeaf(empty,leaves,note_index);
    const std::vector<orchard::WalletSpendInput> spend{{note,witness}};
    const std::vector<orchard::WalletPayment> payment{{400000,recipient.Receiver(orchard::WalletScope::External,{})}};
    const auto transfer_bundle=orchard::WalletBundlePlan::PrepareSpend(receiver,spend,frontier.Root(),payment)
        .Prove(orchard::SigningContext::Create(source.Domain(),0,{}, {},100000));
    const auto transfer=MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::Create(0,{}, {},100000,transfer_bundle.Bytes()));
    const auto spent_nullifier=CycleHash(note.Facts().nullifier);
    ASSERT_TRUE(source.f.ingress->SubmitBody(transfer,TxOrigin::INTERNAL).accepted());
    const OutPoint legacy_coin(source.f.blocks[2].vtx.front().GetTxid(),0);
    ASSERT_TRUE(source.f.ingress->SubmitBody(MempoolTransaction(SelectionSpend(source.f,legacy_coin,
        source.f.replay->ProvenUtxos().at(legacy_coin),100000)),TxOrigin::INTERNAL).accepted());
    const auto& shield=first->Transactions().at(1);const OutPoint base_coin(shield.GetTxid(),0);
    ASSERT_TRUE(source.f.ingress->SubmitBody(MempoolTransaction(SelectionSpend(source.f,base_coin,
        shield.OutputCoin(0,102),200000)),TxOrigin::INTERNAL).accepted());
    BlockAssembler assembler(&source.f.db);WireOrchardAssembler(assembler,source.f);
    const auto second=SolveCatalogTemplate(assembler.CreateOrchardBlock(OrchardMiningPayout));ASSERT_TRUE(second);
    ASSERT_TRUE(source.Submit(second->WireBytes()).connected);ASSERT_TRUE(headers->AddHeader(second->Header()));
    const auto second_catalog=source.f.db.getOrchardCatalogState(second->Header().GetHash());ASSERT_TRUE(second_catalog.ok());
    const auto full_second=exports.path/"full-second.v6";
    const auto second_export=source.f.service->ExportSnapshot(full_second);ASSERT_TRUE(second_export.success)<<second_export.error_message;
    auto compact_second=Access::Capture(source.f.db,OrchardSnapshotProfile::Compact,UtreexoStump::fromForest(live.GetForest()).serialize());
    auto second_metadata=metadata;second_metadata.block_hash=second->Header().GetHash();second_metadata.block_height=second->Height();
    second_metadata.timestamp=second->Header().timestamp;const auto compact_second_path=exports.path/"compact-second.v6";
    ASSERT_TRUE(WriteOrchardSnapshotFileAtomically(compact_second_path,second_metadata,compact_second,{}).success);
    const auto third=SolveCatalogTemplate(assembler.CreateOrchardBlock(OrchardMiningPayout));ASSERT_TRUE(third);
    ASSERT_TRUE(source.Submit(third->WireBytes()).connected);ASSERT_TRUE(headers->AddHeader(third->Header()));
    auto sibling_header=second->Header();++sibling_header.timestamp;
    sibling_header=OrchardAdmissionFixture::SolveHeader(sibling_header);auto sibling_bytes=second->WireBytes();
    const auto sibling_prefix=sibling_header.SerializeForHash();std::copy(sibling_prefix.begin(),sibling_prefix.end(),sibling_bytes.begin());
    for(const bool stateless:{false,true}) {
        shielded_store_fixture::TempDir directory;const auto path=directory.path/"chainstate";
        Access::CreateFresh(path);const auto snapshot=stateless?compact_path:full_path;
        RawIngressGraph isolated;
        struct ContextGuard {DaemonContext* prior=DaemonContext::instance();
            ~ContextGuard(){BlockAcceptor::SetContext(prior);DaemonContext::setInstance(prior);}} guard;
        GetConfig().utreexo_stateless=stateless;
        for(unsigned run=0;run<2;++run) {
            // A real executable has no permanent graph from its previous run.
            // Keep the earlier graph alive but detached until this service is
            // destroyed, rather than accidentally borrowing its undo locators.
            std::unique_ptr<RawIngressGraph> cold_graph;
            if(stateless&&run==1)cold_graph=std::make_unique<RawIngressGraph>();
            auto db=std::make_shared<ChainDB>();ASSERT_EQ(db->init(path),Status::Ok);
            DaemonContext context;context.config=std::make_shared<ConfigService>();
            context.config->Set("datadir",directory.path.string());
            EXPECT_TRUE(context.config->CompactSnapshotBootstrapEnabled()); // Default is on.
            if(!stateless)context.config->Set("assumeutxo_compact_bootstrap","0");
            if(stateless&&run==0)context.config->Set("assumeutxo_snapshot",snapshot.string());
            context.logger=std::make_shared<LoggerService>("");
            context.block_storage=std::make_shared<BlockStorage>();ASSERT_EQ(context.block_storage->init(directory.path/"files"),Status::Ok);
            auto service=std::make_shared<ChainstateService>();context.chainstate=service;
            struct StopGuard {std::shared_ptr<ChainstateService> service;DaemonContext& context;
                ~StopGuard(){if(context.validation_queue)context.validation_queue->stop();context.block_ingress=nullptr;
                    if(auto pool=std::dynamic_pointer_cast<MempoolService>(context.mempool))pool->Stop();
                    context.mempool.reset();service->Stop();context.chainstate.reset();}} stop{service,context};
            DaemonContext::setInstance(&context);service->setOwnedChainDB(db);
            ASSERT_TRUE(service->Init(context));auto* original=service->GetConsensusUTXOSet();
            if(stateless)EXPECT_EQ(original,nullptr);
            context.logger_interface=source.f.logger.get();auto pool=std::make_shared<MempoolService>();
            ASSERT_TRUE(pool->Init(context));context.mempool=pool;
            service->setHeaderChainSelector(headers);service->setRuntimeBlockNotifications(std::make_shared<TypedForkNotices>());
            if(stateless) {
                service->setValidationMode(ValidationMode::STATEFUL);EXPECT_FALSE(service->Start());
                EXPECT_EQ(service->GetActiveTip(),nullptr);service->setValidationMode(ValidationMode::STATELESS);
            }
            ASSERT_TRUE(service->Start());
            if(!stateless&&run==0) {
                EXPECT_FALSE(context.config->CompactSnapshotBootstrapEnabled());
                const auto refused_root=service->LoadSnapshot(bad_root_path);
                EXPECT_FALSE(refused_root.success);EXPECT_TRUE(refused_root.checksum_valid);
                EXPECT_EQ(refused_root.utxos_imported,0u);
                EXPECT_EQ(db->getTip()->height,0);EXPECT_EQ(service->GetConsensusUTXOSet(),original);
                const auto wrong_profile=service->LoadSnapshot(compact_path);EXPECT_FALSE(wrong_profile.success);
                EXPECT_EQ(db->getTip()->height,0);EXPECT_EQ(service->GetConsensusUTXOSet(),original);
                const auto imported=service->LoadSnapshot(snapshot);ASSERT_TRUE(imported.success)<<imported.error_message;
                EXPECT_EQ(service->GetConsensusUTXOSet(),original);
            }
            const auto& selected=run==0?metadata:second_metadata;
            ASSERT_NE(service->GetActiveTip(),nullptr);EXPECT_EQ(service->GetActiveTip()->hash,selected.block_hash);
            EXPECT_EQ(bool(service->GetConsensusUTXOSet()),!stateless);EXPECT_TRUE(service->IsHealthy());
            // This is the selected-owner read used by NodeCore's accumulator
            // RPC. A compact v6 owner must work without a fallback full forest,
            // including the cold restart with no configured snapshot path.
            const auto accumulator=service->getUtreexoRpcSnapshot();ASSERT_TRUE(accumulator.ok());
            EXPECT_EQ(accumulator->block_hash,selected.block_hash);EXPECT_EQ(accumulator->height,selected.block_height);
            EXPECT_EQ(accumulator->compact,stateless);
            if(!stateless) {
                GetConfig().utreexo_stateless=true;
                EXPECT_FALSE(service->getUtreexoRpcSnapshot().ok()); // no legacy fallback in Orchard
                GetConfig().utreexo_stateless=false;
                EXPECT_TRUE(service->getUtreexoRpcSnapshot().ok());
            }
            {auto lease=ChainstateService::AcquireBlockIngressUse(service);ASSERT_TRUE(lease);}
            const auto copy=directory.path/("export-"+std::to_string(run)+".v6");
            if(stateless) {
                context.config->Set("assumeutxo_compact_bootstrap","0");
                {std::ofstream sentinel(copy);sentinel<<"retain on disabled compact export";}
                const auto refused_export=service->ExportSnapshot(copy);
                EXPECT_FALSE(refused_export.success);
                EXPECT_NE(refused_export.error_message.find("disabled by local policy"),std::string::npos);
                {std::ifstream sentinel(copy);EXPECT_EQ(std::string(std::istreambuf_iterator<char>(sentinel),{}),
                    "retain on disabled compact export");}
                const auto previous=GetConfig().utreexo_stateless;GetConfig().utreexo_stateless=true;
                const auto refused_load=service->LoadSnapshot(compact_path);GetConfig().utreexo_stateless=previous;
                EXPECT_FALSE(refused_load.success);
                EXPECT_NE(refused_load.error_message.find("disabled by local policy"),std::string::npos);
                EXPECT_EQ(service->GetActiveTip()->hash,selected.block_hash);EXPECT_TRUE(service->IsHealthy());
                context.config->Set("assumeutxo_compact_bootstrap","1");
            }
            const auto result=service->ExportSnapshot(copy);ASSERT_TRUE(result.success)<<result.error_message;
            const auto expected_path=run==0?snapshot:(stateless?compact_second_path:full_second);
            std::ifstream expected(expected_path,std::ios::binary),actual(copy,std::ios::binary);
            EXPECT_EQ(std::string(std::istreambuf_iterator<char>(expected),{}),std::string(std::istreambuf_iterator<char>(actual),{}));
            if(run==1) {
                const auto before=db->getHeaderMetadata(metadata.block_hash);ASSERT_TRUE(before.ok());std::string error;
                EXPECT_FALSE(service->InvalidateBlock(metadata.block_hash,error));
                EXPECT_EQ(db->getHeaderMetadata(metadata.block_hash)->status_flags,before->status_flags);
                ASSERT_TRUE(service->InvalidateBlock(second_metadata.block_hash,error))<<error;
                ASSERT_EQ(service->GetActiveTip()->hash,metadata.block_hash);
                EXPECT_EQ(db->getOrchardNullifierOwner(spent_nullifier).status(),Status::NotFound);
                auto queue=ValidationQueue::CreateCanonicalOnly();context.validation_queue=queue;
                auto ingress=std::make_shared<BlockIngressService>();ASSERT_TRUE(ingress->Init(context));ASSERT_TRUE(ingress->Start());
                context.block_ingress=ingress.get();queue->start();
                const auto forked=ingress->SubmitHex(util::hex(sibling_bytes),BlockOrigin::P2P);
                ASSERT_TRUE(forked.connected)<<forked.reason;ASSERT_EQ(service->GetActiveTip()->hash,sibling_header.GetHash());
                ASSERT_TRUE(service->ReconsiderBlock(second_metadata.block_hash,error))<<error;
                const auto retained=ingress->SubmitHex(util::hex(third->WireBytes()),BlockOrigin::P2P);
                ASSERT_TRUE(retained.retained()||retained.connected)<<retained.reason;
                service->PumpReplayMetadataRecovery();
                ASSERT_EQ(service->GetActiveTip()->hash,third->Header().GetHash());
                EXPECT_EQ(db->getOrchardNullifierOwner(spent_nullifier).value(),second_metadata.block_hash);
                EXPECT_EQ(db->getOrchardCatalogState(third->Header().GetHash()).value(),
                    source.f.db.getOrchardCatalogState(third->Header().GetHash()).value());
                queue->stop();context.validation_queue.reset();context.block_ingress=nullptr;ingress->Stop();
            }
            if(run==0) {
                auto queue=ValidationQueue::CreateCanonicalOnly();context.validation_queue=queue;
                auto ingress=std::make_shared<BlockIngressService>();ASSERT_TRUE(ingress->Init(context));ASSERT_TRUE(ingress->Start());
                context.block_ingress=ingress.get();queue->start();
                const auto connected=ingress->SubmitHex(util::hex(second->WireBytes()),BlockOrigin::P2P);
                ASSERT_TRUE(connected.connected)<<connected.reason;
                EXPECT_EQ(service->GetActiveTip()->hash,second_metadata.block_hash);
                EXPECT_EQ(db->getOrchardNullifierOwner(spent_nullifier).value(),second_metadata.block_hash);
                EXPECT_EQ(db->getOrchardCatalogState(second_metadata.block_hash).value(),*second_catalog);
                if(!stateless){EXPECT_FALSE(service->GetConsensusUTXOSet()->GetCoin(legacy_coin));
                    EXPECT_FALSE(service->GetConsensusUTXOSet()->GetCoin(base_coin));}
                queue->stop();context.validation_queue.reset();context.block_ingress=nullptr;ingress->Stop();
                if(!stateless) {
                    // Supply genuinely mined historical bodies only after the
                    // assumed tip has advanced. Completion must not rewind it
                    // or overwrite the new coins with the snapshot's base set.
                    for(uint32_t height=1;height<source.f.blocks.size();++height) {
                        const auto& block=source.f.blocks[height];const auto hash=block.GetHash();
                        const auto position=context.block_storage->writeBlock(hash,block);ASSERT_TRUE(position.ok());
                        auto row=db->getHeaderMetadata(hash);ASSERT_TRUE(row.ok());
                        row->file_number=position->file_number;row->data_pos=position->offset;row->data_size=position->size;
                        row->status_flags|=BLOCK_HAVE_DATA;
                        ASSERT_EQ(db->putHeaderMetadata(source.f.token,hash,*row),Status::Ok);
                    }
                    for(unsigned attempt=0;attempt<2000&&
                        service->GetBackgroundValidationProgress().status==ChainstateService::BackgroundValidationStatus::InProgress;++attempt)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    const auto progress=service->GetBackgroundValidationProgress();
                    ASSERT_TRUE(service->IsBackgroundValidationComplete())<<progress.error_message;
                    EXPECT_FALSE(service->IsAssumeUTXOActive());EXPECT_EQ(service->GetActiveTip()->hash,second_metadata.block_hash);
                    EXPECT_FALSE(service->GetConsensusUTXOSet()->GetCoin(legacy_coin));
                    EXPECT_FALSE(service->GetConsensusUTXOSet()->GetCoin(base_coin));
                } else EXPECT_FALSE(service->IsBackgroundValidationComplete());
            }
            pool->Stop();context.mempool.reset();pool.reset();
            service->Stop();context.chainstate.reset();service.reset();
        }
    }
}
TEST(OrchardSnapshotRuntime, IndependentLaterRegtestGatePreservesRoot1ThenExportsRoot2) {
    using namespace consensus;using Access=OrchardSnapshotStoreTestAccess;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=103;
    MutableParams().state_commitment_burial_depth=0;
    source.f.service->setRuntimeBlockNotifications(std::make_shared<TypedForkNotices>());
    auto headers=std::make_shared<HeaderChainSelector>();
    for(const auto& block:source.f.blocks)ASSERT_TRUE(headers->AddHeader(block.header));
    source.f.service->setHeaderChainSelector(headers);
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    shielded_store_fixture::TempDir directory;const auto path=directory.path/"snapshot.v6";
    {std::ofstream previous(path);previous<<"retain before activation";}
    EXPECT_FALSE(source.f.service->ExportSnapshot(path).success);
    {std::ifstream previous(path);EXPECT_EQ(std::string(std::istreambuf_iterator<char>(previous),{}),"retain before activation");}
    BlockAssembler assembler(&source.f.db);WireOrchardAssembler(assembler,source.f);
    const auto second=SolveCatalogTemplate(assembler.CreateOrchardBlock(OrchardMiningPayout));ASSERT_TRUE(second);
    ASSERT_TRUE(source.Submit(second->WireBytes()).connected);
    const auto exported=source.f.service->ExportSnapshot(path);ASSERT_TRUE(exported.success)<<exported.error_message;
    std::ifstream input(path,std::ios::binary);const auto file=ReadOrchardSnapshotFile(input,std::filesystem::file_size(path),OrchardSnapshotProfile::Full);
    EXPECT_EQ(file.metadata.block_height,103u);EXPECT_EQ(file.orchard.parent_catalog.block,first->Header().GetHash());
    const auto database=directory.path/"chainstate";Access::CreateFresh(database);ChainDB db;ASSERT_EQ(db.init(database),Status::Ok);
    BlockStorage files;ASSERT_EQ(files.init(directory.path/"files"),Status::Ok);AnnotatedRecursiveMutex mutex;
    const auto installed=Access::Install(mutex,db,source.f.token,files,*headers,file);ASSERT_TRUE(installed.full);
    EXPECT_EQ(installed.full->GetBestBlock(),second->Header().GetHash());
    ASSERT_NO_THROW(Access::Restore(mutex,db,*headers));
}
TEST(OrchardSnapshotRuntime, UnmarkedEmptyReadyStoreCannotSelfInitializeGenesis) {
    shielded_store_fixture::TempDir directory;const auto path=directory.path/"chainstate";
    OrchardSnapshotStoreTestAccess::CreateFresh(path);auto db=std::make_shared<ChainDB>();ASSERT_EQ(db->init(path),Status::Ok);
    const auto token=ChainWriteToken::CreateForTesting();
    {rocksdb::WriteBatch batch;batch.Delete(storage::OrchardSnapshotFreshKey);ASSERT_EQ(db->writeBatch(token,std::move(batch),true),Status::Ok);}
    DaemonContext context;context.config=std::make_shared<ConfigService>();context.config->Set("datadir",directory.path.string());
    context.logger=std::make_shared<LoggerService>("");context.block_storage=std::make_shared<BlockStorage>();
    ASSERT_EQ(context.block_storage->init(directory.path/"files"),Status::Ok);
    auto service=std::make_shared<ChainstateService>();context.chainstate=service;service->setOwnedChainDB(db);
    EXPECT_FALSE(service->Init(context));EXPECT_FALSE(service->IsStarted());EXPECT_EQ(service->GetActiveTip(),nullptr);
    EXPECT_EQ(db->getTip().status(),Status::NotFound);EXPECT_EQ(db->getBlockHashByHeight(0).status(),Status::NotFound);
    size_t coins=0;ASSERT_EQ(db->forEachUTXO([&](const auto&,uint32_t,const auto&){++coins;return true;}),Status::Ok);EXPECT_EQ(coins,0u);
    service->Stop();context.chainstate.reset();
}
} // namespace dinero
#endif
