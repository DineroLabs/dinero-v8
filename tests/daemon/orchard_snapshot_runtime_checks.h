#pragma once
#include "consensus/utreexo_delta_codec.h"
#include "p2p/orchard_catalog_transfer.h"
#include "p2p/orchard_catalog_server.h"
#include "p2p/orchard_catalog_client.h"
#include "p2p/orchard_catalog_download.h"
#include "p2p/orchard_phone_history.h"
#include "daemon/services/orchard_boundary_replay.h"
#include "daemon/orchard_snapshot_store.h"
#include "daemon/services/block_ingress_service.h"
#include "consensus/validation_queue.h"
#include "daemon/reopen_candidate_journal.h"
#include "daemon/datadir_guard.h"
#include "daemon/daemon_app.h"
#include "wallet/bip39.h"
#include "storage/orchard_activation_origin.h"
#include <sstream>
#include <json/json.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
namespace outbox_detail {
std::string Key(uint64_t);
std::string Encode(const RuntimeOutboxEvent&);
std::string EncodeHead(const RuntimeOutboxCursor&);
}
struct OrchardSnapshotStoreTestAccess {
    using ActivationPublication=OrchardSnapshotStore::ActivationPublication;
    static auto RowsDigest(const ChainDB& db){return OrchardSnapshotStore::HandoffStoreDigest(db,{});}
    static auto AuditStore(AnnotatedRecursiveMutex& mutex,const ChainDB& original,const ChainDB& db,
        const ChainWriteToken& token,const BlockStorage& files,const uint256& parent,
        std::shared_ptr<const consensus::HeaderChainSelector> headers,
        std::shared_ptr<const consensus::OrchardSnapshotSection> proof,const std::function<bool()>& cancel={}){
        std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);
        return OrchardSnapshotStore::AuditActivationStore(mutex,original,db,token,files,parent,std::move(headers),std::move(proof),cancel);}
    static auto CopyParent(const ChainDB& original,const ChainWriteToken& token,
        AnnotatedRecursiveMutex& mutex,const consensus::HeaderChainSelector& headers,
        const consensus::OrchardSnapshotSection& proof,const uint256& source,
        const std::filesystem::path& destination,const std::function<void()>& created,
        const std::function<void()>& choice,const std::function<bool()>& cancel={}){
        return OrchardSnapshotStore::CopyActivationParent(original,token,mutex,headers,proof,
            source,destination,created,choice,cancel);}
    static auto PrepareHandoff(const daemon::DatadirGuard& root,const ChainWriteToken& token,
        std::shared_ptr<const consensus::HeaderChainSelector> headers,
        std::shared_ptr<const consensus::OrchardSnapshotSection> proof,const std::function<bool()>& cancelled={}){
        return OrchardSnapshotStore::PrepareActivationHandoff(root,token,std::move(headers),std::move(proof),cancelled);}
    static void CheckHandoff(const daemon::DatadirGuard& root,const PreparedOrchardActivationHandoff& prepared){
        OrchardSnapshotStore::CheckActivationHandoff(root,prepared);}
#ifdef DINERO_TEST_REAL_CORE_HANDOFF
    static auto SelectHandoff(const daemon::DatadirGuard& root,const CoreReopenReceipt& receipt,
        std::unique_ptr<PreparedOrchardActivationHandoff> prepared,const std::function<bool()>& cancel={}){
        return OrchardSnapshotStore::SelectActivationHandoff(root,receipt.ClosedForHandoff(root),std::move(prepared),cancel);}
    static auto RecoverHandoff(const daemon::DatadirGuard& root,const CoreReopenReceipt& receipt,
        std::shared_ptr<const consensus::HeaderChainSelector> headers,const std::function<bool()>& cancel={}){
        return OrchardSnapshotStore::RecoverActivationHandoff(root,receipt.ClosedForHandoff(root),std::move(headers),cancel);}
    static auto OpenHandoff(const daemon::DatadirGuard& root,const CoreReopenReceipt& receipt,
        AnnotatedRecursiveMutex& mutex,const ChainWriteToken& token,const BlockStorage& files,
        std::shared_ptr<const consensus::HeaderChainSelector> headers,
        std::unique_ptr<PreparedOrchardActivationHandoff> prepared){
        std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);
        return OrchardSnapshotStore::OpenActivationHandoff(root,receipt.ClosedForHandoff(root),mutex,token,files,std::move(headers),std::move(prepared));}
