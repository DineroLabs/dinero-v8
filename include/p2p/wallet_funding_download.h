#pragma once
#include "p2p/orchard_catalog_client.h"
#include "consensus/merkle_root.h"
#include "primitives/block.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace dinero::p2p {
// Bounded, volatile PUBLIC historical bodies only. No ChainDB/index write,
// history receipt, coin authority, wallet identity or transaction request goes
// over this channel. The selected source must recheck ancestry, inclusion,
// output metadata AND current accumulator membership for each actual use.
class WalletFundingDownload final {
public:
    using Peer=OrchardCatalogClient::Peer;using Bytes=OrchardCatalogClient::Bytes;
    using Peers=std::function<std::vector<Peer>()>;
    using Send=std::function<bool(const Peer&,const std::string&,const Bytes&)>;
    static constexpr size_t MaximumBody=4*1024*1024,MaximumBodies=8;
    WalletFundingDownload(Peers peers,Send send):peers_(std::move(peers)),send_(std::move(send)),worker_([this]{Run();}){}
    ~WalletFundingDownload(){Stop();}
    std::shared_ptr<const Bytes> Request(const uint256& hash){
        if(hash.IsNull())throw std::invalid_argument("Funding body identity missing");
        std::lock_guard lock(mutex_);if(stop_)return {};
        if(const auto it=ready_.find(hash);it!=ready_.end())return it->second;
        if(!std::count(wanted_.begin(),wanted_.end(),hash)){
            if(wanted_.size()>=MaximumBodies)throw std::runtime_error("Funding body request capacity exceeded");
            wanted_.push_back(hash);cv_.notify_one();
        }
        return {};
    }
    bool Enqueue(const Peer& peer,std::span<const uint8_t> bytes){
        if(bytes.size()<130||bytes.size()>MaximumBody)return false;
        const auto header=BlockHeader::Deserialize(bytes.data(),bytes.size());if(!header)return false;
        std::lock_guard lock(mutex_);
        if(stop_||!flight_||packet_||received_||wanted_.empty()||header->GetHash()!=wanted_.front()||
            peer.address!=flight_->address||peer.session!=flight_->session)return false;
        packet_=Bytes(bytes.begin(),bytes.end());received_=true;cv_.notify_one();return true;
    }
    void Stop(){
        std::lock_guard join(join_);
        {std::lock_guard lock(mutex_);stop_=true;flight_.reset();packet_.reset();ready_.clear();wanted_.clear();}
        cv_.notify_all();if(worker_.joinable())worker_.join();
    }
private:
    using Clock=std::chrono::steady_clock;
    const Peers peers_;const Send send_;std::mutex mutex_,join_;std::condition_variable cv_;
    bool stop_=false,received_=false;std::deque<uint256> wanted_;
    std::map<uint256,std::shared_ptr<const Bytes>> ready_;
    std::optional<Peer> flight_;std::optional<Bytes> packet_;std::thread worker_;
    void Run()noexcept{
        std::map<std::string,Clock::time_point> cooldown;Clock::time_point sent{},window=Clock::now();size_t used=0;
        for(;;){
            std::optional<Bytes> packet;std::optional<Peer> peer;uint256 wanted;
            {std::unique_lock lock(mutex_);cv_.wait_for(lock,std::chrono::milliseconds(50),[&]{return stop_||packet_.has_value();});
             if(stop_)return;if(wanted_.empty())continue;wanted=wanted_.front();peer=flight_;packet=std::move(packet_);packet_.reset();}
            const auto fail=[&]{
                if(peer){if(cooldown.size()>=32)cooldown.erase(cooldown.begin());cooldown[peer->address]=Clock::now()+std::chrono::minutes(1);}
                std::lock_guard lock(mutex_);flight_.reset();packet_.reset();received_=false;
            };
            try{
                const auto now=Clock::now();const auto connected=peers_();
                if(packet){
                    used+=packet->size(); // Bad bodies also consume the wire budget.
                    const auto body=Block::Deserialize(*packet);bool mutated=false;
                    if(!body||body->GetHash()!=wanted||body->vtx.empty()||
                       consensus::ComputeMerkleRoot(body->vtx,&mutated)!=body->header.merkle_root||mutated)
                        throw std::runtime_error("Funding body inclusion refused");
                    // This is only immutable DATA, not execution of this block.
                    auto bytes=std::make_shared<const Bytes>(std::move(*packet));
                    std::lock_guard lock(mutex_);if(stop_)return;
                    if(wanted_.empty()||wanted_.front()!=wanted)throw std::runtime_error("Funding flight changed");
                    if(ready_.size()>=MaximumBodies)ready_.erase(ready_.begin());
                    ready_.emplace(wanted,std::move(bytes));wanted_.pop_front();flight_.reset();received_=false;continue;
                }
                if(peer){
                    if(now-sent>=std::chrono::seconds(30)||!std::any_of(connected.begin(),connected.end(),
                        [&](const auto& p){return p.address==peer->address&&p.session==peer->session;}))fail();
                    continue;
                }
                for(auto it=cooldown.begin();it!=cooldown.end();)if(it->second<=now)it=cooldown.erase(it);else ++it;
                if(now-window>=std::chrono::minutes(1)){window=now;used=0;}
                if(used>=64*1024*1024)continue; // At most one bounded-body overshoot.
                for(const auto& p:connected)if(p.session&&!p.address.empty()&&p.address.size()<=256&&
                    (p.services&ServiceFlags::NODE_NETWORK)&&!cooldown.contains(p.address)){peer=p;break;}
                if(!peer)continue;
                Bytes request{1,2,0,0,0};request.insert(request.end(),wanted.begin(),wanted.end());
                {std::lock_guard lock(mutex_);if(stop_)return;flight_=peer;received_=false;}
                sent=now;if(!send_(*peer,"getdata",request))fail();
            }catch(...){fail();}
        }
    }
};
} // namespace dinero::p2p
