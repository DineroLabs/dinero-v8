#pragma once
#include <algorithm>
#include <map>
#include "p2p/orchard_catalog_client.h"
#include "p2p/orchard_phone_history_target.h"
#include "p2p/orchard_history_checkpoint.h"
#include "p2p/orchard_history_cache.h"
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
// wallet. Up to window_ connection-bound requests to one serving peer, appended in height order.
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
        std::filesystem::path work,TargetSource target,ProofSource proof,Verifier verify,Peers peers,Send send,
        std::shared_ptr<OrchardHistoryCache> cache={},size_t window=kDefaultWindow)
        :headers_(std::move(headers)),work_(std::move(work)),target_(std::move(target)),proof_(std::move(proof)),
         verify_(std::move(verify)),peers_(std::move(peers)),send_(std::move(send)),cache_(std::move(cache)),
         window_(std::clamp<size_t>(window,1,kMaxWindow)),worker_([this]{Run();}){}
    ~OrchardPhoneHistoryDownload(){Stop();}
    bool Enqueue(const Peer& peer,std::span<const uint8_t> bytes){
        if(bytes.size()<128||bytes.size()>OrchardBlockCandidate::MaxWireSize())return false;
        const auto header=BlockHeader::Deserialize(std::vector<uint8_t>(bytes.begin(),bytes.begin()+128));if(!header)return false;
        const auto hash=header->GetHash();
        std::lock_guard lock(mutex_);
        if(stop_)return false;
        // Accepted only for an outstanding request, by the expected block hash
        // AND the peer/session it was sent to: never by height alone. A late,
        // duplicate, unsolicited or wrong-peer body matches nothing and is left
        // to the normal block path, uncharged here.
        const auto it=std::find_if(inflight_.begin(),inflight_.end(),[&](const auto& entry){
            return entry.second.hash==hash;});
        if(it==inflight_.end()||peer.address!=it->second.address||peer.session!=it->second.session)return false;
        const auto height=it->first;inflight_.erase(it);
        // Charged once, here, whether it is buffered or later found stale.
        charged_bytes_+=bytes.size();
        // Hard memory bound: a body that would exceed the cap is dropped (and
        // re-requested once the buffer drains). History bodies are a few
        // hundred bytes, so this does not bind in practice.
        if(!received_.contains(height)&&received_bytes_+bytes.size()<=kReceivedBytesCap){
            received_.emplace(height,Received{Bytes(bytes.begin(),bytes.end()),hash,peer.address,peer.session,false});
            received_bytes_+=bytes.size();
        }
        cv_.notify_one();return true;
    }
    Progress Observe()const{std::lock_guard lock(mutex_);return progress_;}
    std::shared_ptr<const Data> Ready()const {
        std::shared_ptr<const Data> ready;{std::lock_guard lock(mutex_);if(stop_)return {};ready=ready_;}
        if(!ready)return {};
        try {
            if (cache_) cache_->Check();
            const auto current = target_();
            return current && *current == ready->target && HeaderSelected(*current) ? ready : nullptr;
        } catch (...) { return {}; }
    }
    // The newest history this download verified in full, while the selected
    // chain still contains its target (orchard_history_checkpoint.h). Unlike
    // Ready(), a block that arrived since does not withdraw it: the wallet
    // builds on the checkpoint and reads later blocks from the node's own
    // record. A reorganisation that replaces the target, another snapshot
    // base, or a cache root that no longer checks out withdraws it. The
    // data serves bodies up to its own target only, so the worker verifying
    // newer blocks meanwhile does not disturb a reader.
    std::shared_ptr<const Data> ReadyCheckpoint()const {
        std::shared_ptr<const Data> verified;{std::lock_guard lock(mutex_);if(stop_)return {};verified=checkpoint_;}
        if(!verified)return {};
        try{
            if(cache_)cache_->Check();
            const auto current=target_();
            if(!current||!HeaderSelected(*current))return {};
            const bool stands=OrchardHistoryCheckpointStands(verified->target,*current,
                [this,&current](const uint256& tip,uint32_t height)->std::optional<uint256>{
                    uint256 at;uint32_t found=0;
                    if(!headers_->GetAncestorHashByHash(tip,height,at,found)||found!=current->height)return std::nullopt;
                    return at;
                });
            return stands?verified:nullptr;
        }catch(...){return {};}
    }
    // The same, judged against a target the caller has just read (not the
    // cached one): for the final check before a wallet adopts its replay.
    std::shared_ptr<const Data> ReadyCheckpointFor(const Target& current)const {
        std::shared_ptr<const Data> verified;{std::lock_guard lock(mutex_);if(stop_)return {};verified=checkpoint_;}
        if(!verified)return {};
        try{
            if(cache_)cache_->Check();
            if(!HeaderSelected(current))return {};
            const bool stands=OrchardHistoryCheckpointStands(verified->target,current,
                [this,&current](const uint256& tip,uint32_t height)->std::optional<uint256>{
                    uint256 at;uint32_t found=0;
                    if(!headers_->GetAncestorHashByHash(tip,height,at,found)||found!=current.height)return std::nullopt;
                    return at;
                });
            return stands?verified:nullptr;
        }catch(...){return {};}
    }
    // Is the block (height, hash) the selected target itself or one of its
    // ancestors on the header chain? False for anything that cannot be told.
    bool OnChainOf(const Target& tip,uint32_t height,const uint256& hash)const {
        if(height==tip.height)return hash==tip.hash;
        if(height>tip.height)return false;
        try{
            uint256 at;uint32_t found=0;
            return headers_->GetAncestorHashByHash(tip.hash,height,at,found)&&found==tip.height&&at==hash;
        }catch(...){return false;}
    }
    void Stop(){
        std::lock_guard joining(join_);
        {std::lock_guard lock(mutex_);stop_=true;inflight_.clear();received_.clear();received_bytes_=0;ready_.reset();checkpoint_.reset();}
        cv_.notify_all();if(worker_.joinable())worker_.join();
    }
    // Idle workers (no selected target, or history ready for it) wait up to IdleWait instead
    // of polling every 50 ms; any request, packet or stop still wakes them.
    static constexpr std::chrono::milliseconds IdleWait{1000},ActiveWait{50};
    // Body requests kept in flight to the serving peer. 1 is the previous
    // one-at-a-time behaviour. See docs/design/orchard-history-pipelining.md.
