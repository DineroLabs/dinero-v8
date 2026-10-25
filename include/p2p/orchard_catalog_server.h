#pragma once
#include "p2p/orchard_catalog_transfer.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>
#include <tuple>

namespace dinero::p2p {
// One worker, one immutable payload, bounded connection-bound sessions. All
// source/proof/disk/send work runs outside the ingress mutex and peer threads.
// The preparation callback owns real ChainstateService shutdown admission.
class OrchardCatalogServer final {
public:
    using Transfer=OrchardCatalogTransfer;
    using Bytes=Transfer::Bytes;
    using Clock=std::chrono::steady_clock;
    struct Peer {std::string address;uint64_t session=0;bool operator<(const Peer& b)const {
        return std::tie(address,session)<std::tie(b.address,b.session);}};
    using Selected=std::function<void(const Transfer::Request&)>;
    using Prepare=std::function<std::shared_ptr<const consensus::OrchardSnapshotSection>(const std::function<bool()>&)>;
    using Send=std::function<bool(const Peer&,const std::string&,const Bytes&)>;
    static constexpr size_t MaximumSessions=8,MaximumQueue=32;
    OrchardCatalogServer(Selected selected,Prepare prepare,Send send)
        :selected_(std::move(selected)),prepare_(std::move(prepare)),send_(std::move(send)),worker_([this]{Run();}){}
    ~OrchardCatalogServer(){Stop();}
    OrchardCatalogServer(const OrchardCatalogServer&)=delete;
    OrchardCatalogServer& operator=(const OrchardCatalogServer&)=delete;
    bool Enqueue(Peer peer,const std::string& command,std::span<const uint8_t> payload){
        if(!peer.session||peer.address.empty()||peer.address.size()>256||
           !((command==Transfer::RequestCommand&&payload.size()==151)||
             (command==Transfer::PageRequestCommand&&payload.size()==42)))return false;
        std::lock_guard lock(mutex_);
        if(stop_||queue_.size()>=MaximumQueue)return false;
        // At most one queued flight per connection. A request cannot fan out
        // preparation tasks or force arbitrary response allocation.
        for(const auto& task:queue_)if(Equal(task.peer,peer))return false;
        queue_.push_back({std::move(peer),command,Bytes(payload.begin(),payload.end()),Clock::now()});
        cv_.notify_one();return true;
    }
    void Stop(){
        std::lock_guard joining(join_mutex_);
        {std::lock_guard lock(mutex_);stop_=true;queue_.clear();}cv_.notify_all();
        if(worker_.joinable())worker_.join();
    }
private:
    struct Task{Peer peer;std::string command;Bytes bytes;Clock::time_point queued;};
    struct Session{Transfer::Sender sender;Clock::time_point began,last,window;uint32_t next=0,pages=0;};
    static bool Equal(const Peer& a,const Peer& b){return a.address==b.address&&a.session==b.session;}
    void Run()noexcept {
        std::map<Peer,Session> sessions;std::optional<Transfer::Sender> cache;
        Clock::time_point last_build{},second{},hour{};uint32_t pages=0;uint64_t hourly_bytes=0;
        for(;;){
            Task task;
            {std::unique_lock lock(mutex_);cv_.wait(lock,[&]{return stop_||!queue_.empty();});
             if(stop_)break;task=std::move(queue_.front());queue_.pop_front();}
            const auto now=Clock::now();
            for(auto it=sessions.begin();it!=sessions.end();){
                if(now-it->second.last>std::chrono::seconds(30)||now-it->second.began>std::chrono::minutes(30))it=sessions.erase(it);
                else ++it;
            }
            if(now-task.queued>std::chrono::seconds(30))continue;
            try {
                if(task.command==Transfer::RequestCommand){
                    const auto request=Transfer::Request::Decode(task.bytes);selected_(request);
                    if(sessions.contains(task.peer)||sessions.size()>=MaximumSessions)continue;
                    if(cache){
                        auto identity=cache->Description().request;identity.nonce=request.nonce;
                        if(identity!=request){sessions.clear();cache.reset();}
                    }
                    if(!cache){
                        if(last_build!=Clock::time_point{}&&now-last_build<std::chrono::minutes(1))continue;
                        last_build=now;const auto deadline=now+std::chrono::minutes(10);
                        const auto cancelled=[&]{return stop_.load()||Clock::now()>deadline;};
                        const auto proof=prepare_(cancelled);if(!proof||cancelled())continue;
                        selected_(request);cache.emplace(request,*proof,std::filesystem::path{},cancelled);
                        if(cancelled()){cache.reset();continue;}
                    }
                    auto sender=cache->Rebind(request);selected_(request);
                    if(send_(task.peer,Transfer::ManifestCommand,sender.Description().Encode())){
                        const auto sent=Clock::now();sessions.emplace(task.peer,Session{std::move(sender),sent,sent,sent});
                    }
                } else {
                    auto it=sessions.find(task.peer);if(it==sessions.end())continue;
                    auto& session=it->second;selected_(session.sender.Description().request);
                    const auto request=Transfer::PageRequest::Decode(task.bytes);
                    if(request.transfer!=session.sender.Description().Id()||
                        (request.index!=session.next&&(!session.next||request.index!=session.next-1)))continue;
                    if(now-second>=std::chrono::seconds(1)){second=now;pages=0;}
                    if(now-session.window>=std::chrono::seconds(1)){session.window=now;session.pages=0;}
                    if(now-hour>=std::chrono::hours(1)){hour=now;hourly_bytes=0;}
                    if(pages>=64||session.pages>=16||hourly_bytes>=uint64_t(8)*Transfer::MaximumBytes)continue;
                    const auto page=session.sender.Serve(task.bytes);
                    if(page.size()>uint64_t(8)*Transfer::MaximumBytes-hourly_bytes)continue;
                    ++pages;++session.pages;hourly_bytes+=page.size();
                    if(!send_(task.peer,Transfer::PageCommand,page)){sessions.erase(it);continue;}
                    if(request.index==session.next)++session.next;session.last=Clock::now();
                }
            } catch(...) {sessions.erase(task.peer);}
        }
    }
    const Selected selected_;const Prepare prepare_;const Send send_;
    std::mutex mutex_,join_mutex_;std::condition_variable cv_;std::deque<Task> queue_;std::atomic<bool> stop_{false};
    std::thread worker_;
};
} // namespace dinero::p2p