#endif
    static void NoHandoff(const std::filesystem::path& root){OrchardSnapshotStore::RequireNoActivationHandoff(root);}
    static auto ReadHandoffHeaders(const ChainDB& db,const uint256& first,const std::function<bool()>& cancel={}){
        return OrchardSnapshotStore::ReadActivationHeaders(db,first,cancel);}
    static auto AuditSuffix(AnnotatedRecursiveMutex& mutex,const ChainDB& db,const ChainWriteToken& token,
        const BlockStorage& files,std::shared_ptr<const consensus::HeaderChainSelector> headers,
        std::shared_ptr<const consensus::OrchardSnapshotSection> proof,const std::function<bool()>& cancel={}){
        std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);
        return OrchardSnapshotStore::AuditActivationSuffix(mutex,db,token,files,std::move(headers),std::move(proof),cancel);}
    static auto AuditEvents(AnnotatedRecursiveMutex& mutex,const ChainDB& original,const ChainDB& db,
        const BlockStorage& files,std::shared_ptr<const consensus::HeaderChainSelector> headers,
        std::shared_ptr<const consensus::OrchardSnapshotSection> proof,const std::function<bool()>& cancel={}){
        std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);
        return OrchardSnapshotStore::AuditActivationEvents(mutex,original,db,files,std::move(headers),std::move(proof),cancel);}
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
    static auto RestoreParent(AnnotatedRecursiveMutex& mutex,const ChainDB& db,const consensus::HeaderChainSelector& headers,
        const consensus::OrchardSnapshotSection& proof){
        std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);return OrchardSnapshotStore::RestoreActivationParent(mutex,db,headers,proof);}
    static void RetainProof(AnnotatedRecursiveMutex& mutex,ChainDB& db,const ChainWriteToken& token,
        const consensus::HeaderChainSelector& headers,const consensus::OrchardSnapshotSection& proof,
        const std::function<bool()>& cancelled={}){
        std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);OrchardSnapshotStore::RetainActivationProof(mutex,db,token,headers,proof,cancelled);}
    static auto ReadProof(AnnotatedRecursiveMutex& mutex,const ChainDB& db,const consensus::HeaderChainSelector& headers,
        const uint256& first,const std::function<bool()>& cancelled={}){
        std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);return OrchardSnapshotStore::ReadActivationProof(mutex,db,headers,first,cancelled);}
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
namespace {
struct TransferredActivationParentFixture {
    std::shared_ptr<consensus::HeaderChainSelector> headers=std::make_shared<consensus::HeaderChainSelector>();
    std::optional<consensus::OrchardSnapshotSection> proof;
    CompactStartupFixture disk;
    TransferredActivationParentFixture():disk([this](auto& source){
        for(const auto& b:source.f.blocks)OrchardAdmissionFixture::Require(headers->AddHeader(b.header));
        OrchardAdmissionFixture::Require(headers->AddHeader(source.first->Header())&&headers->AddHeader(source.second->Header()));
        proof=OrchardSnapshotStoreTestAccess::CaptureActivation(source.f.db,*headers);
    },OrchardAdmissionFixture::HistoricalSpend::None,true){}
    void Parent(){auto old=disk.Restore();disk.Undo(*old,disk.second);disk.Undo(*old,disk.first);}
    auto Restore(){return OrchardSnapshotStoreTestAccess::RestoreParent(disk.startup_mutex,disk.reopened,*headers,*proof);}
    void Retain(const std::function<bool()>& cancelled={}){
        OrchardSnapshotStoreTestAccess::RetainProof(disk.startup_mutex,disk.reopened,disk.f.token,*headers,*proof,cancelled);}
    auto Read(const uint256& first,const std::function<bool()>& cancelled={}){
        return OrchardSnapshotStoreTestAccess::ReadProof(disk.startup_mutex,disk.reopened,*headers,first,cancelled);}
};
#ifndef _WIN32
struct ActivationHandoffFixture {
    std::shared_ptr<consensus::HeaderChainSelector> headers=std::make_shared<consensus::HeaderChainSelector>();
    std::shared_ptr<const consensus::OrchardSnapshotSection> proof;
    CompactStartupFixture full;
    std::filesystem::path root,path;daemon::DatadirGuard guard;
#ifdef DINERO_TEST_REAL_CORE_HANDOFF
    std::unique_ptr<DaemonApp> closed_app;
    std::unique_ptr<CoreReopenReceipt> receipt;
    std::shared_ptr<ReopenRpcGate> gate;
#endif
    explicit ActivationHandoffFixture(bool lifecycle=false,
        const std::function<void(CompactStartupFixture&)>& capture_parent={}):full([this](auto& source){
        for(const auto& block:source.f.blocks)OrchardAdmissionFixture::Require(headers->AddHeader(block.header));
        OrchardAdmissionFixture::Require(headers->AddHeader(source.first->Header())&&headers->AddHeader(source.second->Header()));
        proof=std::make_shared<const consensus::OrchardSnapshotSection>(OrchardSnapshotStoreTestAccess::CaptureActivation(source.f.db,*headers));
    },OrchardAdmissionFixture::HistoricalSpend::None,true,true,capture_parent){
        auto name=(std::filesystem::temp_directory_path()/"orchard-handoff-evidence-XXXXXX").string();
        OrchardAdmissionFixture::Require(::mkdtemp(name.data())!=nullptr);root=std::filesystem::canonical(name);
        std::filesystem::create_directory(root/"blockchain");path=root/"blockchain"/"chaindb";
        std::string error;OrchardAdmissionFixture::Require(guard.Acquire(root,error));
        if(lifecycle){
#ifdef DINERO_TEST_REAL_CORE_HANDOFF
            // A REAL, EMPTY-GENESIS lifecycle receipt under this exact root.
            // Only after full Stop do we substitute the separate offline parent
            // fixture. This composes the control mechanisms; it is deliberately
            // NOT the actual activation/named-wallet/queued-payment acceptance.
            closed_app=std::make_unique<DaemonApp>();closed_app->SetLockedDatadir(guard);
            closed_app->SetAutomaticStorageUpgrade(true);
            std::vector<std::string> args{"handoff-lifecycle-fixture","--regtest","--datadir="+root.string(),
                "--p2p.offline=1","--listen=0","--rpc.port=0","--wallet.socket.enable=0",
                "--lightning.oracles.enable=0","--sync-profile=mac_fullblock","--utreexo-stateless=0"};
            std::vector<char*> argv;for(auto& arg:args)argv.push_back(arg.data());
            OrchardAdmissionFixture::Require(closed_app->Init(int(argv.size()),argv.data())&&closed_app->Start());
            const auto wallet=std::dynamic_pointer_cast<WalletService>(closed_app->GetContext().wallet);
            {auto use=WalletService::AcquireWalletUse(wallet);
                use->Wallet().createFromBip39("handoff-named-owner",
                    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about","",false);
                wallet->EnsureRuntimeWalletBindings();}
            gate=closed_app->GetContext().reopen_rpc_gate;receipt=closed_app->CloseForInternalReopen(guard);
            OrchardAdmissionFixture::Require(receipt&&receipt->WalletName()=="handoff-named-owner");
            // App must first be destroyed before moving even this test-only
            // genesis store: keep its identity alive separately for a refusal
            // assertion using a fresh pending-recovery App below.
            closed_app.reset();
            std::filesystem::rename(path,root/"blockchain"/"genesis-fixture-preserved");
#else
            OrchardAdmissionFixture::Require(false); // Real App fixture requires the linked SDK.
#endif
        }
        std::filesystem::copy(full.f.path,path,std::filesystem::copy_options::recursive);
        {std::ofstream wallet(root/"named-wallet.sentinel");wallet<<"opaque test-only named wallet/pending bytes";}
        {std::ofstream lifecycle(root/"blockchain"/"original-lifecycle.sentinel");lifecycle<<"do not rebase or promote original history";}
        std::cout<<"Activation handoff evidence: "<<root.string()<<"\n";
    }
    auto Prepare(const std::function<bool()>& cancel={}){
        return OrchardSnapshotStoreTestAccess::PrepareHandoff(guard,full.f.token,headers,proof,cancel);
    }
#ifdef DINERO_TEST_REAL_CORE_HANDOFF
    void ColdReceipt(){
        receipt.reset();auto fresh=std::make_unique<DaemonApp>();fresh->SetLockedDatadir(guard);
        receipt=fresh->PreparePendingReopen(guard,gate);
        OrchardAdmissionFixture::Require(receipt&&receipt->WalletName()=="handoff-named-owner");
        closed_app=std::move(fresh); // Must be destroyed before actual selection.
    }
#endif
    static auto Files(const std::filesystem::path& dir){
        std::map<std::string,std::string> result;
        for(const auto& item:std::filesystem::recursive_directory_iterator(dir))if(item.is_regular_file()){
            std::ifstream in(item.path(),std::ios::binary);result.emplace(item.path().lexically_relative(dir).generic_string(),
                std::string((std::istreambuf_iterator<char>(in)),{}));OrchardAdmissionFixture::Require(!in.bad());
        }return result;
    }
    void MutateSource(const std::function<void(shielded_store_fixture::Raw&)>& fn){
        auto families=shielded_store_fixture::legacy;families.push_back(shielded_store_fixture::shielded);
        shielded_store_fixture::Raw raw(path,families);fn(raw);
    }
    static void MutateCopy(const std::filesystem::path& path,const std::function<void(shielded_store_fixture::Raw&)>& fn){
        auto families=shielded_store_fixture::legacy;families.push_back(shielded_store_fixture::shielded);
        shielded_store_fixture::Raw raw(path,families);fn(raw);
    }
};
#endif
}
#ifndef _WIN32
TEST(OrchardActivationHeaders, MissingPrehistoryAndBurialRowsPersistAsHeadersOnlyWithoutMovingMarkers){
    ActivationHandoffFixture f;
    const std::vector<uint256> absent{f.full.f.blocks.at(1).GetHash(),f.full.f.blocks.at(50).GetHash(),
        f.full.first->Header().GetHash(),f.full.second->Header().GetHash()};
    f.MutateSource([&](auto& raw){
        rocksdb::WriteOptions sync;sync.sync=true;
        for(const auto& hash:absent){
            ASSERT_TRUE(raw.db->Delete(sync,raw.cf("headers"),"h"+hash.GetHex()).ok());
            ASSERT_TRUE(raw.db->Delete(sync,raw.cf("headers"),"m"+hash.GetHex()).ok());
        }
    });
    {ChainDB source;ASSERT_EQ(source.init(f.path),Status::Ok);
        ASSERT_EQ(source.setValidatedTip(f.full.f.token,f.full.f.blocks.front().GetHash(),0),Status::Ok);}
    const auto original=ActivationHandoffFixture::Files(f.path);auto prepared=f.Prepare();ASSERT_TRUE(prepared);
    const auto& source=prepared->OriginalForInspection();const auto& copy=prepared->CandidateForInspection();
    const auto cold=OrchardSnapshotStoreTestAccess::ReadHandoffHeaders(copy,f.proof->state.block_hash);
    ASSERT_TRUE(cold);ASSERT_TRUE(cold->GetBestHeaderValue());EXPECT_EQ(cold->GetBestHeaderValue()->height,103u);
    EXPECT_EQ(cold->GetBestHeaderValue()->hash,f.full.second->Header().GetHash());
    for(const auto& hash:absent){
        const auto expected=f.headers->GetHeaderValue(hash);ASSERT_TRUE(expected);
        EXPECT_EQ(source.getHeader(hash).status(),Status::NotFound);ASSERT_TRUE(copy.getHeader(hash).ok());
        EXPECT_EQ(copy.getHeader(hash)->SerializeForHash(),expected->header.SerializeForHash());
        EXPECT_EQ(copy.getBlockHeight(hash).value(),int(expected->height));EXPECT_EQ(copy.getBlockWork(hash).value(),expected->chainwork);
        EXPECT_EQ(copy.getHeaderMetadata(hash).status(),Status::NotFound); // No body/CHAIN/SCRIPTS flags manufactured.
    }
    EXPECT_EQ(copy.getBlockHashByHeight(102).status(),Status::NotFound);EXPECT_EQ(copy.getBlockHashByHeight(103).status(),Status::NotFound);
    EXPECT_EQ(copy.getTip()->height,101);EXPECT_EQ(copy.getValidatedTip()->height,0);
    EXPECT_ANY_THROW(consensus::CheckPreparedLegacyRetirementUnderLock(copy,f.proof->retirement.record));
    EXPECT_EQ(ActivationHandoffFixture::Files(f.path),original);
    EXPECT_NO_THROW(OrchardSnapshotStoreTestAccess::CheckHandoff(f.guard,*prepared));
}
TEST(OrchardActivationHeaders, ColdClosureMissingCorruptRowsAndConflictingOriginalNeverRepairOrGrantOwner){
    ActivationHandoffFixture f;auto prepared=f.Prepare();const auto candidate=prepared->CandidatePath();prepared.reset();
    const auto first=f.proof->state.block_hash;const auto key="orchard_activation_headers_v1:"+first.GetHex();
    for(unsigned variant=0;variant<4;++variant){
        const auto path=f.root/"blockchain"/("header-negative-"+std::to_string(variant));
        std::filesystem::copy(candidate,path,std::filesystem::copy_options::recursive);
        ActivationHandoffFixture::MutateCopy(path,[&](auto& raw){
            rocksdb::WriteOptions sync;sync.sync=true;
            if(variant==0)ASSERT_TRUE(raw.db->Delete(sync,raw.cf("default"),key).ok());
            else if(variant==1)raw.put("default",key,"corrupt descriptor");
            else if(variant==2)ASSERT_TRUE(raw.db->Delete(sync,raw.cf("headers"),"h"+first.GetHex()).ok());
            else {std::string row;ASSERT_TRUE(raw.db->Get(rocksdb::ReadOptions(),raw.cf("headers"),"h"+first.GetHex(),&row).ok());
                row.push_back('x');raw.put("headers","h"+first.GetHex(),row);}
        });
        ChainDB cold;ASSERT_EQ(cold.init(path),Status::Ok);const auto before=ChainDBTransactionReadTestPeer::HandoffRows(cold);
        EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::ReadHandoffHeaders(cold,first));
        EXPECT_ANY_THROW(storage::RequireFullOrchardStorage(cold));
        EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(cold),before);
    }
    f.MutateSource([&](auto& raw){
        const auto h=f.full.f.blocks.at(1).GetHash();std::string row;
        ASSERT_TRUE(raw.db->Get(rocksdb::ReadOptions(),raw.cf("headers"),"h"+h.GetHex(),&row).ok());
        row.back()^=1;raw.put("headers","h"+h.GetHex(),row);
    });
    const auto original=ActivationHandoffFixture::Files(f.path);EXPECT_ANY_THROW(f.Prepare());
    EXPECT_EQ(ActivationHandoffFixture::Files(f.path),original);
}
#ifdef DINERO_TEST_REAL_CORE_HANDOFF
namespace {
struct ActivationSuffixFixture:ActivationHandoffFixture {
    AnnotatedRecursiveMutex mutex;OrchardSnapshotStoreTestAccess::ActivationPublication live;
    std::filesystem::path original_path;uint256 parent_rows;
    explicit ActivationSuffixFixture(bool runtime_policy=false):ActivationHandoffFixture(true){
        {ChainDB db;OrchardAdmissionFixture::Require(db.init(path)==Status::Ok&&
            db.setValidatedTip(full.f.token,full.f.blocks.front().GetHash(),0)==Status::Ok);
            if(runtime_policy)OrchardAdmissionFixture::Require(db.setPruneMode(full.f.token,false)==Status::Ok);}
        auto selected=OrchardSnapshotStoreTestAccess::SelectHandoff(guard,*receipt,Prepare());
        original_path=selected->OriginalPath();
        live=OrchardSnapshotStoreTestAccess::OpenHandoff(guard,*receipt,mutex,full.f.token,*full.files,headers,std::move(selected));
        parent_rows=OrchardSnapshotStoreTestAccess::RowsDigest(*live.database);
    }
    void Connect(const std::shared_ptr<const CatalogSolvedTemplate>& source){
        const auto block=OrchardBlockCandidate::DecodeExact(source->WireBytes());
        const auto c=*consensus::SelectedOrchardBlockContext(block.Header(),source->Height());
        auto write=PreparedOrchardChainstateWrite::ConnectCompactIndexed(mutex,*live.database,full.f.token,*full.files,
            *FindBlockIndex(c.block_hash),*live.compact,c,block,*live.database->getHeader(c.parent_hash),
            [&](uint32_t h){return full.Mtp(h);},true,std::nullopt,true);write->Commit();
    }
    void Undo(const std::shared_ptr<const CatalogSolvedTemplate>& source){
        const auto block=OrchardBlockCandidate::DecodeExact(source->WireBytes());
        const auto c=*consensus::SelectedOrchardBlockContext(block.Header(),source->Height());
        auto write=PreparedOrchardChainstateWrite::DisconnectCompactIndexed(mutex,*live.database,full.f.token,*full.files,
            *FindBlockIndex(c.block_hash),*live.compact,c,block,*live.database->getHeader(c.parent_hash),
            [&](uint32_t h){return full.Mtp(h);},true);write->Commit();
    }
    auto Audit(const std::function<bool()>& cancel={}){
        return OrchardSnapshotStoreTestAccess::AuditSuffix(mutex,*live.database,full.f.token,*full.files,headers,proof,cancel);
    }
    auto Events(const std::function<bool()>& cancel={}){
        ChainDB original;OrchardAdmissionFixture::Require(ChainDBTransactionReadTestPeer::HandoffReadOnly(original,original_path)==Status::Ok);
        return OrchardSnapshotStoreTestAccess::AuditEvents(mutex,original,*live.database,*full.files,headers,proof,cancel);
    }
    auto Whole(const std::function<bool()>& cancel={}){
        ChainDB original;OrchardAdmissionFixture::Require(ChainDBTransactionReadTestPeer::HandoffReadOnly(original,original_path)==Status::Ok);
        return OrchardSnapshotStoreTestAccess::AuditStore(mutex,original,*live.database,full.f.token,*full.files,parent_rows,headers,proof,cancel);
    }
};
}
TEST(OrchardActivationStore, ExactOriginalAndCanonicalConnectUndoReconnectMatchEveryFamily){
    ActivationSuffixFixture f;
    for(unsigned phase=0;phase<4;++phase){
        if(phase==0||phase==2){f.Connect(f.full.first);f.Connect(f.full.second);}
        else if(phase==1){f.Undo(f.full.second);f.Undo(f.full.first);}
        else f.Undo(f.full.second);
        const auto rows=ChainDBTransactionReadTestPeer::HandoffRows(*f.live.database);
        const auto before=ActivationHandoffFixture::Files(f.original_path);
        EXPECT_EQ(f.Whole().block,f.live.database->getTip()->hash);
        EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(*f.live.database),rows);
        EXPECT_EQ(ActivationHandoffFixture::Files(f.original_path),before);
        EXPECT_EQ(f.live.database->getValidatedTip()->height,0);
    }
    f.live.compact.reset();f.live.database.reset();
    ChainDB original,cold;
    ASSERT_EQ(ChainDBTransactionReadTestPeer::HandoffReadOnly(original,f.original_path),Status::Ok);
    ASSERT_EQ(ChainDBTransactionReadTestPeer::HandoffReadOnly(cold,f.path),Status::Ok);
    const auto rows=ChainDBTransactionReadTestPeer::HandoffRows(cold);
    const auto files=ActivationHandoffFixture::Files(f.path);
    EXPECT_EQ(OrchardSnapshotStoreTestAccess::AuditStore(f.mutex,original,cold,f.full.f.token,*f.full.files,
        f.parent_rows,f.headers,f.proof).block,f.full.first->Header().GetHash());
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(cold),rows);
    EXPECT_EQ(ActivationHandoffFixture::Files(f.path),files);
}
void GenerateCompletedActivationFixtures(){
    const auto* destination=std::getenv("DINERO_COMPLETED_ACTIVATION_MANIFEST");ASSERT_NE(destination,nullptr);
    ASSERT_FALSE(std::filesystem::exists(destination));Json::Value roots(Json::arrayValue);
    const auto* limit_text=std::getenv("DINERO_COMPLETED_ACTIVATION_LIMIT");
    const auto limit=limit_text?std::stoul(limit_text):8u;ASSERT_GE(limit,1u);ASSERT_LE(limit,8u);
    for(unsigned phase=0;phase<limit;++phase){
        ActivationSuffixFixture f(phase!=7);f.Connect(f.full.first);f.Connect(f.full.second);
        if(phase==1||phase==2){f.Undo(f.full.second);if(phase==2)f.Undo(f.full.first);}
        Json::Value row;row["datadir"]=f.root.string();row["original"]=f.original_path.string();
        row["tip"]=f.live.database->getTip()->hash.GetHex();row["height"]=f.live.database->getTip()->height;
        row["profile"]=ConsensusChecksum(Params());row["phase"]=phase;
        row["refuse"]=phase>=3;
        const auto archives=f.full.files->getBlockFilePath(0).parent_path();
        if(std::filesystem::exists(f.root/"blocks"))std::filesystem::rename(f.root/"blocks",f.root/"genesis-archives-preserved");
        std::filesystem::copy(archives,f.root/"blocks",std::filesystem::copy_options::recursive);
        if(phase==3){rocksdb::WriteBatch extra;extra.Put("untracked-after-conversion","must not be repaired");
            ASSERT_EQ(f.live.database->writeBatch(f.full.f.token,std::move(extra),true),Status::Ok);}
        if(phase==5){const auto tip=*f.live.database->getTip();
            ASSERT_EQ(f.live.database->setValidatedTip(f.full.f.token,tip.hash,tip.height),Status::Ok);}
        f.live.compact.reset();f.live.database.reset();f.receipt.reset();f.closed_app.reset();f.gate.reset();
        if(phase==4){
            std::filesystem::copy(f.original_path,f.root/"original-before-negative",std::filesystem::copy_options::recursive);
            ActivationHandoffFixture::MutateCopy(f.original_path,[](auto& raw){raw.put("default","changed-original-before-image","must refuse");});
        }
        if(phase==6){const auto control=f.root/"blockchain"/storage::OrchardHandoffCompleted;
            const auto retained=control.string()+".preserved-original";std::filesystem::rename(control,retained);
            std::filesystem::create_symlink(std::filesystem::path(retained).filename(),control);}
        f.guard.Release();
        roots.append(row);
    }
    std::ofstream output(destination);ASSERT_TRUE(output);output<<Json::writeString(Json::StreamWriterBuilder(),roots);output.close();ASSERT_TRUE(output);
}
void GenerateSdkActivationFixture(){
    const auto* destination=std::getenv("DINERO_SDK_ACTIVATION_MANIFEST");ASSERT_NE(destination,nullptr);
    ASSERT_FALSE(std::filesystem::exists(destination));
    // Select the private wire magic BEFORE any BlockStorage archive is written.
    MutableParams().magic=0x02a1c925;
    // Source-ready independently replayed parent. No lifecycle receipt or
    // completed selection is injected: the actual SDK must create both.
    shielded_store_fixture::Rows parent_rows;
    for(const auto& name:shielded_store_fixture::legacy)parent_rows[name];
    parent_rows[shielded_store_fixture::shielded];
    ActivationHandoffFixture f(false,[&](auto& source){
        std::vector<uint8_t> seed;
        OrchardAdmissionFixture::Require(bip39::MnemonicToSeed(
            "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about","",seed));
        OrchardAdmissionFixture::Require(seed.size()==source.f.shield_seed.size());
        std::copy(seed.begin(),seed.end(),source.f.shield_seed.begin());
        source.f.shield_amount=500000;
        constexpr std::string_view memo="authenticated phone history";
        std::copy(memo.begin(),memo.end(),source.f.shield_memo.begin());
        const auto* coins=source.f.service->GetConsensusUTXOSet();OrchardAdmissionFixture::Require(coins!=nullptr);
        const auto lock=coins->LockForestShared();
        OrchardAdmissionFixture::Require(source.f.db.putUtreexoCheckpointWithChecksum(
            source.f.token,101,coins->GetForest().serialize())==Status::Ok);
        for(const auto& [key,value]:ChainDBTransactionReadTestPeer::HandoffRows(source.f.db))
            parent_rows.at(key.first).emplace(key.second,value);
    });
    // Keep the visited/undone source intact, but start THIS test from the exact
    // earlier independent parent capture. Never strip an outbox or fabricate a
    // starting cursor from the subsequently visited source. The real SDK alone
    // will create its lifecycle intent, selection and genuine first event.
    std::filesystem::rename(f.path,f.root/"blockchain/visited-source-preserved");
    {std::vector<std::string> names;for(const auto& [name,rows]:parent_rows)names.push_back(name);
     shielded_store_fixture::Raw raw(f.path,names);
     for(const auto& [name,rows]:parent_rows)for(const auto& [key,value]:rows)raw.put(name,key,value);
     ASSERT_EQ(raw.rows(),parent_rows);}
    // Retain the real independently executed parent forest checkpoint. Without
    // it normal legacy startup correctly rewinds to genesis for catch-up; the
    // peer in this boundary-only test intentionally serves no old CSN bodies.
    {const auto* coins=f.full.f.service->GetConsensusUTXOSet();ASSERT_NE(coins,nullptr);
     const auto lock=coins->LockForestShared();
     ASSERT_EQ(consensus::UtreexoStump::fromForest(coins->GetForest()).serialize(),f.proof->parent_catalog.stump);
     ChainDB db;ASSERT_EQ(db.init(f.path),Status::Ok);
     ASSERT_EQ(db.getUtreexoCheckpoint(101).value(),coins->GetForest().serialize());}
    const auto archives=f.full.files->getBlockFilePath(0).parent_path();
    std::filesystem::copy(archives,f.root/"blocks",std::filesystem::copy_options::recursive);
    Json::Value row;row["datadir"]=f.root.string();row["profile"]=ConsensusChecksum(Params());
    row["magic"]=Json::UInt(Params().magic);row["parent"]=f.proof->parent_catalog.block.GetHex();
    row["first_hash"]=f.full.first->Header().GetHash().GetHex();row["second_hash"]=f.full.second->Header().GetHash().GetHex();
    if(std::getenv("DINERO_SDK_LATE_HISTORY_FIXTURE")){
        // A genuinely NEW root with only a normal authenticated v6 floor.
        // No original parent DB, pre-floor bodies or canonical runtime events
        // are copied. The actual SDK must obtain missing history over TCP.
        const auto late=f.root/"late-phone";ASSERT_TRUE(std::filesystem::create_directory(late));
        ASSERT_TRUE(std::filesystem::create_directory(late/"blockchain"));
        daemon::DatadirGuard guard;std::string error;ASSERT_TRUE(guard.Acquire(late,error));
        consensus::HeaderStore store((late/"headers").string());ASSERT_TRUE(store.Open());
        consensus::HeaderChainSelector headers(&store);
        for(const auto& block:f.full.f.blocks)ASSERT_TRUE(headers.AddHeader(block.header));
        ASSERT_TRUE(headers.AddHeader(f.full.first->Header()));ASSERT_TRUE(headers.AddHeader(f.full.second->Header()));
        consensus::SnapshotMetadata metadata;metadata.version=consensus::SNAPSHOT_VERSION_V6;
        metadata.block_height=102;metadata.block_hash=f.full.first->Header().GetHash();metadata.timestamp=f.full.first->Header().timestamp;
        const auto snapshot=late/"input.v6";
        ASSERT_TRUE(consensus::WriteOrchardSnapshotFileAtomically(snapshot,metadata,*f.proof,{}).success);
        std::ifstream input(snapshot,std::ios::binary);
        const auto file=consensus::ReadOrchardSnapshotFile(input,std::filesystem::file_size(snapshot),consensus::OrchardSnapshotProfile::Compact);
        const auto path=late/"blockchain/chaindb";OrchardSnapshotStoreTestAccess::CreateFresh(path);
        ChainDB db;ASSERT_EQ(db.init(path),Status::Ok);BlockStorage files;ASSERT_EQ(files.init(late/"blocks"),Status::Ok);
        AnnotatedRecursiveMutex mutex;
        const auto installed=OrchardSnapshotStoreTestAccess::Install(mutex,db,f.full.f.token,files,headers,file);
        ASSERT_TRUE(installed.compact);ASSERT_FALSE(installed.full);
        row["late_datadir"]=late.string();row["late_floor"]=102;
    }
    const auto proof_file=f.root/"untrusted-catalog-section.bin";
    {std::ofstream out(proof_file,std::ios::binary);ASSERT_TRUE(out);
     consensus::StreamOrchardSnapshotSection(*f.proof,[&](auto bytes){out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        OrchardAdmissionFixture::Require(bool(out));});}
    row["proof"]=proof_file.string();row["headers"]=Json::Value(Json::arrayValue);
    {consensus::HeaderStore store((f.root/"headers").string());ASSERT_TRUE(store.Open());
     consensus::HeaderChainSelector retained(&store);
     const auto add=[&](const BlockHeader& header){OrchardAdmissionFixture::Require(retained.AddHeader(header));
        const auto bytes=header.SerializeForHash();row["headers"].append(util::hex({bytes.begin(),bytes.end()}));};
     for(const auto& block:f.full.f.blocks)add(block.header);
     add(f.full.first->Header());add(f.full.second->Header());}
    for(const auto& [name,block]:std::vector<std::pair<std::string,std::shared_ptr<const CatalogSolvedTemplate>>>{
        {"first",f.full.first},{"second",f.full.second}}){
        const auto path=f.root/("untrusted-"+name+"-body.bin");std::ofstream out(path,std::ios::binary);ASSERT_TRUE(out);
        const auto& bytes=block->WireBytes();out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        ASSERT_TRUE(out);row[name]=path.string();}
    f.guard.Release();std::ofstream output(destination);ASSERT_TRUE(output);
    output<<Json::writeString(Json::StreamWriterBuilder(),row);output.close();ASSERT_TRUE(output);
}
void DiagnoseCompletedActivationRows(){
    const auto* actual=std::getenv("DINERO_COMPLETED_COMPARE_ACTUAL");
    const auto* expected=std::getenv("DINERO_COMPLETED_COMPARE_EXPECTED");ASSERT_NE(actual,nullptr);ASSERT_NE(expected,nullptr);
    const auto a=std::filesystem::canonical(actual),e=std::filesystem::canonical(expected);
    ASSERT_EQ(a.filename(),"chaindb");ASSERT_EQ(a.parent_path().filename(),"blockchain");
    ASSERT_TRUE(a.parent_path().parent_path().filename().string().starts_with("orchard-handoff-evidence-"));
    ASSERT_EQ(e.parent_path().parent_path(),a.parent_path());ASSERT_EQ(e.filename(),"expected");
    ASSERT_TRUE(e.parent_path().filename().string().starts_with(".orchard-store-audit-"));
    ChainDB actual_db,expected_db;
    ASSERT_EQ(ChainDBTransactionReadTestPeer::HandoffReadOnly(actual_db,a),Status::Ok);
    ASSERT_EQ(ChainDBTransactionReadTestPeer::HandoffReadOnly(expected_db,e),Status::Ok);
    const auto aa=ChainDBTransactionReadTestPeer::HandoffRows(actual_db),ee=ChainDBTransactionReadTestPeer::HandoffRows(expected_db);
    std::set<std::pair<std::string,std::string>> keys;for(const auto& [key,value]:aa)keys.insert(key);for(const auto& [key,value]:ee)keys.insert(key);
    Json::Value differences(Json::arrayValue);
    for(const auto& key:keys){const auto x=aa.find(key),y=ee.find(key);if(x!=aa.end()&&y!=ee.end()&&x->second==y->second)continue;
        Json::Value row;row["family"]=key.first;row["key_hex"]=util::hex(std::vector<uint8_t>(key.second.begin(),key.second.end()));
        if(x!=aa.end())row["actual_value_hash"]=storage::catalog::Hash(x->second).GetHex();
        if(y!=ee.end())row["expected_value_hash"]=storage::catalog::Hash(y->second).GetHex();differences.append(row);
    }
    std::cout<<Json::writeString(Json::StreamWriterBuilder(),differences)<<"\n";
}
TEST(OrchardActivationStore, UnknownRowsAndInactiveUndoDeletionRefuseWithoutRepair){
    ActivationSuffixFixture f;f.Connect(f.full.first);f.Connect(f.full.second);f.Undo(f.full.second);
    auto& db=*f.live.database;const auto good=ChainDBTransactionReadTestPeer::HandoffRows(db);
    rocksdb::WriteBatch extra;extra.Put("untracked-after-conversion","not a consensus prefix");
    ASSERT_EQ(db.writeBatch(f.full.f.token,std::move(extra),true),Status::Ok);
    const auto bad=ChainDBTransactionReadTestPeer::HandoffRows(db);EXPECT_ANY_THROW(f.Whole());
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(db),bad);
    rocksdb::WriteBatch remove;remove.Delete("untracked-after-conversion");
    ASSERT_EQ(db.writeBatch(f.full.f.token,std::move(remove),true),Status::Ok);
    const auto key=MakeUtreexoDeltaUndoKey(f.full.second->Header().GetHash());std::string old;
    ASSERT_EQ(db.getRaw(key,old),Status::Ok);rocksdb::WriteBatch missing;missing.Delete(key);
    ASSERT_EQ(db.writeBatch(f.full.f.token,std::move(missing),true),Status::Ok);
    const auto absent=ChainDBTransactionReadTestPeer::HandoffRows(db);EXPECT_ANY_THROW(f.Whole());
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(db),absent);
    rocksdb::WriteBatch restore;restore.Put(key,old);ASSERT_EQ(db.writeBatch(f.full.f.token,std::move(restore),true),Status::Ok);
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(db),good);
    EXPECT_EQ(f.Whole().block,f.full.first->Header().GetHash());
}
TEST(OrchardActivationStore, WrongBeforeImageAndCancellationNeverPublishOrAdvanceAnchor){
    ActivationSuffixFixture f;f.Connect(f.full.first);
    ChainDB original;ASSERT_EQ(ChainDBTransactionReadTestPeer::HandoffReadOnly(original,f.original_path),Status::Ok);
    const auto rows=ChainDBTransactionReadTestPeer::HandoffRows(*f.live.database);
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::AuditStore(f.mutex,original,*f.live.database,f.full.f.token,*f.full.files,
        uint256{},f.headers,f.proof));
    original.close(); // Each audit owns the original reservation; no duplicate open.
    EXPECT_ANY_THROW(f.Whole([]{return true;}));
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(*f.live.database),rows);
    EXPECT_EQ(f.live.database->getValidatedTip()->height,0);EXPECT_FALSE(f.gate->Allows(f.gate->Snapshot()));
    EXPECT_EQ(f.Whole().block,f.full.first->Header().GetHash());
}
TEST(OrchardActivationEventHistory, ActualConnectUndoReconnectReplaysFromOriginalWithoutPublishing){
    ActivationSuffixFixture f;
    for(unsigned phase=0;phase<4;++phase){
        if(phase==0||phase==2){f.Connect(f.full.first);f.Connect(f.full.second);}
        else if(phase==1){f.Undo(f.full.second);f.Undo(f.full.first);}
        else f.Undo(f.full.second);
        const auto rows=ChainDBTransactionReadTestPeer::HandoffRows(*f.live.database);
        const auto tip=*f.live.database->getTip();
        EXPECT_EQ(f.Events().block,tip.hash);
        EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(*f.live.database),rows);
        EXPECT_EQ(f.live.database->getValidatedTip()->height,0);
        EXPECT_FALSE(f.gate->Allows(f.gate->Snapshot()));
    }
    // The live owner and both DB handles are actually destroyed. Reopening the
    // retained stores read-only still proves the whole connect/undo suffix;
    // this audit does not publish a replacement owner or lifecycle receipt.
    const auto rows=ChainDBTransactionReadTestPeer::HandoffRows(*f.live.database);
    f.live.compact.reset();f.live.database.reset();const auto files=ActivationHandoffFixture::Files(f.path);
    ChainDB original,cold;ASSERT_EQ(ChainDBTransactionReadTestPeer::HandoffReadOnly(original,f.original_path),Status::Ok);
    ASSERT_EQ(ChainDBTransactionReadTestPeer::HandoffReadOnly(cold,f.path),Status::Ok);
    EXPECT_EQ(OrchardSnapshotStoreTestAccess::AuditEvents(f.mutex,original,cold,*f.full.files,f.headers,f.proof).block,f.full.first->Header().GetHash());
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(cold),rows);
    EXPECT_EQ(ActivationHandoffFixture::Files(f.path),files);
}
TEST(OrchardActivationEventHistory, ReframedFalseMtpPassesLocalOutboxChecksumButFailsAuthenticatedReplay){
    ActivationSuffixFixture f;f.Connect(f.full.first);auto& db=*f.live.database;
    const auto profile=*consensus::SelectedOrchardBlockContext(f.full.first->Header(),f.full.first->Height());
    const auto page=ReadRuntimeOutboxUnderLock(db,profile,{},128);
    ASSERT_FALSE(page.events.empty());auto event=page.events.back();ASSERT_TRUE(event.orchard_replay);
    event.orchard_replay->branch_mtp[0]=event.orchard_replay->branch_mtp[0]+1;
    const auto key=outbox_detail::Key(event.cursor.sequence);const std::string headkey="runtime_orchard_outbox:v1:head";
    std::string oldevent,oldhead;ASSERT_EQ(db.getRaw(key,oldevent),Status::Ok);ASSERT_EQ(db.getRaw(headkey,oldhead),Status::Ok);
    const auto raw=outbox_detail::Encode(event);event.cursor.digest=storage::catalog::Hash(raw.substr(0,raw.size()-32));
    rocksdb::WriteBatch damage;damage.Put(key,raw);damage.Put(headkey,outbox_detail::EncodeHead(event.cursor));
    ASSERT_EQ(db.writeBatch(f.full.f.token,std::move(damage),true),Status::Ok);
    EXPECT_NO_THROW(ReadRuntimeOutboxUnderLock(db,profile,{},128));
    const auto before=ChainDBTransactionReadTestPeer::HandoffRows(db);
    EXPECT_ANY_THROW(f.Events());EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(db),before);
    rocksdb::WriteBatch restore;restore.Put(key,oldevent);restore.Put(headkey,oldhead);
    ASSERT_EQ(db.writeBatch(f.full.f.token,std::move(restore),true),Status::Ok);
    EXPECT_EQ(f.Events().block,f.full.first->Header().GetHash());
}
TEST(OrchardActivationEventHistory, CancellationAndDifferentOriginalRefuseWithoutRepair){
    ActivationSuffixFixture f;f.Connect(f.full.first);f.Connect(f.full.second);
    const auto before=ChainDBTransactionReadTestPeer::HandoffRows(*f.live.database);
    EXPECT_ANY_THROW(f.Events([]{return true;}));
    ChainDB foreign;const auto path=f.root/"foreign-original";ASSERT_EQ(foreign.init(path),Status::Ok);
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::AuditEvents(f.mutex,foreign,*f.live.database,*f.full.files,f.headers,f.proof));
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(*f.live.database),before);
    EXPECT_EQ(f.Events().block,f.full.second->Header().GetHash());
}
TEST(OrchardActivationSuffix, ColdDescendantAndUndoneParentReauthenticateWithoutMarkerOrOwnerPublication){
    for(const bool descendant:{true,false}){
        ActivationSuffixFixture f;f.Connect(f.full.first);f.Connect(f.full.second);
        if(!descendant){f.Undo(f.full.second);f.Undo(f.full.first);}
        const auto hash=f.live.database->getTip()->hash;
        const auto expected=*f.live.database->getOrchardCatalogState(hash);
        const auto rows=ChainDBTransactionReadTestPeer::HandoffRows(*f.live.database);
        f.live.compact.reset();f.live.database.reset();const auto files=ActivationHandoffFixture::Files(f.path);
        ChainDB cold;ASSERT_EQ(ChainDBTransactionReadTestPeer::HandoffReadOnly(cold,f.path),Status::Ok);
        const auto audited=OrchardSnapshotStoreTestAccess::AuditSuffix(f.mutex,cold,f.full.f.token,*f.full.files,f.headers,f.proof);
        EXPECT_EQ(audited.Encode(),expected);EXPECT_EQ(cold.getValidatedTip()->height,0);
        EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(cold),rows);
        EXPECT_EQ(ActivationHandoffFixture::Files(f.path),files);
        EXPECT_ANY_THROW(storage::RequireFullOrchardStorage(cold));
        EXPECT_FALSE(f.gate->Allows(f.gate->Snapshot()));
    }
}
TEST(OrchardActivationSuffix, WrongNullifierOwnerAndUntrackedNamespaceRefuseEvenWhenSetDigestsMatch){
    ActivationSuffixFixture f;f.Connect(f.full.first);f.Connect(f.full.second);auto& db=*f.live.database;
    const auto state=*db.getOrchardState();const auto commitments=*db.getOrchardCommitmentSets(state);
    uint256 nf,owner;bool found=false;
    ASSERT_EQ(db.forEachOrchardNullifier([&](const auto& n,const auto& o){if(!found){nf=n;owner=o;found=true;}return true;}),Status::Ok);
    ASSERT_TRUE(found);const auto original=ChainDBTransactionReadTestPeer::HandoffRows(db);
    const auto key="O1N"+std::string(reinterpret_cast<const char*>(nf.data),32);
    const auto other=owner==f.full.first->Header().GetHash()?f.full.second->Header().GetHash():f.full.first->Header().GetHash();
    ChainDBTransactionReadTestPeer::HandoffPutOrchardRow(db,f.full.f.token,key,std::string(reinterpret_cast<const char*>(other.data),32));
    ASSERT_EQ(*db.getOrchardCommitmentSets(state),commitments); // Root alone cannot detect this owner substitution.
    const auto bad=ChainDBTransactionReadTestPeer::HandoffRows(db);EXPECT_ANY_THROW(f.Audit());
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(db),bad);
    ChainDBTransactionReadTestPeer::HandoffPutOrchardRow(db,f.full.f.token,key,std::string(reinterpret_cast<const char*>(owner.data),32));
    for(const auto* prefix:{"O1U","R1U"}){
        const auto orphan=std::string(prefix)+std::string(32,'z');
        ChainDBTransactionReadTestPeer::HandoffPutOrchardRow(db,f.full.f.token,orphan,"untracked");
        const auto before=ChainDBTransactionReadTestPeer::HandoffRows(db);EXPECT_ANY_THROW(f.Audit());
        EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(db),before);
        ChainDBTransactionReadTestPeer::HandoffDeleteOrchardRow(db,f.full.f.token,orphan);
    }
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(db),original);
    EXPECT_EQ(f.Audit().block,state.block_hash);EXPECT_EQ(db.getValidatedTip()->height,0);
}
TEST(OrchardActivationSuffix, MissingBodyBadDeltaAndCancellationNeverRepairOrPublish){
    ActivationSuffixFixture f;f.Connect(f.full.first);f.Connect(f.full.second);auto& db=*f.live.database;
    const auto hash=f.full.first->Header().GetHash();const auto key=MakeUtreexoDeltaUndoKey(hash);
    std::string delta;ASSERT_EQ(db.getRaw(key,delta),Status::Ok);
    rocksdb::WriteBatch damage;damage.Put(key,"bad delta");ASSERT_EQ(db.writeBatch(f.full.f.token,std::move(damage),true),Status::Ok);
    const auto bad=ChainDBTransactionReadTestPeer::HandoffRows(db);EXPECT_ANY_THROW(f.Audit());
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(db),bad);
    rocksdb::WriteBatch restore;restore.Put(key,delta);ASSERT_EQ(db.writeBatch(f.full.f.token,std::move(restore),true),Status::Ok);
    const auto body=OrchardBlockCandidate::DecodeExact(f.full.first->WireBytes());
    ASSERT_EQ(db.deleteBlock(f.full.f.token,hash),Status::Ok);
    const auto missing=ChainDBTransactionReadTestPeer::HandoffRows(db);EXPECT_ANY_THROW(f.Audit());
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(db),missing);
    rocksdb::WriteBatch replace;ASSERT_EQ(db.stageOrchardBlock(f.full.f.token,body,true,replace),Status::Ok);
    ASSERT_EQ(db.writeBatch(f.full.f.token,std::move(replace),true),Status::Ok);
    const auto before=ChainDBTransactionReadTestPeer::HandoffRows(db);unsigned calls=0;
    EXPECT_ANY_THROW(f.Audit([&]{return ++calls>100;}));
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(db),before);
    EXPECT_EQ(f.Audit().block,f.full.second->Header().GetHash());
    EXPECT_EQ(db.getValidatedTip()->height,0);EXPECT_FALSE(f.gate->Allows(f.gate->Snapshot()));
}
TEST(OrchardSelectedConsistency, CompletedParentOpensWithoutAdvancingIndependentAnchor){
    ActivationHandoffFixture f(true);AnnotatedRecursiveMutex mutex;
    {ChainDB db;ASSERT_EQ(db.init(f.path),Status::Ok);ASSERT_EQ(db.setValidatedTip(f.full.f.token,f.full.f.blocks.front().GetHash(),0),Status::Ok);}
    auto prepared=f.Prepare();
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::OpenHandoff(f.guard,*f.receipt,mutex,f.full.f.token,*f.full.files,f.headers,std::move(prepared)));
    auto selected=OrchardSnapshotStoreTestAccess::SelectHandoff(f.guard,*f.receipt,f.Prepare());
    const auto original=ActivationHandoffFixture::Files(selected->OriginalPath());const auto original_path=selected->OriginalPath();
    auto live=OrchardSnapshotStoreTestAccess::OpenHandoff(f.guard,*f.receipt,mutex,f.full.f.token,*f.full.files,f.headers,std::move(selected));
    ASSERT_TRUE(live.database&&live.compact);EXPECT_EQ(live.database->getTip()->height,101);
    EXPECT_EQ(live.database->getValidatedTip()->height,0);
    EXPECT_ANY_THROW(storage::RequireFullOrchardStorage(*live.database));
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::RestoreParent(mutex,*live.database,*f.headers,*f.proof));
    {std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);
        EXPECT_EQ(live.compact->SelectedUnderLock(mutex)->Encode(),f.proof->parent_catalog.Encode());}
    const auto header=*live.database->getHeader(f.proof->parent_catalog.block);
    const auto c=*consensus::SelectedOrchardBlockContext(f.full.first->Header(),102);
    const consensus::OrchardTransactionContext tx{102,header.GetHash(),c.activation_height,c.domain};
    const auto capture=[&]{std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);
        return live.compact->CapturePoolCoinsUnderLock(mutex,tx,header,{}).CapturedInputs();};
    EXPECT_EQ(capture(),0u);
    const auto origin=*storage::ReadOrchardActivationOrigin(*live.database);auto foreign=origin;
    foreign.source=storage::catalog::Hash("different validly framed parent source");ASSERT_NE(foreign.source,origin.source);
    // Keep BOTH metadata rows valid and mutually consistent. Framing/hash
    // checks alone must not substitute this foreign origin for the bound one.
    rocksdb::WriteBatch replace_origin;replace_origin.Put(storage::OrchardActivationOriginKey,foreign.Encode());
    replace_origin.Put(storage::OrchardActivationSelectionKey,foreign.Selection());
    ASSERT_EQ(live.database->writeBatch(f.full.f.token,std::move(replace_origin),true),Status::Ok);
    const auto changed_origin=ChainDBTransactionReadTestPeer::HandoffRows(*live.database);
    EXPECT_ANY_THROW(capture());EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(*live.database),changed_origin);
    rocksdb::WriteBatch restore_origin;restore_origin.Put(storage::OrchardActivationOriginKey,origin.Encode());
    restore_origin.Put(storage::OrchardActivationSelectionKey,origin.Selection());
    ASSERT_EQ(live.database->writeBatch(f.full.f.token,std::move(restore_origin),true),Status::Ok);
    EXPECT_EQ(capture(),0u);
    ASSERT_EQ(live.database->setValidatedTip(f.full.f.token,f.full.first->Header().GetHash(),102),Status::Ok);
    const auto changed_anchor=ChainDBTransactionReadTestPeer::HandoffRows(*live.database);
    EXPECT_ANY_THROW(capture());EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(*live.database),changed_anchor);
    ASSERT_EQ(live.database->setValidatedTip(f.full.f.token,f.full.f.blocks.front().GetHash(),0),Status::Ok);
    EXPECT_EQ(capture(),0u);EXPECT_EQ(live.database->getValidatedTip()->height,0);
    live.compact.reset();live.database.reset();f.ColdReceipt();f.closed_app.reset();
    // A harmless private engine open may rotate LOG files. The completed
    // route still reauthenticates the entire exact parent logical baseline.
    auto cold=OrchardSnapshotStoreTestAccess::RecoverHandoff(f.guard,*f.receipt,f.headers);
    auto reopened=OrchardSnapshotStoreTestAccess::OpenHandoff(f.guard,*f.receipt,mutex,f.full.f.token,*f.full.files,f.headers,std::move(cold));
    EXPECT_EQ(reopened.database->getValidatedTip()->height,0);
    EXPECT_EQ(ActivationHandoffFixture::Files(original_path),original);
    ChainDB generic;EXPECT_NE(generic.init(f.path),Status::Ok);
    EXPECT_FALSE(f.gate->Allows(f.gate->Snapshot()));
}
TEST(OrchardSelectedConsistency, RealCompactTransitionsPreserveAnchorAndRefuseUntypedOrChangedOrigin){
    ActivationHandoffFixture f(true);AnnotatedRecursiveMutex mutex;
    {ChainDB db;ASSERT_EQ(db.init(f.path),Status::Ok);ASSERT_EQ(db.setValidatedTip(f.full.f.token,f.full.f.blocks.front().GetHash(),0),Status::Ok);}
    auto selected=OrchardSnapshotStoreTestAccess::SelectHandoff(f.guard,*f.receipt,f.Prepare());
    const auto original_path=selected->OriginalPath();const auto original=ActivationHandoffFixture::Files(original_path);
    auto live=OrchardSnapshotStoreTestAccess::OpenHandoff(f.guard,*f.receipt,mutex,f.full.f.token,*f.full.files,f.headers,std::move(selected));auto& db=*live.database;
    const auto connect=[&](const auto& source){
        const auto block=OrchardBlockCandidate::DecodeExact(source->WireBytes());
        const auto context=*consensus::SelectedOrchardBlockContext(block.Header(),source->Height());
        auto write=PreparedOrchardChainstateWrite::ConnectCompactIndexed(mutex,db,f.full.f.token,*f.full.files,
            *FindBlockIndex(context.block_hash),*live.compact,context,block,*db.getHeader(context.parent_hash),
            [&](uint32_t h){return f.full.Mtp(h);},true,std::nullopt,true);write->Commit();
    };
    const auto undo=[&](const auto& source){
        const auto block=OrchardBlockCandidate::DecodeExact(source->WireBytes());
        const auto context=*consensus::SelectedOrchardBlockContext(block.Header(),source->Height());
        auto write=PreparedOrchardChainstateWrite::DisconnectCompactIndexed(mutex,db,f.full.f.token,*f.full.files,
            *FindBlockIndex(context.block_hash),*live.compact,context,block,*db.getHeader(context.parent_hash),
            [&](uint32_t h){return f.full.Mtp(h);},true);write->Commit();
    };
    const auto first=OrchardBlockCandidate::DecodeExact(f.full.first->WireBytes());
    const auto context=*consensus::SelectedOrchardBlockContext(first.Header(),102);
    {std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);rocksdb::WriteBatch batch;
        EXPECT_ANY_THROW(consensus::StageOrchardCompactChainstateConnectUnderLock(db,f.full.f.token,context,first,
            *db.getHeader(context.parent_hash),f.proof->parent_catalog,[&](uint32_t h){return f.full.Mtp(h);},true,batch,f.proof->retirement.record));
        EXPECT_EQ(batch.Count(),0u);}
    connect(f.full.first);connect(f.full.second);EXPECT_EQ(db.getTip()->height,103);EXPECT_EQ(db.getValidatedTip()->height,0);
    {std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);
        const consensus::OrchardTransactionContext tx{104,f.full.second->Header().GetHash(),context.activation_height,context.domain};
        EXPECT_EQ(live.compact->CapturePoolCoinsUnderLock(mutex,tx,f.full.second->Header(),{}).CapturedInputs(),0u);}
    undo(f.full.second);undo(f.full.first);EXPECT_EQ(db.getTip()->height,101);EXPECT_EQ(db.getValidatedTip()->height,0);
    EXPECT_EQ(db.getOrchardState().status(),Status::NotFound);
    connect(f.full.first);EXPECT_EQ(db.getValidatedTip()->height,0);
    std::string origin;ASSERT_EQ(db.getRaw(storage::OrchardActivationOriginKey,origin),Status::Ok);
    rocksdb::WriteBatch corrupt;corrupt.Put(storage::OrchardActivationOriginKey,"corrupt");
    ASSERT_EQ(db.writeBatch(f.full.f.token,std::move(corrupt),true),Status::Ok);
    const auto before=ChainDBTransactionReadTestPeer::HandoffRows(db);EXPECT_ANY_THROW(connect(f.full.second));
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(db),before);
    rocksdb::WriteBatch restore;restore.Put(storage::OrchardActivationOriginKey,origin);
    ASSERT_EQ(db.writeBatch(f.full.f.token,std::move(restore),true),Status::Ok);
    // Missing BOTH metadata rows must not silently fall back to independent
    // marker semantics. A changed validated marker is also never repaired.
    rocksdb::WriteBatch missing;missing.Delete(storage::OrchardActivationOriginKey);missing.Delete(storage::OrchardActivationSelectionKey);
    ASSERT_EQ(db.writeBatch(f.full.f.token,std::move(missing),true),Status::Ok);
    const auto absent=ChainDBTransactionReadTestPeer::HandoffRows(db);EXPECT_ANY_THROW(connect(f.full.second));
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(db),absent);
    rocksdb::WriteBatch repair;repair.Put(storage::OrchardActivationOriginKey,origin);
    repair.Put(storage::OrchardActivationSelectionKey,storage::OrchardActivationOrigin::Decode(origin).Selection());
    ASSERT_EQ(db.writeBatch(f.full.f.token,std::move(repair),true),Status::Ok);
    ASSERT_EQ(db.setValidatedTip(f.full.f.token,f.full.first->Header().GetHash(),102),Status::Ok);
    const auto advanced=ChainDBTransactionReadTestPeer::HandoffRows(db);EXPECT_ANY_THROW(connect(f.full.second));
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(db),advanced);
    ASSERT_EQ(db.setValidatedTip(f.full.f.token,f.full.f.blocks.front().GetHash(),0),Status::Ok);
    // A valid prepare is NOT durable authority after its selected before-image
    // changes. Commit must recheck the origin/anchor before the first write.
    const auto block=OrchardBlockCandidate::DecodeExact(f.full.second->WireBytes());
    const auto child=*consensus::SelectedOrchardBlockContext(block.Header(),103);
    auto pending=PreparedOrchardChainstateWrite::ConnectCompactIndexed(mutex,db,f.full.f.token,*f.full.files,
        *FindBlockIndex(child.block_hash),*live.compact,child,block,*db.getHeader(child.parent_hash),
        [&](uint32_t h){return f.full.Mtp(h);},true,std::nullopt,true);
    rocksdb::WriteBatch tamper;tamper.Put(storage::OrchardActivationOriginKey,"changed after prepare");
    ASSERT_EQ(db.writeBatch(f.full.f.token,std::move(tamper),true),Status::Ok);
    const auto changed=ChainDBTransactionReadTestPeer::HandoffRows(db);EXPECT_ANY_THROW(pending->Commit());
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(db),changed);pending.reset();
    EXPECT_EQ(ActivationHandoffFixture::Files(original_path),original);
    EXPECT_FALSE(f.gate->Allows(f.gate->Snapshot()));
}
TEST(OrchardSelectedConsistency, SameDatabaseObjectCannotReuseOwnerAfterEngineReopen){
    ActivationHandoffFixture f(true);AnnotatedRecursiveMutex mutex;
    auto selected=OrchardSnapshotStoreTestAccess::SelectHandoff(f.guard,*f.receipt,f.Prepare());
    auto live=OrchardSnapshotStoreTestAccess::OpenHandoff(f.guard,*f.receipt,mutex,f.full.f.token,*f.full.files,f.headers,std::move(selected));
    auto& db=*live.database;const auto header=*db.getHeader(f.proof->parent_catalog.block);
    const auto c=*consensus::SelectedOrchardBlockContext(f.full.first->Header(),102);
    const consensus::OrchardTransactionContext tx{102,header.GetHash(),c.activation_height,c.domain};
    {std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);
        EXPECT_EQ(live.compact->CapturePoolCoinsUnderLock(mutex,tx,header,{}).CapturedInputs(),0u);}
    db.close();const auto copy=f.root/"generation-negative"/"db";
    std::filesystem::create_directories(copy.parent_path());
    std::filesystem::copy(f.path,copy,std::filesystem::copy_options::recursive);
    ASSERT_EQ(db.init(copy),Status::Ok); // Same object, exact logical rows, DIFFERENT open generation.
    const auto before=ChainDBTransactionReadTestPeer::HandoffRows(db);
    {std::lock_guard<AnnotatedRecursiveMutex> lock(mutex);
        EXPECT_ANY_THROW(live.compact->CapturePoolCoinsUnderLock(mutex,tx,header,{}));}
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(db),before);
}
TEST(OrchardActivationHeaders, SelectedInterruptionRecoversAfterDiscardingRamProofAndSelector){
    ActivationHandoffFixture f(true);auto prepared=f.Prepare();const auto original=ActivationHandoffFixture::Files(f.path);
    const auto expected=ChainDBTransactionReadTestPeer::HandoffRows(prepared->CandidateForInspection());
    const auto first=f.proof->state.block_hash;const auto intent=f.root/"blockchain"/storage::OrchardHandoffSelected;
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::SelectHandoff(f.guard,*f.receipt,std::move(prepared),
        [&]{return std::filesystem::exists(intent);}));
    f.headers.reset();f.proof.reset();f.ColdReceipt();f.closed_app.reset();
    auto recovered=OrchardSnapshotStoreTestAccess::RecoverHandoff(f.guard,*f.receipt,{});
    ASSERT_TRUE(recovered);EXPECT_EQ(recovered->CandidatePath(),f.path);
    EXPECT_EQ(ActivationHandoffFixture::Files(recovered->OriginalPath()),original);
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(recovered->CandidateForInspection()),expected);
    EXPECT_FALSE(f.gate->Allows(f.gate->Snapshot()));EXPECT_EQ(f.receipt->WalletName(),"handoff-named-owner");
    const auto cold=OrchardSnapshotStoreTestAccess::ReadHandoffHeaders(recovered->CandidateForInspection(),first);
    ASSERT_TRUE(cold->GetHeaderValue(first));EXPECT_ANY_THROW(storage::RequireFullOrchardStorage(recovered->CandidateForInspection()));
}
TEST(OrchardActivationSelection, ClosedLifecycleBorrowIsThreadBoundAndCannotReviveAfterAbandonment){
    ActivationHandoffFixture f(true);auto intent=f.receipt->ClosedForHandoff(f.guard);
    EXPECT_NO_THROW(intent.Check(f.guard));
    auto foreign=std::async(std::launch::async,[&]{try{intent.Check(f.guard);return false;}catch(const std::logic_error&){return true;}});
    EXPECT_TRUE(foreign.get());
    f.receipt.reset();EXPECT_ANY_THROW(intent.Check(f.guard));EXPECT_FALSE(f.gate->Allows(f.gate->Snapshot()));
    f.ColdReceipt();EXPECT_ANY_THROW(f.receipt->ClosedForHandoff(f.guard));f.closed_app.reset();
    EXPECT_NO_THROW(f.receipt->ClosedForHandoff(f.guard).Check(f.guard));
    EXPECT_ANY_THROW(intent.Check(f.guard)); // Same journal/new receipt cannot resurrect a dead borrow.
}
TEST(OrchardActivationSelection, ExactLifecycleIntentSelectsWithoutDeletingOriginalOrOpeningRpc){
    ActivationHandoffFixture f(true);const auto original=ActivationHandoffFixture::Files(f.path);
    auto prepared=f.Prepare();const auto candidate=prepared->CandidatePath();
    const auto expected_rows=ChainDBTransactionReadTestPeer::HandoffRows(prepared->CandidateForInspection());
    const auto validated=prepared->OriginalForInspection().getValidatedTip();ASSERT_TRUE(validated.ok());
    f.ColdReceipt(); // Live fresh pending App must also be destroyed, even before Init.
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::SelectHandoff(f.guard,*f.receipt,std::move(prepared)));
    EXPECT_EQ(ActivationHandoffFixture::Files(f.path),original);
    EXPECT_NO_THROW(OrchardSnapshotStoreTestAccess::NoHandoff(f.root));f.closed_app.reset();
    prepared=f.Prepare();const auto selected_candidate=prepared->CandidatePath();
    auto selected=OrchardSnapshotStoreTestAccess::SelectHandoff(f.guard,*f.receipt,std::move(prepared));
    ASSERT_TRUE(selected);EXPECT_EQ(selected->CandidatePath(),f.path);EXPECT_FALSE(std::filesystem::exists(selected_candidate));
    EXPECT_TRUE(std::filesystem::exists(candidate)); // The refused earlier candidate remains unselected evidence.
    EXPECT_EQ(ActivationHandoffFixture::Files(selected->OriginalPath()),original);
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(selected->CandidateForInspection()),expected_rows);
    EXPECT_EQ(selected->CandidateForInspection().getValidatedTip()->hash,validated->hash);
    EXPECT_EQ(selected->CandidateForInspection().getValidatedTip()->height,validated->height);
    EXPECT_FALSE(f.gate->Allows(f.gate->Snapshot()));EXPECT_EQ(f.receipt->WalletName(),"handoff-named-owner");
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::NoHandoff(f.root));
    ChainDB generic;EXPECT_EQ(generic.init(f.path),Status::Corruption);
    EXPECT_EQ(generic.init(selected->OriginalPath()),Status::Corruption);
    EXPECT_ANY_THROW(storage::RequireFullOrchardStorage(selected->CandidateForInspection()));
    const auto backup=selected->OriginalPath();selected.reset();f.ColdReceipt();f.closed_app.reset();
    selected=OrchardSnapshotStoreTestAccess::RecoverHandoff(f.guard,*f.receipt,f.headers);
    ASSERT_TRUE(selected);EXPECT_EQ(selected->OriginalPath(),backup);
    EXPECT_EQ(ActivationHandoffFixture::Files(backup),original);
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(selected->CandidateForInspection()),expected_rows);
    EXPECT_FALSE(f.gate->Allows(f.gate->Snapshot()));
}
TEST(OrchardActivationSelection, InterruptedSelectedPhasesRequireExactColdReceiptAndRetainBothStores){
    for(unsigned phase=0;phase<3;++phase){
        ActivationHandoffFixture f(true);const auto original=ActivationHandoffFixture::Files(f.path);
        auto prepared=f.Prepare();const auto candidate=prepared->CandidatePath();
        const auto expected=ChainDBTransactionReadTestPeer::HandoffRows(prepared->CandidateForInspection());
        const auto nonce=candidate.filename().string().substr(std::string("chaindb.orchard-handoff-").size());
        const auto backup=f.root/"blockchain"/("chaindb.orchard-original-"+nonce);
        const auto intent=f.root/"blockchain"/".orchard-handoff-selected-v1";
        const auto done=f.root/"blockchain"/".orchard-handoff-selected-v1.done";
        auto cancel=[&]{
            if(!std::filesystem::exists(intent))return false;
            if(phase==0)return true;
            if(phase==1)return !std::filesystem::exists(f.path);
            return std::filesystem::exists(backup)&&!std::filesystem::exists(candidate);
        };
        EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::SelectHandoff(f.guard,*f.receipt,std::move(prepared),cancel));
        ASSERT_TRUE(std::filesystem::exists(intent));EXPECT_FALSE(std::filesystem::exists(done));
        EXPECT_EQ(std::filesystem::exists(f.path),phase!=1);EXPECT_EQ(std::filesystem::exists(candidate),phase!=2);
        ChainDB generic;EXPECT_EQ(generic.init(f.path),Status::Corruption);
        EXPECT_EQ(std::filesystem::exists(f.path),phase!=1); // No empty DB created in the between-renames gap.
        EXPECT_EQ(ActivationHandoffFixture::Files(phase==0?f.path:backup),original);
        EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::NoHandoff(f.root));EXPECT_FALSE(f.gate->Allows(f.gate->Snapshot()));
        f.ColdReceipt();
        EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::RecoverHandoff(f.guard,*f.receipt,f.headers));
        f.closed_app.reset();auto recovered=OrchardSnapshotStoreTestAccess::RecoverHandoff(f.guard,*f.receipt,f.headers);
        ASSERT_TRUE(recovered);EXPECT_TRUE(std::filesystem::exists(done));EXPECT_EQ(recovered->CandidatePath(),f.path);
        EXPECT_EQ(recovered->OriginalPath(),backup);EXPECT_EQ(ActivationHandoffFixture::Files(backup),original);
        EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(recovered->CandidateForInspection()),expected);
        EXPECT_EQ(f.receipt->WalletName(),"handoff-named-owner");EXPECT_FALSE(f.gate->Allows(f.gate->Snapshot()));
    }
}
TEST(OrchardActivationSelection, ControlAndDirectoryTamperingCannotSelectOrRepairAnotherStore){
    ActivationHandoffFixture f(true);const auto original=ActivationHandoffFixture::Files(f.path);
    auto prepared=f.Prepare();const auto candidate=prepared->CandidatePath();const auto chain=f.root/"blockchain";
    const auto intent=chain/".orchard-handoff-selected-v1";
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::SelectHandoff(f.guard,*f.receipt,std::move(prepared),
        [&]{return std::filesystem::exists(intent);}));
    ASSERT_TRUE(std::filesystem::exists(intent));
    // Preserve exact control, all bad variants and displaced names; never
    // silently fix/delete a production control on a recovery refusal.
    const auto saved=chain/"selected-control.original-evidence";std::filesystem::rename(intent,saved);
    for(unsigned variant=0;variant<4;++variant){
        if(variant==0){std::filesystem::create_symlink(saved,intent);}
        else if(variant==1){std::filesystem::create_hard_link(saved,intent);}
        else if(variant==2){std::ofstream out(intent,std::ios::binary);out<<"corrupt selected intent";out.close();::chmod(intent.c_str(),0400);}
        else {std::filesystem::copy_file(saved,intent);::chmod(intent.c_str(),0600);}
        EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::RecoverHandoff(f.guard,*f.receipt,f.headers));
        EXPECT_EQ(ActivationHandoffFixture::Files(f.path),original);
        std::filesystem::rename(intent,chain/("bad-selected-control-"+std::to_string(variant)));
        // The test-owned extra hardlink must no longer share the real control.
        if(variant==1){std::filesystem::copy_file(saved,chain/"hardlink-control.bytes");
            std::filesystem::rename(saved,chain/"hardlinked-control.original");
            std::filesystem::copy_file(chain/"hardlink-control.bytes",saved);::chmod(saved.c_str(),0400);}
    }
    std::filesystem::rename(saved,intent);
    const auto partial=std::filesystem::path(intent.string()+".partial");
    {std::ofstream out(partial);out<<"interrupted unpublished control";}
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::RecoverHandoff(f.guard,*f.receipt,f.headers));
    std::filesystem::rename(partial,chain/"partial-control.evidence");
    const auto displaced=std::filesystem::path(candidate.string()+".displaced-evidence");
    std::filesystem::rename(candidate,displaced);std::filesystem::create_directory(candidate);
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::RecoverHandoff(f.guard,*f.receipt,f.headers));
    std::filesystem::rename(candidate,chain/"foreign-candidate.evidence");std::filesystem::rename(displaced,candidate);
    const auto magic=Params().magic;MutableParams().magic^=1;
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::RecoverHandoff(f.guard,*f.receipt,f.headers));MutableParams().magic=magic;
    auto selected=OrchardSnapshotStoreTestAccess::RecoverHandoff(f.guard,*f.receipt,f.headers);ASSERT_TRUE(selected);
    EXPECT_EQ(ActivationHandoffFixture::Files(selected->OriginalPath()),original);
}
#endif
TEST(OrchardActivationBaseline, IndependentCopyMatchesEveryRowWithoutReusingAnEngine){
    ActivationHandoffFixture f;
    {ChainDB source;ASSERT_EQ(source.init(f.path),Status::Ok);
     ASSERT_EQ(source.setValidatedTip(f.full.f.token,f.full.f.blocks.front().GetHash(),0),Status::Ok);source.close();}
    ActivationHandoffFixture::MutateCopy(f.path,[](auto& raw){raw.put("default","original-local-beforeimage:test","opaque-preserved-row");});
    auto prepared=f.Prepare();const auto& original=prepared->OriginalForInspection();
    const auto& expected=prepared->CandidateForInspection();const auto origin=*storage::ReadOrchardActivationOrigin(expected);
    const auto cold=OrchardSnapshotStoreTestAccess::ReadHandoffHeaders(expected,origin.first);
    const auto rows=ChainDBTransactionReadTestPeer::HandoffRows(original);const auto files=ActivationHandoffFixture::Files(f.path);
    AnnotatedRecursiveMutex mutex;const auto path=f.root/"baseline-reconstruction";bool created=false,chosen=false;
    auto rebuilt=OrchardSnapshotStoreTestAccess::CopyParent(original,f.full.f.token,mutex,*cold,*f.proof,
        origin.source,path,[&]{created=true;EXPECT_TRUE(std::filesystem::is_directory(path));},[&]{chosen=true;});
    EXPECT_TRUE(created&&chosen);EXPECT_EQ(rebuilt.origin.Encode(),origin.Encode());
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(*rebuilt.database),ChainDBTransactionReadTestPeer::HandoffRows(expected));
    EXPECT_EQ(rebuilt.database->getValidatedTip()->height,0);EXPECT_EQ(rebuilt.database->getTip()->height,101);
    rebuilt.database.reset();const auto rebuilt_files=ActivationHandoffFixture::Files(path);
    bool called=false;
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::CopyParent(original,f.full.f.token,mutex,*cold,*f.proof,
        origin.source,path,[&]{called=true;},[]{ }));
    EXPECT_FALSE(called);EXPECT_EQ(ActivationHandoffFixture::Files(path),rebuilt_files);
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(original),rows);EXPECT_EQ(ActivationHandoffFixture::Files(f.path),files);
}
TEST(OrchardActivationBaseline, WrongSourceAndCancellationNeverCreateOrAlterTheOriginal){
    ActivationHandoffFixture f;auto prepared=f.Prepare();const auto& original=prepared->OriginalForInspection();
    const auto origin=*storage::ReadOrchardActivationOrigin(prepared->CandidateForInspection());
    const auto rows=ChainDBTransactionReadTestPeer::HandoffRows(original);const auto files=ActivationHandoffFixture::Files(f.path);
    AnnotatedRecursiveMutex mutex;auto foreign=origin.source;foreign.begin()[0]^=1;
    const auto path=f.root/"wrong-source-reconstruction";bool called=false;
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::CopyParent(original,f.full.f.token,mutex,*f.headers,*f.proof,
        foreign,path,[&]{called=true;},[]{ }));
    EXPECT_FALSE(called);EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::CopyParent(original,f.full.f.token,mutex,*f.headers,*f.proof,
        origin.source,path,[&]{called=true;},[]{ },[]{return true;}));
    EXPECT_FALSE(called);EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(original),rows);EXPECT_EQ(ActivationHandoffFixture::Files(f.path),files);
}
TEST(OrchardActivationBaseline, FinalRootFenceFailureNeverCommitsChoiceRows){
    ActivationHandoffFixture f;auto prepared=f.Prepare();const auto& original=prepared->OriginalForInspection();
    const auto origin=*storage::ReadOrchardActivationOrigin(prepared->CandidateForInspection());
    const auto rows=ChainDBTransactionReadTestPeer::HandoffRows(original);const auto files=ActivationHandoffFixture::Files(f.path);
    AnnotatedRecursiveMutex mutex;const auto path=f.root/"fence-refused-reconstruction";bool created=false,choice=false;
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::CopyParent(original,f.full.f.token,mutex,*f.headers,*f.proof,
        origin.source,path,[&]{created=true;},[&]{choice=true;throw std::runtime_error("fixture root fence refused");}));
    EXPECT_TRUE(created&&choice);ChainDB rejected;ASSERT_EQ(ChainDBTransactionReadTestPeer::HandoffReadOnly(rejected,path),Status::Ok);
    std::string bytes;EXPECT_EQ(rejected.getRaw(storage::OrchardActivationOriginKey,bytes),Status::NotFound);
    EXPECT_EQ(rejected.getRaw(storage::OrchardActivationSelectionKey,bytes),Status::NotFound);
    EXPECT_FALSE(storage::ReadOrchardCompactStorageBinding(rejected));
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(original),rows);EXPECT_EQ(ActivationHandoffFixture::Files(f.path),files);
}
TEST(OrchardActivationHandoff, ClosedCopyConvertsWithoutMovingHistoryMarkersOrOriginalFiles){
    using namespace consensus;ActivationHandoffFixture f;
    // This simulates lagging independent execution metadata. It is not a
    // complete assumed-state daemon/lifecycle acceptance fixture.
    {ChainDB source;ASSERT_EQ(source.init(f.path),Status::Ok);
     ASSERT_EQ(source.setValidatedTip(f.full.f.token,f.full.f.blocks.front().GetHash(),0),Status::Ok);source.close();}
    // The existing layout upgrade legitimately shares immutable SSTs with
    // its exact rollback counterpart. That narrow case remains supported;
    // the new converted candidate must not create any additional links.
    const auto rollback=std::filesystem::path(f.path.string()+".legacy-v1");std::filesystem::create_directory(rollback);
    bool paired=false;for(const auto& item:std::filesystem::directory_iterator(f.path)){
        if(item.is_regular_file()&&item.path().extension()==".sst"){
            std::filesystem::create_hard_link(item.path(),rollback/item.path().filename());paired=true;break;
        }
    }ASSERT_TRUE(paired);
    const auto original=ActivationHandoffFixture::Files(f.path);
    const auto companions=ActivationHandoffFixture::Files(f.root);
    auto prepared=f.Prepare();ASSERT_TRUE(prepared);
    const auto& source=prepared->OriginalForInspection();const auto& copy=prepared->CandidateForInspection();
    for(const auto& item:std::filesystem::directory_iterator(prepared->CandidatePath()))
        if(item.is_regular_file()&&item.path().extension()==".sst")EXPECT_EQ(std::filesystem::hard_link_count(item.path()),1u);
    const auto before=ChainDBTransactionReadTestPeer::HandoffRows(source),after=ChainDBTransactionReadTestPeer::HandoffRows(copy);
    EXPECT_GT(before.size(),after.size()/2);EXPECT_NE(before,after);
    EXPECT_EQ(source.getTip()->height,101);EXPECT_EQ(copy.getTip()->height,101);
    EXPECT_EQ(source.getValidatedTip()->height,0);EXPECT_EQ(copy.getValidatedTip()->height,0);
    EXPECT_EQ(source.getValidatedTip()->hash,copy.getValidatedTip()->hash);
    EXPECT_NO_THROW(CheckLegacyRetirementBeforeImageUnderLock(copy,f.proof->retirement.record));
    EXPECT_ANY_THROW(CheckPreparedLegacyRetirementUnderLock(copy,f.proof->retirement.record)); // original guard retained
    EXPECT_FALSE(storage::ReadOrchardSnapshotBase(copy));EXPECT_FALSE(storage::ReadOrchardSnapshotBase(source));
    EXPECT_FALSE(storage::ReadOrchardCompactStorageBinding(source));EXPECT_ANY_THROW(storage::RequireFullOrchardStorage(copy));
    const auto origin=storage::ReadOrchardActivationOrigin(copy);ASSERT_TRUE(origin);
    EXPECT_EQ(origin->validated_height,0u);EXPECT_EQ(origin->parent,f.proof->parent_catalog.block);
    EXPECT_EQ(origin->first,f.proof->state.block_hash);EXPECT_EQ(origin->proof,HashOrchardSnapshotSection(*f.proof));
    EXPECT_EQ(copy.getOrchardState().status(),Status::NotFound);EXPECT_EQ(copy.getLegacyRetirementState().status(),Status::NotFound);
    EXPECT_NO_THROW(OrchardSnapshotStoreTestAccess::CheckHandoff(f.guard,*prepared));
    EXPECT_EQ(ActivationHandoffFixture::Files(f.path),original);
    EXPECT_EQ(ActivationHandoffFixture::Files(f.root).at("named-wallet.sentinel"),companions.at("named-wallet.sentinel"));
    EXPECT_EQ(ActivationHandoffFixture::Files(f.root).at("blockchain/original-lifecycle.sentinel"),
        companions.at("blockchain/original-lifecycle.sentinel"));
    // Neither source nor candidate may acquire a second same-process owner.
    ChainDB duplicate;EXPECT_NE(duplicate.init(f.path),Status::Ok);EXPECT_NE(duplicate.init(prepared->CandidatePath()),Status::Ok);
    const auto candidate_path=prepared->CandidatePath();prepared.reset();
    EXPECT_EQ(ActivationHandoffFixture::Files(f.path),original);
    ChainDB cold;ASSERT_EQ(cold.init(candidate_path),Status::Ok);EXPECT_TRUE(storage::ReadOrchardActivationOrigin(cold));
    EXPECT_EQ(cold.getValidatedTip()->height,0);EXPECT_ANY_THROW(storage::RequireFullOrchardStorage(cold));cold.close();
}
TEST(OrchardActivationHandoff, MissingCorruptOriginOrSelectionCannotBecomeFreshParentAuthority){
    ActivationHandoffFixture f;auto prepared=f.Prepare();const auto candidate=prepared->CandidatePath();prepared.reset();
    for(unsigned variant=0;variant<5;++variant){
        const auto path=f.root/"blockchain"/("metadata-negative-"+std::to_string(variant));
        std::filesystem::copy(candidate,path,std::filesystem::copy_options::recursive);
        ActivationHandoffFixture::MutateCopy(path,[&](auto& raw){
            rocksdb::WriteOptions sync;sync.sync=true;
            if(variant==0||variant==4)ASSERT_TRUE(raw.db->Delete(sync,raw.cf("default"),storage::OrchardActivationOriginKey).ok());
            if(variant==1||variant==4)ASSERT_TRUE(raw.db->Delete(sync,raw.cf("default"),storage::OrchardActivationSelectionKey).ok());
            if(variant==2)raw.put("default",storage::OrchardActivationOriginKey,"corrupt");
            if(variant==3)raw.put("default",storage::OrchardActivationSelectionKey,"foreign");
        });
        ChainDB cold;ASSERT_EQ(cold.init(path),Status::Ok);
        const auto rows=ChainDBTransactionReadTestPeer::HandoffRows(cold);
        if(variant==4)EXPECT_FALSE(storage::ReadOrchardActivationOrigin(cold));
        else EXPECT_ANY_THROW(storage::ReadOrchardActivationOrigin(cold));
        EXPECT_ANY_THROW(storage::RequireFullOrchardStorage(cold));
        EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(cold),rows);cold.close();
    }
}
TEST(OrchardActivationHandoff, LiveSourceAndInterruptedPreparationNeverSelectOrAlterTheOriginal){
    ActivationHandoffFixture f;ChainDB live;ASSERT_EQ(live.init(f.path),Status::Ok);
    EXPECT_ANY_THROW(f.Prepare());live.close();
    const auto original=ActivationHandoffFixture::Files(f.path);bool cancelled_after_creation=false;
    EXPECT_ANY_THROW(f.Prepare([&]{
        for(const auto& item:std::filesystem::directory_iterator(f.root/"blockchain"))
            if(item.path().filename().string().starts_with("chaindb.orchard-handoff-"))cancelled_after_creation=true;
        return cancelled_after_creation;
    }));
    EXPECT_TRUE(cancelled_after_creation);EXPECT_EQ(ActivationHandoffFixture::Files(f.path),original);
    unsigned partial=0;for(const auto& item:std::filesystem::directory_iterator(f.root/"blockchain")){
        if(!item.path().filename().string().starts_with("chaindb.orchard-handoff-"))continue;
        ++partial;ChainDB copy;ASSERT_EQ(copy.init(item.path()),Status::Ok);EXPECT_FALSE(storage::ReadOrchardActivationOrigin(copy));
    }
    EXPECT_EQ(partial,1u);
    auto prepared=f.Prepare();EXPECT_TRUE(prepared); // explicit new attempt, NEVER adopt the unselected prefix
    EXPECT_EQ(ActivationHandoffFixture::Files(f.path),original);
}
TEST(OrchardActivationHandoff, AliasedInnerPathCorruptLegacyAndReplacedRootRefuse){
    ActivationHandoffFixture f;
    const auto source=f.path;std::filesystem::rename(source,source.string()+".held-original");
    std::filesystem::create_directory_symlink(source.string()+".held-original",source);
    EXPECT_ANY_THROW(f.Prepare());std::filesystem::remove(source);std::filesystem::rename(source.string()+".held-original",source);
    const auto foreign_link=f.root/"foreign-current-link";
    std::filesystem::create_hard_link(source/"CURRENT",foreign_link);EXPECT_ANY_THROW(f.Prepare());std::filesystem::remove(foreign_link);
    auto prepared=f.Prepare();const auto original=ActivationHandoffFixture::Files(f.path);
    const auto maintenance=f.root/"blockchain"/"reindex_promotion.marker";
    {std::ofstream marker(maintenance);marker<<"unfinished test-only promotion";}
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::CheckHandoff(f.guard,*prepared));
    EXPECT_ANY_THROW(f.Prepare());
    std::filesystem::remove(maintenance);
    const auto previous_magic=Params().magic;MutableParams().magic^=1;
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::CheckHandoff(f.guard,*prepared));MutableParams().magic=previous_magic;
    const auto moved=f.root.string()+".held-root";std::filesystem::rename(f.root,moved);std::filesystem::create_directory(f.root);
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::CheckHandoff(f.guard,*prepared));
    std::filesystem::remove(f.root);std::filesystem::rename(moved,f.root);
    EXPECT_NO_THROW(OrchardSnapshotStoreTestAccess::CheckHandoff(f.guard,*prepared));prepared.reset();
    EXPECT_EQ(ActivationHandoffFixture::Files(f.path),original);
    f.MutateSource([](auto& raw){raw.put(shielded_store_fixture::shielded,"Mshielded_frontier","corrupt");});
    const auto corrupt=ActivationHandoffFixture::Files(f.path);EXPECT_ANY_THROW(f.Prepare());
    EXPECT_EQ(ActivationHandoffFixture::Files(f.path),corrupt);
}
#endif
TEST(OrchardActivationProof, ColdDatabaseReadsAuthenticateWithoutHostProofOrSnapshotFloor){
    using namespace consensus;TransferredActivationParentFixture f;f.Parent();
    const auto first=f.proof->state.block_hash,expected=HashOrchardSnapshotSection(*f.proof);
    EXPECT_FALSE(f.Read(first));const auto before=f.disk.Rows();const auto archives=f.disk.ArchiveBytes();f.Retain();
    auto retained=f.disk.Rows();for(auto it=retained.begin();it!=retained.end();){
        if(it->first.first=="default"&&it->first.second.starts_with("orchard_activation_proof_v1:"))it=retained.erase(it);
        else ++it;
    }
    EXPECT_EQ(retained,before);EXPECT_EQ(f.disk.ArchiveBytes(),archives);
    const auto complete=f.disk.Rows();f.Retain();EXPECT_EQ(f.disk.Rows(),complete);
    f.proof.reset();f.disk.reopened.close();ASSERT_EQ(f.disk.reopened.init(f.disk.f.path),Status::Ok);
    const auto cold=f.disk.Rows();auto proof=f.Read(first);ASSERT_TRUE(proof);
    EXPECT_EQ(HashOrchardSnapshotSection(*proof),expected);EXPECT_EQ(f.disk.Rows(),cold);
    EXPECT_FALSE(storage::ReadOrchardSnapshotBase(f.disk.reopened));
    EXPECT_EQ(f.disk.reopened.getTip()->height,101);EXPECT_EQ(f.disk.reopened.getOrchardState().status(),Status::NotFound);
    EXPECT_ANY_THROW(storage::RequireFullOrchardStorage(f.disk.reopened));
    auto owner=OrchardSnapshotStoreTestAccess::RestoreParent(f.disk.startup_mutex,f.disk.reopened,*f.headers,*proof);
    ASSERT_TRUE(owner);proof.reset();f.disk.Connect(*owner,f.disk.first);f.disk.Undo(*owner,f.disk.first);owner.reset();
    f.disk.reopened.close();ASSERT_EQ(f.disk.reopened.init(f.disk.f.path),Status::Ok);
    proof=f.Read(first);ASSERT_TRUE(proof);EXPECT_EQ(HashOrchardSnapshotSection(*proof),expected);
    EXPECT_FALSE(storage::ReadOrchardSnapshotBase(f.disk.reopened));EXPECT_EQ(f.disk.reopened.getTip()->height,101);
}
TEST(OrchardActivationProof, InterruptedUnselectedChunksResumeOnlyIdenticallyAndGrantNoOwner){
    using namespace consensus;TransferredActivationParentFixture f;f.Parent();
    const auto first=f.proof->state.block_hash,digest=HashOrchardSnapshotSection(*f.proof);
    // HashOrchardSnapshotSection is the same SHA256 as the canonical stream.
    const auto chunk_key="orchard_activation_proof_v1:chunk:"+digest.GetHex()+":0";
    const auto before=f.disk.Rows();bool interrupted=false;
    EXPECT_ANY_THROW(f.Retain([&]{std::string bytes;interrupted=f.disk.reopened.getRaw(chunk_key,bytes)==Status::Ok;return interrupted;}));
    ASSERT_TRUE(interrupted);EXPECT_FALSE(f.Read(first));EXPECT_FALSE(storage::ReadOrchardSnapshotBase(f.disk.reopened));
    auto prefix=f.disk.Rows();ASSERT_NE(prefix,before);
    for(auto it=prefix.begin();it!=prefix.end();){
        if(it->first.first=="default"&&it->first.second.starts_with("orchard_activation_proof_v1:chunk:"))it=prefix.erase(it);
        else ++it;
    }EXPECT_EQ(prefix,before);
    f.disk.reopened.close();ASSERT_EQ(f.disk.reopened.init(f.disk.f.path),Status::Ok);
    EXPECT_FALSE(f.Read(first));f.Retain();auto proof=f.Read(first);ASSERT_TRUE(proof);
    EXPECT_EQ(HashOrchardSnapshotSection(*proof),digest);const auto complete=f.disk.Rows();
    EXPECT_ANY_THROW(f.Read(first,[]{return true;}));EXPECT_EQ(f.disk.Rows(),complete);
}
TEST(OrchardActivationProof, MissingCorruptWrongProfileAndUnburiedCompletedProofsNeverRepair){
    using namespace consensus;TransferredActivationParentFixture f;f.Parent();f.Retain();
    const auto first=f.proof->state.block_hash,digest=HashOrchardSnapshotSection(*f.proof);
    const auto chunk_key="orchard_activation_proof_v1:chunk:"+digest.GetHex()+":0";
    const auto record_key="orchard_activation_proof_v1:record:"+first.GetHex();
    std::string chunk,record;ASSERT_EQ(f.disk.reopened.getRaw(chunk_key,chunk),Status::Ok);
    ASSERT_EQ(f.disk.reopened.getRaw(record_key,record),Status::Ok);
    const auto refuse=[&]{const auto rows=f.disk.Rows();
        EXPECT_ANY_THROW(f.Read(first));
        EXPECT_ANY_THROW(f.Retain());
        EXPECT_EQ(f.disk.Rows(),rows);
    };
    HeaderChainSelector unburied;for(const auto& block:f.disk.f.blocks)ASSERT_TRUE(unburied.AddHeader(block.header));
    ASSERT_TRUE(unburied.AddHeader(f.disk.first->Header()));const auto complete=f.disk.Rows();
    // Same profile/descriptor bytes, different locally selected burial: this
    // specifically requires live header authentication on every cache read.
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::ReadProof(f.disk.startup_mutex,f.disk.reopened,unburied,first));
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::RetainProof(f.disk.startup_mutex,f.disk.reopened,f.disk.f.token,unburied,*f.proof));
    EXPECT_EQ(f.disk.Rows(),complete);
    MutableParams().state_commitment_burial_depth=2;refuse();MutableParams().state_commitment_burial_depth=1;
    const auto magic=Params().magic;MutableParams().magic^=1;refuse();MutableParams().magic=magic;
    f.disk.Mutate([&](auto& raw){auto corrupt=chunk;corrupt.back()^=1;raw.put("default",chunk_key,corrupt);});refuse();
    f.disk.Mutate([&](auto& raw){rocksdb::WriteOptions sync;sync.sync=true;ASSERT_TRUE(raw.db->Delete(sync,chunk_key).ok());});refuse();
    f.disk.Mutate([&](auto& raw){raw.put("default",chunk_key,chunk);raw.put("default",record_key,"malformed");});refuse();
    f.disk.Mutate([&](auto& raw){raw.put("default",record_key,record);});ASSERT_TRUE(f.Read(first));
    const auto other=f.disk.second->Header().GetHash();const auto wrong_key="orchard_activation_proof_v1:record:"+other.GetHex();
    f.disk.Mutate([&](auto& raw){raw.put("default",wrong_key,record);});const auto wrong=f.disk.Rows();
    EXPECT_ANY_THROW(f.Read(other));EXPECT_EQ(f.disk.Rows(),wrong);
    EXPECT_FALSE(storage::ReadOrchardSnapshotBase(f.disk.reopened));EXPECT_EQ(f.disk.reopened.getTip()->height,101);
}
TEST(OrchardActivationParent, AuthenticatedParentFirstConnectUndoColdDatabaseAndReconnectHaveNoSnapshotFloor){
    using namespace consensus;TransferredActivationParentFixture f;f.Parent();
    const auto rows=f.disk.Rows();const auto archives=f.disk.ArchiveBytes();auto owner=f.Restore();ASSERT_TRUE(owner);
    EXPECT_EQ(f.disk.Rows(),rows);EXPECT_EQ(f.disk.ArchiveBytes(),archives);
    EXPECT_FALSE(storage::ReadOrchardSnapshotBase(f.disk.reopened));
    EXPECT_ANY_THROW(storage::RequireFullOrchardStorage(f.disk.reopened));
    {
        std::lock_guard<AnnotatedRecursiveMutex> lock(f.disk.startup_mutex);
        ASSERT_NE(owner->SelectedUnderLock(f.disk.startup_mutex),nullptr);
        EXPECT_EQ(owner->SelectedUnderLock(f.disk.startup_mutex)->height,101u);
        EXPECT_EQ(owner->SelectedUnderLock(f.disk.startup_mutex)->Encode(),f.proof->parent_catalog.Encode());
    }
    const auto context=*SelectedOrchardBlockContext(f.disk.first->Header(),102);
    const auto before=ReadRuntimeOutboxUnderLock(f.disk.reopened,context).head;
    f.disk.Connect(*owner,f.disk.first);
    const auto state=f.disk.reopened.getOrchardState();ASSERT_TRUE(state.ok());EXPECT_EQ(*state,f.proof->state);
    const auto undo=f.disk.reopened.getOrchardUndoParent(*state);ASSERT_TRUE(undo.ok());EXPECT_FALSE(undo->has_value());
    const auto event=ReadRuntimeOutboxUnderLock(f.disk.reopened,context,before,1);ASSERT_EQ(event.events.size(),1u);
    EXPECT_EQ(event.events[0].direction,RuntimeBlockDirection::Connect);EXPECT_EQ(event.events[0].context.height,102u);
    ASSERT_TRUE(event.events[0].orchard_replay);EXPECT_FALSE(event.events[0].orchard_replay->parent);
    EXPECT_EQ(event.events[0].orchard_replay->next,*state);EXPECT_FALSE(storage::ReadOrchardSnapshotBase(f.disk.reopened));
    f.disk.Undo(*owner,f.disk.first);owner.reset();
    EXPECT_EQ(f.disk.reopened.getTip()->height,101);EXPECT_EQ(f.disk.reopened.getOrchardState().status(),Status::NotFound);
    EXPECT_EQ(f.disk.reopened.getLegacyRetirementState().status(),Status::NotFound);
    f.disk.reopened.close();ASSERT_EQ(f.disk.reopened.init(f.disk.f.path),Status::Ok);
    const auto cold_rows=f.disk.Rows();const auto cold_archives=f.disk.ArchiveBytes();owner=f.Restore();ASSERT_TRUE(owner);
    EXPECT_EQ(f.disk.Rows(),cold_rows);EXPECT_EQ(f.disk.ArchiveBytes(),cold_archives);
    f.disk.Connect(*owner,f.disk.first);f.disk.Connect(*owner,f.disk.second);
    EXPECT_EQ(f.disk.reopened.getTip()->height,103);f.disk.Undo(*owner,f.disk.second);f.disk.Undo(*owner,f.disk.first);
    EXPECT_EQ(f.disk.reopened.getTip()->height,101);EXPECT_FALSE(storage::ReadOrchardSnapshotBase(f.disk.reopened));
    EXPECT_ANY_THROW(storage::RequireFullOrchardStorage(f.disk.reopened));
}
TEST(OrchardActivationParent, DescendantUnburiedCorruptBeforeImagesAndMissingOffPathNodesRefuseWithoutRepair){
    using namespace consensus;TransferredActivationParentFixture f;
    const auto descendant=f.disk.Rows();EXPECT_ANY_THROW(f.Restore());EXPECT_EQ(f.disk.Rows(),descendant);
    f.Parent();const auto intact=f.disk.Rows();const auto archives=f.disk.ArchiveBytes();
    MutableParams().state_commitment_burial_depth=2;EXPECT_ANY_THROW(f.Restore());MutableParams().state_commitment_burial_depth=1;
    auto forged=*f.proof;++forged.retirement.record.retired_value;
    EXPECT_ANY_THROW(OrchardSnapshotStoreTestAccess::RestoreParent(f.disk.startup_mutex,f.disk.reopened,*f.headers,forged));
    const auto marker=f.disk.reopened.getForestTipMarker();ASSERT_TRUE(marker.ok());auto wrong=*marker;++wrong.height;
    ASSERT_EQ(f.disk.reopened.putForestTipMarker(f.disk.f.token,wrong),Status::Ok);const auto bad_marker=f.disk.Rows();
    EXPECT_ANY_THROW(f.Restore());EXPECT_EQ(f.disk.Rows(),bad_marker);
    ASSERT_EQ(f.disk.reopened.putForestTipMarker(f.disk.f.token,*marker),Status::Ok);
    const auto frontier=f.disk.reopened.getShieldedState(ChainDB::ShieldedStateRecord::Frontier);ASSERT_TRUE(frontier.ok());
    ASSERT_EQ(f.disk.reopened.putShieldedState(f.disk.f.token,ChainDB::ShieldedStateRecord::Frontier,"corrupt"),Status::Ok);
    const auto bad_frontier=f.disk.Rows();EXPECT_ANY_THROW(f.Restore());EXPECT_EQ(f.disk.Rows(),bad_frontier);
    ASSERT_EQ(f.disk.reopened.putShieldedState(f.disk.f.token,ChainDB::ShieldedStateRecord::Frontier,*frontier),Status::Ok);
    EXPECT_EQ(f.disk.Rows(),intact);EXPECT_EQ(f.disk.ArchiveBytes(),archives);ASSERT_TRUE(f.Restore());
    uint256 offpath;const auto& catalog=f.proof->parent_catalog;
    storage::catalog::Tree(storage::catalog::Kind::Transactions,[&](const auto& id){return ReadOrchardSnapshotNode(*f.proof,id);})
        .ForEachNode(catalog.transactions,catalog.transaction_count,[&](const auto& id,const auto&){
            if(offpath.IsNull()&&id!=catalog.transactions)offpath=id;return true;
        });ASSERT_FALSE(offpath.IsNull());
    const auto key=std::string("Morchard_catalog_v1/node/")+std::string(reinterpret_cast<const char*>(offpath.data),32);
    f.disk.Mutate([&](auto& raw){rocksdb::WriteOptions sync;sync.sync=true;ASSERT_TRUE(raw.db->Delete(sync,raw.cf("utreexo"),key).ok());});
    const auto missing=f.disk.Rows();EXPECT_ANY_THROW(f.Restore());EXPECT_EQ(f.disk.Rows(),missing);
    f.disk.Mutate([&](auto& raw){raw.put("utreexo",key,ReadOrchardSnapshotNode(*f.proof,offpath));});ASSERT_TRUE(f.Restore());
    f.disk.Mutate([](auto& raw){raw.put("shielded_state_v1","O1Zuntracked","retain-corruption");});
    const auto untracked=f.disk.Rows();EXPECT_ANY_THROW(f.Restore());EXPECT_EQ(f.disk.Rows(),untracked);
    EXPECT_EQ(f.disk.ArchiveBytes(),archives);
}
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
TEST(OrchardCatalogDownload, OwnedWorkerDrivesActualServerAndRetainsOnlyUnpublishedProof){
    using namespace consensus;using Download=p2p::OrchardCatalogDownload;using Server=p2p::OrchardCatalogServer;
    using Transfer=p2p::OrchardCatalogTransfer;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    auto headers=std::make_shared<HeaderChainSelector>();for(const auto& b:source.f.blocks)ASSERT_TRUE(headers->AddHeader(b.header));
    ASSERT_TRUE(headers->AddHeader(first->Header()));
    const auto proof=std::make_shared<const OrchardSnapshotSection>(OrchardSnapshotStoreTestAccess::CaptureActivation(source.f.db,*headers));
    const auto before=source.f.db.getOrchardState().value();
    std::atomic<bool> enabled=false;std::atomic<unsigned> requests=0,pages=0,builds=0;
    std::mutex mutex;std::condition_variable cv;std::shared_ptr<Download> host;
    Server server([&](const auto& r){r.CheckSelected(*headers);},[&](const auto&){++builds;return proof;},
        [&](const auto& peer,const auto& command,const auto& bytes){
            return host->Enqueue({peer.address,peer.session,0},command,bytes);
        });
    host=std::make_shared<Download>(headers,source.f.path.parent_path(),[&]{return enabled.load();},
        []{return std::vector<Download::Peer>{{"archive",31,ServiceFlags::NODE_ORCHARD_CATALOG_V1}};},
        [&](const auto& peer,const auto& command,const auto& bytes){
            if(command==Transfer::RequestCommand)++requests;else ++pages;
            const auto accepted=server.Enqueue({peer.address,peer.session},command,bytes);
            cv.notify_all();return accepted;
        });
    enabled=true;host->Wake();
    // No daemon scheduler tick or direct actor calls. Both actual protocol
    // workers exchange their bounded messages over this in-process transport.
    const auto deadline=Download::Clock::now()+std::chrono::seconds(15);
    while(!host->ReadyProof()&&Download::Clock::now()<deadline){
        std::unique_lock lock(mutex);cv.wait_for(lock,std::chrono::milliseconds(20));
    }
    const auto result=host->ReadyProof();ASSERT_TRUE(result);
    EXPECT_EQ(HashOrchardSnapshotSection(*result),HashOrchardSnapshotSection(*proof));
    EXPECT_EQ(requests,1u);EXPECT_GT(pages,0u);EXPECT_EQ(builds,1u);
    uint256 nonce;nonce.data[0]=1;const auto request=Transfer::Request::Selected(*headers,nonce);
    const auto manifest=Transfer::Manifest{request,1,nonce}.Encode();
    EXPECT_FALSE(host->Enqueue({"archive",32,0},Transfer::ManifestCommand,manifest));
    EXPECT_FALSE(host->Enqueue({"archive",31,0},Transfer::ManifestCommand,manifest)); // no remaining flight
    EXPECT_EQ(source.f.db.getOrchardState().value(),before);
    EXPECT_EQ(source.f.service->GetActiveTip()->hash,first->Header().GetHash());
    host->Stop();server.Stop();EXPECT_FALSE(host->ReadyProof());
    // Host shutdown cannot revoke immutable DATA already copied by a caller.
    EXPECT_NO_THROW(OrchardSnapshotStore::VerifyCatalogTransferProof(*result,*headers));
}
TEST(OrchardCatalogClient, HostCancellationPoisonsCompletion){
    using namespace consensus;using Client=p2p::OrchardCatalogClient;using Transfer=p2p::OrchardCatalogTransfer;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    auto headers=std::make_shared<HeaderChainSelector>();for(const auto& b:source.f.blocks)ASSERT_TRUE(headers->AddHeader(b.header));
    ASSERT_TRUE(headers->AddHeader(first->Header()));uint256 nonce;nonce.data[0]=77;
    const auto request=Transfer::Request::Selected(*headers,nonce);
    const auto proof=OrchardSnapshotStoreTestAccess::CaptureActivation(source.f.db,*headers);
    Transfer::Sender sender(request,proof,source.f.path.parent_path());bool cancelled=false;
    Client client(request,headers,source.f.path.parent_path(),[&]{return cancelled;});
    Client::Peer peer{"archive",61,ServiceFlags::NODE_ORCHARD_CATALOG_V1};const auto began=Client::Clock::now();
    ASSERT_TRUE(client.Begin(peer,began));ASSERT_TRUE(client.Next(began));
    const auto manifest_at=began+std::chrono::minutes(5)+std::chrono::milliseconds(900);
    ASSERT_TRUE(client.Receive(peer,Transfer::ManifestCommand,sender.Description().Encode(),manifest_at));
    ASSERT_TRUE(client.Next(manifest_at));cancelled=true;
    EXPECT_FALSE(client.Next(manifest_at+std::chrono::milliseconds(1)));
    EXPECT_EQ(client.Current(),Client::State::Failed);EXPECT_ANY_THROW(client.Take());
    // Receiver cancellation must poison a complete staged stream too, even
    // when every page and its digest were otherwise correct.
    Transfer::Receiver receiver(request,sender.Description().Encode(),*headers,source.f.path.parent_path());
    while(receiver.NextPage()<sender.Description().Pages())receiver.Accept(sender.Serve(receiver.RequestNext()));
    EXPECT_ANY_THROW(receiver.Finish(*headers,[]{return true;}));
    EXPECT_ANY_THROW(receiver.Finish(*headers));
}
TEST(OrchardCatalogClient, SlowPreparationDoesNotStartPagesInAnOldRateWindow){
    using namespace consensus;using Client=p2p::OrchardCatalogClient;using Transfer=p2p::OrchardCatalogTransfer;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    auto headers=std::make_shared<HeaderChainSelector>();for(const auto& b:source.f.blocks)ASSERT_TRUE(headers->AddHeader(b.header));
    ASSERT_TRUE(headers->AddHeader(first->Header()));uint256 nonce;nonce.data[0]=78;
    const auto request=Transfer::Request::Selected(*headers,nonce);
    // Deliberately unfinished/untrusted page stream tests scheduling only.
    // No complete digest/proof is supplied and no owner may be taken from it.
    const Transfer::Manifest manifest{request,uint64_t(32)*Transfer::PageBytes,nonce};
    Client client(request,headers,source.f.path.parent_path());Client::Peer peer{"archive",62,ServiceFlags::NODE_ORCHARD_CATALOG_V1};
    const auto began=Client::Clock::now(),tick=began+std::chrono::minutes(5),received=tick+std::chrono::milliseconds(900);
    ASSERT_TRUE(client.Begin(peer,began));ASSERT_TRUE(client.Next(began));EXPECT_FALSE(client.Next(tick));
    ASSERT_TRUE(client.Receive(peer,Transfer::ManifestCommand,manifest.Encode(),received));
    for(uint32_t i=0;i<16;++i){
        const auto out=client.Next(received);ASSERT_TRUE(out);EXPECT_EQ(Transfer::PageRequest::Decode(out->bytes).index,i);
        const auto id=manifest.Id();std::vector<uint8_t> page(id.begin(),id.end());
        for(unsigned j=0;j<4;++j)page.push_back(uint8_t(i>>(8*j)));
        for(unsigned j=0;j<4;++j)page.push_back(uint8_t(Transfer::PageBytes>>(8*j)));
        page.resize(40+Transfer::PageBytes);
        ASSERT_TRUE(client.Receive(peer,Transfer::PageCommand,page,received));
    }
    EXPECT_FALSE(client.Next(received+std::chrono::milliseconds(200)));
    EXPECT_TRUE(client.Next(received+std::chrono::seconds(1)));
    EXPECT_ANY_THROW(client.Take());client.Cancel();
}
TEST(OrchardCatalogDownload, FailedPeerRotatesAndNetworkPauseDiscardsOldFlight){
    using namespace consensus;using Download=p2p::OrchardCatalogDownload;using Transfer=p2p::OrchardCatalogTransfer;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    auto headers=std::make_shared<HeaderChainSelector>();for(const auto& b:source.f.blocks)ASSERT_TRUE(headers->AddHeader(b.header));
    ASSERT_TRUE(headers->AddHeader(first->Header()));const auto proof=OrchardSnapshotStoreTestAccess::CaptureActivation(source.f.db,*headers);
    std::atomic<bool> enabled=false;std::atomic<unsigned> bad=0,good=0;
    std::mutex mutex;std::condition_variable cv;std::vector<uint256> nonces;
    std::unique_ptr<Transfer::Sender> sender;std::unique_ptr<Download> host;
    host=std::make_unique<Download>(headers,source.f.path.parent_path(),[&]{return enabled.load();},
        []{return std::vector<Download::Peer>{{"bad",41,ServiceFlags::NODE_ORCHARD_CATALOG_V1},
                                             {"good",42,ServiceFlags::NODE_ORCHARD_CATALOG_V1}};},
        [&](const auto& peer,const auto& command,const auto& bytes){
            if(peer.address=="bad"){++bad;return false;}
            if(command==Transfer::RequestCommand){
                const auto request=Transfer::Request::Decode(bytes);sender=std::make_unique<Transfer::Sender>(request,proof,source.f.path.parent_path());
                {std::lock_guard lock(mutex);nonces.push_back(request.nonce);}++good;
                return host->Enqueue(peer,Transfer::ManifestCommand,sender->Description().Encode());
            }
            return host->Enqueue(peer,Transfer::PageCommand,sender->Serve(bytes));
        });
    const auto await=[&](auto predicate){
        const auto deadline=Download::Clock::now()+std::chrono::seconds(15);
        while(!predicate()&&Download::Clock::now()<deadline){std::unique_lock lock(mutex);cv.wait_for(lock,std::chrono::milliseconds(20));}
        return predicate();
    };
    enabled=true;host->Wake();ASSERT_TRUE(await([&]{return bool(host->ReadyProof());}));
    EXPECT_EQ(bad,1u);EXPECT_EQ(good,1u);
    enabled=false;host->Wake();ASSERT_TRUE(await([&]{return !host->ReadyProof();}));
    enabled=true;host->Wake();ASSERT_TRUE(await([&]{return bool(host->ReadyProof());}));
    EXPECT_EQ(bad,1u);EXPECT_EQ(good,2u); // bad ADDRESS remains in bounded cooldown
    host->Stop();ASSERT_EQ(nonces.size(),2u);EXPECT_NE(nonces[0],nonces[1]);
}
TEST(OrchardCatalogDownload, IngressIsBoundedAndStopJoinsWithoutPublishing){
    using namespace consensus;using Download=p2p::OrchardCatalogDownload;using Transfer=p2p::OrchardCatalogTransfer;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    auto headers=std::make_shared<HeaderChainSelector>();for(const auto& b:source.f.blocks)ASSERT_TRUE(headers->AddHeader(b.header));
    ASSERT_TRUE(headers->AddHeader(first->Header()));
    std::mutex mutex;std::condition_variable cv;bool entered=false,release=false;std::atomic<unsigned> sends=0;
    Download host(headers,source.f.path.parent_path(),[]{return true;},
        []{return std::vector<Download::Peer>{{"archive",51,ServiceFlags::NODE_ORCHARD_CATALOG_V1}};},
        [&](const auto&,const auto&,const auto&){
            ++sends;std::unique_lock lock(mutex);entered=true;cv.notify_all();
            cv.wait_for(lock,std::chrono::seconds(10),[&]{return release;});return false;
        });
    {std::unique_lock lock(mutex);ASSERT_TRUE(cv.wait_for(lock,std::chrono::seconds(5),[&]{return entered;}));}
    std::vector<uint8_t> bytes(195);
    EXPECT_FALSE(host.Enqueue({"archive",52,0},Transfer::ManifestCommand,bytes));
    auto oversized=bytes;oversized.push_back(0);EXPECT_FALSE(host.Enqueue({"archive",51,0},Transfer::ManifestCommand,oversized));
    for(unsigned i=0;i<Download::MaximumQueue;++i)EXPECT_TRUE(host.Enqueue({"archive",51,0},Transfer::ManifestCommand,bytes));
    EXPECT_FALSE(host.Enqueue({"archive",51,0},Transfer::ManifestCommand,bytes));
    {std::lock_guard lock(mutex);release=true;cv.notify_all();}host.Stop();
    EXPECT_EQ(sends,1u);EXPECT_FALSE(host.ReadyProof());EXPECT_FALSE(host.Enqueue({"archive",51,0},Transfer::ManifestCommand,bytes));
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
TEST(OrchardPhoneHistory, SequentialBodiesExtendWithoutSkippingAndReadyDataIsNotState){
    using namespace consensus;using Access=OrchardSnapshotStoreTestAccess;using Download=p2p::OrchardPhoneHistoryDownload;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    source.f.service->setRuntimeBlockNotifications(std::make_shared<CanonicalPoolNotices>());
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    auto headers=std::make_shared<HeaderChainSelector>();for(const auto& b:source.f.blocks)ASSERT_TRUE(headers->AddHeader(b.header));
    ASSERT_TRUE(headers->AddHeader(first->Header()));
    auto proof=std::make_shared<const OrchardSnapshotSection>(Access::CaptureActivation(source.f.db,*headers));
    const auto a=source.f.db.getOrchardCatalogState(first->Header().GetHash());ASSERT_TRUE(a.ok());
    BlockAssembler assembler(&source.f.db);WireOrchardAssembler(assembler,source.f);
    const auto second=SolveCatalogTemplate(assembler.CreateOrchardBlock(OrchardMiningPayout));ASSERT_TRUE(second);
    ASSERT_TRUE(source.Submit(second->WireBytes()).connected);ASSERT_TRUE(headers->AddHeader(second->Header()));
    const auto b=source.f.db.getOrchardCatalogState(second->Header().GetHash());ASSERT_TRUE(b.ok());
    const auto unchanged=ChainDBTransactionReadTestPeer::HandoffRows(source.f.db);
    std::mutex mutex;std::condition_variable cv;std::deque<std::vector<uint8_t>> requests;
    std::optional<Download::Target> target;
    Download host(headers,source.f.path.parent_path(),[&]{std::lock_guard lock(mutex);return target;},[&]{return proof;},
        [&](auto p,const auto& t){return Access::ReplayActivation(std::move(p),headers,t.height,t.hash);},
        []{return std::vector<Download::Peer>{{"archive",71,ServiceFlags::NODE_NETWORK}};},
        [&](const auto&,const auto& command,const auto& bytes){
            std::lock_guard lock(mutex);EXPECT_EQ(command,"getdata");requests.push_back(bytes);cv.notify_all();return true;});
    const auto request=[&](const auto& body){
        std::unique_lock lock(mutex);
        if(!cv.wait_for(lock,std::chrono::seconds(5),[&]{return !requests.empty();}))return false;
        const auto bytes=std::move(requests.front());requests.pop_front();
        std::vector<uint8_t> expected{1,2,0,0,0};const auto hash=body.Header().GetHash();expected.insert(expected.end(),hash.begin(),hash.end());
        return bytes==expected;
    };
    const auto ready=[&]{const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(std::chrono::steady_clock::now()<deadline){if(host.Ready())return true;std::this_thread::sleep_for(std::chrono::milliseconds(10));}return false;};
    {std::lock_guard lock(mutex);target=Download::Target{102,102,first->Header().GetHash(),storage::catalog::Hash(*a),first->Header().GetHash()};}
    ASSERT_TRUE(request(*first));EXPECT_FALSE(host.Enqueue({"archive",72,0},first->WireBytes()));
    ASSERT_TRUE(host.Enqueue({"archive",71,0},first->WireBytes()));
    ASSERT_TRUE(ready());auto prefix=host.Ready();ASSERT_TRUE(prefix);
    EXPECT_EQ(prefix->Body(102),first->WireBytes());EXPECT_ANY_THROW(prefix->Body(103));
    EXPECT_FALSE(host.Enqueue({"archive",71,0},first->WireBytes()));
    {std::lock_guard lock(mutex);target=Download::Target{102,103,second->Header().GetHash(),storage::catalog::Hash(*b),first->Header().GetHash()};}
    ASSERT_TRUE(request(*second));ASSERT_TRUE(host.Enqueue({"archive",71,0},second->WireBytes()));ASSERT_TRUE(ready());
    auto complete=host.Ready();ASSERT_TRUE(complete);EXPECT_EQ(complete->Body(102),first->WireBytes());EXPECT_EQ(complete->Body(103),second->WireBytes());
    // A retained earlier DATA view cannot silently acquire a later target.
    EXPECT_ANY_THROW(prefix->Body(103));EXPECT_EQ(host.Observe().verified_height,103u);
    {std::lock_guard lock(mutex);target->catalog.data[0]^=1;}
    const auto refused=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(host.Observe().error.empty()&&std::chrono::steady_clock::now()<refused)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(host.Observe().error.empty());EXPECT_FALSE(host.Ready());
    {std::lock_guard lock(mutex);target->hash.data[0]^=1;}
    EXPECT_FALSE(host.Ready());host.Stop();EXPECT_FALSE(host.Ready());
    EXPECT_FALSE(host.Enqueue({"archive",71,0},second->WireBytes()));
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(source.f.db),unchanged);
}
TEST(OrchardPhoneHistory, CorruptFirstBodyNeverPublishesAndStopDiscardsFlight){
    using namespace consensus;using Access=OrchardSnapshotStoreTestAccess;using Download=p2p::OrchardPhoneHistoryDownload;
    CanonicalPoolFixture source(true);MutableParams().orchard_catalog_commitment_activation_height=102;
    MutableParams().state_commitment_burial_depth=0;
    const auto first=SolveCatalogTemplate(source.Build());ASSERT_TRUE(first);ASSERT_TRUE(source.Submit(first->WireBytes()).connected);
    auto headers=std::make_shared<HeaderChainSelector>();for(const auto& b:source.f.blocks)ASSERT_TRUE(headers->AddHeader(b.header));
    ASSERT_TRUE(headers->AddHeader(first->Header()));
    auto proof=std::make_shared<const OrchardSnapshotSection>(Access::CaptureActivation(source.f.db,*headers));
    const auto catalog=source.f.db.getOrchardCatalogState(first->Header().GetHash());ASSERT_TRUE(catalog.ok());
    const auto unchanged=ChainDBTransactionReadTestPeer::HandoffRows(source.f.db);
    const Download::Target selected{102,102,first->Header().GetHash(),storage::catalog::Hash(*catalog),first->Header().GetHash()};
    std::mutex mutex;std::condition_variable cv;unsigned sends=0;
    Download host(headers,source.f.path.parent_path(),[&]{return std::optional(selected);},[&]{return proof;},
        [&](auto p,const auto& t){return Access::ReplayActivation(std::move(p),headers,t.height,t.hash);},
        []{return std::vector<Download::Peer>{{"archive",81,ServiceFlags::NODE_NETWORK}};},
        [&](const auto&,const auto&,const auto&){std::lock_guard lock(mutex);++sends;cv.notify_all();return true;});
    {std::unique_lock lock(mutex);ASSERT_TRUE(cv.wait_for(lock,std::chrono::seconds(5),[&]{return sends!=0;}));}
    auto wrong_header=first->WireBytes();wrong_header.at(4)^=1;
    EXPECT_FALSE(host.Enqueue({"archive",81,0},wrong_header));EXPECT_FALSE(host.Enqueue({"other",81,0},first->WireBytes()));
    auto corrupt=first->WireBytes();corrupt.back()^=1; // Same requested header, invalid authenticated body.
    ASSERT_TRUE(host.Enqueue({"archive",81,0},corrupt));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(host.Observe().error.empty()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(host.Observe().error.empty());EXPECT_FALSE(host.Ready());EXPECT_EQ(host.Observe().verified_height,0u);
    host.Stop();EXPECT_FALSE(host.Enqueue({"archive",81,0},first->WireBytes()));
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(source.f.db),unchanged);
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