#ifndef DINERO_ORCHARD_HISTORY_DEFAULT_WINDOW
#define DINERO_ORCHARD_HISTORY_DEFAULT_WINDOW 8
#endif
    // Overridable at compile time so the whole test suite can also run at 1
    // (the previous behaviour): -DDINERO_ORCHARD_HISTORY_DEFAULT_WINDOW=1.
    static constexpr size_t kDefaultWindow=DINERO_ORCHARD_HISTORY_DEFAULT_WINDOW,kMaxWindow=16;
    static_assert(kDefaultWindow>=1&&kDefaultWindow<=kMaxWindow);
    // Hard bound on bodies received but not yet appended.
    static constexpr size_t kReceivedBytesCap=size_t(32)<<20;
    static constexpr std::chrono::seconds RequestTimeout{30};
    size_t Window()const{return window_;}
    uint64_t Iterations()const{return iterations_.load();}

private:
    const std::shared_ptr<const consensus::HeaderChainSelector> headers_;const std::filesystem::path work_;
    const TargetSource target_;const ProofSource proof_;const Verifier verify_;const Peers peers_;const Send send_;
    const std::shared_ptr<OrchardHistoryCache> cache_;
    const size_t window_;
    // The public cache is an optimization over revalidated bytes. Its first
    // refusal (for example records written under another datadir root
    // identity) retires it for this download: bodies are then fetched from
    // peers instead of retrying the same refused record forever.
    std::atomic<bool> cache_retired_{false};std::atomic<uint64_t> iterations_{0};
    bool Cached()const{return cache_&&!cache_retired_.load();}
    // Retire only for a record-level refusal: if the cache root itself no
    // longer checks out (datadir replaced, lock lost) keep failing closed
    // instead of refetching all history from peers in a loop.
    void RetireCache(){cache_->Check();cache_retired_.store(true);}
    mutable std::mutex mutex_;std::mutex join_;std::condition_variable cv_;
    // One outstanding body request. At most window_ of them, all to one
    // serving peer; keyed by height. Guarded by mutex_.
    struct Flight {uint256 hash;std::string address;uint64_t session=0;Clock::time_point deadline;};
    std::map<uint32_t,Flight> inflight_;
    // Bodies received (or read from the cache) and not yet appended, keyed by
    // height. Nothing here is trusted until it is revalidated against the
    // selected chain's hash at its height, at append time. Guarded by mutex_.
    struct Received {Bytes bytes;uint256 hash;std::string address;uint64_t session=0;bool cached=false;};
    std::map<uint32_t,Received> received_;size_t received_bytes_=0;
    uint64_t charged_bytes_=0; // wire bytes accepted by Enqueue, taken into the per-minute budget by Run
    std::shared_ptr<const Data> ready_;Progress progress_;
    // The last fully verified history; kept across pure tip extensions (ready_ is not).
    std::shared_ptr<const Data> checkpoint_;std::atomic<bool> stop_{false};std::thread worker_;
    bool HeaderSelected(const Target& target) const {
        consensus::HeaderIndexEntry header;
        return headers_ && headers_->GetHeaderAtHeightCopy(target.height, header) && header.hash == target.hash;
    }
    void Run()noexcept {
        std::optional<Target> selected;std::optional<Peer> peer;
        std::shared_ptr<const consensus::OrchardSnapshotSection> proof;
        std::unique_ptr<OrchardBoundaryReplay> replay;std::shared_ptr<Data::Corpus> corpus;
        std::map<std::string,Clock::time_point> cooldown;Clock::time_point window=Clock::now();
        uint64_t window_bytes=0;uint32_t next=0;
        // Caller holds mutex_.
        const auto clear_window=[&]{inflight_.clear();received_.clear();received_bytes_=0;};
        // Only on a discard (a reorganisation, another target, a failed check): a pure extension keeps the checkpoint.
        const auto unpublish=[&]{std::lock_guard lock(mutex_);clear_window();ready_.reset();checkpoint_.reset();};
        const auto discard=[&]{unpublish();replay.reset();corpus.reset();proof.reset();selected.reset();peer.reset();};
        // A target that is not selected at this instant (the node is between
        // two tips) withdraws what is being offered as ready, not the
        // checkpoint: the verified prefix is still held, and whoever takes
        // the checkpoint checks it against the selected chain itself
        // (ReadyCheckpoint, ReadyCheckpointFor). Withdrawing it here would
        // make it last only until the next block arrives. Requests in flight
        // are dropped; bodies already received are KEPT (they are revalidated
        // by hash before they are appended), so a pause never turns them into
        // re-downloads or a gap.
        const auto pause=[&]{
            std::lock_guard lock(mutex_);
            inflight_.clear();ready_.reset();
            if(replay)progress_.ready=false;
            else progress_={};
        };
        const auto cool=[&](const std::string& address){
            if(cooldown.size()>=32)cooldown.erase(cooldown.begin());cooldown[address]=Clock::now()+std::chrono::minutes(1);
        };
        // The serving peer failed (timed out, disconnected, or a send failed):
        // cool it and drop ALL of its outstanding requests at once, to be
        // re-requested from the next eligible peer, instead of letting each
        // run to its own deadline. Bodies it already delivered are kept.
        const auto fail_peer=[&]{
            if(!peer)return;
            cool(peer->address);
            {std::lock_guard lock(mutex_);
             for(auto it=inflight_.begin();it!=inflight_.end();)
                 it=it->second.address==peer->address&&it->second.session==peer->session?inflight_.erase(it):std::next(it);}
            peer.reset();
        };
        // The selected chain's block hash at `height`, for the current target.
        const auto expected_at=[&](uint32_t height){
            uint256 hash;uint32_t found=0;
            if(!headers_->GetAncestorHashByHash(selected->hash,height,hash,found)||found!=selected->height)
                throw std::runtime_error("phone history selected header unavailable");
            return hash;
        };
        bool idle=false;
        for(;;){
            {std::unique_lock lock(mutex_);cv_.wait_for(lock,idle?IdleWait:ActiveWait,[&]{return stop_||received_.contains(next);});
             if(stop_)break;
             // Include abandoned requests and bodies later found stale in the wire budget.
             window_bytes+=charged_bytes_;charged_bytes_=0;}
            ++iterations_;idle=false;
            // The peer that sent the body being appended, so a bad body cools that peer.
            std::optional<std::pair<std::string,uint64_t>> appending_from;
            try{
                const auto current=target_();
                if (!current || !HeaderSelected(*current)) {
                    pause();
                    idle = !current;
                    continue;
                }
                const auto same_base=[&](const Target& target){
                    return selected&&target.base==selected->base&&target.activation==selected->activation;
                };
                if(selected&&*selected!=*current){
                    using Result=OrchardBoundaryReplay::RetargetResult;
                    const auto retarget = same_base(*current)
                        ? replay->RetargetSelectedHistory(current->height, current->hash) : Result::PrefixChanged;
                    if (retarget == Result::Unselected) {
                        pause();
                        continue;
                    }
                    if (retarget == Result::PrefixChanged) {
                        discard();
                    } else {
                        selected = current;
                        // Drop requests whose block is no longer the selected
                        // chain's at that height, and anything above the new
                        // target; buffered bodies are revalidated at append.
                        std::vector<std::pair<uint32_t,uint256>> outstanding;
                        {std::lock_guard lock(mutex_);ready_.reset();
                         for(const auto& [height,flight]:inflight_)outstanding.push_back({height,flight.hash});}
                        std::vector<uint32_t> stale;
                        for(const auto& [height,hash]:outstanding)
                            if(height>selected->height||expected_at(height)!=hash)stale.push_back(height);
                        std::lock_guard lock(mutex_);
                        for(const auto height:stale)inflight_.erase(height);
                        for(auto it=received_.upper_bound(selected->height);it!=received_.end();){
                            received_bytes_-=it->second.bytes.size();it=received_.erase(it);}
                    }
                }
                if(!replay){
                    proof=proof_();if(!proof)continue;
                    selected=current;replay=verify_(proof,*selected);if(!replay){discard();continue;}
                    // Finish initializing the private corpus on a same-base
                    // tip change. The next iteration can then retarget it.
                    if(const auto now=target_();now&&!same_base(*now)){discard();continue;}
                    if(Cached())try{const auto size=consensus::OrchardSnapshotSectionEncodedSize(*proof);
                        cache_->Put('p',proof->state.block_hash,size,consensus::ORCHARD_SNAPSHOT_MAX_SECTION_BYTES,
                            [&](const auto& sink){Data::Require(consensus::StreamOrchardSnapshotSection(*proof,sink)==size);});}
                        catch(...){RetireCache();}
                    corpus=std::make_shared<Data::Corpus>(work_);next=selected->activation;
                    {std::lock_guard lock(mutex_);clear_window();}
                }
                {std::lock_guard lock(mutex_);progress_={true,false,next-1,selected->height,selected->hash,{}};}
                if(next>selected->height){
                    if (!replay->AtSelectedTarget()) {
                        if (!HeaderSelected(*selected)) {
                            pause();
                            continue;
                        }
                        throw std::runtime_error("phone history verifier is out of step with the selected target");
                    }
                    if(storage::catalog::Hash(replay->Catalog().Encode())!=selected->catalog)
                        throw std::runtime_error("phone history final catalog mismatch");
                    if(target_()!=selected)continue;
                    if(cache_)cache_->Check();
                    std::lock_guard lock(mutex_);if(stop_)break;
                    ready_=std::shared_ptr<const Data>(new Data(*selected,proof,corpus));checkpoint_=ready_;progress_.ready=true;idle=true;continue;
                }
                // Append received bodies strictly in height order. Nothing is
                // skipped: a gap stops appending until it is filled.
                bool paused=false,moved=false;
                for(;;){
                    std::optional<Received> body;
                    {std::lock_guard lock(mutex_);if(const auto it=received_.find(next);it!=received_.end())body=it->second;}
                    if(!body)break;
                    const auto expected=expected_at(next);
                    if(body->hash!=expected){
                        // Requested for a block the selected chain has since
                        // replaced: never appended; re-requested by the new hash.
                        std::lock_guard lock(mutex_);
                        if(const auto it=received_.find(next);it!=received_.end()){received_bytes_-=it->second.bytes.size();received_.erase(it);}
                        break;
                    }
                    if(!body->cached)appending_from=std::pair{body->address,body->session};
                    const auto decoded=OrchardBlockCandidate::DecodeExact(body->bytes);
                    if(decoded.Header().GetHash()!=expected)throw std::runtime_error("phone history body/selected target mismatch");
                    const auto space=std::filesystem::space(work_);
                    if(space.available<(uint64_t(512)<<20)+3*body->bytes.size())throw std::runtime_error("phone history disk reserve unavailable");
                    const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    const auto checked=replay->AppendForSelectedHistory(decoded,uint64_t(now));
                    // Not selected at this instant: the body stays buffered and is retried.
                    if(!checked){pause();paused=true;break;}
                    if(checked->Context().height!=next||!checked->MatchesWire(decoded))throw std::runtime_error("phone history validation unavailable");
                    if(stop_)break;
                    if(Cached()&&!body->cached)try{cache_->Put('b',expected,body->bytes.size(),OrchardBlockCandidate::MaxWireSize(),
                        [&](const auto& sink){sink(body->bytes);});}catch(...){RetireCache();}
                    corpus->Append(next,body->bytes);
                    ++next;appending_from.reset();
                    // Progress reflects every appended body before anything
                    // further is requested (as with one request at a time).
                    {std::lock_guard lock(mutex_);
                     if(const auto it=received_.find(next-1);it!=received_.end()){received_bytes_-=it->second.bytes.size();received_.erase(it);}
                     progress_.verified_height=next-1;}
                    // Retarget before requesting anything on a changed chain.
                    if(next>selected->height||target_()!=selected){moved=true;break;}
                }
                if(stop_)break;
                if(paused||moved||next>selected->height)continue;
                const auto now=Clock::now();
                const auto connected=peers_();
                if(peer&&!std::any_of(connected.begin(),connected.end(),[&](const auto& p){return p.address==peer->address&&p.session==peer->session;}))fail_peer();
                bool timed_out=false;
                {std::lock_guard lock(mutex_);
                 timed_out=std::any_of(inflight_.begin(),inflight_.end(),[&](const auto& entry){return now>=entry.second.deadline;});}
                if(timed_out)fail_peer();
                for(auto it=cooldown.begin();it!=cooldown.end();)if(it->second<=now)it=cooldown.erase(it);else ++it;
                if(!peer)for(const auto& p:connected)if(p.session&&!p.address.empty()&&p.address.size()<=256&&
                    (p.services&ServiceFlags::NODE_NETWORK)&&!cooldown.contains(p.address)){peer=p;break;}
                if(now-window>=std::chrono::minutes(1)){window=now;window_bytes=0;}
                // Fill the window, lowest height first, never above the target.
                const uint64_t last=std::min<uint64_t>(selected->height,uint64_t(next)+window_-1);
                for(uint64_t h=next;h<=last;++h){
                    const auto height=uint32_t(h);
                    {std::lock_guard lock(mutex_);
                     if(inflight_.contains(height)||received_.contains(height))continue;
                     if(received_bytes_>=kReceivedBytesCap)break;}
                    const auto wanted=expected_at(height);
                    // The cache path sends no request for a body it holds.
                    if(Cached()){
                        std::optional<Bytes> held;
                        try{if(cache_->Has('b',wanted))cache_->Read('b',wanted,OrchardBlockCandidate::MaxWireSize(),
                            [&](uint64_t size,const auto& read){held=Bytes(size);read(*held);});}
                        catch(...){RetireCache();held.reset();}
                        if(held){
                            {std::lock_guard lock(mutex_);
                             if(!received_.contains(height)){received_bytes_+=held->size();
                                 received_.emplace(height,Received{std::move(*held),wanted,{},0,true});}}
                            // The cache filled the next body to append: append
                            // it (and report it) before requesting anything further.
                            if(height==next)break;
                            continue;
                        }
                    }
                    if(!peer)break;
                    // A soft per-minute wire budget may overshoot by up to the
                    // window's bounded bodies, whose sizes are not known until receipt.
                    if(window_bytes>=64*1024*1024)break;
                    Bytes request{1,2,0,0,0};request.insert(request.end(),wanted.begin(),wanted.end());
                    {std::lock_guard lock(mutex_);if(stop_)break;
                     inflight_.insert_or_assign(height,Flight{wanted,peer->address,peer->session,now+RequestTimeout});}
                    if(!send_(*peer,"getdata",request)){fail_peer();break;}
                }
            }catch(const std::exception& error){
                // A bad body cools the peer that sent it; the whole window, the
                // buffer and the verified prefix are discarded (unchanged).
                if(appending_from)cool(appending_from->first);else fail_peer();
                discard();std::lock_guard lock(mutex_);progress_.ready=false;progress_.verified_height=0;progress_.error=error.what();
            }catch(...){
                if(appending_from)cool(appending_from->first);else fail_peer();
                discard();std::lock_guard lock(mutex_);progress_.ready=false;progress_.verified_height=0;progress_.error="phone history verification failed";}
        }
        discard();
    }
};
} // namespace dinero::p2p
