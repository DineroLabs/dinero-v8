#pragma once
#include "p2p/orchard_catalog_client.h"
#include "crypto/secure_random.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

namespace dinero::p2p {
// Packet-woken host for the thread-affine client. Proof DATA only: no database
// writes, owner enrollment, lifecycle/validation flags or wallet callbacks.
// All callbacks run on the owned worker, outside the ingress mutex. Stop must
// join before the host releases network/chain dependencies.
class OrchardCatalogDownload final {
public:
    using Client=OrchardCatalogClient;using Transfer=Client::Transfer;
    using Peer=Client::Peer;using Bytes=Client::Bytes;using Clock=Client::Clock;
    using Needed=std::function<bool()>;
    using Peers=std::function<std::vector<Peer>()>;
    using Send=std::function<bool(const Peer&,const std::string&,const Bytes&)>;
    static constexpr size_t MaximumQueue=4,MaximumCooldowns=32;
    OrchardCatalogDownload(std::shared_ptr<const consensus::HeaderChainSelector> headers,
        std::filesystem::path work,Needed needed,Peers peers,Send send)
        :headers_(std::move(headers)),work_(std::move(work)),needed_(std::move(needed)),
         peers_(std::move(peers)),send_(std::move(send)),worker_([this]{Run();}){}
    ~OrchardCatalogDownload(){Stop();}
    OrchardCatalogDownload(const OrchardCatalogDownload&)=delete;
    OrchardCatalogDownload& operator=(const OrchardCatalogDownload&)=delete;
    bool Enqueue(Peer peer,const std::string& command,std::span<const uint8_t> payload){
        if(!((command==Transfer::ManifestCommand&&payload.size()==195)||
             (command==Transfer::PageCommand&&payload.size()>=40&&payload.size()<=40+Transfer::PageBytes)))return false;
        std::lock_guard lock(mutex_);
        if(stop_||!flight_||!Equal(peer,*flight_)||queue_.size()>=MaximumQueue)return false;
        queue_.push_back({std::move(peer),command,Bytes(payload.begin(),payload.end())});
        cv_.notify_one();return true;
    }
    // A copy of unpublished proof data, not an installation capability.
    // Selection is rechecked here; conversion must independently reauthenticate.
    std::shared_ptr<const consensus::OrchardSnapshotSection> ReadyProof()const {
        std::shared_ptr<const consensus::OrchardSnapshotSection> proof;
        std::optional<Transfer::Request> request;
        {std::lock_guard lock(mutex_);if(stop_)return {};proof=proof_;request=ready_;}
        if(!proof||!request)return {};
        try{request->CheckSelected(*headers_);return proof;}catch(...){return {};}
    }
    void Wake(){cv_.notify_one();}
    void Stop(){
        std::lock_guard joining(join_mutex_);
        {std::lock_guard lock(mutex_);stop_=true;queue_.clear();flight_.reset();proof_.reset();ready_.reset();}
        cv_.notify_all();if(worker_.joinable())worker_.join();
    }
private:
    struct Packet{Peer peer;std::string command;Bytes bytes;};
    static bool Equal(const Peer& a,const Peer& b){return a.address==b.address&&a.session==b.session;}
    void Run()noexcept {
        std::unique_ptr<Client> client;std::optional<Peer> peer;std::optional<Transfer::Request> request;
        std::map<std::string,Clock::time_point> cooldown;
        const auto clear=[&]{
            if(client)client->Cancel();client.reset();peer.reset();request.reset();
            std::lock_guard lock(mutex_);flight_.reset();queue_.clear();proof_.reset();ready_.reset();
        };
        const auto failed=[&]{
            if(peer){
                if(cooldown.size()>=MaximumCooldowns)cooldown.erase(std::min_element(cooldown.begin(),cooldown.end(),
                    [](const auto& a,const auto& b){return a.second<b.second;}));
                cooldown[peer->address]=Clock::now()+std::chrono::minutes(1);
            }
            clear();
        };
        // Timer drives rate-limit/retry/deadline checks even without packets;
        // packet notification drives the next sequential flight immediately.
        // Never borrow the daemon's five-second general scheduler.
        for(;;){
            std::optional<Packet> packet;
            {std::unique_lock lock(mutex_);
             cv_.wait_for(lock,std::chrono::milliseconds(50),[&]{return stop_||!queue_.empty();});
             if(stop_)break;
             if(!queue_.empty()){packet=std::move(queue_.front());queue_.pop_front();}}
            try {
                if(!needed_()){clear();continue;}
                // Poll the local selected branch even after a completed flight;
                // no peer response may pin a losing activation proof forever.
                if(client){
                    if(client->Current()==Client::State::Taken){
                        std::optional<Transfer::Request> selected;
                        {std::lock_guard lock(mutex_);selected=ready_;}
                        if(selected)selected->CheckSelected(*headers_);
                        else clear();
                        continue;
                    }
                    const auto connected=peers_();
                    if(!std::any_of(connected.begin(),connected.end(),[&](const auto& p){
                        return peer&&Equal(p,*peer)&&(p.services&ServiceFlags::NODE_ORCHARD_CATALOG_V1);})){
                        failed();continue;
                    }
                } else {
                    const auto now=Clock::now();
                    for(auto it=cooldown.begin();it!=cooldown.end();)
                        if(it->second<=now)it=cooldown.erase(it);else ++it;
                    for(const auto& candidate:peers_()){
                        if(!candidate.session||candidate.address.empty()||candidate.address.size()>256||
                           !(candidate.services&ServiceFlags::NODE_ORCHARD_CATALOG_V1)||cooldown.contains(candidate.address))continue;
                        const auto bytes=secure_random_bytes(32);uint256 nonce;std::copy(bytes.begin(),bytes.end(),nonce.begin());
                        request=Transfer::Request::Selected(*headers_,nonce);
                        client=std::make_unique<Client>(*request,headers_,work_,[this]{return stop_.load();});
                        if(!client->Begin(candidate,now)){client.reset();continue;}
                        peer=candidate;{std::lock_guard lock(mutex_);if(stop_)break;flight_=candidate;}
                        break;
                    }
                }
                if(stop_)break;
                if(!client)continue;
                if(packet)client->Receive(packet->peer,packet->command,packet->bytes,Clock::now());
                if(client->Current()==Client::State::Ready){
                    auto proof=client->Take();
                    request->CheckSelected(*headers_);
                    std::lock_guard lock(mutex_);if(stop_)break;
                    proof_=std::move(proof);ready_=request;flight_.reset();queue_.clear();continue;
                }
                if(const auto out=client->Next(Clock::now())){
                    if(!send_(out->peer,out->command,out->bytes)){failed();continue;}
                }
                if(client&&client->Current()==Client::State::Failed)failed();
            }catch(...){failed();}
        }
        clear();
    }
    const std::shared_ptr<const consensus::HeaderChainSelector> headers_;
    const std::filesystem::path work_;const Needed needed_;const Peers peers_;const Send send_;
    mutable std::mutex mutex_;std::mutex join_mutex_;std::condition_variable cv_;
    std::deque<Packet> queue_;std::optional<Peer> flight_;std::optional<Transfer::Request> ready_;
    std::shared_ptr<const consensus::OrchardSnapshotSection> proof_;std::atomic<bool> stop_{false};std::thread worker_;
};
} // namespace dinero::p2p
