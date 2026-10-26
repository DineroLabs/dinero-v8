#pragma once
#include "p2p/orchard_catalog_client.h"
#include "p2p/orchard_phone_history_target.h"
#include "daemon/services/orchard_boundary_replay.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <atomic>
#include <map>

namespace dinero::p2p {
// Public block DATA in a private disposable MAC spool. The verifier-root-bound
// index preserves the original validated byte digests independently of the MAC.
// This is neither a canonical runtime log nor a wallet-completion capability.
// Consumers must reauthenticate the proof/target and replay through genuine
// constructors; no serialized row can mint a seal or an applied cursor.
class OrchardPhoneHistoryData final {
public:
    using Bytes=std::vector<uint8_t>;
    const OrchardPhoneHistoryTarget target;
    const std::shared_ptr<const consensus::OrchardSnapshotSection> activation_proof;
    Bytes Body(uint32_t height)const {
        Require(height>=target.activation&&height<=target.height);
        std::lock_guard lock(corpus_->mutex);
        const auto digest=corpus_->index.Get(Key(height));
        const auto bytes=corpus_->spool.Get(BodyKey(height));
        Require(digest&&digest->size()==32&&bytes&&Hash(*bytes)==*digest);return *bytes;
    }
private:
    friend class OrchardPhoneHistoryDownload;
    struct Corpus {
        std::mutex mutex;
        wallet::detail::RuntimeReplaySpool spool;
        wallet::detail::RuntimeReplayIndex index{spool};
        explicit Corpus(const std::filesystem::path& work):spool(work){}
        void Append(uint32_t height,std::span<const uint8_t> body){
            std::lock_guard lock(mutex);Require(!index.Get(Key(height)));
            {wallet::detail::RuntimeReplaySpool::Batch batch(spool);spool.Insert(BodyKey(height),body);batch.Commit();}
            index.Set(Key(height),Hash(body));
        }
    };
    const std::shared_ptr<Corpus> corpus_;
    OrchardPhoneHistoryData(OrchardPhoneHistoryTarget t,std::shared_ptr<const consensus::OrchardSnapshotSection> p,
        std::shared_ptr<Corpus> c):target(t),activation_proof(std::move(p)),corpus_(std::move(c)){}
    static void Require(bool ok){if(!ok)throw std::runtime_error("Orchard phone history DATA refused");}
    static Bytes Hash(std::span<const uint8_t> body){Bytes out(32);SHA256(body.data(),body.size(),out.data());return out;}
    static wallet::detail::RuntimeReplayIndex::Key Key(uint32_t height){
        wallet::detail::RuntimeReplayIndex::Key k{'h'};for(unsigned i=0;i<4;++i)k[i+1]=uint8_t(height>>(8*i));return k;}
    static Bytes BodyKey(uint32_t height){Bytes k{'b'};for(unsigned i=0;i<4;++i)k.push_back(uint8_t(height>>(8*i)));return k;}
};

// Independent store-only full-body flight. It never enters tip download queues,
// writes ChainDB, substitutes snapshot state, creates runtime events or calls a
// wallet. One connection-bound request and one queued bounded body at a time.
class OrchardPhoneHistoryDownload final {
public:
    using Target=OrchardPhoneHistoryTarget;using Data=OrchardPhoneHistoryData;
    using Peer=OrchardCatalogClient::Peer;using Bytes=OrchardCatalogClient::Bytes;
    using Clock=std::chrono::steady_clock;
    using TargetSource=std::function<std::optional<Target>()>;
    using ProofSource=std::function<std::shared_ptr<const consensus::OrchardSnapshotSection>()>;
    using Verifier=std::function<std::unique_ptr<OrchardBoundaryReplay>(
        std::shared_ptr<const consensus::OrchardSnapshotSection>,const Target&)>;
    using Peers=std::function<std::vector<Peer>()>;
    using Send=std::function<bool(const Peer&,const std::string&,const Bytes&)>;
    struct Progress {bool requested=false,ready=false;uint32_t verified_height=0,target_height=0;uint256 target_hash;std::string error;};
    OrchardPhoneHistoryDownload(std::shared_ptr<const consensus::HeaderChainSelector> headers,
        std::filesystem::path work,TargetSource target,ProofSource proof,Verifier verify,Peers peers,Send send)
        :headers_(std::move(headers)),work_(std::move(work)),target_(std::move(target)),proof_(std::move(proof)),
         verify_(std::move(verify)),peers_(std::move(peers)),send_(std::move(send)),worker_([this]{Run();}){}
    ~OrchardPhoneHistoryDownload(){Stop();}
    bool Enqueue(const Peer& peer,std::span<const uint8_t> bytes){
        if(bytes.size()<128||bytes.size()>OrchardBlockCandidate::MaxWireSize())return false;
        const auto header=BlockHeader::Deserialize(std::vector<uint8_t>(bytes.begin(),bytes.begin()+128));if(!header)return false;
        std::lock_guard lock(mutex_);
        if(stop_||!flight_||body_received_||packet_||peer.address!=flight_->address||peer.session!=flight_->session||
            header->GetHash()!=wanted_)return false;
        packet_=Bytes(bytes.begin(),bytes.end());body_received_=true;cv_.notify_one();return true;
    }
    Progress Observe()const{std::lock_guard lock(mutex_);return progress_;}
    std::shared_ptr<const Data> Ready()const {
        std::shared_ptr<const Data> ready;{std::lock_guard lock(mutex_);if(stop_)return {};ready=ready_;}
        if(!ready)return {};
        try{const auto current=target_();return current&&*current==ready->target?ready:nullptr;}catch(...){return {};}
    }
    void Stop(){
        std::lock_guard joining(join_);
        {std::lock_guard lock(mutex_);stop_=true;flight_.reset();packet_.reset();ready_.reset();}
        cv_.notify_all();if(worker_.joinable())worker_.join();
    }
private:
    const std::shared_ptr<const consensus::HeaderChainSelector> headers_;const std::filesystem::path work_;
    const TargetSource target_;const ProofSource proof_;const Verifier verify_;const Peers peers_;const Send send_;
    mutable std::mutex mutex_;std::mutex join_;std::condition_variable cv_;
    std::optional<Peer> flight_;uint256 wanted_;std::optional<Bytes> packet_;
    bool body_received_=false;
    std::shared_ptr<const Data> ready_;Progress progress_;std::atomic<bool> stop_{false};std::thread worker_;
    void Run()noexcept {
        std::optional<Target> selected;std::optional<Peer> peer;
        std::shared_ptr<const consensus::OrchardSnapshotSection> proof;
        std::unique_ptr<OrchardBoundaryReplay> replay;std::shared_ptr<Data::Corpus> corpus;
        std::map<std::string,Clock::time_point> cooldown;Clock::time_point sent{},window=Clock::now();
        uint64_t window_bytes=0;uint32_t next=0;bool awaiting=false;
        const auto unpublish=[&]{std::lock_guard lock(mutex_);flight_.reset();packet_.reset();ready_.reset();};
        const auto discard=[&]{unpublish();replay.reset();corpus.reset();proof.reset();selected.reset();awaiting=false;peer.reset();};
        const auto fail_peer=[&]{
            if(peer){if(cooldown.size()>=32)cooldown.erase(cooldown.begin());cooldown[peer->address]=Clock::now()+std::chrono::minutes(1);}
            peer.reset();awaiting=false;std::lock_guard lock(mutex_);flight_.reset();packet_.reset();
        };
        for(;;){
            std::optional<Bytes> packet;
            {std::unique_lock lock(mutex_);cv_.wait_for(lock,std::chrono::milliseconds(50),[&]{return stop_||packet_.has_value();});
             if(stop_)break;if(packet_){packet=std::move(packet_);packet_.reset();}}
            try{
                const auto current=target_();
                if(!current){discard();std::lock_guard lock(mutex_);progress_={};continue;}
                if(selected&&*selected!=*current){
                    if(current->base==selected->base&&current->activation==selected->activation&&current->height>=selected->height&&replay){
                        replay->ExtendSelectedTarget(current->height,current->hash);selected=current;
                        std::lock_guard lock(mutex_);ready_.reset();
                    }else discard();
                }
                if(!replay){
                    proof=proof_();if(!proof)continue;
                    selected=current;replay=verify_(proof,*selected);if(!replay){discard();continue;}
                    if(target_()!=selected){discard();continue;}
                    corpus=std::make_shared<Data::Corpus>(work_);next=selected->activation;
                }
                {std::lock_guard lock(mutex_);progress_={true,false,next-1,selected->height,selected->hash,{}};}
                if(next>selected->height){
                    if(!replay->AtSelectedTarget()||storage::catalog::Hash(replay->Catalog().Encode())!=selected->catalog)
                        throw std::runtime_error("phone history final catalog mismatch");
                    if(target_()!=selected){discard();continue;}
                    std::lock_guard lock(mutex_);if(stop_)break;
                    ready_=std::shared_ptr<const Data>(new Data(*selected,proof,corpus));progress_.ready=true;continue;
                }
                if(packet){
                    if(!awaiting||!peer)continue;
                    // Charge received bytes even if decoding/proof checks
                    // refuse them. Invalid peers cannot evade the wire budget
                    // by rotating through the bounded cooldown inventory.
                    window_bytes+=packet->size();
                    const auto body=OrchardBlockCandidate::DecodeExact(*packet);
                    uint256 expected;uint32_t found=0;
                    if(!headers_->GetAncestorHashByHash(selected->hash,next,expected,found)||found!=selected->height||body.Header().GetHash()!=expected)
                        throw std::runtime_error("phone history body/selected target mismatch");
                    const auto space=std::filesystem::space(work_);
                    if(space.available<(uint64_t(512)<<20)+3*packet->size())throw std::runtime_error("phone history disk reserve unavailable");
                    const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    const auto checked=replay->Append(body,uint64_t(now));
                    if(!checked||checked->Context().height!=next||!checked->MatchesWire(body))throw std::runtime_error("phone history validation unavailable");
                    if(stop_)break;if(target_()!=selected){discard();continue;}
                    corpus->Append(next,*packet);++next;awaiting=false;
                    std::lock_guard lock(mutex_);flight_.reset();
                }
                const auto now=Clock::now();
                const auto connected=peers_();
                if(peer&&!std::any_of(connected.begin(),connected.end(),[&](const auto& p){return p.address==peer->address&&p.session==peer->session;}))fail_peer();
                if(awaiting){if(now-sent>=std::chrono::seconds(30))fail_peer();continue;}
                if(next>selected->height)continue;
                for(auto it=cooldown.begin();it!=cooldown.end();)if(it->second<=now)it=cooldown.erase(it);else ++it;
                if(!peer)for(const auto& p:connected)if(p.session&&!p.address.empty()&&p.address.size()<=256&&
                    (p.services&ServiceFlags::NODE_NETWORK)&&!cooldown.contains(p.address)){peer=p;break;}
                if(!peer)continue;
                if(now-window>=std::chrono::minutes(1)){window=now;window_bytes=0;}
                // A soft per-minute wire budget may overshoot by ONE bounded
                // body, whose size is not known until receipt; no history cap.
                if(window_bytes>=64*1024*1024)continue;
                uint256 wanted;uint32_t found=0;
                if(!headers_->GetAncestorHashByHash(selected->hash,next,wanted,found)||found!=selected->height)
                    throw std::runtime_error("phone history selected header unavailable");
                Bytes request{1,2,0,0,0};request.insert(request.end(),wanted.begin(),wanted.end());
                {std::lock_guard lock(mutex_);if(stop_)break;flight_=peer;wanted_=wanted;body_received_=false;}
                awaiting=true;sent=now;if(!send_(*peer,"getdata",request))fail_peer();
            }catch(const std::exception& error){
                fail_peer();discard();std::lock_guard lock(mutex_);progress_.ready=false;progress_.verified_height=0;progress_.error=error.what();
            }catch(...){fail_peer();discard();std::lock_guard lock(mutex_);progress_.ready=false;progress_.verified_height=0;progress_.error="phone history verification failed";}
        }
        discard();
    }
};
} // namespace dinero::p2p
